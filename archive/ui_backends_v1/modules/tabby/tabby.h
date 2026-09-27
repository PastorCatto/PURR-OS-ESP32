#pragma once
// tabby.h — Tabby UI backend: a keyboard-first shell for the T-Deck Plus.
//
// ── Why another backend ─────────────────────────────────────────────────────
// Cupcake (Android-launcher grid) and MiniWin (Win95 desktop) are both
// *pointer-first* metaphors: they assume you aim at a target and tap it. The
// T-Deck Plus is not that device. It is a palm-sized handheld whose dominant
// input is a physical QWERTY thumb keyboard, with a 4-way trackball + click
// under your right thumb, and touch as the least ergonomic of the three (you
// have to let go of the device to reach the middle of the screen).
//
// Tabby is built the other way round — keyboard first, trackball second,
// touch third:
//
//   * The home screen is a vertical app list, not a grid. Lists are what a
//     d-pad/trackball navigates well; grids need 2-axis aiming.
//   * Typing any printable character filters that list live. No "open the
//     search field first" step — the shell is always listening, the way a
//     launcher search or a shell prompt is. On a device with a real keyboard
//     this is by far the fastest path to an app, and it scales to any number
//     of installed apps without paging.
//   * Trackball is registered as a real LVGL ENCODER (rotate = move, click =
//     activate), not translated into fake PREV/NEXT keypresses the way
//     cupcake_hal.c does it. That means it also drives focus inside apps.
//   * Every widget joins the keyboard focus group, not just textareas — so
//     buttons and lists inside ordinary apps are reachable from the keyboard,
//     which they are not under Cupcake.
//
// Touch still works everywhere: the list rows, the hint bar, and every app
// widget are ordinary clickable LVGL objects.
//
// ── Relationship to the systemui module ─────────────────────────────────────
// Tabby does NOT implement a status bar, notification panel, Recents, or lock
// screen. It hosts source/modules/systemui/ for all of that, via
// purr_systemui_host_t — the same module Cupcake hosts. Tabby is the second
// consumer, which is the point: that module is meant to be backend-agnostic.
//
// ── Rendering ───────────────────────────────────────────────────────────────
// LVGL today. tabby_shell.c keeps every drawing call inside its own clearly
// marked rendering section, drawing only rectangles, text, and a highlight
// bar — deliberately no LVGL-specific widget types in the shell's own chrome
// — so a direct-framebuffer renderer (push_pixels() straight through
// catcall_display_t, no object tree, no compositor) can replace it without
// touching the shell's input/filter/selection logic. See that section's
// comment for what such a renderer would and would not cover.

#include <stdint.h>
#include <stdbool.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// ── HAL (tabby_hal.c) ───────────────────────────────────────────────────────

int      tabby_hal_init(void);
uint16_t tabby_hal_width(void);
uint16_t tabby_hal_height(void);

// The shared focus group every keyboard/encoder-navigable object joins.
// NULL if no input catcall was registered at HAL-init time (a touch-only
// build), in which case callers just skip group membership.
lv_group_t *tabby_hal_group(void);

// uptime_ms() of the last real input event from any source — feeds the
// systemui module's idle-lock timeout through Tabby's host hook table.
uint64_t tabby_hal_last_activity_ms(void);

// True when a physical keyboard is among the registered input catcalls, by
// the same capability test the rest of the codebase uses (a keyboard-class
// driver implements set_backlight; a trackball does not). Drives both the
// shell's hint text and whether the on-screen keyboard is ever shown.
bool tabby_hal_has_physical_keyboard(void);

// ── Shell (tabby_shell.c) ───────────────────────────────────────────────────

// Builds the home screen (filter bar, app list, hint bar) and hands the
// system UI its host table. Call once, after the HAL and app_manager are up.
void tabby_shell_init(void);

// Per-tick housekeeping — drives the hosted system UI's own tick and
// refreshes the app list if the registry changed. ~200ms is plenty.
void tabby_shell_tick(void);

// Shows the home screen and returns focus to it: clears any active filter,
// re-focuses the list. Called by the shell's own Esc/Home handling and by
// the system UI when it returns home.
void tabby_shell_go_home(void);

// ── Window backend (tabby_win.c) ────────────────────────────────────────────

// Registers Tabby's catcall_ui_t implementation with the kernel.
void tabby_win_register(void);

// Hides every window the foreground app currently has shown, not just the
// one app_manager tracked at launch — backs the system UI's Home action.
// Same shared-stack approach as cupcake_win.c's equivalent (an app that
// opened a sub-window on top of its root would otherwise leave it visible).
void tabby_win_hide_foreground(void);

#ifdef __cplusplus
}
#endif
