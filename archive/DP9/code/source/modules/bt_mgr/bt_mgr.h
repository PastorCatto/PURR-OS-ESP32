#pragma once
// bt_mgr.h — PURR OS Bluetooth manager (public API)
//
// BLE only — despite being asked for "classic + BLE," the T-Deck Plus's
// ESP32-S3 has no classic Bluetooth (BR/EDR) radio hardware at all
// (SOC_BT_CLASSIC_SUPPORTED isn't defined for this chip in ESP-IDF's
// soc_caps.h, only SOC_BLE_SUPPORTED is). Host stack is NimBLE (migrated
// from Bluedroid — see CoreOS/sdkconfig_tdeck_plus.overrides' Bluetooth
// section and bt_mgr.c's own header comment for why): Bluedroid's
// dual-mode capability was never usable on this chip in the first place,
// and its startup couldn't even fit in this board's internal-DRAM budget
// on real hardware. This header's API is stack-agnostic either way —
// callers (settings.c) never needed to change.
//
// Controller/host bring-up is LAZY, not boot-time: bt_mgr_init() itself
// does nothing but reset state. The actual NimBLE controller only comes
// up the first time bt_mgr_ensure_active() runs (via bt_mgr_set_enabled(
// true) or mesh_ble_set_advertising(true)). Confirmed live this session:
// the BT controller's own internal MALLOC_CAP_DMA allocations permanently
// exhaust this board's small reserved DMA-capable memory pool
// (CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL, 32KB) within seconds of boot —
// and unlike NimBLE's *host*-side buffers (routed to PSRAM earlier this
// session), MALLOC_CAP_DMA is hard-coded internal-RAM-only on this chip,
// so the controller's footprint can't be moved. That pool is also what
// the SD card driver needs for every single block read
// (esp_dma_capable_malloc(), diskio_sdmmc) — a permanently BT-occupied
// pool meant every SD read failed for the rest of boot, which is what
// broke .meow/.hiss script loading. Keeping the controller off until a
// user actually asks for Bluetooth keeps that pool free the rest of the
// time, which is the common case.

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char    name[32];
    uint8_t addr[6];
    int8_t  rssi;
} bt_scan_result_t;

int  bt_mgr_init(void);
void bt_mgr_deinit(void);

// Brings the NimBLE controller+host up, if not already up — idempotent,
// safe to call every time (returns ESP_OK immediately once already
// active). This is what used to happen unconditionally inside
// bt_mgr_init() at boot; now it only runs the first time a user actually
// asks for Bluetooth, via bt_mgr_set_enabled(true) or
// mesh_ble_set_advertising(true) (either can be first, depending on how
// the user gets to Bluetooth — Settings' toggle or the Meshtastic phone-
// companion feature directly). Internally: nimble_port_init() through
// ble_store_config_init() (what bt_mgr_init() used to do), then gives the
// registered GATT provider (see bt_mgr_register_gatt_provider()) one
// chance to queue its service, THEN starts the host — NimBLE only
// registers GATT services queued before the host's first start.
// Returns ESP_FAIL if nimble_port_init() fails (e.g. still out of DMA-
// capable memory for some other reason) — same failure this used to
// surface at boot, just later and much rarer now.
esp_err_t bt_mgr_ensure_active(void);

// Registers the single callback invoked by bt_mgr_ensure_active() right
// before it starts the NimBLE host for the first time — the one place
// left to call ble_gatts_add_svcs() safely now that host start-up no
// longer happens at a fixed point in boot. One slot, not a list: exactly
// one caller exists today (meshtastic's mesh_ble.c); revisit if a second
// ever needs it.
void bt_mgr_register_gatt_provider(void (*queue_fn)(void));

// True once bt_mgr_ensure_active() has successfully brought up the
// NimBLE host. False before that's ever happened, or if it failed —
// confirmed live on this board's tight DMA-capable-memory budget
// (esp_nimble_hci_init()'s own HCI buffer allocation can fail here even
// with BLE 5.0 features disabled). Any other module that calls NimBLE
// APIs directly (mesh_ble.c) MUST check this first — calling into an
// uninitialized NimBLE host is a hard crash (LoadProhibited), not a
// graceful failure.
bool bt_mgr_host_ready(void);

// Returns true if Bluetooth actually ended up enabled (false on an
// enable attempt means bt_mgr_ensure_active() failed — e.g. the DMA pool
// really is unavailable right now for some other reason). Always
// succeeds when disabling.
bool bt_mgr_set_enabled(bool on);
bool bt_mgr_is_enabled(void);

// Blocking scan (duration_sec seconds) — fills the internal result list,
// read back via bt_mgr_scan_count()/bt_mgr_scan_at().
int  bt_mgr_scan(uint32_t duration_sec);
int  bt_mgr_scan_count(void);
bool bt_mgr_scan_at(int idx, bt_scan_result_t *out);

// Connects to a scanned device and requests bonding ("Just Works" — no PIN
// entry UI exists yet, so IO capability is fixed at NoInputNoOutput).
esp_err_t bt_mgr_pair(const uint8_t addr[6]);

#ifdef __cplusplus
}
#endif
