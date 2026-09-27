#pragma once
// boot_splash.h — raw-framebuffer boot splash, see boot_splash.c

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ── Generic raw splash ──────────────────────────────────────────────────────
//
// Draws directly through catcall_display_t, so it works when there is no UI
// backend at all. That is true in two situations, not one: at boot before any
// backend has started, and during a game-mode transition after the backend has
// been deliberately unloaded. Both need to tell the user something is happening
// while the screen would otherwise sit frozen or black.
//
// `steps` is however many times the caller will call purr_splash_advance().
// Pass the real number — the bar is scaled to it, so a mismatch just makes the
// bar finish early or never fill.
void purr_splash_show(const char *title, int steps);

// Advances the bar one step of `steps` and redraws only that region.
// No-op if purr_splash_show() was never called or no display is registered.
void purr_splash_advance(void);

// Optional single line under the bar saying what is happening right now
// ("unloading meshtastic", "restoring systemui"). Each call clears the
// previous line first, so it can be called as often as wanted. Centred,
// truncated to the panel width rather than wrapping.
void purr_splash_status(const char *line);

// ── Boot splash ─────────────────────────────────────────────────────────────
//
// Thin wrapper over the above, kept as its own name because the boot path calls
// it from several device kernels and the title is part of what the OS looks
// like. Call once, right after the display driver initializes.
void boot_splash_show(void);
void boot_splash_advance(void);

#define BOOT_SPLASH_STEPS 4

#ifdef __cplusplus
}
#endif
