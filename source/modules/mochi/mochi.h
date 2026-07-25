#pragma once
// mochi.h — Mochi UI backend: an iOS-style springboard for the T-Deck Plus.
//
// Named for the look: soft, rounded, light. iOS's design language is squircle
// icons on a light ground, generous spacing, one accent colour, and chrome
// that gets out of the way — which is what this backend implements.
//
// ── What "iOS style" means concretely here ──────────────────────────────────
//   * Springboard home screen: a paged grid of rounded-square ("squircle") app
//     icons with the app name underneath, page dots below the grid, and a
//     translucent dock pinned along the bottom holding the first few apps.
//   * A home indicator bar instead of an Android-style Back/Home/Recents row.
//     Mochi asks the systemui module to suppress its nav bar (see
//     purr_systemui_host_t::suppress_navbar) and draws the indicator itself;
//     every other systemui surface — status bar, drag-down panels, Recents,
//     lock screen — is kept, because each maps cleanly onto an iOS equivalent
//     (Notification Center, Control Center, the app switcher, the lock screen).
//   * iOS system palette: systemGroupedBackground (#F2F2F7) ground, white
//     panels, systemBlue (#007AFF) accent, iOS label greys.
//   * Press feedback by scale, not colour — an icon shrinks slightly under the
//     finger and springs back, the way a real springboard icon does.
//
// ── Not a touch-only port ───────────────────────────────────────────────────
// A literal iOS springboard would be touch-only, which would be a downgrade on
// a device with a keyboard and a trackball. Mochi keeps a selection cursor: the
// trackball and arrow keys move it across the grid (wrapping row to row and
// into the dock), Enter or a ball click opens the selected app, and the
// selected icon draws an iOS-style focus ring. Touch still works exactly as you
// would expect, and using it moves the cursor to whatever you touched, so the
// two input models never disagree about what is selected.
//
// ── Trackball note (why this backend sets edit mode) ────────────────────────
// LVGL only delivers encoder motion to the focused object as LV_KEY_LEFT/RIGHT
// when the object's group is in *editing* mode; otherwise enc_diff is consumed
// by lv_group_focus_next/prev (lv_indev.c's indev_encoder_proc). With a single
// full-screen shell object in the group — which is what a springboard is —
// focus-stepping has nowhere to go and the ball appears dead. Mochi therefore
// puts its group into editing mode whenever the springboard holds focus. This
// is exactly the bug Tabby shipped with.

#include <stdint.h>
#include <stdbool.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// ── HAL (mochi_hal.c) ───────────────────────────────────────────────────────

int      mochi_hal_init(void);
uint16_t mochi_hal_width(void);
uint16_t mochi_hal_height(void);

// Shared focus group for the keypad and encoder indevs. NULL on a build with
// no input catcalls (touch only), in which case callers skip group membership.
lv_group_t *mochi_hal_group(void);

// uptime_ms() of the last real input event from any source — feeds the
// systemui module's idle-lock timeout through Mochi's host hook table.
uint64_t mochi_hal_last_activity_ms(void);

// True when a physical keyboard is among the registered inputs, by the same
// capability test the rest of the codebase uses (keyboard-class drivers
// implement set_backlight; a trackball does not). Suppresses the on-screen
// keyboard, which would otherwise eat a third of a 240px-tall panel.
bool mochi_hal_has_physical_keyboard(void);

// ── Icon set (mochi_springboard.c) ──────────────────────────────────────────
// Mochi owns the system icon set and publishes it here, rather than each
// consumer keeping its own name->icon map (which is how cupcake/tabby/mochi
// each ended up with a copy of the same table). The launcher is the natural
// owner: it is the one component that must have an icon for every registered
// app, so any table it keeps is already the complete one.
//
// Icons are Ionicons, rendered white with alpha by source/assets/icons/
// convert_icons.py — white-on-tint is how an iOS app icon is built, so the
// same asset works on the springboard's coloured squircles, in Recents cards,
// and anywhere else a per-app mark is wanted.
//
// Never returns NULL: unmatched names fall back to a generic tool glyph.
const lv_img_dsc_t *mochi_icon_for_app(const char *name);

// Deterministic per-app accent colour, from a fixed saturated palette rather
// than a hash-to-RGB (which yields muddy mid-tones that read as noise). The
// same app is the same colour everywhere it appears.
lv_color_t mochi_color_for_app(const char *name);

// ── Springboard (mochi_springboard.c) ───────────────────────────────────────

// Builds the springboard (icon grid, page dots, dock, home indicator) and
// hands the system UI its host table. Call once, after the HAL and app_manager
// are both up.
void mochi_springboard_init(void);

// Per-tick housekeeping — drives the hosted system UI's tick and rebuilds the
// grid when the app registry changes. ~200ms is plenty.
void mochi_springboard_tick(void);

// Returns to the springboard and restores keyboard/trackball focus to it
// (including re-entering encoder edit mode). Called by the window backend
// once the last app window is hidden.
void mochi_springboard_go_home(void);

// ── Window backend (mochi_win.c) ────────────────────────────────────────────

// Registers Mochi's catcall_ui_t implementation with the kernel.
void mochi_win_register(void);

// Hides every window the foreground app currently has shown, not just the one
// app_manager tracked at launch — backs the system UI's Home action, and
// catches lazily-created sub-windows a plain hide-app->window would miss.
void mochi_win_hide_foreground(void);

#ifdef __cplusplus
}
#endif
