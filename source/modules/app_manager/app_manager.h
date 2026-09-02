#pragma once
// app_manager.h — PURR OS app manager public API
//
// The app manager is a .purr system module that:
//   - Scans /flash/apps and /sdcard/apps for .meow, .hiss, .paws, .claw files,
//     plus /sdcard/personal/<current user>/*.claw (loaded at runtime — see
//     APP_TIER_PERSONAL below)
//   - Maintains a registry of available and running apps
//   - Provides the Cat Apps launcher UI (app grid over MiniWin)
//   - Enforces tier boundaries: .paws gets no kernel calls, .claw gets all,
//     .meow/.hiss run interpreted, a personal app gets only claw_loader's
//     fixed import table (see app_tier_t below for the full split)

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "../../kernel/catcalls/catcall_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

// ── App tiers ─────────────────────────────────────────────────────────────────

typedef enum {
    APP_TIER_MEOW = 0,   // Lua script — sandboxed VM, win.*/sd.*/system.* only
    APP_TIER_PAWS = 1,   // Compiled userland — win.*/sd.* only
    APP_TIER_CLAW = 2,   // Compiled kernel-access — full kernel API
    APP_TIER_HISS = 3,   // Lua script — same VM/launch path as .meow, plus
                          // kitt.*/radio.*/gps.* bindings (lua_runtime.c
                          // registers these only when the launching script's
                          // tier is HISS). Trust is extension-only, same as
                          // every other tier here — no flash-vs-SD gating.
    APP_TIER_KITTEN = 4, // Lua script — like .hiss (same VM/launch path,
                          // same kitt.*/radio.*/gps.* bindings, same
                          // purr-sig/Developer-Mode consent gate), but the
                          // first one found on SD autoruns at boot without
                          // being manually launched (see app_manager_init()).
    APP_TIER_PERSONAL = 5, // Compiled, loaded at RUNTIME (never linked into
                          // this firmware) via source/modules/claw_loader/ —
                          // NOT the same trust level as APP_TIER_CLAW despite
                          // both compiling from source: a personal app can
                          // only reach the fixed, small set of host functions
                          // claw_loader.c's import table names (see
                          // claw_elf.h's "External (host) symbols" section —
                          // "the real capability boundary for loaded code").
                          // Stored per-user under /sdcard/personal/<user>/,
                          // scanned only for whoever is currently logged in
                          // (see app_manager_scan_ex()) — never shown for
                          // another account.
} app_tier_t;

// ── App entry ─────────────────────────────────────────────────────────────────

typedef enum {
    APP_STATE_IDLE    = 0,
    APP_STATE_RUNNING = 1,
    APP_STATE_STOPPED = 2,
    APP_STATE_ERROR   = 3,
} app_state_t;

// Where an app is allowed/meant to run, when it's reachable through a
// REMOTE session (app_manager.h's own "Remote mode" section below) — a
// purely local app (never listed by a remote provider at all) has no
// occasion to be anything but the implicit REMOTE=0 default, so this
// only actually matters once a caller is deciding what a REMOTE-listed
// entry's tap should do. REMOTE=0 on purpose: every app that predates
// this (i.e. every app that doesn't declare otherwise via app.pcat's new
// `placement` key) keeps today's exact behavior — launched on whichever
// device is already running it (REMOTEAPPS_ACTION_LAUNCH), shown on
// THAT device's own screen — with zero migration needed. See app_manager
// .c's decide_placement() for how HYBRID actually gets resolved.
typedef enum {
    APP_PLACE_REMOTE = 0,   // launch/stop-from-afar only — today's only behavior
    APP_PLACE_LOCAL  = 1,   // download once, always launch locally after that
    APP_PLACE_HYBRID = 2,   // either, decided per-connection by decide_placement()
} app_placement_t;

typedef struct {
    char        name[48];        // display name (from filename or embedded manifest)
    char        path[256];       // full path to the app file
    app_tier_t  tier;
    app_state_t state;
    char        error[96];       // populated on APP_STATE_ERROR
    // Declared by the app via purr_module_header_t::speed_demon. app_manager
    // enters speed demon before init() and exits it when the app reports it is
    // done, so the app itself calls neither. See docs/15_SpeedDemon.md.
    bool        speed_demon;
    purr_win_t  window;          // set automatically when the app calls purr_win_create();
                                  // 0 if it hasn't (yet), or never will
    // Internal-RAM free bytes at the moment this app was launched (0 while
    // idle/stopped) — see app_manager_kill_worst_offender()'s doc comment
    // in app_manager.c for why this exists: approximates "how much has this
    // app cost since it started" (mem_free_at_launch minus the current free
    // reading) without needing real per-task heap accounting.
    uint32_t    mem_free_at_launch;
    // See app_placement_t's own doc comment just above.
    app_placement_t placement;
    // REMOTE-listing only (always true for a genuinely local entry, see
    // app_manager_remote.c's own handle_list() for where this is
    // actually computed) — false means this app has no real, extractable
    // file behind it to serve (a pre-linked/compiled-in app like
    // Terminal or Settings, or one already loaded from claw_loader's
    // personal-space storage) and can never be downloaded, only run on
    // the server. A UI offering the install-vs-run-remote choice
    // (cheetah_home.c's launch()) must check this BEFORE showing
    // "Install" — a real, reported bug when it didn't: choosing Install
    // for a pre-linked app like Terminal always failed, with a confusing
    // "install failed" notification as the only sign anything was wrong.
    bool downloadable;
} app_entry_t;

// ── Public API ────────────────────────────────────────────────────────────────

// Called at boot by the kernel module loader
int  app_manager_init(void);
void app_manager_deinit(void);

// Re-scan app paths and rebuild the registry (hot-reload from SD)
int  app_manager_scan(void);

// Same, but include_sd=false skips "/sdcard/..." scan paths — for
// boot-recovery callers where SD may still be degraded post-hang and
// scan_dir()'s opendir()/readdir() calls have no timeout of their own.
// See app_manager_scan_ex()'s definition comment for the full story.
int  app_manager_scan_ex(bool include_sd);

// Launch an app by index or by path. Returns 0 on success.
int  app_manager_launch_idx(int idx);
int  app_manager_launch_path(const char *path);
int  app_manager_launch_by_name(const char *name);

// Stop a running app by index
void app_manager_stop(int idx);

// For a native app that ends on its own terms: clears the manager's record that
// it is running, so tapping its icon launches it again instead of taking
// app_manager_launch_path()'s `state == APP_STATE_RUNNING` early return. An
// exclusive app that drives the panel directly has no window to re-show, so that
// early return is silently inert — the app simply never starts again.
//
// Takes the app NAME. The calling task's handle cannot be used: native apps are
// launched on a short-lived task that calls init() and exits, so an app that
// spawns its own long-lived task from init() is not the task app_manager tracks.
//
// Call just before the app's task deletes itself.
void app_manager_notify_exited(const char *name);

// Registry access (for Cat Apps UI)
int              app_manager_count(void);
const app_entry_t *app_manager_get(int idx);

// ── Local unlock gate ────────────────────────────────────────────────────
// app_manager_init() no longer scans/populates the local registry
// unconditionally at boot (unless OOBE hasn't completed yet — see
// app_manager_init()'s own comment: there's no account to unlock as in
// that case, so it behaves exactly as before). Once OOBE has completed,
// app_manager_count()/get() report empty for the LOCAL registry until
// app_manager_notify_unlocked() has been called — real idleness, not a
// scan that already ran silently underneath a login screen overlay.
// Confirmed via boot log this was previously NOT the case: the Desktop
// was fully built/populated the same tick the login screen first showed,
// well before the login actually succeeded.
//
// Call from wherever LOCAL login success is decided — today, that's
// systemui_login.c right after user_mgr_set_logged_in() on the (non-
// server) login path. Idempotent: calling it again while already
// unlocked is a no-op, so a relock->unlock cycle never double-scans.
void app_manager_notify_unlocked(void);

// Symmetric reset — local registry goes back to idle/empty. Best-effort:
// wired from a clean local-logout/relock hook wherever one already
// exists; not deeply pursued if none does yet. Does not affect remote
// mode's own registry/state.
void app_manager_notify_locked(void);

// Open the Cat Apps launcher UI over the current MiniWin context
void app_manager_open_launcher(void);

// The path of the .meow script currently being launched — set right before
// launch_meow() creates its task, valid for lua_runtime's init() to read
// during that same launch. Only one Lua VM runs at a time on these boards.
const char *app_manager_get_pending_meow_path(void);

// The script's own source, preloaded into a PSRAM buffer by launch_meow()
// (on the launching caller's own stack, before meow_task() exists) so that
// meow_task() itself never has to fopen() anything — see launch_meow()'s
// comment for why that matters. NULL if nothing is pending. Ownership stays
// with app_manager: meow_task() frees this right after lua_runtime's init()
// returns, so lua_runtime must not free or retain the pointer past that call.
const char *app_manager_get_pending_meow_code(size_t *out_len);

// True when the script currently being launched is .hiss-tier — set in
// launch_meow() from app->tier, read by lua_runtime_init() to decide whether
// to register the extra kitt.*/radio.*/gps.* Lua globals. .meow scripts
// always read false here.
bool app_manager_get_pending_meow_privileged(void);

// ── Remote mode ─────────────────────────────────────────────────────────────
// Points app_manager_count()/app_manager_get()/app_manager_launch_idx()/
// app_manager_stop() at a REMOTE device's app list instead of this device's
// own, for as long as remote mode stays on. This is the mechanism behind
// Milkbar's "Desktop" button: the same launcher UI already running on this
// device (cheetah_home.c's icon grid, systemui_xp.c's taskbar/Start Menu —
// both drive themselves purely off these four calls) hands off to a
// different app source with no code of their own aware anything changed.
//
// app_manager.c itself gains NO proximity_rpc/pairing dependency for this —
// it stays universal across every device, including radio-less targets like
// esp32p4 (see app_manager_remote.c's own header comment on exactly this
// split, and why app_manager_remote is its own module rather than folded in
// here: REQUIRES app_manager -> app_manager_remote already exists one way,
// and app_manager requiring it back would be circular). Instead, whoever
// turns remote mode on SUPPLIES the actual network calls via this small
// provider contract; app_manager only owns the state (which app source is
// current) and the periodic background refresh.
typedef struct {
    // Fetches the remote device's current app list into out[] (capacity
    // max). Returns the count actually written, or -1 on failure (a timed-
    // out/failed round trip — remote mode stays ON regardless; the registry
    // just keeps showing whatever the last successful fetch had until the
    // next one succeeds). Same blocking-call rule as proximity_rpc_call()
    // itself: the provider's implementation must do this work on its OWN
    // background task — app_manager's remote-mode refresh task is that task,
    // never the caller of app_manager_count()/get() (typically the UI/LVGL
    // render task, which must never block on a network round trip).
    int  (*list)(const uint8_t mac[6], app_entry_t *out, int max);
    bool (*launch)(const uint8_t mac[6], const char *name);
    bool (*stop)(const uint8_t mac[6], const char *name);
} app_manager_remote_provider_t;

// Turns remote mode on: pointed at `mac`, backed by `provider`. Spawns (or
// restarts, if remote mode was already on for a different mac/provider) a
// background task that calls provider->list() every few seconds to keep the
// registry current. The local registry (this device's own apps) is left
// completely untouched underneath — app_manager_clear_remote() reveals it
// again exactly as it was, no re-scan needed. False if `mac`/`provider` is
// NULL, `provider->list` is NULL, or the background task fails to start
// (out of memory) — remote mode is NOT considered on in that case.
bool app_manager_set_remote(const uint8_t mac[6], const app_manager_remote_provider_t *provider);

// Turns remote mode off. A no-op if it wasn't on. Blocks briefly for the
// background refresh task to actually exit — same "wait for the task,
// don't just drop it" discipline app_manager_stop() itself already follows.
void app_manager_clear_remote(void);

bool app_manager_is_remote(void);

// The mac remote mode is currently pointed at (false, out_mac untouched,
// if remote mode is off). For a caller that needs to know WHICH server
// it's talking to — e.g. Server Manager (source/apps/system/
// server_manager/), reached only via the synthetic "Server Manager" entry
// app_manager_count()/get()/launch_idx() inject into the remote-mode list
// whenever the current session is a server admin
// (app_manager_is_remote() && user_mgr_is_admin(user_mgr_current_user())).
// cheetah_home.c/systemui_xp.c need no changes for that entry to show up —
// both already render whatever app_manager_count()/get() report, the same
// reason the retired "Milk Bottle" synthetic row (milkbar_app.c, earlier
// this session) needed no per-UI special-casing either.
bool app_manager_remote_mac(uint8_t out_mac[6]);

// ── Placement (local / remote / hybrid) ─────────────────────────────────────
// Resolves `declared` (an app_entry_t's own app_placement_t, from a remote
// listing) against a capability compare into an ACTUAL placement to act
// on for THIS tap — never HYBRID itself, since HYBRID's whole point is
// "not decided yet until asked."
//
// REMOTE/LOCAL declared: returned as-is, no comparison needed. HYBRID:
// prefer whichever side has stronger compute; if neither/both do, prefer
// REMOTE only if the peer actually has a display (no point defaulting to
// remote on a peer with nothing to show it on) — otherwise LOCAL.
//
// Plain booleans, not PROXIMITY_CAP_* bitmasks — the caller (which does
// have proximity.h) extracts these itself, so app_manager.c stays free
// of a proximity/proximity_rpc dependency even for just the capability
// bit values, same reasoning app_manager_remote_provider_t's own doc
// comment gives for why remote mode's actual network calls are injected
// rather than linked directly.
app_placement_t app_manager_decide_placement(app_placement_t declared,
                                              bool my_strong_compute,
                                              bool peer_strong_compute,
                                              bool peer_has_display);

#ifdef __cplusplus
}
#endif
