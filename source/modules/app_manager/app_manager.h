#pragma once
// app_manager.h — PURR OS app manager public API
//
// The app manager is a .purr system module that:
//   - Scans /flash/apps and /sdcard/apps for .meow, .hiss, .paws, .claw files
//   - Maintains a registry of available and running apps
//   - Provides the Cat Apps launcher UI (app grid over MiniWin)
//   - Enforces tier boundaries: .paws gets no kernel calls, .claw gets all,
//     .meow/.hiss run interpreted (see app_tier_t below for the split between them)

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
} app_tier_t;

// ── App entry ─────────────────────────────────────────────────────────────────

typedef enum {
    APP_STATE_IDLE    = 0,
    APP_STATE_RUNNING = 1,
    APP_STATE_STOPPED = 2,
    APP_STATE_ERROR   = 3,
} app_state_t;

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

#ifdef __cplusplus
}
#endif
