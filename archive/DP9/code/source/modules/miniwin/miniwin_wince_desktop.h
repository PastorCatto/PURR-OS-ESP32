// miniwin_wince_desktop.h — WinCE-style taskbar+start-menu desktop chrome
// for the generic MiniWin .purr module. Only compiled in when
// CONFIG_PURR_MINIWIN_DESKTOP_WINCE is set (see miniwin_module.c).
#pragma once
#include "MiniWin/miniwin.h"

#ifdef __cplusplus
extern "C" {
#endif

// Called by miniwin_win.c's mw_win_create()/mw_win_destroy() so every app
// window that goes through purr_win_create() automatically gets a taskbar
// button — no changes needed in settings.c/fileman.c/etc.
void wce_taskbar_register(mw_handle_t handle, const char *name);
void wce_taskbar_unregister(mw_handle_t handle);

// Explicitly repaints every system layer window by handle (wallpaper,
// taskbar+menu), always ending in mw_paint_all() — see the .c file's top
// comment for why that's not optional here (MiniWin's focused-window
// occlusion-bypass shortcut). Call whenever something that was covering
// part of the screen goes away — an app window closing, or the credential
// dialog dismissing (login success or unlock). Used by miniwin_win.c's
// mw_win_destroy() and this file's own on_lock_transition()/cred_verify_
// and_proceed().
void wce_redraw_layers(void);

// The taskbar+Start Menu window's handle. This window resizes itself live
// (grows upward to cover the Start Menu popup when open, shrinks back to
// just the bottom strip when closed — see resize_taskbar_window() in the
// .c file), so its on-screen origin isn't fixed; miniwin_module.c's task
// loop uses this handle plus wce_status_rect() below (not a hardcoded
// rect) to correctly target the clock/battery corner's periodic repaint
// regardless of the menu's current state.
mw_handle_t wce_taskbar_handle(void);

// The shared credential dialog's window handle — invisible and inert
// until a lock fires or boot-time login is pending (see miniwin_lock_
// set_transition_cb()'s callback and mw_user_init()'s own boot-login gate
// in the .c file), at which point it's made visible, brought to the front
// of the z-order (outranking any open app window), and painted. Used by
// miniwin_module.c's task loop to target the lock footer's own slow
// periodic repaint.
mw_handle_t wce_lock_handle(void);

// Delivers one real keystroke to the credential dialog — backspace,
// printable-character append, Enter-as-submit (calls user_mgr_verify()),
// Tab-to-switch-field when the username is editable. A no-op if the
// dialog isn't currently showing. Called both from the dialog's own
// MW_KEY_PRESSED_MESSAGE handler (the boot-login case: nothing is locked
// then, so miniwin_keyboard.c's plain find-focused-window routing already
// delivers keys here) and directly from miniwin_keyboard.c while actually
// locked, bypassing miniwin_lock_handle_key()'s legacy Space→Enter-only
// dismiss path — see that file's own call site comment.
void wce_credential_dialog_handle_key(uint8_t keycode);

// Flips the taskbar corner between a real wall clock and a battery-level
// glyph. Called by miniwin_module.c's task loop every few seconds — see
// that call site for the actual rotation cadence.
void wce_desktop_toggle_status(void);

// Bounding rect (window-local — see wce_taskbar_handle()'s comment on why
// this can't be a fixed constant) of the taskbar corner's clock/battery
// readout — used by miniwin_module.c's task loop to scope that corner's
// periodic repaint to just itself.
void wce_status_rect(mw_util_rect_t *out);

// Note: the old lock overlay's own battery/RAM footer (wce_lock_info_rect())
// is gone — replaced by a richer on-demand "Status" window (uptime/battery/
// node count/unread messages) opened from the credential dialog's own
// Status button, see this file's "Window 3" section.

// Height in pixels of the taskbar strip along the bottom of the screen
// *at rest* (Start Menu closed) — miniwin_win.c's mw_win_create() still
// subtracts exactly this much from every app window's rect, same as
// before the taskbar became its own resizable window; app windows only
// ever need to avoid the permanent strip, never the transient popup.
int16_t wce_taskbar_height(void);

#ifdef __cplusplus
}
#endif
