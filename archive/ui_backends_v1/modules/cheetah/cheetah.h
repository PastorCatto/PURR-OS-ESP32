#pragma once
// cheetah.h — Cheetah: the XP-style desktop for PURR OS.
//
// Named after Mac OS X 10.0's own codename — a real cat name (this project's
// own naming convention) that also nods to the Apple/iOS-7 design lineage
// this module's window backend renders for every app (see cheetah_win.c),
// even though the desktop it draws itself stays Windows-XP-styled on
// purpose (the deliberately minimal UI benchmark this OS measures app
// bloat against).
//
// ── History ──────────────────────────────────────────────────────────────
// Started life as "Flow", built for a BlackBerry 10-style Active Frames
// Home Screen — that work is paused, not deleted (see the project's own
// plan history), and this module was repurposed rather than rewritten from
// scratch: cheetah_hal.c and cheetah_win.c are generic LVGL window/input
// plumbing, not BB10-specific, so only cheetah_home.c's actual content
// changed (Active Frames + All Apps paging -> a plain XP desktop). Renamed
// from Flow to Cheetah once the BB10 framing no longer matched what this
// module does.
//
// ── What it draws now ───────────────────────────────────────────────────
// A single desktop pane — no paging, no Active Frames, no minimize gesture
// of its own (the taskbar in source/modules/systemui/systemui_xp.c owns
// running-app switching/minimizing now, one button per running app). Icons
// are a curated FAVORITES subset of the registry, not every app — that's
// what the Start Menu already covers. Long-press empty desktop space opens
// a "Choose Favorites..." picker; see cheetah_home.c for the persistence
// model (NVS-backed, newline-delimited name list).
//
// ── Lineage ──────────────────────────────────────────────────────────────
// Built on Mochi's HAL/window-backend shape (source/modules/mochi/) rather
// than written fresh: window management is generic (every app already
// targets the shared catcall_ui_t contract regardless of backend) and
// Mochi's input/render plumbing is proven on this exact device. Cheetah
// also reuses Mochi's icon/colour tables (mochi_icon_for_app() /
// mochi_color_for_app(), see mochi.h) instead of keeping a third copy of
// the same name->icon map.
//
// ── Performance ──────────────────────────────────────────────────────────
// Render path is the shared purr_lv_flush.h off-screen compose / async
// flush / double-buffer path both existing LVGL backends already use.
//
// ── Orientation ──────────────────────────────────────────────────────────
// The desktop lays out from cheetah_hal_width()/height() at build time via
// derived column/row counts, never a hardcoded landscape constant — so the
// same code is correct on a landscape device (T-Deck Plus) and a portrait
// one alike.
//
// ── Login ────────────────────────────────────────────────────────────────
// NOT Cheetah's job. Real credential UI (the Windows-XP-style welcome
// screen, and the idle-lock screen that now shares it) is shared
// infrastructure in source/modules/systemui/ (systemui_login.c), called
// from systemui's own purr_systemui_init() the same way the status bar and
// taskbar already are — every host that hosts systemui, Cheetah included,
// gets it for free. An earlier pass built this into the module directly
// (the file was called cheetah_lock.c, from before the rename); that was
// the wrong layer for it and was removed once systemui grew a real
// implementation of its own.
//
// ── App-widget theme ─────────────────────────────────────────────────────
// cheetah_win.c is also the ONE place that decides how every app's
// buttons/lists/menus/textareas actually render — apps only ever call the
// abstract purr_win_*() API, never draw themselves. That theme is iOS-7
// grouped-table style (with dark mode support, see purr_kernel_dark_mode_
// enabled()), independent of this file's own XP desktop above it.

#include <stdint.h>
#include <stdbool.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// ── HAL (cheetah_hal.c) ─────────────────────────────────────────────────────

int      cheetah_hal_init(void);
uint16_t cheetah_hal_width(void);
uint16_t cheetah_hal_height(void);

// Shared focus group for the keypad/trackball indevs. NULL on a build with
// no input catcalls (touch only) — callers skip group membership then.
lv_group_t *cheetah_hal_group(void);

// uptime_ms() of the last real input event from any source — feeds
// systemui's idle-lock timeout through Cheetah's host hook table.
uint64_t cheetah_hal_last_activity_ms(void);

// Block until any in-flight async flush completes. Must be called before
// Cheetah deletes its render task — see mochi_hal_wait_flush_idle()'s matching
// comment for why (the SPI bus can be held across an async push return).
void cheetah_hal_wait_flush_idle(void);

// True when a physical keyboard is among the registered inputs (same
// capability test as every other LVGL backend: a keyboard-class driver
// implements set_backlight, a trackball does not).
bool cheetah_hal_has_physical_keyboard(void);

// Shadow suppression, wired to the UI-effects toggle at init.
void cheetah_hal_set_shadows_enabled(bool on);

// ── Desktop (cheetah_home.c) ─────────────────────────────────────────────

// Builds the desktop and hands the system UI its host table. Call once,
// after the HAL and app_manager are both up.
void cheetah_home_init(void);

// Per-tick housekeeping — drives the hosted system UI's tick and rebuilds
// the desktop when the app registry changes. ~200ms is plenty.
void cheetah_home_tick(void);

// Returns to the desktop and restores keyboard/trackball focus to it.
// Called by the window backend once the last app window is hidden.
void cheetah_home_go_home(void);

// ── Window backend (cheetah_win.c) ───────────────────────────────────────

// Registers Cheetah's catcall_ui_t implementation with the kernel.
void cheetah_win_register(void);
// Release the UI catcall on unload — see purr_kernel_unregister_ui().
void cheetah_win_unregister(void);

// Hides every window the foreground app currently has shown (not just the
// one app_manager tracked at launch). Backs the taskbar's own minimize path
// (systemui_xp.c calls this via purr_systemui_host_t::hide_foreground_windows).
void cheetah_win_hide_foreground(void);

#ifdef __cplusplus
}
#endif
