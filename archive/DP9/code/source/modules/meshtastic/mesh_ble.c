// mesh_ble.c — Meshtastic BLE phone-API companion service (see mesh_ble.h
// for the UUID-verification caveat before relying on this for real interop).
//
// NimBLE host stack — migrated from Bluedroid along with bt_mgr.c (see that
// file's header comment and CoreOS/sdkconfig_tdeck_plus.overrides for why).
//
// IMPORTANT ordering constraint: NimBLE's ble_gatts_add_svcs() only *queues*
// a service — it gets registered once, automatically, the moment the NimBLE
// host actually starts running (internally, on the host's first pass through
// ble_hs_start()). Queue too late and the service silently never exists.
//
// The NimBLE controller/host don't come up at boot at all anymore (see
// bt_mgr.h's header comment — confirmed live that bringing the BT
// controller up unconditionally at boot permanently starved this board's
// small internal DMA-capable memory pool, breaking SD card reads for the
// rest of boot). Bring-up is now lazy: bt_mgr_ensure_active() runs the
// first time the user actually asks for Bluetooth (Settings' toggle or
// mesh_ble_set_advertising(true) below), and only THEN is there a host to
// queue a service into. So instead of calling ble_gatts_add_svcs()
// directly from mesh_ble_init() (P3, boot time — long before that first
// activation), this file registers mesh_ble_queue_gatt_service() with
// bt_mgr via bt_mgr_register_gatt_provider(); bt_mgr_ensure_active() calls
// it back at exactly the right moment — after ble_svc_gap_init()/
// ble_svc_gatt_init(), before the host's first start.
//
// Standard NimBLE ble_gatt_svc_def attribute-table pattern (same shape as
// IDF's own bleprph/main/gatt_svr.c example): one service, three
// characteristics (toradio write, fromradio app-handled read, fromnum
// notify), advertised only when mesh_ble_set_advertising(true) is called
// (Settings' Bluetooth toggle, via bt_mgr).
//
// Known limitation: mesh_rx_cb_t (meshtastic.h) only carries
// (from, portnum, payload, len) — not the packet's original `to`/hop_limit/
// channel — so a forwarded FromRadio.packet always reports `to` as
// broadcast even if the original packet was a direct message. Good enough
// for the phone app to see message content; not a full packet mirror.

#include "mesh_ble.h"
#include "meshtastic.h"
#include "mesh_radio.h"
#include "bt_mgr.h"
#include "sdkconfig.h"

// This whole file only builds against NimBLE (CONFIG_BT_NIMBLE_ENABLED,
// currently tdeck_plus only — see bt_mgr.c's matching guard/comment). Every
// device compiles this module unconditionally, so the #else stub below
// keeps mesh_ble_init()/deinit()/set_advertising() real, linkable no-ops
// everywhere else instead of a build failure on missing NimBLE headers.
#ifdef CONFIG_BT_NIMBLE_ENABLED

#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "esp_log.h"
#include <pb_encode.h>
#include <pb_decode.h>
#include "meshtastic/mesh.pb.h"
#include "meshtastic/portnums.pb.h"
#include <string.h>

static const char *TAG = "mesh_ble";

#define MESH_BLE_FRAME_MAX  256
#define MESH_BLE_QUEUE_LEN  8

// See mesh_ble.h — reproduced from memory of Meshtastic's public BLE spec,
// byte-reversed (little-endian) from the human-readable UUID strings. Same
// byte order NimBLE's ble_uuid128_t/BLE_UUID128_INIT expects (over-the-air
// LSB-first, same convention Bluedroid used) — carried over unchanged.
static const ble_uuid128_t s_svc_uuid = BLE_UUID128_INIT(
    0xfd,0xea,0x73,0xe2,0xca,0x5d,0xa8,0x9f,0x1f,0x46,0xa8,0x15,0x18,0xb2,0xa1,0x6b);
static const ble_uuid128_t s_toradio_uuid = BLE_UUID128_INIT(
    0xe7,0x01,0x44,0x12,0x66,0x78,0xdd,0xa1,0xad,0x4d,0x9e,0x12,0xd2,0x76,0x5c,0xf7);
static const ble_uuid128_t s_fromradio_uuid = BLE_UUID128_INIT(
    0x02,0x00,0x12,0xac,0x42,0x02,0x78,0xb8,0xed,0x11,0x93,0x49,0x9e,0xe6,0x55,0x2c);
static const ble_uuid128_t s_fromnum_uuid = BLE_UUID128_INIT(
    0x53,0x44,0xe3,0x47,0x75,0xaa,0x70,0xa6,0x66,0x4f,0x00,0xa8,0x8c,0xa1,0x9d,0xed);

static uint16_t s_toradio_val_handle;
static uint16_t s_fromradio_val_handle;
static uint16_t s_fromnum_val_handle;

static uint8_t s_own_addr_type;
static bool    s_want_advertising = false;

static uint8_t s_fromnum_val[4] = {0, 0, 0, 0};

// ── Outgoing frame queue (mesh RX → phone) ───────────────────────────────────

static uint8_t  s_out_queue[MESH_BLE_QUEUE_LEN][MESH_BLE_FRAME_MAX];
static size_t   s_out_len[MESH_BLE_QUEUE_LEN];
static int      s_out_head = 0, s_out_tail = 0, s_out_count = 0;
static uint32_t s_fromnum = 0;

static void enqueue_frame(const uint8_t *data, size_t len) {
    if (len > MESH_BLE_FRAME_MAX) return;
    if (s_out_count >= MESH_BLE_QUEUE_LEN) {
        // drop oldest — a phone that's fallen behind will re-sync via fromnum
        s_out_head = (s_out_head + 1) % MESH_BLE_QUEUE_LEN;
        s_out_count--;
    }
    memcpy(s_out_queue[s_out_tail], data, len);
    s_out_len[s_out_tail] = len;
    s_out_tail = (s_out_tail + 1) % MESH_BLE_QUEUE_LEN;
    s_out_count++;

    s_fromnum++;
    memcpy(s_fromnum_val, &s_fromnum, sizeof(s_fromnum));

    // s_fromnum_val_handle is only ever assigned by NimBLE once the GATT
    // service actually finishes registering, which only happens after the
    // host starts (see this file's header comment) — itself only triggered
    // by the user turning Bluetooth on (bt_mgr_ensure_active()). But
    // mesh_manager_add_rx_callback(on_mesh_rx) below is registered
    // unconditionally at P3 boot, so a mesh packet can arrive — and did,
    // live — long before Bluetooth is ever touched. Calling
    // ble_gatts_chr_updated() on a still-zero/unregistered handle in that
    // window crashed the device outright (Core 1 LoadProhibited inside
    // NimBLE's own ble_gatts_chr_updated, confirmed live from a passively-
    // received mesh packet, no Settings interaction at all). Frames still
    // queue above regardless — harmless, and lets a phone that connects
    // later catch up via fromradio reads — only the live notify needs to
    // wait. bt_mgr_host_ready() is the same "is the host actually up" check
    // mesh_ble_set_advertising() already gates start_advertising() on.
    if (bt_mgr_host_ready()) {
        ble_gatts_chr_updated(s_fromnum_val_handle);
    }
}

static bool dequeue_frame(uint8_t *out, size_t *out_len) {
    if (s_out_count == 0) { *out_len = 0; return false; }
    size_t n = s_out_len[s_out_head];
    memcpy(out, s_out_queue[s_out_head], n);
    *out_len = n;
    s_out_head = (s_out_head + 1) % MESH_BLE_QUEUE_LEN;
    s_out_count--;
    return true;
}

// ── Mesh RX → FromRadio ───────────────────────────────────────────────────────

static void on_mesh_rx(uint32_t from_node, uint32_t to_node, int channel_idx, int portnum, const uint8_t *payload, size_t len) {
    (void)to_node; (void)channel_idx;  // see file header note — this bridge doesn't mirror channel/to context either
    meshtastic_FromRadio fr = meshtastic_FromRadio_init_zero;
    fr.which_payload_variant = meshtastic_FromRadio_packet_tag;
    fr.packet.from = from_node;
    fr.packet.to   = (uint32_t)MESH_BROADCAST;   // see file header note — original `to` isn't available here
    fr.packet.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    fr.packet.decoded.portnum = (meshtastic_PortNum)portnum;
    size_t n = len < sizeof(fr.packet.decoded.payload.bytes) ? len : sizeof(fr.packet.decoded.payload.bytes);
    memcpy(fr.packet.decoded.payload.bytes, payload, n);
    fr.packet.decoded.payload.size = (pb_size_t)n;

    uint8_t wire[MESH_BLE_FRAME_MAX];
    pb_ostream_t os = pb_ostream_from_buffer(wire, sizeof(wire));
    if (pb_encode(&os, meshtastic_FromRadio_fields, &fr)) {
        enqueue_frame(wire, os.bytes_written);
    }
}

// ── ToRadio write → mesh TX ───────────────────────────────────────────────────

static void handle_toradio_write(const uint8_t *data, size_t len) {
    meshtastic_ToRadio tr = meshtastic_ToRadio_init_zero;
    pb_istream_t is = pb_istream_from_buffer(data, len);
    if (!pb_decode(&is, meshtastic_ToRadio_fields, &tr)) {
        ESP_LOGW(TAG, "toradio decode failed");
        return;
    }
    if (tr.which_payload_variant != meshtastic_ToRadio_packet_tag) return;
    if (tr.packet.which_payload_variant != meshtastic_MeshPacket_decoded_tag) return;
    if (tr.packet.decoded.portnum != meshtastic_PortNum_TEXT_MESSAGE_APP) return;

    char text[241];
    size_t n = tr.packet.decoded.payload.size < sizeof(text) - 1 ? tr.packet.decoded.payload.size : sizeof(text) - 1;
    memcpy(text, tr.packet.decoded.payload.bytes, n);
    text[n] = '\0';

    uint32_t to = tr.packet.to ? tr.packet.to : (uint32_t)MESH_BROADCAST;
    mesh_manager_send_text(to, 0, text);   // primary channel — see file header note
}

// ── GATT access callbacks (one per characteristic) ───────────────────────────
// NimBLE's access model is synchronous per-characteristic, unlike
// Bluedroid's one big event-switch callback keyed by handle — no deferred
// "app response" dance needed for the app-handled fromradio read; just
// compute and append to ctxt->om directly.

static int toradio_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle; (void)attr_handle; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;

    uint8_t buf[MESH_BLE_FRAME_MAX];
    uint16_t len = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len) != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    handle_toradio_write(buf, len);
    return 0;
}

static int fromradio_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle; (void)attr_handle; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_UNLIKELY;

    // Each read drains one queued FromRadio frame (or 0 bytes if the queue
    // is empty), matching Meshtastic's real protocol.
    uint8_t frame[MESH_BLE_FRAME_MAX];
    size_t  frame_len = 0;
    dequeue_frame(frame, &frame_len);
    int rc = os_mbuf_append(ctxt->om, frame, frame_len);
    return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int fromnum_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle; (void)attr_handle; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_UNLIKELY;

    int rc = os_mbuf_append(ctxt->om, s_fromnum_val, sizeof(s_fromnum_val));
    return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

// ── GATT service table ────────────────────────────────────────────────────────

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid       = &s_toradio_uuid.u,
                .access_cb  = toradio_access_cb,
                .flags      = BLE_GATT_CHR_F_WRITE,
                .val_handle = &s_toradio_val_handle,
            }, {
                .uuid       = &s_fromradio_uuid.u,
                .access_cb  = fromradio_access_cb,
                .flags      = BLE_GATT_CHR_F_READ,
                .val_handle = &s_fromradio_val_handle,
            }, {
                .uuid       = &s_fromnum_uuid.u,
                .access_cb  = fromnum_access_cb,
                .flags      = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_fromnum_val_handle,
            }, {
                0,   // no more characteristics in this service
            }
        },
    },
    {
        0,   // no more services
    },
};

// Invoked by bt_mgr_ensure_active() (via bt_mgr_register_gatt_provider())
// right before it starts the NimBLE host for the first time — see this
// file's header comment for why this can't just happen inside
// mesh_ble_init() anymore.
static void mesh_ble_queue_gatt_service(void) {
    int rc = ble_gatts_count_cfg(s_gatt_svcs);
    if (rc != 0) { ESP_LOGE(TAG, "count_cfg failed: %d", rc); return; }
    rc = ble_gatts_add_svcs(s_gatt_svcs);
    if (rc != 0) { ESP_LOGE(TAG, "add_svcs failed: %d", rc); return; }
    ESP_LOGI(TAG, "GATT service queued");
}

// ── GAP (connection lifecycle) callback ──────────────────────────────────────

static void start_advertising(void);

static int gap_event_cb(struct ble_gap_event *event, void *arg) {
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        ESP_LOGI(TAG, "peer connected, status=%d", event->connect.status);
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "peer disconnected, reason=0x%x", event->disconnect.reason);
        if (s_want_advertising) start_advertising();
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "peer subscribe: attr_handle=%d notify=%d indicate=%d",
                 event->subscribe.attr_handle, event->subscribe.cur_notify,
                 event->subscribe.cur_indicate);
        break;
    default:
        break;
    }
    return 0;
}

// ── Public API ────────────────────────────────────────────────────────────────

static void start_advertising(void) {
    // Deliberately queried here, not in mesh_ble_init() — at init() time
    // (during static-module load, P3) the NimBLE host doesn't even exist
    // yet (see this file's header comment), so ble_hs_id_infer_auto()
    // would fail. By the time mesh_ble_set_advertising(true) calls this,
    // bt_mgr_ensure_active() has already run and synced with the
    // controller.
    int rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) { ESP_LOGE(TAG, "infer_auto failed: %d", rc); return; }

    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    static const char *dev_name = "PURR-Mesh";
    fields.name           = (const uint8_t *)dev_name;
    fields.name_len        = strlen(dev_name);
    fields.name_is_complete = 1;

    fields.uuids128           = (ble_uuid128_t[]) { s_svc_uuid };
    fields.num_uuids128        = 1;
    fields.uuids128_is_complete = 1;

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) { ESP_LOGE(TAG, "adv_set_fields failed: %d", rc); return; }

    struct ble_gap_adv_params adv_params = {0};
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &adv_params, gap_event_cb, NULL);
    if (rc != 0) ESP_LOGE(TAG, "adv_start failed: %d", rc);
}

void mesh_ble_set_advertising(bool on) {
    if (on) {
        // Brings the controller/host up on demand if nothing has yet —
        // covers the case where a user enables the Meshtastic phone-
        // companion feature directly, without ever visiting Settings'
        // Bluetooth toggle. Idempotent/cheap if already active.
        if (bt_mgr_ensure_active() != ESP_OK) {
            ESP_LOGW(TAG, "cannot advertise — BLE host activation failed");
            return;
        }
    }
    // Live check, not a cached flag from init() time — the host may come
    // up long after mesh_ble_init() ran (or never, if activation fails).
    if (!bt_mgr_host_ready()) return;

    s_want_advertising = on;
    if (on) start_advertising();
    else    ble_gap_adv_stop();
}

int mesh_ble_init(void) {
    // No NimBLE calls here anymore — the host doesn't exist yet at P3
    // boot time (see this file's header comment). Just register this
    // module's rx callback and hand bt_mgr the callback that queues our
    // GATT service whenever the host actually comes up later.
    mesh_manager_add_rx_callback(on_mesh_rx);
    bt_mgr_register_gatt_provider(mesh_ble_queue_gatt_service);
    ESP_LOGI(TAG, "init complete (BLE companion service pending activation)");
    return 0;
}

void mesh_ble_deinit(void) {
    mesh_manager_remove_rx_callback(on_mesh_rx);
    if (!bt_mgr_host_ready()) return;
    mesh_ble_set_advertising(false);
}

#else  // !CONFIG_BT_NIMBLE_ENABLED — see this file's top-of-file comment

int  mesh_ble_init(void) { return 0; }
void mesh_ble_deinit(void) {}
void mesh_ble_set_advertising(bool on) { (void)on; }

#endif  // CONFIG_BT_NIMBLE_ENABLED
