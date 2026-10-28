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

/* ABI v2 (plan6.md): the full lifecycle, an event hook, and a catcall dispatcher. It is purely
 * additive: a v2 table starts with the v1 table (abi_version is 2 and on_create/on_destroy are
 * in the same places) and appends the new callbacks, and the env gains fields after
 * console_print. A runtime that knows v2 accepts both; one that only knows v1 refuses a v2 app
 * at the version check, as it always has. */
#define PURR_APP_ABI_VERSION_2 2u

/* The only reason on_destroy exists yet: the synchronous run above always finishes this way,
 * never because of a kill or an update (AppRuntime/SPEC.md section 5's richer reason set,
 * "killed for memory" among them, has nothing to report until the manager and OOM killer are
 * built). */
#define PURR_APP_DESTROY_NORMAL 0
/* The reasons on_destroy can be called with in v2 (AppRuntime/SPEC.md section 5). A kill for memory
 * may not run on_destroy at all, so anything worth keeping is saved earlier, on leaving the
 * foreground (on_save_state), not only here. */
#define PURR_APP_DESTROY_USER         0   /* the user (or a dispatcher) closed it */
#define PURR_APP_DESTROY_UPDATE       1   /* being replaced by a new version */
#define PURR_APP_DESTROY_MEMORY       2   /* killed to free memory */
#define PURR_APP_DESTROY_UNRESPONSIVE 3   /* stopped for not handling events */
#define PURR_APP_DESTROY_SHUTDOWN     4   /* the system is going down */

/* on_low_memory levels. */
#define PURR_APP_MEM_LOW      1
#define PURR_APP_MEM_CRITICAL 2

typedef struct {
    /* Writes `s` to whichever console the app was run from. An app cannot call purr_cli_puts
     * itself (no linkage to it, same reason purr_module_abi.h's puts/printf exist) -- this is
     * the one catcall-shaped thing this minimal runtime hands out. The real `console` catcall
     * (AppRuntime/SPEC.md section 8) carries a handle instead of being a bare function
     * pointer; this shortcut works because the whole call is synchronous on the caller's own
     * task, with exactly one instance ever running at a time. */
    void (*console_print)(const char *s);

    /* v2 (appended; a v1 app never reads past console_print). env_version is
     * PURR_APP_ABI_VERSION_2 or later when catcall_get is set. */
    uint32_t env_version;

    /* Looks a catcall up by name (purr_catcalls.h). Returns the catcall's versioned table, or NULL
     * if the app was not granted it, the board does not have it, or the table is older than
     * min_version. An app imports nothing else from the system. */
    const void *(*catcall_get)(const char *name, uint32_t min_version);
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

/* Event types delivered to on_event (v2). */
#define PURR_APPEV_USER   1   /* a bus event the app subscribed to, or one posted to it: name + data */
#define PURR_APPEV_TIMER  2   /* a sched timer fired: arg = the timer id */
#define PURR_APPEV_KEY    3   /* a key, when the app has input focus: arg = key code, data = modifiers (decimal) */
#define PURR_APPEV_UI     4   /* a `ui` catcall widget event: arg = widget handle (0 = the window), name = what happened
                               * ("click", "changed", "back", "action") */
#define PURR_APPEV_WAKE   5   /* a `power` alarm fired: arg = alarm index, name = "alarm" (or "alarm-off": it was cut) */
#define PURR_APPEV_PERM   6   /* the user answered a permission prompt: arg = 1 allowed / 0 not, name = the permission */

typedef struct {
    uint32_t type;             /* PURR_APPEV_* */
    int32_t  arg;
    const char *name;          /* event name for USER events, else NULL */
    const char *data;          /* free-form payload, else NULL */
} purr_app_event_t;

/* The v2 table: the v1 table first (so the fields line up), then the rest of the lifecycle.
 * Every new callback is optional (NULL = the app does not care). Each call runs on the app's own
 * event task (AppRuntime/SPEC.md section 5), so callbacks must return quickly. */
typedef struct {
    purr_app_table_t base;     /* base.abi_version == PURR_APP_ABI_VERSION_2 */

    void (*on_resume)(void);                           /* came to the foreground */
    void (*on_pause)(void);                            /* left the foreground */
    uint32_t (*on_save_state)(void *buf, uint32_t cap);/* write saved state, return its length */
    void (*on_low_memory)(int level);                  /* PURR_APP_MEM_*: free what you can */
    void (*on_event)(const purr_app_event_t *ev);      /* bus events, timers, keys */

    /* What the app declares about itself. The signed header has no room for a manifest yet
     * (AppSDK/SPEC.md section 5), so until it does an app states these here and the runtime reads
     * them at load. memory_needed is the app's own block (data, heap and stack) in bytes, for
     * admission; kind is PURR_APP_KIND_* and flags are PURR_APP_F_* (purr_runtime.h). */
    uint32_t memory_needed;
    uint8_t  kind;
    uint8_t  flags;
    uint16_t reserved;
} purr_app_table2_t;

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
