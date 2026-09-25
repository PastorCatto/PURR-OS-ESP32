#pragma once
// purr_console_login.h — the shared, ready-made login/exec pair every
// kernel_*_boot.c can hand straight to purr_console_set_login_fn()/
// purr_console_set_exec_fn(), instead of reimplementing the same
// user_mgr/app_manager glue per device.
//
// Deliberately its OWN component, not folded into purr_console itself:
// purr_console.h's own doc comment on purr_console_login_fn explains why
// that core must stay linkable with ZERO dependency on user_mgr/
// app_manager (the same reasoning rnode_module.c reaches pairing.c
// through a kernel callback instead of a direct #include) — a future
// minimal/recovery-only console build must still be able to link
// purr_console alone. This module is the opposite: it exists ONLY to
// pull user_mgr + app_manager in, for every kernel boot file that already
// links both anyway for its normal boot purposes and doesn't want to
// hand-roll the login prompt a fourth or fifth time.
//
// Lifted verbatim from kernel_tdeck_plus/kernel_tdp_boot.c's own
// tdp_console_login()/tdp_console_exec() (the first, and until now only,
// real implementation) — see purr_console.h's purr_console_login_fn/
// purr_console_exec_fn typedefs for the exact contract these two match.

#include "purr_console.h"

#ifdef __cplusplus
extern "C" {
#endif

// A login gate matching purr_console_login_fn exactly: enumerates
// user_mgr accounts (prompting for a username only when more than one
// exists), then either auto-logs-in a no-password account (zero
// friction, same contract user_mgr.h's own header comment documents) or
// prompts for a password via user_mgr_verify(), retrying ("Login
// incorrect") until some account is actually authenticated. Calls
// user_mgr_set_logged_in() + app_manager_notify_unlocked() on success —
// the same unlock call systemui_login.c/systemui_login_ios.c already
// make, just from the console instead of a graphical login screen.
void purr_console_login_default_login_fn(const purr_console_io_t *io);

// An exec handler matching purr_console_exec_fn exactly:
// app_manager_launch_by_name(args), reporting launched/failed.
void purr_console_login_default_exec_fn(const char *args);

#ifdef __cplusplus
}
#endif
