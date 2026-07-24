// miniwin_lock.h — idle-timeout screen lock, shared by both WinCE desktop
// variants (miniwin_wince_desktop.c, kernel_tdeck_plus_arduino/wince_shell.cpp).
// Mirrors Cupcake's lock (cupcake_ui.c/cupcake_hal.c) — same
// purr_kernel_screen_timeout_min()/purr_kernel_uptime_ms() idle check, same
// "purr_settings"/"brightness" NVS key on wake — just drawn with mw_gl_*
// calls instead of LVGL, since MiniWin has no compositing top layer to hang
// a persistent overlay object off of.
//
// Unlike Cupcake's own lock (which sits on lv_layer_top(), always above
// every other window by construction), the WinCE desktop window is just
// another window at the bottom of MiniWin's z-order — an open app window
// stays visible and interactive on top of the lock overlay unless something
// actively gets it out of the way. See miniwin_lock_set_transition_cb()
// below, which each desktop variant uses to minimise every open app window
// the moment the lock fires (and restore them on unlock) so the overlay
// actually covers everything, not just the desktop background.
//
// Dismissing is a deliberate gesture, not "any input" — the first input
// while the backlight is off always just wakes it (so the lock hint text
// is visible before anything can dismiss it). Once lit: a keyboard/
// trackball-click sequence of Space immediately followed by Enter
// dismisses (any other key resets the sequence); a touch only dismisses
// if it lands inside the on-screen "tap to unlock" hotspot the caller
// draws and hit-tests against.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Seeds the activity clock to "now" — call once at desktop startup, after
// mw_init(). Without this the clock defaults to 0 and boot itself (WiFi/BT/
// LoRa bring-up) can exceed a short timeout, instant-locking before any
// input ever arrives (same boot-race Cupcake's HAL init guards against).
void miniwin_lock_init(void);

// True while the lock overlay should be drawn — callers short-circuit their
// own paint function to just the overlay when this is true.
bool miniwin_lock_is_locked(void);

// Call ~once a second from each desktop variant's task loop. No-op unless
// idle time (now - last activity) has exceeded purr_kernel_screen_timeout_min().
void miniwin_lock_check_idle(void);

// Fired synchronously the instant s_locked actually flips value (true = just
// locked, false = just unlocked) — from inside miniwin_lock_check_idle()/
// miniwin_lock_handle_key()/miniwin_lock_handle_touch(), never polled. Each
// desktop variant registers its own callback (once, at startup) to minimise/
// restore its taskbar's app windows — see this header's top comment for why
// that's necessary at all.
typedef void (*miniwin_lock_transition_cb_t)(bool locked);
void miniwin_lock_set_transition_cb(miniwin_lock_transition_cb_t cb);

// Call for every drained INPUT_EVENT_KEY_DOWN while locked, keycode masked
// to its low byte (same convention miniwin_keyboard.c already posts to
// windows). Returns true if the event was consumed by the lock (always the
// case while locked) — caller swallows it instead of routing it further.
bool miniwin_lock_handle_key(uint8_t keycode);

// Call on a real touch-down while locked. in_hotspot: caller already
// hit-tested the touch point against its own drawn "tap to unlock" rect.
// Returns true if the event was consumed by the lock.
bool miniwin_lock_handle_touch(bool in_hotspot);

// Call for any other locked-relevant input that isn't a discrete key or a
// touch-down (trackball roll deltas, key-up releases) — wakes the backlight
// like the others but can never itself dismiss, and resets the Space→Enter
// sequence if the screen's already lit (so idle trackball jitter can't
// leave a stale Space waiting for a later Enter). Returns true if the event
// was consumed by the lock.
bool miniwin_lock_handle_other(void);

#ifdef __cplusplus
}
#endif
