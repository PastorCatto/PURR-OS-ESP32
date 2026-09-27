#pragma once
// purr_crash_guard.h — kernel-level crash-loop guard for P2/P3 modules and
// apps (P1 REQUIRED failures stay on the existing immediate-panic path —
// see purr_module.h's own documented priority semantics).
//
// Tracks failures per named entity (module name or app name) in NVS, so
// the count survives a hard reset (the exact case a real crash produces —
// there's no RAM state left to count in). After 5 strikes, that entity is
// marked disabled (purr_crash_guard_is_disabled()) and a recoverable
// ("blue") panic screen is shown via purr_kernel_panic_ex() — see
// purr_kernel.h. A genuine hang shows the blue screen immediately on the
// first occurrence instead of waiting for 5, since a hung task blocks the
// device right now regardless of how many times this has happened before.
//
// A "strike" is only ever a real failure signal — init()/lua_run_code()
// returning non-zero, a task that had to be force-killed after hanging,
// or a hard reset correlated (via a persisted breadcrumb) to whatever was
// active when it happened. A fast-but-successful return (every native
// app's normal "build UI and return" lifecycle, and non-looping .meow
// scripts) never counts, no matter how quickly it completes.

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Call right before starting/launching entity_name (a module's init() or
// an app's task) — records it as the "currently active" breadcrumb, so a
// hard crash before the matching mark_stop()/mark_hang() can still be
// attributed to it on the next boot.
void purr_crash_guard_mark_start(const char *entity_name);

// Call once entity_name has finished on its own (module init() returned,
// or an app's task self-completed). ok=false records a strike (using
// reason for the panic message, if this is the 5th). ok=true just clears
// the breadcrumb — a normal, successful completion is never a strike
// regardless of how quickly it happened.
void purr_crash_guard_mark_stop(const char *entity_name, bool ok, const char *reason);

// Call when entity_name had to be force-killed after hanging past its
// caller's own timeout. Always records a strike AND shows the blue panic
// screen immediately (purr_kernel_panic_ex(), recoverable=true) —
// noreturn in practice (loops until the user forces a reset).
void purr_crash_guard_mark_hang(const char *entity_name, const char *reason);

// True once entity_name has hit the strike threshold — callers (the
// static module loader, app_manager's launch functions) must check this
// before starting/launching and skip (log + notify) if disabled.
bool purr_crash_guard_is_disabled(const char *entity_name);

// Call once, very early at boot — after NVS is up AND after the display/
// touch catcalls are registered (so a blue screen can actually render if
// this trips), but before any P2/P3 module loads. Checks esp_reset_reason()
// for an unclean reset and, if the previous boot left a breadcrumb behind,
// records a strike against whatever was active when the device went down.
void purr_crash_guard_check_reset_reason(void);

// A genuine hang (mark_hang()) now reboots outright instead of parking on
// a panic screen that might itself hang trying to draw on a wedged
// display — see purr_crash_guard.c's record_strike_and_maybe_panic() for
// why. That reboot is a clean esp_restart(), so check_reset_reason()'s
// own esp_reset_reason()-based "unclean reset" check won't catch it; this
// separate marker is how the next boot finds out a recovery is in
// progress.
//
// purr_crash_guard_pending_recovery() — non-destructive peek. Call early
// in boot (before bringing up SD/display/radio) to decide whether to use
// bounded-timeout bring-up instead of today's unwrapped calls. name_out/
// reason_out may be NULL if you only need the bool. Returns false (and
// leaves outputs untouched) if nothing is pending.
bool purr_crash_guard_pending_recovery(char *name_out, size_t name_sz,
                                        char *reason_out, size_t reason_sz);

// Call exactly once, after every device has had its one bounded bring-up
// attempt for this boot (kernel_tdp_boot.c, right after
// purr_kernel_load_static_modules() returns) — ends the "recovering"
// window so a later purr_kernel_enable_static_module() call or a
// subsequent unrelated boot never sees a stale pending-recovery flag.
void purr_crash_guard_clear_pending_recovery(void);

#ifdef __cplusplus
}
#endif
