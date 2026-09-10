#pragma once
// kittenui.h — KittenUI public API
//
// KittenUI is PURR OS's small-screen UI module built on LVGL.
// It adds a theme system on top of LVGL and routes display/touch
// through catcalls so no module has direct hardware access.
//
// Usage from an app:
//   #include "kittenui.h"          — theme API + display size helpers
//   #include "lvgl.h"              — all LVGL widget APIs directly
//
// Apps create screens and widgets using standard LVGL. KittenUI just
// handles init, HAL wiring, and theming.

#include "kittenui_theme.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ── Init / deinit (called by kernel module loader) ────────────────────────────

int  kittenui_init(void);
void kittenui_deinit(void);

// ── HAL helpers ───────────────────────────────────────────────────────────────

int      kittenui_hal_init(void);
uint16_t kittenui_hal_width(void);
uint16_t kittenui_hal_height(void);

// ── Theme application ─────────────────────────────────────────────────────────

// Called internally after lv_init(). Apply the registered theme to all
// existing LVGL styles. Safe to call again for hot-swap.
void kittenui_apply_theme_styles(void);

#ifdef __cplusplus
}
#endif
