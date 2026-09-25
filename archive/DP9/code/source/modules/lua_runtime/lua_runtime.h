#pragma once
// lua_runtime.h — Lua 5.4 scripting runtime for PURR OS (.meow apps)
//
// Adapted from PURR-OS-0.11's lua_runtime.cpp/.h — that version was C++ and
// bound directly to KITT's display/touch singleton; this version is plain C
// and only ever talks through purr_win.h/purr_kernel.h catcall wrappers, same
// rule every other app in this codebase follows.

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// PURR_MOD_SYSTEM init()/deinit() — called by the kernel module loader like
// any other module. init() creates the (single, global) Lua state, registers
// the system/sd/win modules, then runs the pending .meow script named by
// app_manager_get_pending_meow_path() — matching app_manager.c's existing
// launch_meow()/meow_task() dispatch, which already calls this via the
// kernel module registry ("lua_runtime") and expects init() to both set up
// and run the script in one call (init() only returns once the script has
// finished, or immediately if it errors).
int  lua_runtime_init(void);
void lua_runtime_deinit(void);

// Run Lua source directly — used by lua_runtime_init() for the pending
// .meow script, also usable by a future terminal/REPL-style caller.
bool lua_run_file(const char *path);
bool lua_run_code(const char *code, const char *name);

#ifdef __cplusplus
}
#endif
