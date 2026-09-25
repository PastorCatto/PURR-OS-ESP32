#pragma once
// nougat.h — Nougat UI public API
//
// LVGL v9 UI backend, Tab5-only (ESP32-P4) — ports Cupcake's nav bar/status
// bar/recents/home screen 1:1 in spirit onto v9's real API surface, not
// reimagined. Every other UI backend in this codebase stays on LVGL v8; see
// CoreOS/CMakeLists.txt's EXCLUDE_COMPONENTS and idf_component.yml's
// per-target lvgl/lvgl version resolution for how the two coexist in one
// build tree.
//
// Phase 1 (this pass): HAL + full catcall_ui_t widget backend only, matching
// cupcake_win.c's surface so real apps can run under Nougat exactly as they
// do under Cupcake. Phase 2 adds the actual chrome (nav bar, status bar,
// recents, home screen/dock/launcher) — nougat_ui_init()/tick() below don't
// exist yet; nougat_module.c's task loop just drives lv_timer_handler()
// directly until they do.

#include <stdint.h>
#include <stdbool.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

int      nougat_hal_init(void);
uint16_t nougat_hal_width(void);
uint16_t nougat_hal_height(void);

// The lv_group_t physical-keyboard keypresses are dispatched through — NULL
// if no input catcall is registered (Tab5 has none as of Phase 1: touch
// only). Mirrors cupcake_hal_keypad_group()'s contract exactly, kept for
// forward-compatibility with any future Tab5 input accessory rather than
// because it does anything today.
lv_group_t *nougat_hal_keypad_group(void);

// uptime_ms() of the last real input event — mirrors
// cupcake_hal_last_activity_ms(), for a future idle-timeout/lock screen
// (explicitly out of scope for Nougat's Phase 2 chrome port, but the HAL
// tracks it the same way regardless so that work isn't blocked later).
uint64_t nougat_hal_last_activity_ms(void);

void nougat_win_register(void);

#ifdef __cplusplus
}
#endif
