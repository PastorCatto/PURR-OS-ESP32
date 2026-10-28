/*
 * purr_runtime.h - the app manager (plan6.md, AppRuntime/SPEC.md sections 3-11).
 *
 * Pure logic over a small host interface, with no FreeRTOS and no heap, so it is tested on the PC
 * with fakes (test/host/test_runtime.c) and wired to the device by a thin layer that gives each
 * instance a task and a queue. It owns the instance table and states, launch and admission,
 * switching and focus, saved state, the out-of-memory killer and teardown, and a Hyprland-style
 * control surface: text config with rules and binds, dispatchers, queries and an event stream.
 *
 * It never knows about a UI. A UI (the card stack, the text shell) is a client: it subscribes to
 * the event stream to follow what runs, and calls dispatchers to change it.
 */
#ifndef PURR_RUNTIME_H
#define PURR_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#include "purr_app_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PURR_RT_MAX_APPS    8
#define PURR_RT_NAME_LEN    32
#define PURR_RT_MAX_STATE   4096
#define PURR_RT_MAX_SINKS   4
#define PURR_RT_MAX_RULES   16
#define PURR_RT_MAX_BINDS   16
#define PURR_RT_MAX_EXEC    8
#define PURR_RT_MAX_SUBS    4
#define PURR_RT_PREFIX_LEN  24
#define PURR_RT_LINE_LEN    96

/* App flags: from the app's header, and changed by rules. */
#define PURR_APP_F_BACKGROUND_OK 0x01u  /* may keep running when it leaves the foreground */
#define PURR_APP_F_SAVES_STATE   0x02u  /* implements on_save_state */
#define PURR_APP_F_PERSISTENT    0x04u  /* never chosen as an out-of-memory victim */
#define PURR_APP_F_PRIVILEGED    0x08u  /* may use the ctl catcall */
#define PURR_APP_F_SUSPEND       0x10u  /* rule: never in the background; suspend on leaving */
#define PURR_APP_F_NOFOCUS       0x20u  /* rule: does not take the foreground when opened */
#define PURR_APP_F_FOCUS         0x40u  /* rule: takes the foreground when opened, even a daemon */

#define PURR_APP_KIND_COMMAND_LINE 0
#define PURR_APP_KIND_DAEMON       1
#define PURR_APP_KIND_UI           2

typedef enum {
    PURR_RT_STARTING = 0,
    PURR_RT_FOREGROUND,
    PURR_RT_BACKGROUND,
    PURR_RT_SUSPENDED,
    PURR_RT_STOPPING,
    PURR_RT_STOPPED,
} purr_rt_state_t;

/* Lifecycle events the manager asks the platform to deliver on an app's own task. */
#define PURR_RT_EV_CREATE      16
#define PURR_RT_EV_RESUME      17
#define PURR_RT_EV_PAUSE       18
#define PURR_RT_EV_SAVE_STATE  19
#define PURR_RT_EV_LOW_MEMORY  20
#define PURR_RT_EV_DESTROY     21
#define PURR_RT_EV_APP         22   /* a PURR_APPEV_* event: app_ev */

typedef struct {
    int type;                          /* PURR_RT_EV_* */
    int arg;                           /* DESTROY: reason; LOW_MEMORY: level */
    const void *state;                 /* CREATE: saved state, or NULL */
    uint32_t state_len;
    void *buf;                         /* SAVE_STATE: where to write */
    uint32_t cap;
    uint32_t *out_len;                 /* SAVE_STATE: how much was written */
    const purr_app_event_t *app_ev;    /* EV_APP */
} purr_rt_event_t;

/* What loading an app yields. The platform adapts a v1 table to v2 before returning it. */
typedef struct {
    const purr_app_table2_t *table;
    const purr_app_env_t *env;
    uint32_t handle;                   /* opaque, passed back to unload */
    uint32_t memory_needed;            /* bytes: its block, for admission */
    uint8_t kind;
    uint8_t flags;
} purr_rt_app_t;

#define PURR_RT_HOST_UNTRUSTED (-2)

typedef struct {
    void *ctx;
    /* Find, verify and load an app by name. 0 on success; PURR_RT_HOST_UNTRUSTED if the image is
     * unsigned, tampered with, or signed by a key that may not sign apps (an app that is not
     * signed never runs, in any secure mode); any other negative value for "not found". */
    int  (*load)(void *ctx, const char *name, purr_rt_app_t *out);
    void (*unload)(void *ctx, uint32_t handle);
    /* Run one event on the app's own task and wait for it to return (the device layer posts to
     * the task's queue and waits, with a timeout). Returns the callback's result (CREATE), or
     * < 0 if the app did not answer in time. NULL means "call it directly" (host tests). */
    int  (*deliver)(void *ctx, int id, const purr_rt_app_t *app, const purr_rt_event_t *ev);
    uint32_t (*now_ms)(void *ctx);
    uint32_t (*free_bytes)(void *ctx);
    int  (*save_state)(void *ctx, const char *name, const void *buf, uint32_t len);
    /* Returns the state length, 0 if none saved, < 0 on error. */
    int  (*load_state)(void *ctx, const char *name, void *buf, uint32_t cap);
    void (*log)(void *ctx, const char *line);
} purr_rt_host_t;

typedef struct {
    uint32_t max_apps;                 /* <= PURR_RT_MAX_APPS */
    uint32_t reserve;                  /* bytes kept free for the system when admitting */
    uint32_t low;                      /* free bytes below which apps are told to trim */
    uint32_t critical;                 /* free bytes below which victims are stopped */
    uint32_t margin;                   /* recover above low/critical by this much (hysteresis) */
    uint32_t min_kill_ms;              /* least time between two out-of-memory kills */
    uint32_t unresponsive_ms;          /* foreground silent for this long = unresponsive */
} purr_rt_cfg_t;

typedef struct {
    int      id;                       /* 0 = free slot */
    char     name[PURR_RT_NAME_LEN];
    purr_rt_app_t app;
    purr_rt_state_t state;
    uint8_t  flags;                    /* effective: header flags with rules applied */
    uint8_t  unresponsive;             /* already reported */
    uint8_t  pinned;
    uint32_t memory;                   /* bytes held (rule override or header) */
    uint32_t started_ms, last_used_ms, last_beat_ms;
    char     subs[PURR_RT_MAX_SUBS][PURR_RT_PREFIX_LEN];
} purr_rt_inst_t;

typedef struct {
    uint8_t  effect;                   /* PURR_RT_RULE_* */
    uint32_t arg;                      /* `memory` rules: bytes */
    char     name[PURR_RT_NAME_LEN];   /* glob; empty = any */
    int8_t   kind;                     /* -1 = any */
} purr_rt_rule_t;

typedef struct {
    uint8_t  mods;                     /* PURR_RT_MOD_* */
    char     key[16];
    char     dispatch[PURR_RT_LINE_LEN];
} purr_rt_bind_t;

#define PURR_RT_MOD_SHIFT 1u
#define PURR_RT_MOD_CTRL  2u
#define PURR_RT_MOD_ALT   4u
#define PURR_RT_MOD_SUPER 8u

typedef void (*purr_rt_sink_fn)(void *ctx, const char *line);

typedef struct {
    purr_rt_host_t host;
    purr_rt_cfg_t  cfg;
    purr_rt_cfg_t  cfg_defaults;       /* what a reload falls back to for options the file omits */
    purr_rt_inst_t inst[PURR_RT_MAX_APPS];
    int next_id;
    int fg_id;                         /* 0 = nothing in the foreground */
    uint8_t pressure;                  /* 0 ok, 1 low, 2 critical */
    uint32_t last_kill_ms;
    int kills;                         /* out-of-memory kills so far, for tests and `memory` */
    uint8_t in_emit;
    purr_rt_sink_fn sink[PURR_RT_MAX_SINKS];
    void *sink_ctx[PURR_RT_MAX_SINKS];
    purr_rt_rule_t rules[PURR_RT_MAX_RULES];
    int nrules;
    purr_rt_bind_t binds[PURR_RT_MAX_BINDS];
    int nbinds;
    char exec_once[PURR_RT_MAX_EXEC][PURR_RT_NAME_LEN];   /* opened after the first login */
    int nexec;
    char exec_boot[PURR_RT_MAX_EXEC][PURR_RT_NAME_LEN];   /* opened at boot, with no user */
    int nexec_boot;
    uint8_t state_buf[PURR_RT_MAX_STATE];
} purr_rt_t;

/* ------------------------------------------------------------------ setup */
void purr_rt_init(purr_rt_t *rt, const purr_rt_host_t *host);
/* Makes the current options the defaults a config reload starts from. Call after setting them
 * (purr_rt_init sets them to the built-in values). */
void purr_rt_set_defaults(purr_rt_t *rt);
const char *purr_rt_state_name(purr_rt_state_t s);
const char *purr_rt_reason_name(int reason);
const char *purr_rt_kind_name(int kind);

/* Runs one event on an app's table (the part the device's per-app task calls). Returns the
 * callback's result for CREATE, else 0. */
int purr_rt_invoke(const purr_rt_app_t *app, const purr_rt_event_t *ev);

/* ---------------------------------------------------------------- lifecycle */
/* Opens an app by name: admission, create, then foreground or background by its kind and rules.
 * Returns the new instance id (> 0) or a negative error (see PURR_RT_E_*). */
#define PURR_RT_E_LOAD      (-1)   /* could not be found or verified */
#define PURR_RT_E_FULL      (-2)   /* no free instance slot */
#define PURR_RT_E_MEMORY    (-3)   /* does not fit, even after stopping what may be stopped */
#define PURR_RT_E_CREATE    (-4)   /* on_create failed */
#define PURR_RT_E_NOAPP     (-5)   /* no such instance */
#define PURR_RT_E_STATE     (-6)   /* not allowed in the app's state */
#define PURR_RT_E_ARG       (-7)
#define PURR_RT_E_UNTRUSTED (-8)   /* refused: not signed by a trusted key (see host.load) */
int purr_rt_open(purr_rt_t *rt, const char *name);

/* Stops an instance. `reason` is PURR_APP_DESTROY_*. */
int purr_rt_stop(purr_rt_t *rt, int id, int reason);
int purr_rt_focus(purr_rt_t *rt, int id);
int purr_rt_background(purr_rt_t *rt, int id);   /* leaves the foreground; keeps running if allowed */
int purr_rt_suspend(purr_rt_t *rt, int id);
int purr_rt_resume(purr_rt_t *rt, int id);       /* same as focus for a suspended app */

/* Resolves "3", "notes", "active", "next", "prev" to an instance id, or 0. */
int purr_rt_ref(purr_rt_t *rt, const char *ref);
purr_rt_inst_t *purr_rt_get(purr_rt_t *rt, int id);
int purr_rt_count(const purr_rt_t *rt);

/* The platform tells the manager an app is alive (it handled an event). */
void purr_rt_heartbeat(purr_rt_t *rt, int id);

/* Call now and then (the device layer does it every second): memory pressure and unresponsive
 * apps. Returns the number of apps stopped. */
int purr_rt_tick(purr_rt_t *rt);

/* ---------------------------------------------------------------- events */
int  purr_rt_add_sink(purr_rt_t *rt, purr_rt_sink_fn fn, void *ctx);
void purr_rt_remove_sink(purr_rt_t *rt, purr_rt_sink_fn fn, void *ctx);
/* Publishes `name>>data` to every sink, and to apps subscribed to a matching prefix. */
void purr_rt_emit(purr_rt_t *rt, const char *name, const char *data);
int  purr_rt_subscribe(purr_rt_t *rt, int id, const char *prefix);
void purr_rt_unsubscribe(purr_rt_t *rt, int id, const char *prefix);
/* Delivers a PURR_APPEV_USER event to one app (the `post` dispatcher). */
int  purr_rt_post(purr_rt_t *rt, int id, const char *name, const char *data);

/* ----------------------------------------------------------- control surface */
/* A dispatcher line such as "focus notes" or "open clock". The reply (or the error text) is
 * written to out. Returns 0 on success, < 0 on failure. */
int purr_rt_dispatch(purr_rt_t *rt, const char *line, char *out, size_t cap);
/* A query line such as "apps", "apps -j", "activeapp", "memory", "rules", "binds", "version". */
int purr_rt_query(purr_rt_t *rt, const char *line, char *out, size_t cap);
/* A key press against the binds. Returns 1 if a bind consumed it. `key` is a name such as
 * "Tab", "Return" or a single character. */
int purr_rt_key(purr_rt_t *rt, unsigned mods, const char *key);

/* --------------------------------------------------------------------- config */
/* Parses an apps.conf. Replaces the rules, binds, the exec-boot and exec-once lists and the `general:` options; a bad line is reported in `err` (one line each) and skipped. Returns the number of bad
 * lines, so 0 means the file was clean. Emits `config>>reloaded`. An option the file does not set
 * goes back to its default, so removing a line and reloading undoes it. */
int purr_rt_config_load(purr_rt_t *rt, const char *text, char *err, size_t errcap);
/* Opens the exec-boot apps in order: the ones the system needs with nobody logged in (a
 * shell, a daemon every user relies on). Returns how many opened. Call at boot. */
int purr_rt_exec_boot(purr_rt_t *rt);
/* Opens the exec-once apps in order: everything else, which waits until a user has logged in
 * (user apps live in the user's own folder, and have no business running before anyone is
 * there). Returns how many opened. Call once, after the first login. */
int purr_rt_exec_once(purr_rt_t *rt);

#ifdef __cplusplus
}
#endif

#endif /* PURR_RUNTIME_H */
