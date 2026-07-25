#pragma once
// cupcake.h — Cupcake UI public API
//
// Android 1.5 ("Cupcake")-style launcher: a home screen with a handful of
// pinned app shortcuts plus a bottom dock, and a separate full-screen app
// drawer (opened from the dock's center button) listing every registered
// app. Status bar + drag-down notification panel forked from Cardstack.

#include <stdint.h>
#include <stdbool.h>
#include "lvgl.h"
#include "../../kernel/catcalls/catcall_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

// The status bar and nav bar are no longer Cupcake's — they live in the
// systemui module (source/modules/systemui/), which Cupcake hosts. Their
// geometry constants moved there too: see PURR_SYSTEMUI_STATUS_H and
// purr_systemui_navbar_height() in systemui.h. Nothing in Cupcake needs the
// status height any more (app windows go genuinely full-screen and the bars
// draw over them); cupcake_ui.c uses the nav bar height only to sit the home
// dock above it.

int      cupcake_hal_init(void);
uint16_t cupcake_hal_width(void);
uint16_t cupcake_hal_height(void);

// The lv_group_t physical-keyboard keypresses are dispatched through (see
// cupcake_hal.c's keypad_read_cb) — NULL if no input catcall was registered
// at HAL-init time. cupcake_win.c adds every textarea to this group and
// focuses one on purr_win_textarea_focus() so BBQ20 keystrokes land on the
// right widget.
lv_group_t *cupcake_hal_keypad_group(void);

// uptime_ms() of the last real input event (touch press, physical key,
// trackball nav step) — see cupcake_hal.c's mark_activity(). Used by
// cupcake_ui.c's idle-timeout check.
uint64_t cupcake_hal_last_activity_ms(void);

// Builds the home screen, dock, and (hidden) app drawer. Safe to call once,
// after the HAL and app_manager are both up.
void cupcake_ui_init(void);

// Per-tick housekeeping. The launcher itself is static once built, so this
// just drives the hosted system UI's own tick (status bar, notifications,
// running apps, idle lock, the bars' auto-hide countdowns).
// Call periodically (every ~200ms is plenty).
void cupcake_ui_tick(void);

// True once the idle timeout has fired and the lock overlay is showing
// (or the screen is dark waiting to be woken) — cleared only by the
// overlay's own tap/swipe-to-dismiss handler. Thin forwarder to
// purr_systemui_is_locked(); kept as a cupcake_* name because cupcake_hal.c
// is the caller and shouldn't need to know who owns the lock screen.
bool cupcake_ui_is_locked(void);

// Called by cupcake_hal.c the moment new input arrives while locked: makes
// the (still-locked) lock screen visible again by restoring brightness.
// Does NOT clear the locked state — that's a separate, deliberate dismiss
// gesture on the overlay itself. Forwards to purr_systemui_wake().
void cupcake_ui_wake(void);

// Icon-enhanced variant of purr_win_list_set_items() — same deferred-rebuild
// behavior as the portable version (see ck_list_set_items_async_cb()'s
// comment in cupcake_win.c), but each row also gets an icon glyph, matching
// lv_list_add_btn()'s own (list, icon, txt) shape. icons[i] is an LV_SYMBOL_*
// string constant or NULL for no icon — a font glyph, not a bitmap asset, so
// this doesn't need any new catcall_ui_t image-widget capability.
//
// Only meaningful when Cupcake (LVGL) is the active UI backend — this header
// is only usable by app code that both REQUIRES the cupcake component and
// guards every call behind #ifdef CONFIG_PURR_UI_BACKEND_CUPCAKE, falling
// back to the portable purr_win_list_set_items() otherwise (see msn.c).
void cupcake_win_list_set_items_icon(purr_wid_t wid, const char **items,
                                      const char **icons, int count);

// Hides every window currently shown by the foreground app, not just the
// one app_manager tracked at launch (app->window) — see cupcake_win.c's
// "Foreground window stack" comment for why that distinction matters once
// an app opens any lazily-created sub-window on top of its root. Used by
// the Lollipop nav bar's Home button (cupcake_ui.c) instead of hiding a
// single window handle.
void cupcake_win_hide_foreground(void);

#ifdef __cplusplus
}
#endif
