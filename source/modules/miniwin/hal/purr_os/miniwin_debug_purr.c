// miniwin_debug_purr.c — mw_debug_print_assert() override for PURR OS.
//
// MiniWin's own vendored implementation (MiniWin/miniwin_debug.c —
// deliberately excluded from this module's CMakeLists.txt SRCS, see that
// file's comment there) draws a crude built-in message directly via
// mw_hal_lcd_* calls and then hangs in a bare, unescapable while(true){} —
// completely disconnected from purr_crash_guard's strike tracking,
// dump-logs, and hold-to-force-reset controls. Confirmed live: this is
// exactly why a UI freeze could still occur with no panic screen or
// recovery path shown, even after fixing the specific reentrancy-guard
// bug that used to trigger it — ANY MW_ASSERT() failure anywhere in
// MiniWin's vendored code (not just that one already-fixed path) hits
// this same dead end.
//
// This file implements the same mw_debug_print_assert() signature
// (miniwin_debug.h) so the linker picks THIS definition instead of the
// (unbuilt) vendored one — redirecting every MiniWin-internal assertion
// failure into purr_crash_guard_mark_hang(), which shows the real (blue,
// recoverable) panic screen with dump-logs/force-reset controls and
// feeds the same strike-tracking every other P2/P3 failure does, instead
// of a silent, unrecoverable freeze.

#include "miniwin_debug.h"
#include "purr_crash_guard.h"
#include <stdio.h>

#ifndef NDEBUG

void mw_debug_print_assert(bool expression, const char *function_name, int32_t line_number, const char *message)
{
    if (expression) return;   // assert passed — matches the vendored function's own contract

    char reason[96];
    snprintf(reason, sizeof(reason), "MW_ASSERT %s:%ld %s",
             function_name ? function_name : "?", (long)line_number, message ? message : "");

    // "miniwin" — the same entity name the UI-hang heartbeat path
    // (purr_kernel.c's health_watchdog_task) already uses via
    // purr_kernel_ui()->name — so an internal MiniWin assert and a
    // MiniWin heartbeat staleness count toward the SAME strike total,
    // both correctly attributed to the UI backend rather than treated as
    // two unrelated problems.
    purr_crash_guard_mark_hang("miniwin", reason);
    // noreturn in practice — purr_kernel_panic_ex() loops until the user
    // forces a reset. Nothing after this point runs again this boot.
}

#endif
