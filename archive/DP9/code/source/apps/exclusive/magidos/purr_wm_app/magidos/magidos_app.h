#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Register MagiDOS with the MiniWin window manager.
// Call once during system init (after purr_wm_init).
// MagiDOS appears as a launchable app in the WM; it opens its window on demand.
void magidos_register(void);

#ifdef __cplusplus
}
#endif
