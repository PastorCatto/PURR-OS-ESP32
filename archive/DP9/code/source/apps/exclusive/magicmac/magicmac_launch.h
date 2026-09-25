#pragma once
// magicmac_launch.h — MagicMac pre-launch screen
//
// Draws directly via catcall_display. No MiniWin dependency.
// Handles ROM check, loading status, and no-ROM warning with reboot button.

#include "magicmac_cfg.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LAUNCH_ROM_LOADING = 0,  // scanning for ROM
    LAUNCH_ROM_FOUND,        // ROM found, about to start emulator
    LAUNCH_ROM_MISSING,      // ROM not found — show warning + reboot button
} launch_state_t;

// Show the pre-launch screen. Blocks until either:
//   - ROM is found     → returns true  (caller starts emulator)
//   - User hits reboot → calls purr_kernel_reboot(), never returns
//   - ROM missing, no touch available → returns false after timeout
bool magicmac_launch(const magicmac_cfg_t *cfg);

#ifdef __cplusplus
}
#endif
