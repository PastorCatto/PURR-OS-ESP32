/*
 * purr_app_abi.h - the call table between a `.cat` app and the minimal runtime (AppRuntime/
 * SPEC.md, "scoped minimal-first" section 1, chunk C).
 *
 * The full spec (section 5) gives an app five lifecycle callbacks and a env/catcall surface
 * built out through `sched`/`app`/`storage`/`console`/`ui`. None of that exists in code yet
 * -- this is the first real slice, sized to what a single, synchronous, foreground-only
 * instance needs to prove the loader (commands.c's load_relocatable_file(), already proven
 * for kernel modules) can load and run a PURR_IMG_APP payload at all, before any multitasking,
 * OOM killer, or real catcall dispatch is built.
 *
 * Trimmed from the full spec on purpose:
 *   - Only on_create/on_destroy. No resume/pause/save-state/low-memory: there is no second
 *     instance to switch away from yet, and no OOM killer to call them.
 *   - purr_app_env_t carries exactly one function pointer (console_print), not a catcall_get
 *     dispatcher. AppRuntime/SPEC.md section 8 calls `console` out as "the first interface
 *     apps use for text, before any `ui` exists" -- this is that, minus the dispatch
 *     machinery, which has nothing to route to yet.
 *   - The whole run is synchronous: the shell's `run <app>` command calls on_create(), then
 *     immediately on_destroy() -- there is no event loop, no sched catcall to create one with.
 *     This is enough to prove load -> relocate -> call -> lifecycle end to end on real
 *     hardware; a real foreground/background instance comes once the manager (section 3-4 of
 *     the spec) is built.
 *
 * Included by BOTH sides, same convention as purr_module_abi.h: the runtime (which has the
 * rest of coreos available) and an app's own standalone compile (purrstrap `modules build
 * --kind app`, which reuses the exact module build pipeline -- CatFormat/SPEC.md's own
 * payload format, superseded for now: see that spec's note).
 */
#ifndef PURR_APP_ABI_H
#define PURR_APP_ABI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PURR_APP_ABI_VERSION 1u

/* The only reason on_destroy exists yet: the synchronous run above always finishes this way,
 * never because of a kill or an update (AppRuntime/SPEC.md section 5's richer reason set,
 * "killed for memory" among them, has nothing to report until the manager and OOM killer are
 * built). */
#define PURR_APP_DESTROY_NORMAL 0

typedef struct {
    /* Writes `s` to whichever console the app was run from. An app cannot call purr_cli_puts
     * itself (no linkage to it, same reason purr_module_abi.h's puts/printf exist) -- this is
     * the one catcall-shaped thing this minimal runtime hands out. The real `console` catcall
     * (AppRuntime/SPEC.md section 8) carries a handle instead of being a bare function
     * pointer; this shortcut works because the whole call is synchronous on the caller's own
     * task, with exactly one instance ever running at a time. */
    void (*console_print)(const char *s);
} purr_app_env_t;

/* Named purr_app_table_t, not purr_app_entry_t: purr_appmgr.h already uses purr_app_entry_t
 * for a REGISTRY entry (name/version/size/verified), a completely different thing that is
 * also in scope everywhere this header is -- commands.c includes both. Same naming hazard
 * purr_module_abi.h avoids by calling its table purr_module_table_t, not *_entry_t. */
typedef struct {
    uint32_t abi_version;      /* must equal PURR_APP_ABI_VERSION; the runtime refuses a mismatch */

    /* First start: receives env and the state saved from the last run, or NULL/0 (nothing
     * saves state yet -- AppRuntime/SPEC.md section 5's on_save_state is out of scope here).
     * Return value is just logged by the runtime for now; it has nothing to react to it with. */
    int (*on_create)(const purr_app_env_t *env, const void *state, uint32_t state_len);

    /* Last chance to clean up before the runtime unmaps the app. `reason` is always
     * PURR_APP_DESTROY_NORMAL today. May be NULL if the app has nothing to tear down. */
    void (*on_destroy)(int reason);
} purr_app_table_t;

/* An app's entry point (CatFormat/SPEC.md section 6: "a function that returns the lifecycle
 * table"). Takes no arguments -- unlike a module's entry (purr_module_abi.h), which receives
 * the core table immediately because its commands can run at any time after load. An app's
 * only call into the host is through env, handed to on_create() by the runtime, not at entry
 * time, so there is nothing for entry() itself to take. */
typedef const purr_app_table_t *(*purr_app_entry_fn)(void);

#ifdef __cplusplus
}
#endif

#endif /* PURR_APP_ABI_H */
