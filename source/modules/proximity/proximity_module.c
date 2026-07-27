// proximity_module.c — PURR_MOD_SYSTEM registration + shared ESP-NOW
// dispatch (beacon discovery + pairing/relay frame routing) + in-RAM
// beacon device table.
//
// No Kconfig feature gate, unlike meshtastic/meshcore — nothing else
// contends for ESP-NOW/WiFi, so the two-layer stub/real gate pattern
// doesn't apply here. Matches wifi_mgr's simpler "always compiled in,
// device.pcat opt-in" precedent.
//
// One lightweight task, not meshcore's heavy protocol-stack shape —
// ESP-NOW's own API is callback-driven and non-blocking, so there's no
// need for a tight poll loop or a TX queue beyond the mandatory recv-
// callback handoff. No NVS access anywhere in this module's task, so
// (unlike meshtastic/meshcore) there's no PSRAM-stack-vs-flash-cache
// hazard to design around — a PSRAM-backed task stack is safe here with
// no caveats.
//
// This module owns ESP-NOW's single global recv-callback slot for the
// whole OS (esp_now_register_recv_cb() has no multi-subscriber concept —
// see proximity.h's header comment), so beyond beacon discovery it also
// dispatches other frame types (pairing, relay) to handlers registered via
// proximity_register_handler() — see the "Remote radio companion" plan.

#include "proximity.h"
#include "../../kernel/core/purr_kernel.h"
#include "../../kernel/core/purr_module.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/idf_additions.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "proximity";

#define BEACON_INTERVAL_MS  8000UL
#define STALE_MS            30000UL
#define TICK_MS              500UL
#define PROXIMITY_WATCHDOG_STALE_MS 5000UL

#define BEACON_MAGIC   0x50525842UL   // 'PRXB'
#define BEACON_VERSION 1

// Leading .type byte matches every other frame this module dispatches
// (PROXIMITY_FRAME_BEACON) — magic+version stay as a second layer of
// defense-in-depth against a stray non-PurrOS ESP-NOW sender on the same
// channel, not the primary discriminator anymore.
typedef struct __attribute__((packed)) {
    uint8_t  type;
    uint32_t magic;
    uint8_t  version;
    uint8_t  caps;
    char     name[20];
} beacon_payload_t;

// ESP-NOW's hard per-packet cap in this ESP-IDF version — sized to fit any
// frame type this module carries, not just beacon.
typedef struct {
    uint8_t mac[6];
    int8_t  rssi;
    uint8_t payload[250];
    size_t  payload_len;
} rx_queue_item_t;

#define MAX_FRAME_HANDLERS 4
typedef struct {
    bool                       used;
    proximity_frame_type_t     type;
    proximity_frame_handler_t  handler;
} frame_handler_slot_t;

static bool          s_ready = false;
static TaskHandle_t  s_task = NULL;
// Which allocator actually created s_task's stack — deinit() must delete
// with the matching function (vTaskDeleteWithCaps() for a WithCaps-created
// task, plain vTaskDelete() otherwise), same rule app_manager.c/meshtastic_
// module.c/meshcore_module.cpp all follow; mismatching them is a real bug,
// not just a style nit.
static bool          s_task_uses_psram_stack = false;
static QueueHandle_t s_rx_queue = NULL;
static uint8_t       s_own_mac[6];
static char          s_own_name[20];
static uint8_t       s_own_caps = 0;
static volatile uint32_t s_last_heartbeat_ms = 0;

static const uint8_t s_broadcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static proximity_device_t s_devices[PROXIMITY_MAX_DEVICES];
static int                 s_device_count = 0;

static frame_handler_slot_t s_handlers[MAX_FRAME_HANDLERS];

// ── ESP-NOW send-status callback ────────────────────────────────────────
// esp_now_send() returning ESP_OK only means "handed to the radio driver"
// — this is the only way to find out whether a peer actually 802.11-ACKed
// it. Diagnostic-only for now (just logs); a failure here on a unicast
// frame very likely means a channel mismatch — ESP-NOW is channel-bound,
// and peer.channel = 0 ("ride whatever channel STA is currently on," see
// proximity_add_peer()) drifts if this device's WiFi is actively scanning/
// reconnecting to an AP the other peer isn't also on.
static void on_espnow_send(const uint8_t *mac, esp_now_send_status_t status) {
    if (status != ESP_NOW_SEND_SUCCESS && mac) {
        ESP_LOGW(TAG, "send to %02X:%02X:%02X:%02X:%02X:%02X FAILED (no ACK — likely channel mismatch)",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
}

// ── ESP-NOW recv callback ───────────────────────────────────────────────
// Runs in WiFi-driver task context (confirmed via ESP-IDF's own docs) —
// minimal work only: copy raw bytes into a queue item, no validation, no
// table access, no logging. All real work happens on proximity_task().
static void on_espnow_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    if (!info || !info->src_addr || !data || len <= 0) return;
    if ((size_t)len > sizeof(((rx_queue_item_t *)0)->payload)) return;
    if (!s_rx_queue) return;

    rx_queue_item_t item;
    memcpy(item.mac, info->src_addr, 6);
    item.rssi = info->rx_ctrl ? (int8_t)info->rx_ctrl->rssi : 0;
    item.payload_len = (size_t)len;
    memcpy(item.payload, data, item.payload_len);
    xQueueSend(s_rx_queue, &item, 0);
}

// ── Device table ─────────────────────────────────────────────────────────

static int find_device_by_mac(const uint8_t *mac) {
    for (int i = 0; i < s_device_count; i++) {
        if (memcmp(s_devices[i].mac, mac, 6) == 0) return i;
    }
    return -1;
}

static void handle_beacon(const rx_queue_item_t *item, uint32_t now_ms) {
    if (item->payload_len != sizeof(beacon_payload_t)) return;

    beacon_payload_t beacon;
    memcpy(&beacon, item->payload, sizeof(beacon));
    if (beacon.magic != BEACON_MAGIC || beacon.version != BEACON_VERSION) return;   // not one of ours

    char name[sizeof(beacon.name) + 1];
    memcpy(name, beacon.name, sizeof(beacon.name));
    name[sizeof(beacon.name)] = 0;   // beacon.name may not be NUL-terminated if it filled the field exactly

    int idx = find_device_by_mac(item->mac);
    if (idx < 0) {
        if (s_device_count >= PROXIMITY_MAX_DEVICES) return;   // table full
        idx = s_device_count++;
        memcpy(s_devices[idx].mac, item->mac, 6);
        s_devices[idx].first_seen_ms = now_ms;

        char notify_body[64];
        snprintf(notify_body, sizeof(notify_body), "%s is nearby", name);
        purr_kernel_notify("Nearby device", notify_body, "proximity");
    }
    strncpy(s_devices[idx].name, name, sizeof(s_devices[idx].name) - 1);
    s_devices[idx].name[sizeof(s_devices[idx].name) - 1] = 0;
    s_devices[idx].caps = beacon.caps;
    s_devices[idx].rssi = item->rssi;
    s_devices[idx].last_seen_ms = now_ms;
}

static void handle_rx_item(const rx_queue_item_t *item, uint32_t now_ms) {
    if (memcmp(item->mac, s_own_mac, 6) == 0) return;   // ESP-NOW broadcast can loop back to sender
    if (item->payload_len < 1) return;

    uint8_t type = item->payload[0];
    if (type == PROXIMITY_FRAME_BEACON) {
        handle_beacon(item, now_ms);
        return;
    }

    for (int i = 0; i < MAX_FRAME_HANDLERS; i++) {
        if (s_handlers[i].used && (uint8_t)s_handlers[i].type == type) {
            s_handlers[i].handler(item->mac, item->rssi, item->payload + 1, item->payload_len - 1);
            return;
        }
    }
    // No handler registered for this type — silently ignore, same as
    // meshtastic's own unknown-portnum handling.
}

static void prune_stale(uint32_t now_ms) {
    for (int i = 0; i < s_device_count; ) {
        if (now_ms - s_devices[i].last_seen_ms >= STALE_MS) {
            memmove(&s_devices[i], &s_devices[i + 1],
                    sizeof(proximity_device_t) * (size_t)(s_device_count - i - 1));
            s_device_count--;
        } else {
            i++;
        }
    }
}

static void send_beacon(void) {
    beacon_payload_t beacon;
    beacon.type = PROXIMITY_FRAME_BEACON;
    beacon.magic = BEACON_MAGIC;
    beacon.version = BEACON_VERSION;
    beacon.caps = s_own_caps;
    memset(beacon.name, 0, sizeof(beacon.name));
    strncpy(beacon.name, s_own_name, sizeof(beacon.name) - 1);

    esp_err_t err = esp_now_send(s_broadcast_mac, (const uint8_t *)&beacon, sizeof(beacon));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_now_send() failed: %d", (int)err);
    }
}

// ── Task ─────────────────────────────────────────────────────────────────

static void proximity_task(void *arg) {
    (void)arg;
    uint32_t last_beacon_ms = 0;

    for (;;) {
        uint32_t now_ms = (uint32_t)purr_kernel_uptime_ms();

        if (now_ms - last_beacon_ms >= BEACON_INTERVAL_MS) {
            send_beacon();
            last_beacon_ms = now_ms;
        }

        rx_queue_item_t item;
        while (xQueueReceive(s_rx_queue, &item, 0) == pdTRUE) {
            handle_rx_item(&item, now_ms);
        }

        prune_stale(now_ms);
        s_last_heartbeat_ms = now_ms;

        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    }
}

// ── Lifecycle ────────────────────────────────────────────────────────────

bool proximity_ready(void)    { return s_ready; }
bool proximity_is_alive(void) {
    if (!s_ready) return false;
    return ((uint32_t)purr_kernel_uptime_ms() - s_last_heartbeat_ms) < PROXIMITY_WATCHDOG_STALE_MS;
}

int proximity_device_count(void) { return s_device_count; }

bool proximity_device_at(int idx, proximity_device_t *out) {
    if (idx < 0 || idx >= s_device_count) return false;
    *out = s_devices[idx];
    return true;
}

void proximity_set_own_caps(uint8_t caps) { s_own_caps = caps; }

void proximity_get_own_name(char *out, size_t out_len) {
    if (!out || out_len == 0) return;
    snprintf(out, out_len, "%s", s_own_name);
}

// ── Shared ESP-NOW frame dispatch ───────────────────────────────────────

void proximity_register_handler(proximity_frame_type_t type, proximity_frame_handler_t handler) {
    // NULL means "unregister" — frees the slot entirely (not just clears
    // the function pointer), so a frame arriving right after a module's
    // deinit() can't dispatch through a stale/NULL handler.
    for (int i = 0; i < MAX_FRAME_HANDLERS; i++) {
        if (s_handlers[i].used && s_handlers[i].type == type) {
            if (!handler) {
                s_handlers[i].used = false;
                s_handlers[i].handler = NULL;
            } else {
                s_handlers[i].handler = handler;
            }
            return;
        }
    }
    if (!handler) return;   // unregistering a type with no active slot — nothing to do
    for (int i = 0; i < MAX_FRAME_HANDLERS; i++) {
        if (!s_handlers[i].used) {
            s_handlers[i].used = true;
            s_handlers[i].type = type;
            s_handlers[i].handler = handler;
            return;
        }
    }
    ESP_LOGW(TAG, "no free frame handler slots for type %d", (int)type);
}

bool proximity_add_peer(const uint8_t *mac) {
    if (!mac) return false;
    if (esp_now_is_peer_exist(mac)) return true;

    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, mac, 6);
    peer.channel = 0;              // ride whatever channel the STA interface is currently on
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;           // no PMK/LMK yet — see proximity.h's scoping note
    esp_err_t err = esp_now_add_peer(&peer);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_now_add_peer(unicast) failed: %d", (int)err);
        return false;
    }
    return true;
}

bool proximity_send_unicast(const uint8_t *mac, proximity_frame_type_t type,
                             const uint8_t *payload, size_t payload_len) {
    if (!mac || payload_len > PROXIMITY_MAX_PAYLOAD) return false;
    if (!proximity_add_peer(mac)) return false;

    uint8_t buf[1 + PROXIMITY_MAX_PAYLOAD];
    buf[0] = (uint8_t)type;
    if (payload_len) memcpy(buf + 1, payload, payload_len);

    esp_err_t err = esp_now_send(mac, buf, (int)(1 + payload_len));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_now_send(unicast) failed: %d", (int)err);
        return false;
    }
    return true;
}

// Brings up the WiFi driver (STA mode, not connected to anything) if it
// isn't already — most devices (tdeck_plus) have wifi_mgr/a specialized
// boot file that already did this, but this module can't assume that (see
// heltec's device.pcat: no wifi_mgr, no dedicated boot file at all). Safe
// to call unconditionally: esp_wifi_get_mode() cleanly distinguishes
// "already initialized" (ESP_OK) from "never initialized"
// (ESP_ERR_WIFI_NOT_INIT), so a device that already brought WiFi up itself
// (kernel_tdp_boot.c's own esp_netif_init()/esp_wifi_init()/_set_mode()/
// wifi_mgr's esp_wifi_start() sequence) is untouched by this.
static void ensure_wifi_ready(void) {
    wifi_mode_t mode;
    if (esp_wifi_get_mode(&mode) == ESP_OK) return;   // already up — nothing to do

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init() failed: %d", (int)err);
        return;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default() failed: %d", (int)err);
        return;
    }
    // esp_netif_create_default_wifi_sta() is NOT idempotent — it allocates
    // a fresh netif every call — so this only runs if no default STA netif
    // exists yet (it wouldn't on a device landing in the branch above, but
    // checking directly is cheap and avoids ever assuming that).
    if (!esp_netif_get_handle_from_ifkey("WIFI_STA_DEF")) {
        esp_netif_create_default_wifi_sta();
    }

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&wifi_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init() failed: %d", (int)err);
        return;
    }
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode() failed: %d", (int)err);
        return;
    }
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start() failed: %d", (int)err);
    }
}

int proximity_init(void) {
    esp_read_mac(s_own_mac, ESP_MAC_WIFI_STA);
    snprintf(s_own_name, sizeof(s_own_name), "PurrOS-%02X%02X", s_own_mac[4], s_own_mac[5]);

    ensure_wifi_ready();

    esp_err_t err = esp_now_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_init() failed: %d", (int)err);
        return -1;
    }

    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, s_broadcast_mac, 6);
    peer.channel = 0;              // ride whatever channel the STA interface is currently on
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;           // broadcast frames can't be encrypted anyway (ESP-NOW limitation)
    err = esp_now_add_peer(&peer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_add_peer(broadcast) failed: %d", (int)err);
        esp_now_deinit();
        return -1;
    }

    esp_now_register_recv_cb(on_espnow_recv);
    esp_now_register_send_cb(on_espnow_send);

    s_rx_queue = xQueueCreate(8, sizeof(rx_queue_item_t));
    if (!s_rx_queue) {
        ESP_LOGE(TAG, "failed to create rx queue");
        esp_now_deinit();
        return -1;
    }

    s_device_count = 0;
    // s_own_caps is deliberately NOT reset here — same load-order reasoning
    // as s_handlers below: a module that wants to advertise a capability
    // (e.g. oled_ui_init() setting PROXIMITY_CAP_RADIO_COMPANION) may run
    // its own init() before or after this one depending on load_priority
    // (oled_ui is P2/IMPORTANT, this module is P3/OPTIONAL — oled_ui goes
    // first), and resetting here would silently discard a capability set
    // by a module that happened to init first.
    // s_handlers is deliberately NOT cleared here: registration is decoupled
    // from this module's own lifecycle (proximity_register_handler() just
    // writes a file-scope array, safe to call regardless of ESP-NOW init
    // state) — another module's init() may run before or after this one
    // (load order within the same priority/type tier isn't guaranteed, see
    // purr_kernel.c), and wiping registrations here would silently drop a
    // handler registered by a module that happened to init first. Static
    // globals already start zeroed; proximity_deinit() clears them on
    // actual subsystem shutdown.
    s_last_heartbeat_ms = (uint32_t)purr_kernel_uptime_ms();

    // No NVS access anywhere in proximity_task()'s call graph (device
    // table is deliberately in-RAM only, see proximity.h) — a PSRAM stack
    // is safe here with no caveats, unlike meshtastic/meshcore.
    s_task_uses_psram_stack = heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0;
    BaseType_t ok = s_task_uses_psram_stack
        ? xTaskCreateWithCaps(proximity_task, "proximity", 4096, NULL, 3, &s_task, MALLOC_CAP_SPIRAM)
        : xTaskCreate(proximity_task, "proximity", 4096, NULL, 3, &s_task);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "failed to create proximity task");
        vQueueDelete(s_rx_queue);
        s_rx_queue = NULL;
        esp_now_deinit();
        return -1;
    }

    purr_kernel_health_register("proximity", proximity_is_alive);

    s_ready = true;
    ESP_LOGI(TAG, "ready, own name=%s mac=%02X:%02X:%02X:%02X:%02X:%02X",
             s_own_name, s_own_mac[0], s_own_mac[1], s_own_mac[2], s_own_mac[3], s_own_mac[4], s_own_mac[5]);
    return 0;
}

void proximity_deinit(void) {
    // Callbacks OFF FIRST, before the task is deleted.
    //
    // The original order deleted the task and only then unregistered — which
    // left a window where the task could be killed inside an ESP-NOW callback
    // holding an internal lock, after which esp_now_deinit() below waits on
    // that lock forever. Confirmed on hardware: speed demon hung here, with the
    // UI backend already unloaded, so the device was a black screen with no way
    // back.
    //
    // Unregistering first means no new callback can start, and any in flight
    // completes before vTaskDelete() is reached.
    esp_now_unregister_recv_cb();
    esp_now_unregister_send_cb();

    if (s_task) {
        // Must match whichever allocator actually created this task's
        // stack (see proximity_init()'s comment) — vTaskDelete() on a
        // WithCaps-created task's handle is the wrong call, not just a
        // style nit (matches app_manager.c's/meshtastic_module.c's own
        // documented rule).
        if (s_task_uses_psram_stack) vTaskDeleteWithCaps(s_task);
        else                         vTaskDelete(s_task);
        s_task = NULL;
    }
    esp_now_deinit();
    if (s_rx_queue) {
        vQueueDelete(s_rx_queue);
        s_rx_queue = NULL;
    }
    s_ready = false;
    s_device_count = 0;
    s_own_caps = 0;
    memset(s_handlers, 0, sizeof(s_handlers));
}

// ── Module header ─────────────────────────────────────────────────────────

PURR_MODULE_REGISTER(proximity) = {
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_SYSTEM,
    .load_priority     = PURR_PRIORITY_OPTIONAL,
    .name              = "proximity",
    .version           = "1.0.0",
    .kernel_min        = "0.11.1",
    .kernel_max        = "",
    .provided_catcalls = 0,
    .required_catcalls = 0,
    .init              = proximity_init,
    .deinit            = proximity_deinit,
};
