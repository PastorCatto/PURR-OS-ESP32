#pragma once
// boot_splash.h — raw-framebuffer boot splash, see boot_splash.c

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Draws the full splash (background, title, empty progress bar) via
// purr_kernel_display() directly. No-op if no display is registered yet.
// Call once, right after the display driver initializes — well before any
// UI backend (Cupcake/MiniWin/etc.) has started, which is the whole point.
void boot_splash_show(void);

// Advances the progress bar by one step out of BOOT_SPLASH_STEPS (below)
// and redraws just that region. No-op if boot_splash_show() was never
// called or the display isn't registered.
void boot_splash_advance(void);

#define BOOT_SPLASH_STEPS 4

#ifdef __cplusplus
}
#endif
