#pragma once
// systemui.h — PURR OS system UI: the persistent chrome that sits above every
// app window. Status bar, drag-down Notifications/Running-Apps panels, the
// Back/Home/Recents nav bar, the Recents card carousel, and the idle lock
// screen.
//
// Extracted from Cupcake (was cupcake_ui.c, then cupcake_systemui.c) so the
// UI backend hosting it is left as *just a launcher* — home screen, wallpaper,
// favourites dock, app drawer. Nothing in here knows what a launcher looks
// like; it reaches back through purr_systemui_host_t for the handful of things
// only the host can answer.
//
// ── Threading ───────────────────────────────────────────────────────────────
// This module has NO task of its own, deliberately. LVGL is not thread-safe
// and the whole UI serializes on the host backend's render loop under
// purr_kernel_ui_lock() — so the host calls purr_systemui_init() once and
// purr_systemui_tick() periodically from inside that loop, and every function
// here runs on the host's task. Giving this module its own task would mean
// taking the UI lock from two places and is not worth the deadlock surface.
//
// ── Portability ─────────────────────────────────────────────────────────────
// Currently LVGL-only: every surface is built on lv_layer_top(), a compositing
// layer LVGL paints and hit-tests above the active screen's entire tree, which
// is what lets this chrome persist over full-screen app windows without the
// layout reserving space for it. MiniWin has no equivalent top layer (see
// miniwin_lock.h), which is why it keeps its own separate implementation
// rather than consuming this module.
//
// When CONFIG_PURR_SYSTEMUI is off, systemui.c compiles to stubs for every
// function below — same approach meshtastic_module.c already uses for its own
// gate. Callers need no #ifdef; purr_systemui_navbar_height() just returns 0
// and nothing is drawn.

#include <stdint.h>
#include <stdbool.h>
#include "lvgl.h"
#include "../../kernel/core/purr_kernel.h"
#include "../user_mgr/user_mgr.h"

#ifdef __cplusplus
extern "C" {
#endif

// ── TEMPORARY DIAGNOSTIC: corner radius on/off ──────────────────────────────
//
// Set to 1 to force every rounded corner square. Purely a measurement switch —
// nothing ships with this on.
//
// Why it is worth measuring: LVGL's software renderer draws a rounded corner by
// computing per-pixel arc coverage for anti-aliasing, which is among the more
// expensive things it does. Crucially it costs the SAME whether the surface is
// opaque or translucent — which fits the observation that turning effects off
// changed the frame rate not at all. If corners are the cost, this flag will
// show it immediately in the frame histogram; if it changes nothing, the
// biggest remaining candidate is eliminated and the next step is per-frame
// render-vs-flush instrumentation rather than another guess.
// Measured on hardware: corners cost roughly a third of a frame during a
// full-screen scroll (mean frame 47-76ms with, 22-43ms without). Left OFF —
// i.e. corners restored — because the look is deliberate and the half-height
// shade below recovers area cost more cheaply. Flip to 1 to re-measure.
#ifndef PURR_FX_TEST_NO_RADIUS
#define PURR_FX_TEST_NO_RADIUS 0
#endif

static inline void purr_fx_radius(lv_obj_t *obj, lv_coord_t r)
{
    lv_obj_set_style_radius(obj, PURR_FX_TEST_NO_RADIUS ? 0 : r, 0);
}

// ── Translucency, in one place ──────────────────────────────────────────────
//
// Every surface that is deliberately see-through sets its background opacity
// through this instead of calling lv_obj_set_style_bg_opa() directly. With
// effects on it behaves exactly as the call it replaces. With effects off the
// surface becomes fully opaque and is filled with the user's accent colour.
//
// Centralised rather than left as an `if` at each of the ~16 call sites for
// two reasons: the sites are spread across four files and two System UI styles,
// and a site that forgets the check does not fail loudly — it just quietly
// stays translucent, so the setting appears to half-work. One helper means
// "which surfaces are effects?" is answered by which ones call this.
//
// Deliberately NOT applied to:
//   LV_OPA_TRANSP — invisible hit-zones. Filling those with accent would paint
//                   opaque blocks over the UI.
//   LV_OPA_COVER  — already opaque, so there is no effect to disable.
//
// The colour is written after the opacity so it wins over whatever bg_color
// the call site set moments earlier; that ordering is why this takes the object
// rather than returning a value.
static inline void purr_systemui_fx_bg_opa(lv_obj_t *obj, lv_opa_t translucent_opa)
{
    if (purr_kernel_ui_effects_enabled()) {
        lv_obj_set_style_bg_opa(obj, translucent_opa, 0);
    } else {
        lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(obj, lv_color_hex(purr_kernel_accent_color()), 0);
    }
}

// Variant for surfaces whose OWN colour carries meaning — per-app tile colours,
// the active/inactive page dots. These are translucent for depth, but flooding
// them with a single accent would destroy the thing they encode: every tile
// would look alike, and the page dots would stop showing which page you are on.
// So effects-off makes them opaque at the colour the call site already chose.
//
// The rule of thumb: if the surface is CHROME (panels, bars, scrims, cards)
// use purr_systemui_fx_bg_opa(). If the surface is CONTENT whose colour is
// information, use this one.
static inline void purr_systemui_fx_bg_opa_keep(lv_obj_t *obj, lv_opa_t translucent_opa)
{
    lv_obj_set_style_bg_opa(obj,
        purr_kernel_ui_effects_enabled() ? translucent_opa : LV_OPA_COVER, 0);
}

// Same decision for a border/outline that is only there to suggest depth
// through a translucent edge. Collapses to fully opaque accent when off.
static inline void purr_systemui_fx_border_opa(lv_obj_t *obj, lv_opa_t translucent_opa)
{
    if (purr_kernel_ui_effects_enabled()) {
        lv_obj_set_style_border_opa(obj, translucent_opa, 0);
    } else {
        lv_obj_set_style_border_opa(obj, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(obj, lv_color_hex(purr_kernel_accent_color()), 0);
    }
}

// Height of the status strip along the top, and of the nav bar along the
// bottom. Prefer purr_systemui_navbar_height() over the raw constant when
// laying out host content — it accounts for the module being compiled out.
#define PURR_SYSTEMUI_STATUS_H 22
#define PURR_SYSTEMUI_NAVBAR_H 40

// All bundled icon assets are 48x48 source images; lv_img_set_zoom() takes
// 256 = 100%, so this converts a desired on-screen pixel size into that scale
// factor. Used here for Recents cards; the host backend that provides
// icon_for_app() below has its own copy for its own tiles/dock, since a
// pixel size that looks right on a small launcher tile and one that looks
// right on a large Recents card are different call sites, not a value worth
// sharing.
#define ICON_ZOOM(px) (uint16_t)(((px) * 256) / 48)

// Everything the system UI needs from whichever backend is hosting it. All
// fields are required (no NULL checks) except as noted — the host builds one
// of these as a static const and hands it to purr_systemui_init().
//
// Deliberately no lv_obj_t* anywhere: the host and the system UI each own
// their own object trees and never reach into each other's.
typedef struct {
    // Screen geometry, straight from the host's HAL.
    uint16_t (*width)(void);
    uint16_t (*height)(void);

    // Per-app visual identity, so Recents cards match the launcher's own
    // tiles/dock rather than inventing a second look for the same app.
    // icon_for_app() must never return NULL — return a fallback icon.
    const lv_img_dsc_t *(*icon_for_app)(const char *name);
    lv_color_t          (*tint_color)(const char *name, uint8_t base);

    // Hide the host's app drawer/overlay, if it has one and it's open.
    // Called by the "return to home screen" paths, which have to clear the
    // drawer along with every app window. May be NULL if the host has no
    // such overlay.
    void (*hide_drawer)(void);

    // Hide *every* window the foreground app currently has open — not just
    // the one app_manager tracked at launch. Backs the nav bar's Home
    // button; an app that opened a sub-window on top of its root would
    // otherwise leave that sub-window visible after Home.
    void (*hide_foreground_windows)(void);

    // uptime_ms() of the last real input event, for the idle-lock timeout.
    uint64_t (*last_activity_ms)(void);

    // The wallpaper currently in effect, or NULL for none. Used by the lock
    // screen so it matches the home screen rather than being a flat slab.
    //
    // A hook rather than a shared asset because each host resolves "current
    // wallpaper" differently — Mochi has one compiled into the firmware,
    // Cupcake loads a user-selected image off SPIFFS or SD and may have no
    // image at all if that load failed. Returning NULL is a normal answer, not
    // an error, and the lock screen falls back to a plain dark background.
    //
    // May itself be NULL on a host with no concept of a wallpaper.
    const lv_img_dsc_t *(*wallpaper)(void);

    // Omit the Back/Home/Recents nav bar, leaving every other surface (status
    // bar, drag-down panels, Recents, lock screen) intact. For hosts whose
    // design language has no such bar — an iOS-style springboard uses a home
    // indicator and gestures instead, and an Android nav bar under it looks
    // simply wrong.
    //
    // Defaults to false for any host that doesn't mention it, which is why
    // this is a plain bool rather than a callback: a static const initializer
    // that omits the field zero-fills it, so existing hosts keep their nav bar
    // with no edit. When true, purr_systemui_navbar_height() also reports 0,
    // so a host laying content out against it reclaims the space.
    bool suppress_navbar;

    // The host's shared LVGL group for its keyboard/trackball indevs — the
    // same group each host's own <name>_hal_group() already returns for its
    // own widgets (mochi_hal_group(), flow_hal_group(), tabby_hal_group()).
    // Optional: may be NULL, both for a host with no such concept and for one
    // that simply hasn't wired it up yet (a static initializer that omits
    // this field zero-fills it, same as suppress_navbar above).
    //
    // Needed here specifically so a systemui-owned text-entry surface (the
    // login screen in systemui_login.c) can join the SAME group a physical
    // keyboard indev is already bound to — an indev routes key events to
    // `lv_group_get_focused(indev's group)`, so a widget outside that group
    // never receives them no matter how it's focused. Without this hook,
    // typing a password would only ever reach the on-screen keyboard, never
    // a hardware one, on every device that has both.
    lv_group_t *(*group)(void);
} purr_systemui_host_t;

// Multi-user boot-time login plumbing — core wiring only, no UI of its own.
// See user_mgr.h for the account model this calls into.
//
// Call once, from each style's purr_systemui_init() (android's and ios's real
// implementations both do — see systemui_android.c/systemui_ios.c), after
// user_mgr itself has initialised (it is PURR_PRIORITY_IMPORTANT, same tier
// as systemui, so load order between the two is not guaranteed — this
// function tolerates user_mgr not being up yet by simply doing nothing,
// rather than assuming it always is).
//
// Deliberately does NOT show any lock/login UI — that is a real, separate
// piece of work (a password-entry widget belongs somewhere in the boot
// flow, matching how ck_build_lock_screen()'s existing lock screen has none
// today — it is purely gesture-dismissed) intentionally deferred until the
// planned out-of-box setup flow exists to actually let someone SET a
// password. Until then this only ever exercises its one reachable branch:
// bootstrap default user "milkaholic" has no password, so the account
// intended to be usable with zero configuration logs in with zero
// friction. The has-a-password branch is real and will do the right thing
// (not auto-login) the moment something can set one — it deliberately does
// not try to force a lock screen open with no way to dismiss it correctly,
// which would be a genuine lockout risk to ship ahead of that widget.
static inline void purr_systemui_boot_login_check(void) {
    const char *username = user_mgr_default_username();
    if (!username || !username[0]) return;   // user_mgr not up yet — see doc comment above
    if (user_mgr_is_logged_in()) return;      // already handled this boot

    if (!user_mgr_has_password(username)) {
        user_mgr_set_logged_in(username);
    }
    // else: intentionally left logged out — see this function's doc comment.
}

// The real login prompt purr_systemui_boot_login_check() above deliberately
// stops short of (see its own doc comment) — a Windows-XP-welcome-screen-
// style credential UI (systemui_login.c), shared across every host the same
// way the status bar and lock screen already are, rather than each backend
// building its own.
//
// Call once, LAST in the host's purr_systemui_init() — after every other
// surface (panels, status bar, the idle-lock screen) has been built — so
// this ends up the topmost lv_layer_top() object purely by creation order,
// the same way the idle-lock screen already relies on being built last.
// No-ops (does not build anything) if a session already exists — including
// via purr_systemui_boot_login_check()'s own no-password auto-login, which
// a host should still call separately and earlier, matching both real
// implementations (systemui_android.c / systemui_ios.c) today.
void purr_systemui_show_login(const purr_systemui_host_t *host);

// The idle-lock screen, sharing systemui_login.c's exact UI (tile picker →
// password → Go) rather than a separate minimal dim-and-tap overlay — real
// Windows locks to the same welcome screen it boots to. The one visual
// difference: the row for user_mgr_current_user() gets a small "Logged on"
// tag, since — unlike the boot-time call — a real session already exists.
// try_unlock() succeeding still requires the password (locking is a real
// security boundary, not a dismiss gesture); it also restores brightness
// and clears purr_systemui_relock_active() on success.
//
// A style's lock_check_idle()-equivalent calls this in place of building its
// own overlay when it decides the idle timeout has fired; this function
// alone owns the dim-on-show / restore-on-unlock pairing, so the two can't
// drift out of sync across two files. No-ops if no accounts exist or the
// login screen was never built (nothing to lock against).
void purr_systemui_show_relock(const purr_systemui_host_t *host);

// True from purr_systemui_show_relock() until its own successful unlock —
// the shared equivalent of what each style's purr_systemui_is_locked() used
// to track locally. A style's purr_systemui_is_locked() should simply
// return this.
bool purr_systemui_relock_active(void);

// Builds every surface. Call once, from the host's UI task, after the host's
// own screens exist and its HAL is up. `host` must outlive the call (a static
// const is the expected shape) — it is retained, not copied.
void purr_systemui_init(const purr_systemui_host_t *host);

// Per-tick housekeeping: refreshes status icons/notifications/running apps,
// checks the idle timeout, services the nav bar's and status row's auto-hide
// countdowns. ~200ms cadence is plenty. Safe to call before init (no-op).
void purr_systemui_tick(void);

// Height the host should keep its own bottom-docked content clear of — the
// nav bar's footprint, or 0 when this module is compiled out. App windows
// themselves need no such allowance: they go genuinely full-screen and this
// chrome draws over them.
int16_t purr_systemui_navbar_height(void);

// Which app_manager index is currently foregrounded, or -1 for "home screen".
// Owned here because every mutation point except the host's own launcher taps
// (nav Back/Home, Running Apps Open/Kill, Recents card tap/kill) lives here.
int purr_systemui_foreground_idx(void);

// An app just came to the foreground: records `idx` and auto-hides the nav
// bar and status row. The host calls this after launching/restoring a window.
void purr_systemui_enter_app(int idx);

// Back to the home screen: clears the foreground index and restores the nav
// bar and status row permanently (no auto-hide countdown). Does NOT itself
// hide or stop any app window — callers decide whether "leaving" means hide
// (Home) or stop (Back).
void purr_systemui_return_home(void);

// Opens the app switcher. Exists on the contract because a host whose design
// language has no nav bar (see suppress_navbar) still needs a way in — Mochi
// binds it to a long-press on its home indicator, the way iOS does. Safe to
// call with nothing running; the switcher shows its own empty state.
void purr_systemui_open_recents(void);

// True while the app switcher is showing. Lets a host route a "go home"
// gesture to closing the switcher first, rather than skipping past it.
bool purr_systemui_recents_open(void);

// Closes the switcher if it is open; no-op otherwise.
void purr_systemui_close_recents(void);

// True once the idle timeout has fired and the lock overlay is showing (or
// the screen is dark waiting to be woken) — cleared only by the overlay's own
// dismiss gesture.
bool purr_systemui_is_locked(void);

// Called the moment new input arrives while locked: makes the (still-locked)
// lock screen visible again by restoring brightness. Does NOT clear the
// locked state — that's a separate, deliberate dismiss gesture on the overlay.
void purr_systemui_wake(void);

// Re-apply the effects/accent styling to surfaces that are ALREADY BUILT.
//
// Necessary because the fx helpers above decide translucent-vs-accent at
// CONSTRUCTION time, and the long-lived chrome is constructed exactly once:
// build_panel() runs from purr_systemui_init(), and opening the shade only
// slides that same object back down. So without this, toggling the setting
// appears to do nothing to the notification shade until the next reboot —
// which is precisely how it failed the first time it was tried on hardware.
//
// Transient surfaces (notification cards, recents cards) are rebuilt every time
// they are shown and pick the setting up on their own; this only has to reach
// the persistent ones, plus a notification rebuild so cards refresh in place.
//
// MUST be called from the UI task with the UI lock held — it touches LVGL
// objects. Settings' own widget callbacks already satisfy both, since they run
// inside the host backend's render loop.
void purr_systemui_fx_refresh(void);

#ifdef __cplusplus
}
#endif
