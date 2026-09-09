#pragma once
// driver_manager.h — PURR OS driver manager public API
//
// STATUS (2026-09): the flash/SD .purr scan this module was designed
// around is disabled — see driver_manager.c's own top comment for why it
// could never have worked for a real out-of-tree driver in the first
// place. Every driver on every current device loads via static
// registration from device.pcat instead. This header's types
// (drv_entry_t, drv_status_t) and API are kept as the shape a future
// loadable-driver mechanism (built on claw_loader, not this scan) should
// grow back into — driver_manager_get_count()/get_entry() currently
// always report empty, which is honest, not a bug.

#include <stdint.h>
#include <stdbool.h>
#include "sig_mgr.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DRV_STATUS_OK     = 0,  // fully validated, within version range
    DRV_STATUS_COMPAT = 1,  // beyond kernel_max, but all required_catcalls present
    DRV_STATUS_FAIL   = 2,  // required_catcall missing — driver did not load
    DRV_STATUS_SKIP   = 3,  // skipped (wrong chip, IDF version, etc.)
} drv_status_t;

typedef struct {
    char         name[32];
    char         version[12];
    char         type[16];          // "display", "touch", "input", "radio", "gps"
    drv_status_t status;
    char         fail_reason[64];   // populated on DRV_STATUS_FAIL
    // sig_mgr_classify() result for this .purr file — see sig_mgr.h.
    // Purely informational for OK/COMPAT/SKIP: an unsigned driver still
    // loads (every driver in this tree is unsigned today; this field is
    // what makes that VISIBLE, not what changes it). SIG_TIER_TAMPERED is
    // the one tier that changes behavior — see load_driver()'s own
    // comment — which is why DRV_STATUS_FAIL doesn't need its own
    // separate "tampered" status: fail_reason plus this field already say so.
    sig_tier_t   sig_tier;
} drv_entry_t;

// Called at boot — scans paths and loads all drivers found
int  driver_manager_init(void);
void driver_manager_deinit(void);

// Get the loaded driver list (for UI / driver status screen)
int              driver_manager_get_count(void);
const drv_entry_t *driver_manager_get_entry(int idx);

// Status badge string for UI: "[OK]", "[COMPAT]", "[FAIL]", "[SKIP]"
const char *drv_status_badge(drv_status_t s);

#ifdef __cplusplus
}
#endif
