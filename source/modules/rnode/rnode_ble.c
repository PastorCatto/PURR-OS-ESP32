// rnode_ble.c — see rnode_ble.h. NimBLE ordering constraints, lazy bring-
// up reasoning, and the ble_gatts_chr_updated()-before-handle-assignment
// crash lesson are all identical to meshtastic/mesh_ble.c's own header
// comment — not re-explained here, see that file.
//
// Real NUS UUIDs (confirmed via direct fetch of RNode_Firmware_CE's own
// src/ble/BLESerial.h — see the plan doc): service 6e400001-b5a3-f393-
// e0a9-e50e24dcca9e, RX (host->device, WRITE) 6e400002-..., TX (device->
// host, NOTIFY) 6e400003-.... Byte-reversed into BLE_UUID128_INIT's
// little-endian array form, same transformation mesh_ble.c's own UUIDs
// already use (confirmed by reversing its own array back into Meshtastic's
// real published UUID and matching it, before doing the same here).
//
// TX direction design: RX-characteristic writes and CMD_DATA radio
// arrivals can both trigger an outbound reply from two DIFFERENT task
// contexts (the NimBLE host task itself, synchronously inside the RX
// write's access callback; and rnode_module.c's own dedicated radio-RX
// task). Rather than call NimBLE notify APIs directly from either of
// those — the host-task case in particular is untested territory (calling
// back into NimBLE's own API from inside a callback already running on
// its host task isn't something this codebase has done before, unlike
// mesh_ble.c's enqueue_frame(), which is only ever reached from OTHER
// tasks calling INTO NimBLE, never from within the host task's own
// callback) — rnode_proto's output callback here just enqueues onto a
// plain FreeRTOS queue (memcpy, no NimBLE calls, safe from any context),
// and a small dedicated TX task drains it and performs the actual
// notify(s) on its own task context. Large frames (CMD_DATA, up to ~1KB
// once KISS-encoded) are chunked to the negotiated ATT MTU minus the
// 3-byte ATT header — KISS's own byte-stuffing survives being split
// across multiple notifies exactly like it survives being split across
// multiple incoming writes (see rnode_kiss.h's decoder).

#include "rnode_ble.h"
#include "rnode_kiss.h"
#include "rnode_proto.h"
#include "../bt_mgr/bt_mgr.h"
#include "sdkconfig.h"

// Same "always compile a real, linkable stub when NimBLE isn't in this
// build" shape mesh_ble.c uses — every device compiles this module
// unconditionally (source/modules/* always is), CONFIG_BT_NIMBLE_ENABLED
// is only on where a device's own [modules] bt = "bt_mgr" turns it on.
#ifdef CONFIG_BT_NIMBLE_ENABLED

#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>

static const char *TAG = "rnode_ble";

#define RNODE_BLE_FRAME_MAX  1024   // see rnode_proto.c's send_reply() for the worst-case encoded size this bounds
#define RNODE_BLE_QUEUE_LEN  4
#define RNODE_BLE_DEFAULT_ATT_MTU 23   // un-negotiated ATT default (20-byte payload after the 3-byte header)

// Nordic UART Service — see this file's own header comment for the
// human-readable UUIDs these arrays are the byte-reversed form of.
static const ble_uuid128_t s_svc_uuid = BLE_UUID128_INIT(
    0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,0x93,0xf3,0xa3,0xb5,0x01,0x00,0x40,0x6e);
static const ble_uuid128_t s_rx_uuid = BLE_UUID128_INIT(
    0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,0x93,0xf3,0xa3,0xb5,0x02,0x00,0x40,0x6e);
static const ble_uuid128_t s_tx_uuid = BLE_UUID128_INIT(
    0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,0x93,0xf3,0xa3,0xb5,0x03,0x00,0x40,0x6e);

static uint16_t s_rx_val_handle;
static uint16_t s_tx_val_handle;

static uint8_t  s_own_addr_type;
static bool     s_want_advertising = false;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static bool     s_tx_subscribed = false;
static uint16_t s_att_mtu = RNODE_BLE_DEFAULT_ATT_MTU;

static QueueHandle_t  s_tx_queue = NULL;
static TaskHandle_t   s_tx_task_handle = NULL;
static volatile bool  s_tx_task_running = false;

typedef struct {
    uint8_t data[RNODE_BLE_FRAME_MAX];
    size_t  len;
} rnode_ble_tx_item_t;

// ── rnode_proto's output callback — enqueue only, see file header ───────

static void proto_output(const uint8_t *kiss_frame, size_t len) {
    if (len == 0 || len > RNODE_BLE_FRAME_MAX) {
        ESP_LOGW(TAG, "outbound frame too large (%u), dropped", (unsigned)len);
        return;
    }
    rnode_ble_tx_item_t item;
    item.len = len;
    memcpy(item.data, kiss_frame, len);
    if (xQueueSend(s_tx_queue, &item, 0) != pdTRUE) {
        // Drop-oldest, same reasoning as mesh_ble.c's own enqueue_frame():
        // a host that's fallen behind (or never subscribed) shouldn't
        // stall live radio RX forever. One slot dropped from the front,
        // then this item queued.
        rnode_ble_tx_item_t discard;
        xQueueReceive(s_tx_queue, &discard, 0);
        xQueueSend(s_tx_queue, &item, 0);
    }
}

// ── TX drain task — the only place that actually calls into NimBLE's
// notify API, always from this task's own context, never inline from an
// access callback (see file header comment). ────────────────────────────

static void tx_task(void *arg) {
    (void)arg;
    rnode_ble_tx_item_t item;
    while (s_tx_task_running) {
        if (xQueueReceive(s_tx_queue, &item, pdMS_TO_TICKS(50)) != pdTRUE) continue;
        ESP_LOGD(TAG, "tx_task: dequeued %u bytes (host_ready=%d conn=%d subscribed=%d)",
                 (unsigned)item.len, bt_mgr_host_ready(), s_conn_handle != BLE_HS_CONN_HANDLE_NONE, s_tx_subscribed);
        if (!bt_mgr_host_ready() || s_conn_handle == BLE_HS_CONN_HANDLE_NONE || !s_tx_subscribed) {
            continue;   // nothing to deliver to right now — frame is simply dropped, same as mesh_ble.c's own "phone re-syncs later" tolerance
        }

        size_t chunk_max = (s_att_mtu > 3) ? (size_t)(s_att_mtu - 3) : 20;
        size_t sent = 0;
        while (sent < item.len) {
            size_t n = item.len - sent;
            if (n > chunk_max) n = chunk_max;
            struct os_mbuf *om = ble_hs_mbuf_from_flat(item.data + sent, n);
            if (!om) { ESP_LOGW(TAG, "mbuf alloc failed"); break; }
            int rc = ble_gatts_notify_custom(s_conn_handle, s_tx_val_handle, om);
            if (rc != 0) { ESP_LOGW(TAG, "notify failed rc=%d", rc); break; }
            ESP_LOGD(TAG, "notified %u bytes", (unsigned)n);
            sent += n;
        }
    }
    s_tx_task_handle = NULL;
    vTaskDelete(NULL);
}

// ── RX-characteristic write -> KISS decode -> rnode_proto ────────────────
// Runs synchronously on the NimBLE host task, same as meshtastic's own
// toradio_access_cb()/handle_toradio_write() — catcall_radio_send() is a
// bounded SPI transaction, not a long block, so handling it inline here
// (via rnode_proto_handle_frame(), which may call straight into
// catcall_radio_t) is consistent with that existing precedent.

static rnode_kiss_decoder_t s_rx_decoder;

static int rx_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                         struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle; (void)attr_handle; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        ESP_LOGW(TAG, "rx_access_cb: unexpected op=%d", ctxt->op);
        return BLE_ATT_ERR_UNLIKELY;
    }

    uint8_t buf[512];
    uint16_t len = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len) != 0) {
        ESP_LOGW(TAG, "rx_access_cb: mbuf_to_flat failed");
        return BLE_ATT_ERR_UNLIKELY;
    }
    ESP_LOGD(TAG, "rx write: %u bytes", (unsigned)len);
    for (uint16_t i = 0; i < len; i++) {
        if (rnode_kiss_decoder_feed(&s_rx_decoder, buf[i])) {
            ESP_LOGD(TAG, "rx frame complete: cmd=0x%02X len=%u", s_rx_decoder.buf[0], (unsigned)s_rx_decoder.len);
            rnode_proto_handle_frame(s_rx_decoder.buf, s_rx_decoder.len);
        }
    }
    return 0;
}

static int tx_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                         struct ble_gatt_access_ctxt *ctxt, void *arg) {
    // Notify-only in practice, but NimBLE requires an access_cb per
    // characteristic — a bare read (if a client ever issues one instead
    // of subscribing) just returns nothing pending, matching real NUS
    // implementations, which are notify-driven, not poll/read-driven.
    (void)conn_handle; (void)attr_handle; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_UNLIKELY;
    return 0;
}

// ── GATT service table ────────────────────────────────────────────────

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid       = &s_rx_uuid.u,
                .access_cb  = rx_access_cb,
                .flags      = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
                .val_handle = &s_rx_val_handle,
            }, {
                .uuid       = &s_tx_uuid.u,
                .access_cb  = tx_access_cb,
                .flags      = BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_tx_val_handle,
            }, {
                0,   // no more characteristics in this service
            }
        },
    },
    {
        0,   // no more services
    },
};

static void rnode_ble_queue_gatt_service(void) {
    // See mesh_ble.c's own header comment for why this must happen here
    // (right before the NimBLE host's first start) rather than in
    // rnode_ble_init() — the host doesn't exist yet at P3 boot time.
    // 512 (BLE_BUFFER_SIZE, matches real RNode/nRF52 firmware's own request) + 3-byte ATT header
    ble_att_set_preferred_mtu(515);
    int rc = ble_gatts_count_cfg(s_gatt_svcs);
    if (rc != 0) { ESP_LOGE(TAG, "count_cfg failed: %d", rc); return; }
    rc = ble_gatts_add_svcs(s_gatt_svcs);
    if (rc != 0) { ESP_LOGE(TAG, "add_svcs failed: %d", rc); return; }
    ESP_LOGI(TAG, "GATT service queued (Nordic UART Service)");
}

// ── GAP (connection lifecycle) callback ──────────────────────────────────

static void start_advertising(void);

static int gap_event_cb(struct ble_gap_event *event, void *arg) {
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        ESP_LOGI(TAG, "peer connected, status=%d", event->connect.status);
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            s_tx_subscribed = false;
            rnode_kiss_decoder_reset(&s_rx_decoder);
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "peer disconnected, reason=0x%x", event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_tx_subscribed = false;
        s_att_mtu = RNODE_BLE_DEFAULT_ATT_MTU;
        if (s_want_advertising) start_advertising();
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "peer subscribe: attr_handle=%d notify=%d",
                 event->subscribe.attr_handle, event->subscribe.cur_notify);
        if (event->subscribe.attr_handle == s_tx_val_handle) {
            s_tx_subscribed = event->subscribe.cur_notify;
        }
        break;
    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU negotiated: %d", event->mtu.value);
        s_att_mtu = event->mtu.value;
        break;
    default:
        break;
    }
    return 0;
}

// ── Public API ────────────────────────────────────────────────────────

static void start_advertising(void) {
    int rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) { ESP_LOGE(TAG, "infer_auto failed: %d", rc); return; }

    // Split across the primary advertisement and the scan response —
    // confirmed live (BLE_HS_EMSGSIZE, rc=4): flags + full name + a
    // 128-bit service UUID together overflow the legacy 31-byte
    // advertising PDU (3 + 12 + 18 = 33 bytes here). Standard fix for
    // this exact combination, and how real NUS peripherals normally do
    // it: name in the primary packet (discoverable by name at a glance),
    // the UUID in the separate scan-response packet (which gets its own
    // 31-byte budget) — a central doing full GATT discovery after
    // connecting doesn't actually need the UUID pre-advertised anyway,
    // this is purely a "help a scanner recognize it" nicety.
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    static const char *dev_name = "PURR-RNode";
    fields.name             = (const uint8_t *)dev_name;
    fields.name_len          = strlen(dev_name);
    fields.name_is_complete  = 1;

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) { ESP_LOGE(TAG, "adv_set_fields failed: %d", rc); return; }

    struct ble_hs_adv_fields rsp_fields = {0};
    rsp_fields.uuids128            = (ble_uuid128_t[]) { s_svc_uuid };
    rsp_fields.num_uuids128         = 1;
    rsp_fields.uuids128_is_complete = 1;

    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0) { ESP_LOGE(TAG, "adv_rsp_set_fields failed: %d", rc); return; }

    struct ble_gap_adv_params adv_params = {0};
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &adv_params, gap_event_cb, NULL);
    if (rc != 0) ESP_LOGE(TAG, "adv_start failed: %d", rc);
}

void rnode_ble_set_advertising(bool on) {
    if (on) {
        if (bt_mgr_ensure_active() != ESP_OK) {
            ESP_LOGW(TAG, "cannot advertise — BLE host activation failed");
            return;
        }
    }
    if (!bt_mgr_host_ready()) return;

    s_want_advertising = on;
    if (on) start_advertising();
    else    ble_gap_adv_stop();
}

int rnode_ble_init(void) {
    rnode_kiss_decoder_reset(&s_rx_decoder);
    if (!s_tx_queue) s_tx_queue = xQueueCreate(RNODE_BLE_QUEUE_LEN, sizeof(rnode_ble_tx_item_t));
    s_tx_task_running = true;
    if (!s_tx_task_handle) {
        // 3072 stack-overflowed the very first time this task actually
        // called ble_gatts_notify_custom()/ble_hs_mbuf_from_flat() —
        // confirmed live (esp_task_wdt's own stack-overflow detector,
        // not a guess). NimBLE's own host task (bt_mgr.c's host_task(),
        // via nimble_port_freertos_init()) needs real depth for the same
        // class of mbuf/ATT calls; matching that order of magnitude here
        // instead of guessing a smaller number a second time.
        xTaskCreate(tx_task, "rnode_ble_tx", 6144, NULL, 3, &s_tx_task_handle);
    }
    rnode_proto_init(proto_output);
    bt_mgr_register_gatt_provider(rnode_ble_queue_gatt_service);
    ESP_LOGI(TAG, "init complete (BLE service pending activation)");
    return 0;
}

void rnode_ble_deinit(void) {
    rnode_proto_deinit();
    s_tx_task_running = false;   // tx_task deletes itself on its next wake
    if (!bt_mgr_host_ready()) return;
    rnode_ble_set_advertising(false);

    // Actually tear the NimBLE host down — not just stop advertising —
    // to reclaim its ~64-70KB DRAM cost (see rnode_module.c's own
    // RNODE_MIN_FREE_INTERNAL comment for that measurement). Confirmed
    // live this was missing: a real reload-path test (rnode_module.c's
    // reload_radio_companion_stack(), called right after this function
    // returns) failed with "proximity_rpc: alloc failed... PSRAM and
    // internal RAM both exhausted" and "homebase: failed to create
    // homebase task" — advertising-stop alone leaves NimBLE's own
    // allocations fully resident, so the companion stack had nowhere
    // near enough memory to come back up. bt_mgr_deinit() is bt_mgr's
    // own real teardown (nimble_port_stop() + state reset) — calling it
    // directly (not via purr_kernel_module_set_enabled("bt_mgr", ...))
    // is safe and correct here: bt_mgr the PURR module stays "loaded"
    // from the kernel's point of view, just returned to the same
    // lazy/dormant resting state it's already in before ANY BLE
    // consumer ever calls bt_mgr_ensure_active() — see bt_mgr_init()'s
    // own header comment for that being bt_mgr's normal, expected idle
    // state, not a torn-down one.
    bt_mgr_deinit();
}

#else  // !CONFIG_BT_NIMBLE_ENABLED — see this file's top-of-file comment

int  rnode_ble_init(void) { return 0; }
void rnode_ble_deinit(void) {}
void rnode_ble_set_advertising(bool on) { (void)on; }

#endif  // CONFIG_BT_NIMBLE_ENABLED
