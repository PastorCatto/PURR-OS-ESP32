// bt_mgr.c — PURR OS Bluetooth manager (BLE only, see bt_mgr.h)
//
// NimBLE host stack — migrated from Bluedroid (see
// CoreOS/sdkconfig_tdeck_plus.overrides' Bluetooth section for the full
// story: Bluedroid's own startup couldn't allocate its internal work queue
// on this board's tight internal-DRAM budget, confirmed live via a real
// crash the very first time this module ever ran on real hardware).
// NimBLE funnels scan results, connection lifecycle, and security events
// through one GAP event callback per operation, rather than Bluedroid's
// separate GAP+GATTC callbacks — bt_mgr_scan()/bt_mgr_pair() both hand
// gap_event_cb() to the NimBLE calls that need one.
//
// GAP scan (blocking, semaphore-gated like wifi_mgr_scan()), and pairing via
// a GAP connection with security initiated ("Just Works" — IO capability
// NoInputNoOutput, no PIN entry UI exists yet).

#include "bt_mgr.h"
#include "esp_log.h"
#include "sdkconfig.h"

// bt_mgr is unconditionally compiled into every device's firmware (it's
// always in modulestrap's target list, regardless of device.pcat), but
// NimBLE's headers/component only exist when CONFIG_BT_NIMBLE_ENABLED=y
// (currently only tdeck_plus, via its .overrides — see bt_mgr.h's header
// comment). Confirmed live via a full 10-device build: every other device
// failed outright on a missing nimble/nimble_port.h. The #else branch below
// gives every caller (settings.c, mesh_ble.c) a real symbol back — a
// permanent "not available" answer — instead of a build failure.
#ifdef CONFIG_BT_NIMBLE_ENABLED

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>

// Declared, not defined, by NimBLE's store/config component (part of the
// same "bt" component this file already REQUIRES) — bonding-key storage.
// bleprph's own reference example forward-declares this the same way
// rather than including its deeply-nested header path.
void ble_store_config_init(void);

static const char *TAG = "bt_mgr";
#define MAX_SCAN_RESULTS 24

static bool s_enabled = false;
static bool s_host_ok = false;
static bt_scan_result_t s_scan[MAX_SCAN_RESULTS];
static int  s_scan_count = 0;
static SemaphoreHandle_t s_scan_done_sem = NULL;

static uint8_t s_own_addr_type;

// See bt_mgr_register_gatt_provider()'s header comment — single slot,
// invoked once from bt_mgr_ensure_active() right before the host starts.
static void (*s_gatt_provider)(void) = NULL;

// ── GAP event callback ───────────────────────────────────────────────────────

static int gap_event_cb(struct ble_gap_event *event, void *arg) {
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        struct ble_hs_adv_fields fields;
        if (ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data) != 0) break;
        if (s_scan_count >= MAX_SCAN_RESULTS) break;

        bt_scan_result_t *out = &s_scan[s_scan_count];
        memcpy(out->addr, event->disc.addr.val, 6);
        out->rssi = (int8_t)event->disc.rssi;

        if (fields.name && fields.name_len) {
            uint8_t n = fields.name_len;
            if (n > sizeof(out->name) - 1) n = sizeof(out->name) - 1;
            memcpy(out->name, fields.name, n);
            out->name[n] = '\0';
        } else {
            snprintf(out->name, sizeof(out->name), "%02X:%02X:%02X:%02X:%02X:%02X",
                     out->addr[0], out->addr[1], out->addr[2], out->addr[3], out->addr[4], out->addr[5]);
        }
        s_scan_count++;
        break;
    }
    case BLE_GAP_EVENT_DISC_COMPLETE:
        if (s_scan_done_sem) xSemaphoreGive(s_scan_done_sem);
        break;
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            ESP_LOGI(TAG, "connected — requesting encryption/bonding");
            int rc = ble_gap_security_initiate(event->connect.conn_handle);
            if (rc != 0) ESP_LOGW(TAG, "security_initiate failed: %d", rc);
        } else {
            ESP_LOGW(TAG, "connect failed, status=%d", event->connect.status);
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected, reason=0x%x", event->disconnect.reason);
        break;
    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "encryption change, status=%d", event->enc_change.status);
        break;
    default:
        break;
    }
    return 0;
}

// ── Public API ────────────────────────────────────────────────────────────────

void bt_mgr_register_gatt_provider(void (*queue_fn)(void)) {
    s_gatt_provider = queue_fn;
}

// Once the host actually comes up, it stays up for the rest of the
// module's lifetime (matching wifi_mgr's "driver always started"
// approach) — s_enabled just gates whether Settings/the Meshtastic
// companion service are allowed to actually scan or advertise; disabling
// cancels any in-progress scan immediately. Enabling is what triggers the
// lazy bring-up in the first place.
bool bt_mgr_set_enabled(bool on) {
    if (on) {
        if (bt_mgr_ensure_active() != ESP_OK) {
            ESP_LOGW(TAG, "cannot enable — host activation failed");
            return false;
        }
        s_enabled = true;
        return true;
    }
    s_enabled = false;
    if (s_host_ok) ble_gap_disc_cancel();
    return true;
}

bool bt_mgr_is_enabled(void) { return s_enabled; }

bool bt_mgr_host_ready(void) { return s_host_ok; }

int bt_mgr_scan(uint32_t duration_sec) {
    if (!s_enabled) return -1;
    s_scan_count = 0;

    struct ble_gap_disc_params disc_params = {0};
    disc_params.passive           = 0;      // active scan — request scan-response (name) data
    disc_params.filter_duplicates = 1;
    disc_params.itvl              = 0x50;
    disc_params.window            = 0x30;

    if (!s_scan_done_sem) s_scan_done_sem = xSemaphoreCreateBinary();
    if (!s_scan_done_sem) {
        // xSemaphoreCreateBinary() was never checked before — under real
        // memory pressure it can return NULL, and xSemaphoreTake() on a
        // NULL handle hits FreeRTOS's own configASSERT(pxQueue) and aborts
        // the whole device. Confirmed live via a real crash (assert failed:
        // xQueueSemaphoreTake ... ( pxQueue )) triggered from Settings'
        // Bluetooth "Scan" button — same fix carried over from the
        // Bluedroid version of this file.
        ESP_LOGE(TAG, "xSemaphoreCreateBinary failed — out of memory?");
        return -1;
    }

    int rc = ble_gap_disc(s_own_addr_type, (int32_t)(duration_sec * 1000), &disc_params, gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_disc failed: %d", rc);
        return -1;
    }

    xSemaphoreTake(s_scan_done_sem, pdMS_TO_TICKS((duration_sec + 2) * 1000));
    return s_scan_count;
}

int  bt_mgr_scan_count(void) { return s_scan_count; }

bool bt_mgr_scan_at(int idx, bt_scan_result_t *out) {
    if (idx < 0 || idx >= s_scan_count || !out) return false;
    *out = s_scan[idx];
    return true;
}

esp_err_t bt_mgr_pair(const uint8_t addr[6]) {
    if (!s_enabled) return ESP_ERR_INVALID_STATE;

    ble_addr_t peer_addr;
    peer_addr.type = BLE_ADDR_PUBLIC;
    memcpy(peer_addr.val, addr, 6);

    // Security (encryption/bonding) is requested from gap_event_cb() once
    // BLE_GAP_EVENT_CONNECT reports success — same "connect, then encrypt"
    // shape the previous Bluedroid version used across its GATTC connect
    // callback.
    int rc = ble_gap_connect(s_own_addr_type, &peer_addr, 30000, NULL, gap_event_cb, NULL);
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

// ── Module lifecycle ──────────────────────────────────────────────────────────

static void on_reset(int reason) {
    ESP_LOGW(TAG, "nimble host reset, reason=%d", reason);
}

static void on_sync(void) {
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) { ESP_LOGE(TAG, "ensure_addr failed: %d", rc); return; }
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) { ESP_LOGE(TAG, "infer_auto failed: %d", rc); return; }
    ESP_LOGI(TAG, "nimble host synced, addr_type=%d", s_own_addr_type);
}

static void host_task(void *param) {
    (void)param;
    // Returns only once nimble_port_stop() (bt_mgr_deinit()) runs.
    nimble_port_run();
    nimble_port_freertos_deinit();
}

int bt_mgr_init(void) {
    // Deliberately does nothing but reset state — the NimBLE controller
    // used to come up unconditionally right here, at boot, before it's
    // even known whether the user wants Bluetooth this session. Confirmed
    // live: the controller's own MALLOC_CAP_DMA allocations permanently
    // exhaust this board's small reserved DMA-capable memory pool within
    // seconds, which then breaks every SD card read (and therefore every
    // .meow/.hiss script load) for the rest of boot. See bt_mgr.h's header
    // comment and bt_mgr_ensure_active() for where bring-up actually
    // happens now.
    s_enabled = false;
    s_host_ok = false;
    return 0;
}

esp_err_t bt_mgr_ensure_active(void) {
    if (s_host_ok) return ESP_OK;

    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) { ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(ret)); return ret; }

    ble_hs_cfg.reset_cb        = on_reset;
    ble_hs_cfg.sync_cb         = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    // "Just Works" bonding: no PIN entry UI exists yet, so IO capability is
    // fixed at NoInputNoOutput. Good enough for simple accessories; devices
    // that require a displayed/typed passkey aren't supported. Direct
    // equivalent of the Bluedroid version's ESP_IO_CAP_NONE +
    // ESP_LE_AUTH_REQ_SC_BOND + 16-byte max key size.
    ble_hs_cfg.sm_io_cap        = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding       = 1;
    ble_hs_cfg.sm_mitm          = 1;
    ble_hs_cfg.sm_sc            = 1;
    ble_hs_cfg.sm_our_key_dist   = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    // Base GAP/GATT services — like any other queued service, these must be
    // registered before the host starts (see the loop below and
    // mesh_ble.c's header comment for the full ordering story).
    ble_svc_gap_init();
    ble_svc_gatt_init();
    int name_rc = ble_svc_gap_device_name_set("PURR-Mesh");
    if (name_rc != 0) ESP_LOGW(TAG, "gap_device_name_set failed: %d", name_rc);

    ble_store_config_init();
    s_host_ok = true;

    // One chance for the registered GATT provider (mesh_ble.c) to queue
    // its service — must happen after ble_svc_gap_init()/ble_svc_gatt_init()
    // above and before nimble_port_freertos_init() below actually starts
    // the host, since NimBLE only registers services queued before the
    // host's first start.
    if (s_gatt_provider) s_gatt_provider();

    nimble_port_freertos_init(host_task);
    ESP_LOGI(TAG, "activated (BLE only, NimBLE)");
    return ESP_OK;
}

void bt_mgr_deinit(void) {
    bt_mgr_set_enabled(false);
    if (s_host_ok) {
        // nimble_port_stop() alone only stops the host's run loop — it
        // does NOT free the ~64-70KB the controller+host stack actually
        // allocated (esp_bt_controller_init()/_enable() + esp_nimble_
        // init(), both called from inside nimble_port_init()). Confirmed
        // live via a real rnode_module.c offload/reload cycle test: the
        // radio-companion stack still failed to reinit ("proximity_rpc:
        // alloc failed... PSRAM and internal RAM both exhausted",
        // "homebase: failed to create homebase task") even with
        // nimble_port_stop() (and a 500ms settle pause) already in place
        // — the exact same "stop() vs deinit(), memory stays allocated
        // until deinit()" lesson this session already learned once for
        // esp_wifi_stop()/_deinit(), recurring here for NimBLE.
        // nimble_port_deinit() (a separate call — see nimble_port.h's own
        // "Deinitialize controller and NimBLE host stack" doc comment) is
        // the actual free. Calling it immediately after nimble_port_stop()
        // returns, from THIS task rather than the NimBLE host task itself,
        // is the exact pattern ESP-IDF's own blecent example uses for its
        // "stack init and deinit procedure" (examples/bluetooth/nimble/
        // blecent/main/main.c, stack_init_deinit()) — safe because
        // nimble_port_stop() already blocks (via its own semaphore) until
        // the host's stop procedure is fully serviced before returning, so
        // there's nothing left in flight for deinit to race against.
        int rc = nimble_port_stop();
        if (rc == 0) {
            esp_err_t derr = nimble_port_deinit();
            if (derr != ESP_OK) {
                ESP_LOGW(TAG, "nimble_port_deinit() -> %s (continuing anyway)", esp_err_to_name(derr));
            }
        } else {
            ESP_LOGW(TAG, "nimble_port_stop() -> %d, skipping nimble_port_deinit() (BLE memory not reclaimed)", rc);
        }
    }
    // s_host_ok must go back to false here — found live via the same
    // offload/reload test: without this, bt_mgr_ensure_active()'s own
    // "if (s_host_ok) return ESP_OK;" fast path permanently believes the
    // host is still up after this function runs, so a LATER bt_mgr_
    // ensure_active() call (RNode re-enabled, or any other BLE consumer)
    // silently no-ops instead of actually restarting NimBLE.
    s_host_ok = false;
    if (s_scan_done_sem) { vSemaphoreDelete(s_scan_done_sem); s_scan_done_sem = NULL; }
}

#else  // !CONFIG_BT_NIMBLE_ENABLED — see this file's top-of-file comment

int  bt_mgr_init(void) { return 0; }
void bt_mgr_deinit(void) {}
esp_err_t bt_mgr_ensure_active(void) { return ESP_ERR_NOT_SUPPORTED; }
void bt_mgr_register_gatt_provider(void (*queue_fn)(void)) { (void)queue_fn; }
bool bt_mgr_host_ready(void) { return false; }
bool bt_mgr_set_enabled(bool on) { (void)on; return false; }
bool bt_mgr_is_enabled(void) { return false; }
int  bt_mgr_scan(uint32_t duration_sec) { (void)duration_sec; return -1; }
int  bt_mgr_scan_count(void) { return 0; }
bool bt_mgr_scan_at(int idx, bt_scan_result_t *out) { (void)idx; (void)out; return false; }
esp_err_t bt_mgr_pair(const uint8_t addr[6]) { (void)addr; return ESP_ERR_NOT_SUPPORTED; }

#endif  // CONFIG_BT_NIMBLE_ENABLED

// ── .purr module header ───────────────────────────────────────────────────────
#include "purr_module.h"

PURR_MODULE_REGISTER(bt_mgr) = {
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_SYSTEM,
    .load_priority     = PURR_PRIORITY_IMPORTANT,
    .name              = "bt_mgr",
    .version           = "1.0.0",
    .kernel_min        = "0.11.1",
    .kernel_max        = "",
    .provided_catcalls = 0,
    .required_catcalls = 0,
    .init              = bt_mgr_init,
    .deinit            = bt_mgr_deinit,
};
