// driver_manager.c — PURR OS driver manager
//
// ── Status, 2026-09 ──────────────────────────────────────────────────────
// This module's original design scanned /flash/drivers and /sdcard/drivers
// for .purr driver files at boot. That path is DISABLED, not merely idle:
// the loader it depended on (what used to be load_driver() below) read a
// purr_module_header_t — including its raw init()/deinit() FUNCTION
// POINTERS — directly off disk with fread(). That can only ever produce a
// valid, callable address if the file is a byte-exact dump of this exact
// firmware build's own address space; it is not a real loader for an
// independently compiled object, and never was. Confirmed: those two
// directories are empty on every device in this tree today, so this was
// silently a no-op rather than a crash, but it would not have worked the
// moment a real .purr file existed. See docs/10_ModuleLoading.md and
// source/modules/claw_loader/ for the loader that actually IS proven on
// hardware (claw_elf.c's real ELF32/Xtensa relocation) — that is the
// mechanism a future loadable-driver pass should build on, not this one.
//
// Every real driver on every device today loads via static registration
// (device.pcat -> purrstrap-generated purr_device_glue.c), which never
// touched this module at all. This module stays registered (device.pcat
// across every device names it, and the Services app reads its count/
// entry API) but now honestly reports "nothing to scan yet" instead of
// silently claiming a scan happened.

#include "driver_manager.h"
#include "../../kernel/core/purr_kernel.h"
#include "esp_log.h"

static const char *TAG = "drv_mgr";

int driver_manager_init(void) {
    ESP_LOGW(TAG, "flash/SD driver loading is not implemented — the old "
                  ".purr scan path could never load a real out-of-tree "
                  "driver (see this file's own top comment). All drivers "
                  "on this build are statically compiled in via "
                  "device.pcat. See claw_loader/ for the loader mechanism "
                  "a real out-of-tree driver would need.");
    return 0;
}

void driver_manager_deinit(void) {
}

int driver_manager_get_count(void) {
    return 0;
}

const drv_entry_t *driver_manager_get_entry(int idx) {
    (void)idx;
    return NULL;
}

const char *drv_status_badge(drv_status_t s) {
    switch (s) {
    case DRV_STATUS_OK:     return "[OK]";
    case DRV_STATUS_COMPAT: return "[COMPAT]";
    case DRV_STATUS_FAIL:   return "[FAIL]";
    case DRV_STATUS_SKIP:   return "[SKIP]";
    default:                return "[?]";
    }
}

// ── .purr module header ───────────────────────────────────────────────────────

PURR_MODULE_REGISTER(driver_manager) = {
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_SYSTEM,
    // Explicit — unset (0) sorted this ahead of P1 drivers (see miniwin's
    // matching comment for the failure that exposed it).
    .load_priority     = PURR_PRIORITY_IMPORTANT,
    .name              = "driver_manager",
    .version           = "1.0.0",
    .kernel_min        = "0.11.1",
    .kernel_max        = "",
    .provided_catcalls = 0,
    .required_catcalls = 0,
    .init              = driver_manager_init,
    .deinit            = driver_manager_deinit,
};
