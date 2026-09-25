// rnode_module.c — PURR_MOD_SYSTEM registration + lifecycle for RNode
// mode. See the plan doc for the full design; this file just wires
// together rnode_ble.c (transport) + rnode_proto.c (protocol) + a
// dedicated radio-RX task, and enforces the same one-physical-radio
// mutual exclusion meshtastic/meshcore/reticulum already enforce against
// each other and now against this module too.
//
// KNOWN LIMITATION (Stage 4 of the plan doc, not yet implemented):
// bt_mgr.c's single GATT-provider slot only ever registers a service
// once per boot (the first time bt_mgr_ensure_active() runs) — switching
// INTO RNode mode without a reboot, after Bluetooth was already active
// this boot under a different backend (e.g. meshtastic's own BLE
// companion), will not update the advertised GATT service. Fine for this
// module's own Stage 1-3 proof steps (RNode mode as the first BLE
// activation this boot); a real fix needs bt_mgr.c to support swapping
// providers after the host has already started — see the plan doc's own
// "BLE GATT-provider slot" section for the design.

#include "rnode_ble.h"
#include "rnode_proto.h"
#include "../../kernel/core/purr_module.h"
#include "../../kernel/core/purr_kernel.h"
#include "../../kernel/catcalls/catcall_radio.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "esp_netif.h"

static const char *TAG = "rnode";

// ── Radio-companion offload/reload ───────────────────────────────────────
// RNode mode and the PURR-to-PURR pairing/proximity/server stack were
// confirmed live to NOT safely fit together at the same time on zero-
// PSRAM hardware (heltec) — see the plan doc's own "RNode mode + PURR-to-
// PURR pairing together" section. Rather than try to squeeze both into
// memory simultaneously (repeatedly confirmed unsafe — either BLE itself
// fails to init, or it "succeeds" while starving the rest of the device
// into a real crash), this module now treats them as sequential, not
// simultaneous: tell any connected client to disconnect first (while the
// network stack can still carry that message), THEN actually tear the
// stack down (unload the PURR modules AND deinit the underlying WiFi
// driver — unloading the modules alone does NOT free WiFi's own memory,
// confirmed live: nothing in this codebase calls esp_wifi_deinit()
// anywhere else, only esp_wifi_init()/esp_wifi_stop() — the driver's own
// buffer pools stay allocated until deinit is called, module-unload or
// not), THEN start RNode. Reversed symmetrically when RNode mode ends.
//
// Order matters both ways: offload unloads "leaf" consumers before the
// proximity/pairing modules they depend on for their own transport;
// reload brings proximity/pairing back up first, so the leaf consumers
// have something to depend on again before their own init() runs.
static const char *kRadioCompanionModules[] = {
    "server_mgr", "homebase", "app_manager_remote", "msn_relay", "wifi_mgr",
    "proximity_rpc", "pairing", "proximity",
};
#define RADIO_COMPANION_MODULE_COUNT \
    (sizeof(kRadioCompanionModules) / sizeof(kRadioCompanionModules[0]))

// Set only if offload_radio_companion_stack() actually found something to
// tear down — module_deinit() must only reverse this if module_init()
// actually did it (a plain "rnode alone" build, e.g. rnode_test, never
// has this stack compiled in at all, and must stay that way on the way
// back out too — reloading WiFi where it never existed before would be a
// real, unwanted behavior change for that profile).
static bool s_offloaded_radio_companion = false;

static void offload_radio_companion_stack(void) {
    // Step 1 — tell any connected client to disconnect, while proximity_
    // rpc/pairing are both still up to actually carry the message.
    // Routed through purr_kernel_notify_radio_offload_needed() rather
    // than calling pairing_force_logout_all() (pairing.h) directly — see
    // that kernel-level function's own doc comment for why: a direct
    // #include/REQUIRES would force pairing.c's entire implementation
    // into every rnode build, including rnode-alone profiles that never
    // want it. Safe, cheap no-op if pairing was never loaded — see
    // pairing_force_logout_all()'s own doc comment for why this is a
    // best-effort broadcast to the whole trust list, not a precise
    // "who's currently connected" query (this codebase tracks no live
    // session state today).
    ESP_LOGI(TAG, "offload: asking any connected client to disconnect first (if applicable)");
    purr_kernel_notify_radio_offload_needed();

    // Step 2 — unload the PURR modules themselves, leaf-first.
    bool any_unloaded = false;
    for (size_t i = 0; i < RADIO_COMPANION_MODULE_COUNT; i++) {
        const char *name = kRadioCompanionModules[i];
        if (purr_kernel_get_module(name)) {
            ESP_LOGI(TAG, "offload: stopping '%s'", name);
            purr_kernel_module_set_enabled(name, false);
            any_unloaded = true;
        }
    }

    // Step 3 — actually tear the WiFi driver down. Unloading the PURR
    // modules above does NOT do this by itself — see this section's own
    // header comment. esp_wifi_get_mode() is the same "already up?" probe
    // proximity_module.c's own ensure_wifi_ready()/wifi_mgr.c already use
    // (ESP_OK = initialized, ESP_ERR_WIFI_NOT_INIT = never was) — only
    // stop+deinit if it's genuinely there to tear down.
    wifi_mode_t mode;
    if (esp_wifi_get_mode(&mode) == ESP_OK) {
        ESP_LOGI(TAG, "offload: stopping + deiniting the WiFi driver");
        esp_wifi_stop();
        esp_err_t err = esp_wifi_deinit();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "offload: esp_wifi_deinit() -> %s (continuing anyway)", esp_err_to_name(err));
        }
        any_unloaded = true;
    }

    s_offloaded_radio_companion = any_unloaded;
    if (any_unloaded) {
        ESP_LOGI(TAG, "offload complete — radio-companion stack + WiFi driver torn down");
    }
}

static void reload_radio_companion_stack(void) {
    if (!s_offloaded_radio_companion) return;
    s_offloaded_radio_companion = false;

    ESP_LOGI(TAG, "reload: bringing the WiFi driver back up");
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "reload: esp_netif_init() failed: %s", esp_err_to_name(err));
        return;
    }
    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&wifi_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "reload: esp_wifi_init() failed: %s", esp_err_to_name(err));
        return;
    }
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "reload: esp_wifi_set_mode() failed: %s", esp_err_to_name(err));
        return;
    }
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "reload: esp_wifi_start() failed: %s", esp_err_to_name(err));
        return;
    }

    // Reload in the REVERSE order offload used — proximity/pairing back
    // up first, so the leaf consumers (server_mgr etc) have a working
    // transport underneath them by the time their own init() runs.
    for (size_t i = RADIO_COMPANION_MODULE_COUNT; i-- > 0; ) {
        const char *name = kRadioCompanionModules[i];
        // Not "if already loaded" — these were EXPLICITLY unloaded by
        // offload_radio_companion_stack() above (or were never loaded at
        // all, in which case this call is a harmless, expected no-op —
        // purr_kernel_module_set_enabled() on an unregistered name just
        // returns an error code, doesn't crash).
        ESP_LOGI(TAG, "reload: starting '%s'", name);
        purr_kernel_module_set_enabled(name, true);
    }
    ESP_LOGI(TAG, "reload complete — radio-companion stack + WiFi driver back up");
}

// Real, measured cost of bringing BLE up (bt_mgr_ensure_active(), inside
// rnode_ble_init()/_set_advertising()) on this device's own internal-only
// DRAM (no PSRAM to offload to — heltec, primarily): ~64-70KB, confirmed
// live via heapwatch before/after captures (see the plan doc). Combined
// with whatever the rest of the system already has running (WiFi/
// proximity/pairing, if this module is being asked to coexist with
// them), this can genuinely leave too little contiguous memory for this
// module's own RX task (4096-byte stack) to safely start — confirmed
// live the hard way: a first attempt without this guard let the task
// allocation "succeed" while leaving internal_free in the low hundreds
// of bytes, and the whole device hit a real Guru Meditation crash + boot
// loop shortly after from starvation elsewhere, not just this module.
// RNODE_MIN_FREE_INTERNAL is checked BEFORE ever calling rnode_ble_init()
// (bt_mgr's own BLE activation isn't cleanly reversible once started —
// see this file's own "KNOWN LIMITATION" comment on bt_mgr's single-slot
// GATT provider for the same "there's no clean undo" theme) — declining
// here, before spending that cost, is the same "fail honestly rather
// than clamp-and-lie" principle rnode_proto.c's own radio-parameter
// echo-back already follows, applied to memory instead of a radio
// setter's return code.
#define RNODE_MIN_FREE_INTERNAL (100 * 1024)

// Same backup-poll interval reasoning as meshtastic_module.c's own
// MESH_RX_POLL_TIMEOUT_MS — only used when the active radio driver
// doesn't support wait_rx_signal().
#define RNODE_RX_POLL_TIMEOUT_MS 500UL
#define RNODE_WATCHDOG_STALE_MS  5000UL

static TaskHandle_t      s_rx_task = NULL;
static volatile bool     s_running = false;
static volatile uint32_t s_last_heartbeat_ms = 0;

static void rnode_rx_task(void *arg) {
    (void)arg;
    while (!purr_kernel_radio()) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    while (s_running) {
        rnode_proto_poll_radio();
        s_last_heartbeat_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);

        const catcall_radio_t *radio = purr_kernel_radio();
        if (!radio || !radio->wait_rx_signal) {
            vTaskDelay(pdMS_TO_TICKS(10));
        } else {
            radio->wait_rx_signal(RNODE_RX_POLL_TIMEOUT_MS);
        }
    }
    s_rx_task = NULL;
    vTaskDelete(NULL);
}

static bool rnode_manager_is_alive(void) {
    if (!s_running) return false;
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    return (now_ms - s_last_heartbeat_ms) < RNODE_WATCHDOG_STALE_MS;
}

// ── Module lifecycle ────────────────────────────────────────────────────

static int module_init(void) {
    // Mutually exclusive with meshtastic, meshcore, AND reticulum — one
    // physical radio, one catcall_radio_t slot. Same shape as their own
    // guards, mirrored here (see meshtastic_module.c/meshcore_module.cpp/
    // reticulum_module.cpp, each of which now also declines when this
    // module's preference is active).
    purr_mesh_backend_t pref = purr_kernel_mesh_backend_get();
    if (pref != PURR_MESH_BACKEND_RNODE) {
        ESP_LOGI(TAG, "declining to start — mesh backend preference is not rnode");
        return PURR_MODULE_INIT_DECLINED;
    }
    if (purr_kernel_get_module("meshtastic")) {
        ESP_LOGW(TAG, "refusing to start — meshtastic is active (stop it first)");
        return PURR_MODULE_INIT_DECLINED;
    }
    if (purr_kernel_get_module("meshcore")) {
        ESP_LOGW(TAG, "refusing to start — meshcore is active (stop it first)");
        return PURR_MODULE_INIT_DECLINED;
    }
    if (purr_kernel_get_module("reticulum")) {
        ESP_LOGW(TAG, "refusing to start — reticulum is active (stop it first)");
        return PURR_MODULE_INIT_DECLINED;
    }

    // Make room first — see this file's own "Radio-companion offload/
    // reload" section header comment. Safe to call even on a build that
    // never had this stack at all (rnode_test): every step is gated on
    // "is this actually here right now", not assumed.
    offload_radio_companion_stack();

    // See RNODE_MIN_FREE_INTERNAL's own comment — checked before BLE
    // activation, which isn't cleanly reversible once started. Checked
    // AFTER offload_radio_companion_stack() above, deliberately — that's
    // the whole point of offloading first.
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (free_internal < RNODE_MIN_FREE_INTERNAL) {
        ESP_LOGW(TAG, "declining to start — only %u bytes internal DRAM free (need >= %u); "
                 "BLE activation + this module's own RX task would starve the rest of the device",
                 (unsigned)free_internal, (unsigned)RNODE_MIN_FREE_INTERNAL);
        // Don't strand the device with neither RNode NOR the radio-
        // companion stack running — put back what offload_radio_
        // companion_stack() just tore down before declining.
        reload_radio_companion_stack();
        return PURR_MODULE_INIT_DECLINED;
    }

    rnode_ble_init();
    // RNode mode shouldn't require a separate trip through Settings' own
    // Bluetooth toggle first — advertise as soon as this module is the
    // active backend, same self-contained activation reticulum's own
    // module_init() already does for its own radio interface.
    rnode_ble_set_advertising(true);

    s_running = true;
    BaseType_t ok = xTaskCreate(rnode_rx_task, "rnode_rx", 4096, NULL, 3, &s_rx_task);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "failed to create rx task");
        s_running = false;
        // Same reasoning as the memory-guard decline above — this is a
        // genuine failure past the point of no easy BLE undo (see
        // RNODE_MIN_FREE_INTERNAL's own comment), so the radio-companion
        // stack stays down here; nothing further to reload for THIS
        // specific failure mode (BLE itself is already up and can't be
        // cleanly torn back down either — see this file's own "KNOWN
        // LIMITATION" header comment).
        return -1;
    }

    purr_kernel_health_register("rnode", rnode_manager_is_alive);
    purr_kernel_notify("RNode ready", "RNode mode active — advertising over BLE", "rnode");
    ESP_LOGI(TAG, "ready — RNode mode active (BLE, Nordic UART Service)");
    return 0;
}

static void module_deinit(void) {
    s_running = false;   // rx task deletes itself on its next loop iteration
    rnode_ble_set_advertising(false);
    rnode_ble_deinit();   // now really tears NimBLE down — see its own doc comment

    // Settle pause before reload — nimble_port_stop() (inside
    // rnode_ble_deinit() -> bt_mgr_deinit()) only SIGNALS bt_mgr's host
    // task to exit; the actual cleanup (nimble_port_freertos_deinit(),
    // which frees the host task's own resources) runs asynchronously on
    // that task's own context, after bt_mgr_deinit() has already
    // returned here. Calling reload_radio_companion_stack() immediately,
    // with no pause, confirmed live to still see NimBLE's memory as
    // resident — proximity_rpc/homebase both failed to reinit ("alloc
    // failed... PSRAM and internal RAM both exhausted"). Same magnitude,
    // same "let async teardown actually finish" reasoning as boot.c's
    // own P2->P3 500ms settle pause.
    vTaskDelay(pdMS_TO_TICKS(500));

    // See this file's own "Radio-companion offload/reload" section header
    // comment — no-ops if module_init() never actually offloaded anything
    // (rnode_test-style builds).
    reload_radio_companion_stack();
}

// ── Module header ─────────────────────────────────────────────────────

PURR_MODULE_REGISTER(rnode) = {
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_SYSTEM,
    .load_priority     = PURR_PRIORITY_OPTIONAL,
    .name              = "rnode",
    .version           = "0.1.0",
    .kernel_min        = "0.11.1",
    .kernel_max        = "",
    .provided_catcalls = 0,
    .required_catcalls = 0,   // checked at runtime via purr_kernel_radio(), not this field — same pattern meshtastic_module.c's own registration comment establishes
    .init              = module_init,
    .deinit            = module_deinit,
};
