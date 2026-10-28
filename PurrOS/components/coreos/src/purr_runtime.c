/*
 * purr_runtime.c - the app manager (purr_runtime.h, plan6.md). Plain C, no heap, no OS calls: the
 * platform comes in through purr_rt_host_t, so all of this runs on the PC under the host tests.
 */
#include "purr_runtime.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define RULE_BACKGROUND 1
#define RULE_PERSISTENT 2
#define RULE_SUSPEND    3
#define RULE_MEMORY     4
#define RULE_FOCUS      5
#define RULE_NOFOCUS    6

/* Internal reason: stopped with no on_destroy (the `kill` dispatcher). */
#define REASON_KILLED 5

/* ------------------------------------------------------------------ helpers */

static uint32_t now_ms(purr_rt_t *rt)
{
    return rt->host.now_ms ? rt->host.now_ms(rt->host.ctx) : 0;
}

static uint32_t free_bytes(purr_rt_t *rt)
{
    return rt->host.free_bytes ? rt->host.free_bytes(rt->host.ctx) : 0xFFFFFFFFu;
}

static void log_line(purr_rt_t *rt, const char *fmt, ...)
{
    if (!rt->host.log) {
        return;
    }
    char line[PURR_RT_LINE_LEN * 2];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    rt->host.log(rt->host.ctx, line);
}

/* A bounded output buffer: writes never overrun and the result is always terminated. */
typedef struct {
    char *p;
    size_t cap, len;
} ob_t;

static void ob_init(ob_t *o, char *buf, size_t cap)
{
    o->p = buf;
    o->cap = cap;
    o->len = 0;
    if (cap) {
        buf[0] = '\0';
    }
}

static void ob_printf(ob_t *o, const char *fmt, ...)
{
    if (o->cap == 0 || o->len + 1 >= o->cap) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(o->p + o->len, o->cap - o->len, fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    o->len += ((size_t)n < o->cap - o->len) ? (size_t)n : o->cap - o->len - 1;
}

static int ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') {
            x = (char)(x - 'A' + 'a');
        }
        if (y >= 'A' && y <= 'Z') {
            y = (char)(y - 'A' + 'a');
        }
        if (x != y) {
            return 0;
        }
    }
    return *a == *b;
}

/* Glob with * and ?. */
static int glob_match(const char *pat, const char *s)
{
    if (*pat == '\0') {
        return *s == '\0';
    }
    if (*pat == '*') {
        return glob_match(pat + 1, s) || (*s && glob_match(pat, s + 1));
    }
    if (*s && (*pat == '?' || *pat == *s)) {
        return glob_match(pat + 1, s + 1);
    }
    return 0;
}

static void copy_str(char *dst, size_t cap, const char *src)
{
    if (cap == 0) {
        return;
    }
    size_t n = strlen(src);
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) {
        s[--n] = '\0';
    }
    return s;
}

const char *purr_rt_state_name(purr_rt_state_t s)
{
    switch (s) {
    case PURR_RT_STARTING:   return "starting";
    case PURR_RT_FOREGROUND: return "foreground";
    case PURR_RT_BACKGROUND: return "background";
    case PURR_RT_SUSPENDED:  return "suspended";
    case PURR_RT_STOPPING:   return "stopping";
    case PURR_RT_STOPPED:    return "stopped";
    }
    return "?";
}

const char *purr_rt_reason_name(int reason)
{
    switch (reason) {
    case PURR_APP_DESTROY_USER:         return "closed";
    case PURR_APP_DESTROY_UPDATE:       return "update";
    case PURR_APP_DESTROY_MEMORY:       return "memory";
    case PURR_APP_DESTROY_UNRESPONSIVE: return "unresponsive";
    case PURR_APP_DESTROY_SHUTDOWN:     return "shutdown";
    case REASON_KILLED:                 return "killed";
    }
    return "unknown";
}

const char *purr_rt_kind_name(int kind)
{
    switch (kind) {
    case PURR_APP_KIND_COMMAND_LINE: return "command-line";
    case PURR_APP_KIND_DAEMON:       return "daemon";
    case PURR_APP_KIND_UI:           return "ui";
    }
    return "?";
}

/* --------------------------------------------------------------------- setup */

void purr_rt_init(purr_rt_t *rt, const purr_rt_host_t *host)
{
    memset(rt, 0, sizeof(*rt));
    rt->host = *host;
    rt->cfg.max_apps = PURR_RT_MAX_APPS;
    rt->cfg.reserve = 0;
    rt->cfg.low = 0;
    rt->cfg.critical = 0;
    rt->cfg.margin = 0;
    rt->cfg.min_kill_ms = 0;
    rt->cfg.unresponsive_ms = 0;          /* 0 = never report */
    rt->next_id = 0;
    rt->cfg_defaults = rt->cfg;
}

void purr_rt_set_defaults(purr_rt_t *rt)
{
    rt->cfg_defaults = rt->cfg;
}

int purr_rt_invoke(const purr_rt_app_t *app, const purr_rt_event_t *ev)
{
    const purr_app_table2_t *t = app->table;
    if (t == NULL) {
        return 0;
    }
    switch (ev->type) {
    case PURR_RT_EV_CREATE:
        return t->base.on_create ? t->base.on_create(app->env, ev->state, ev->state_len) : 0;
    case PURR_RT_EV_RESUME:
        if (t->on_resume) {
            t->on_resume();
        }
        return 0;
    case PURR_RT_EV_PAUSE:
        if (t->on_pause) {
            t->on_pause();
        }
        return 0;
    case PURR_RT_EV_SAVE_STATE:
        if (t->on_save_state && ev->buf && ev->out_len) {
            *ev->out_len = t->on_save_state(ev->buf, ev->cap);
        }
        return 0;
    case PURR_RT_EV_LOW_MEMORY:
        if (t->on_low_memory) {
            t->on_low_memory(ev->arg);
        }
        return 0;
    case PURR_RT_EV_DESTROY:
        if (t->base.on_destroy) {
            t->base.on_destroy(ev->arg);
        }
        return 0;
    case PURR_RT_EV_APP:
        if (t->on_event && ev->app_ev) {
            t->on_event(ev->app_ev);
        }
        return 0;
    }
    return 0;
}

static int deliver(purr_rt_t *rt, purr_rt_inst_t *in, const purr_rt_event_t *ev)
{
    if (rt->host.deliver) {
        return rt->host.deliver(rt->host.ctx, in->id, &in->app, ev);
    }
    return purr_rt_invoke(&in->app, ev);
}

purr_rt_inst_t *purr_rt_get(purr_rt_t *rt, int id)
{
    if (id <= 0) {
        return NULL;
    }
    for (int i = 0; i < PURR_RT_MAX_APPS; i++) {
        if (rt->inst[i].id == id) {
            return &rt->inst[i];
        }
    }
    return NULL;
}

int purr_rt_count(const purr_rt_t *rt)
{
    int n = 0;
    for (int i = 0; i < PURR_RT_MAX_APPS; i++) {
        if (rt->inst[i].id != 0) {
            n++;
        }
    }
    return n;
}

/* --------------------------------------------------------------------- events */

int purr_rt_add_sink(purr_rt_t *rt, purr_rt_sink_fn fn, void *ctx)
{
    for (int i = 0; i < PURR_RT_MAX_SINKS; i++) {
        if (rt->sink[i] == NULL) {
            rt->sink[i] = fn;
            rt->sink_ctx[i] = ctx;
            return 0;
        }
    }
    return -1;
}

void purr_rt_remove_sink(purr_rt_t *rt, purr_rt_sink_fn fn, void *ctx)
{
    for (int i = 0; i < PURR_RT_MAX_SINKS; i++) {
        if (rt->sink[i] == fn && rt->sink_ctx[i] == ctx) {
            rt->sink[i] = NULL;
            rt->sink_ctx[i] = NULL;
        }
    }
}

static int starts_with(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

void purr_rt_emit(purr_rt_t *rt, const char *name, const char *data)
{
    char line[PURR_RT_LINE_LEN * 2];
    snprintf(line, sizeof(line), "%s>>%s", name, data ? data : "");
    for (int i = 0; i < PURR_RT_MAX_SINKS; i++) {
        if (rt->sink[i]) {
            rt->sink[i](rt->sink_ctx[i], line);
        }
    }
    /* An app's on_event may emit too; it reaches the sinks but is not fanned back out to apps,
     * which keeps a chatty subscriber from looping the manager. */
    if (rt->in_emit) {
        return;
    }
    rt->in_emit = 1;
    purr_app_event_t aev = {.type = PURR_APPEV_USER, .arg = 0, .name = name, .data = data};
    purr_rt_event_t ev = {.type = PURR_RT_EV_APP, .app_ev = &aev};
    for (int i = 0; i < PURR_RT_MAX_APPS; i++) {
        purr_rt_inst_t *in = &rt->inst[i];
        if (in->id == 0 || in->state == PURR_RT_SUSPENDED || in->state == PURR_RT_STOPPING ||
            in->state == PURR_RT_STOPPED || in->state == PURR_RT_STARTING) {
            continue;
        }
        for (int s = 0; s < PURR_RT_MAX_SUBS; s++) {
            if (in->subs[s][0] && starts_with(name, in->subs[s])) {
                deliver(rt, in, &ev);
                break;                     /* once per event, however many prefixes match */
            }
        }
    }
    rt->in_emit = 0;
}

static void emitf(purr_rt_t *rt, const char *name, const char *fmt, ...)
{
    char data[PURR_RT_LINE_LEN];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(data, sizeof(data), fmt, ap);
    va_end(ap);
    purr_rt_emit(rt, name, data);
}

int purr_rt_subscribe(purr_rt_t *rt, int id, const char *prefix)
{
    purr_rt_inst_t *in = purr_rt_get(rt, id);
    if (in == NULL) {
        return PURR_RT_E_NOAPP;
    }
    for (int s = 0; s < PURR_RT_MAX_SUBS; s++) {
        if (strcmp(in->subs[s], prefix) == 0) {
            return 0;
        }
    }
    for (int s = 0; s < PURR_RT_MAX_SUBS; s++) {
        if (in->subs[s][0] == '\0') {
            copy_str(in->subs[s], sizeof(in->subs[s]), prefix);
            return 0;
        }
    }
    return -1;
}

void purr_rt_unsubscribe(purr_rt_t *rt, int id, const char *prefix)
{
    purr_rt_inst_t *in = purr_rt_get(rt, id);
    if (in == NULL) {
        return;
    }
    for (int s = 0; s < PURR_RT_MAX_SUBS; s++) {
        if (strcmp(in->subs[s], prefix) == 0) {
            in->subs[s][0] = '\0';
        }
    }
}

int purr_rt_post(purr_rt_t *rt, int id, const char *name, const char *data)
{
    purr_rt_inst_t *in = purr_rt_get(rt, id);
    if (in == NULL) {
        return PURR_RT_E_NOAPP;
    }
    if (in->state == PURR_RT_SUSPENDED || in->state == PURR_RT_STOPPING ||
        in->state == PURR_RT_STOPPED || in->state == PURR_RT_STARTING) {
        return PURR_RT_E_STATE;
    }
    purr_app_event_t aev = {.type = PURR_APPEV_USER, .arg = 0, .name = name, .data = data};
    purr_rt_event_t ev = {.type = PURR_RT_EV_APP, .app_ev = &aev};
    deliver(rt, in, &ev);
    return 0;
}

/* ------------------------------------------------------------------- states */

static void set_state(purr_rt_t *rt, purr_rt_inst_t *in, purr_rt_state_t ns)
{
    if (in->state == ns) {
        return;
    }
    purr_rt_state_t old = in->state;
    in->state = ns;
    if (old == PURR_RT_FOREGROUND && rt->fg_id == in->id) {
        rt->fg_id = 0;
    }
    if (ns == PURR_RT_FOREGROUND) {
        rt->fg_id = in->id;
        in->last_used_ms = now_ms(rt);
        in->last_beat_ms = in->last_used_ms;
        in->unresponsive = 0;
    }
    emitf(rt, "state", "%d,%s,%s,%s", in->id, in->name, purr_rt_state_name(old),
          purr_rt_state_name(ns));
}

static void save_state_of(purr_rt_t *rt, purr_rt_inst_t *in)
{
    if (!(in->flags & PURR_APP_F_SAVES_STATE) || !rt->host.save_state) {
        return;
    }
    uint32_t len = 0;
    purr_rt_event_t ev = {.type = PURR_RT_EV_SAVE_STATE, .buf = rt->state_buf,
                          .cap = PURR_RT_MAX_STATE, .out_len = &len};
    deliver(rt, in, &ev);
    if (len > 0 && len <= PURR_RT_MAX_STATE) {
        rt->host.save_state(rt->host.ctx, in->name, rt->state_buf, len);
    }
}

/* The foreground app steps back: saves state, is told, and goes to the background if it may
 * run there, else suspended. */
static void leave_foreground(purr_rt_t *rt, purr_rt_inst_t *in, int force_suspend)
{
    save_state_of(rt, in);
    purr_rt_event_t ev = {.type = PURR_RT_EV_PAUSE};
    deliver(rt, in, &ev);
    int bg = (in->flags & PURR_APP_F_BACKGROUND_OK) && !(in->flags & PURR_APP_F_SUSPEND) &&
             !force_suspend;
    set_state(rt, in, bg ? PURR_RT_BACKGROUND : PURR_RT_SUSPENDED);
}

static void bring_to_front(purr_rt_t *rt, purr_rt_inst_t *in)
{
    if (rt->fg_id == in->id) {
        return;
    }
    purr_rt_inst_t *prev = purr_rt_get(rt, rt->fg_id);
    if (prev) {
        leave_foreground(rt, prev, 0);
    }
    purr_rt_event_t ev = {.type = PURR_RT_EV_RESUME};
    deliver(rt, in, &ev);
    set_state(rt, in, PURR_RT_FOREGROUND);
    emitf(rt, "focus", "%d,%s", in->id, in->name);
}

/* ------------------------------------------------------------------- rules */

static void apply_rules(purr_rt_t *rt, purr_rt_inst_t *in)
{
    for (int i = 0; i < rt->nrules; i++) {
        const purr_rt_rule_t *r = &rt->rules[i];
        if (r->name[0] && !glob_match(r->name, in->name)) {
            continue;
        }
        if (r->kind >= 0 && r->kind != in->app.kind) {
            continue;
        }
        switch (r->effect) {
        case RULE_BACKGROUND: in->flags |= PURR_APP_F_BACKGROUND_OK; break;
        case RULE_PERSISTENT: in->flags |= PURR_APP_F_PERSISTENT; break;
        case RULE_SUSPEND:
            in->flags |= PURR_APP_F_SUSPEND;
            in->flags &= (uint8_t)~PURR_APP_F_BACKGROUND_OK;
            break;
        case RULE_MEMORY:     in->memory = r->arg; break;
        case RULE_FOCUS:      in->flags |= PURR_APP_F_FOCUS; in->flags &= (uint8_t)~PURR_APP_F_NOFOCUS; break;
        case RULE_NOFOCUS:    in->flags |= PURR_APP_F_NOFOCUS; in->flags &= (uint8_t)~PURR_APP_F_FOCUS; break;
        default: break;
        }
    }
}

/* ------------------------------------------------------------- victims, OOM */

/* The next app to stop to free memory, or NULL: suspended apps first, then background, least
 * recently used first, the largest first among equals. Never the foreground app, an app being
 * started or stopped, or a persistent or pinned one (AppRuntime/SPEC.md section 7). */
static purr_rt_inst_t *pick_victim(purr_rt_t *rt)
{
    purr_rt_inst_t *best = NULL;
    for (int i = 0; i < PURR_RT_MAX_APPS; i++) {
        purr_rt_inst_t *in = &rt->inst[i];
        if (in->id == 0 || (in->state != PURR_RT_SUSPENDED && in->state != PURR_RT_BACKGROUND)) {
            continue;
        }
        if ((in->flags & PURR_APP_F_PERSISTENT) || in->pinned) {
            continue;
        }
        if (best == NULL) {
            best = in;
            continue;
        }
        int cls_in = in->state == PURR_RT_SUSPENDED ? 0 : 1;
        int cls_best = best->state == PURR_RT_SUSPENDED ? 0 : 1;
        if (cls_in != cls_best) {
            if (cls_in < cls_best) {
                best = in;
            }
        } else if (in->last_used_ms != best->last_used_ms) {
            if (in->last_used_ms < best->last_used_ms) {
                best = in;
            }
        } else if (in->memory > best->memory) {
            best = in;
        }
    }
    return best;
}

static void tell_low_memory(purr_rt_t *rt, int level)
{
    purr_rt_event_t ev = {.type = PURR_RT_EV_LOW_MEMORY, .arg = level};
    for (int i = 0; i < PURR_RT_MAX_APPS; i++) {
        purr_rt_inst_t *in = &rt->inst[i];
        /* A suspended app has no runnable task to tell; it is the first to be stopped anyway. */
        if (in->id && (in->state == PURR_RT_FOREGROUND || in->state == PURR_RT_BACKGROUND)) {
            deliver(rt, in, &ev);
        }
    }
}

/* ------------------------------------------------------------ open and stop */

static void next_foreground(purr_rt_t *rt)
{
    purr_rt_inst_t *best = NULL;
    for (int i = 0; i < PURR_RT_MAX_APPS; i++) {
        purr_rt_inst_t *in = &rt->inst[i];
        if (in->id == 0 || in->app.kind == PURR_APP_KIND_DAEMON ||
            (in->state != PURR_RT_BACKGROUND && in->state != PURR_RT_SUSPENDED)) {
            continue;
        }
        if (best == NULL || in->last_used_ms > best->last_used_ms ||
            (in->last_used_ms == best->last_used_ms && in->id > best->id)) {
            best = in;
        }
    }
    if (best) {
        bring_to_front(rt, best);
    }
}

int purr_rt_stop(purr_rt_t *rt, int id, int reason)
{
    purr_rt_inst_t *in = purr_rt_get(rt, id);
    if (in == NULL) {
        return PURR_RT_E_NOAPP;
    }
    if (in->state == PURR_RT_STOPPING || in->state == PURR_RT_STOPPED) {
        return PURR_RT_E_STATE;
    }
    int was_fg = (rt->fg_id == id);
    /* State is saved on leaving the foreground, not at kill time, because a kill cannot rely on
     * running app code; but an orderly close of the foreground app is the last chance. */
    if (was_fg && (reason == PURR_APP_DESTROY_USER || reason == PURR_APP_DESTROY_UPDATE ||
                   reason == PURR_APP_DESTROY_SHUTDOWN)) {
        save_state_of(rt, in);
    }
    set_state(rt, in, PURR_RT_STOPPING);
    if (reason != PURR_APP_DESTROY_UNRESPONSIVE && reason != REASON_KILLED) {
        purr_rt_event_t ev = {.type = PURR_RT_EV_DESTROY, .arg = reason};
        deliver(rt, in, &ev);
    }
    if (rt->host.unload) {
        rt->host.unload(rt->host.ctx, in->app.handle);
    }
    char name[PURR_RT_NAME_LEN];
    copy_str(name, sizeof(name), in->name);
    emitf(rt, "closeapp", "%d,%s,%s", id, name, purr_rt_reason_name(reason));
    if (reason == PURR_APP_DESTROY_MEMORY || reason == PURR_APP_DESTROY_UNRESPONSIVE ||
        reason == REASON_KILLED) {
        emitf(rt, "killed", "%d,%s,%s", id, name, purr_rt_reason_name(reason));
    }
    memset(in, 0, sizeof(*in));
    if (was_fg) {
        rt->fg_id = 0;
        next_foreground(rt);
    }
    return 0;
}

int purr_rt_open(purr_rt_t *rt, const char *name)
{
    if (name == NULL || name[0] == '\0' || strlen(name) >= PURR_RT_NAME_LEN) {
        return PURR_RT_E_ARG;
    }
    purr_rt_app_t app;
    memset(&app, 0, sizeof(app));
    int lr = rt->host.load ? rt->host.load(rt->host.ctx, name, &app) : -1;
    if (lr == PURR_RT_HOST_UNTRUSTED) {
        /* An app that is not signed by a trusted key never runs, in any secure mode. */
        emitf(rt, "refused", "%s,untrusted", name);
        return PURR_RT_E_UNTRUSTED;
    }
    if (lr != 0) {
        return PURR_RT_E_LOAD;
    }

    /* Rules decide the flags and memory, so admission uses what will really be held. Build a
     * scratch instance for that, and only claim a slot once the app fits. */
    purr_rt_inst_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    copy_str(tmp.name, sizeof(tmp.name), name);
    tmp.app = app;
    tmp.flags = app.flags;
    tmp.memory = app.memory_needed;
    apply_rules(rt, &tmp);

    /* A free slot, and room under the configured cap. */
    uint32_t cap = rt->cfg.max_apps ? rt->cfg.max_apps : PURR_RT_MAX_APPS;
    while ((uint32_t)purr_rt_count(rt) >= cap) {
        purr_rt_inst_t *v = pick_victim(rt);
        if (v == NULL) {
            if (rt->host.unload) {
                rt->host.unload(rt->host.ctx, app.handle);
            }
            return PURR_RT_E_FULL;
        }
        purr_rt_stop(rt, v->id, PURR_APP_DESTROY_MEMORY);
    }

    /* Admission: its block plus the system reserve must fit; otherwise stop what may be stopped,
     * least important first, and try again. The foreground app is never the price. */
    while (free_bytes(rt) < tmp.memory + rt->cfg.reserve) {
        purr_rt_inst_t *v = pick_victim(rt);
        if (v == NULL) {
            if (rt->host.unload) {
                rt->host.unload(rt->host.ctx, app.handle);
            }
            return PURR_RT_E_MEMORY;
        }
        rt->kills++;
        rt->last_kill_ms = now_ms(rt);
        purr_rt_stop(rt, v->id, PURR_APP_DESTROY_MEMORY);
    }

    purr_rt_inst_t *in = NULL;
    for (int i = 0; i < PURR_RT_MAX_APPS; i++) {
        if (rt->inst[i].id == 0) {
            in = &rt->inst[i];
            break;
        }
    }
    if (in == NULL) {
        if (rt->host.unload) {
            rt->host.unload(rt->host.ctx, app.handle);
        }
        return PURR_RT_E_FULL;
    }
    *in = tmp;
    in->id = ++rt->next_id;
    in->state = PURR_RT_STARTING;
    in->started_ms = now_ms(rt);
    in->last_used_ms = in->started_ms;
    in->last_beat_ms = in->started_ms;

    /* Hand back the state saved from the last run, if the app keeps any. */
    const void *state = NULL;
    uint32_t state_len = 0;
    if ((in->flags & PURR_APP_F_SAVES_STATE) && rt->host.load_state) {
        int n = rt->host.load_state(rt->host.ctx, in->name, rt->state_buf, PURR_RT_MAX_STATE);
        if (n > 0) {
            state = rt->state_buf;
            state_len = (uint32_t)n;
        }
    }
    purr_rt_event_t ev = {.type = PURR_RT_EV_CREATE, .state = state, .state_len = state_len};
    int id = in->id;
    if (deliver(rt, in, &ev) < 0) {
        if (rt->host.unload) {
            rt->host.unload(rt->host.ctx, in->app.handle);
        }
        memset(in, 0, sizeof(*in));
        return PURR_RT_E_CREATE;
    }
    emitf(rt, "openapp", "%d,%s", id, in->name);

    int take_fg = !(in->flags & PURR_APP_F_NOFOCUS) &&
                  (in->app.kind != PURR_APP_KIND_DAEMON || (in->flags & PURR_APP_F_FOCUS));
    if (take_fg) {
        bring_to_front(rt, in);
    } else {
        set_state(rt, in, PURR_RT_BACKGROUND);
    }
    return id;
}

int purr_rt_focus(purr_rt_t *rt, int id)
{
    purr_rt_inst_t *in = purr_rt_get(rt, id);
    if (in == NULL) {
        return PURR_RT_E_NOAPP;
    }
    if (in->state == PURR_RT_STOPPING || in->state == PURR_RT_STOPPED) {
        return PURR_RT_E_STATE;
    }
    bring_to_front(rt, in);
    return 0;
}

int purr_rt_background(purr_rt_t *rt, int id)
{
    purr_rt_inst_t *in = purr_rt_get(rt, id);
    if (in == NULL) {
        return PURR_RT_E_NOAPP;
    }
    if (in->state != PURR_RT_FOREGROUND) {
        return PURR_RT_E_STATE;
    }
    leave_foreground(rt, in, 0);
    return 0;
}

int purr_rt_suspend(purr_rt_t *rt, int id)
{
    purr_rt_inst_t *in = purr_rt_get(rt, id);
    if (in == NULL) {
        return PURR_RT_E_NOAPP;
    }
    if (in->state == PURR_RT_FOREGROUND) {
        leave_foreground(rt, in, 1);
        return 0;
    }
    if (in->state == PURR_RT_BACKGROUND) {
        set_state(rt, in, PURR_RT_SUSPENDED);
        return 0;
    }
    return PURR_RT_E_STATE;
}

int purr_rt_resume(purr_rt_t *rt, int id)
{
    purr_rt_inst_t *in = purr_rt_get(rt, id);
    if (in == NULL) {
        return PURR_RT_E_NOAPP;
    }
    if (in->state != PURR_RT_SUSPENDED && in->state != PURR_RT_BACKGROUND) {
        return PURR_RT_E_STATE;
    }
    bring_to_front(rt, in);
    return 0;
}

int purr_rt_ref(purr_rt_t *rt, const char *ref)
{
    if (ref == NULL || ref[0] == '\0') {
        return 0;
    }
    if (ieq(ref, "active")) {
        return rt->fg_id;
    }
    if (ieq(ref, "next") || ieq(ref, "prev")) {
        int forward = ieq(ref, "next");
        int best = 0, wrap = 0;
        for (int i = 0; i < PURR_RT_MAX_APPS; i++) {
            int id = rt->inst[i].id;
            if (id == 0 || rt->inst[i].state == PURR_RT_STOPPING) {
                continue;
            }
            if (forward) {
                if (id > rt->fg_id && (best == 0 || id < best)) {
                    best = id;
                }
                if (wrap == 0 || id < wrap) {
                    wrap = id;
                }
            } else {
                if (id < rt->fg_id && id > best) {
                    best = id;
                }
                if (id > wrap) {
                    wrap = id;
                }
            }
        }
        return best ? best : wrap;
    }
    if (ref[0] >= '0' && ref[0] <= '9') {
        int n = 0;
        for (const char *p = ref; *p; p++) {
            if (*p < '0' || *p > '9') {
                return 0;
            }
            n = n * 10 + (*p - '0');
        }
        return purr_rt_get(rt, n) ? n : 0;
    }
    for (int i = 0; i < PURR_RT_MAX_APPS; i++) {
        if (rt->inst[i].id && strcmp(rt->inst[i].name, ref) == 0) {
            return rt->inst[i].id;
        }
    }
    return 0;
}

void purr_rt_heartbeat(purr_rt_t *rt, int id)
{
    purr_rt_inst_t *in = purr_rt_get(rt, id);
    if (in) {
        in->last_beat_ms = now_ms(rt);
        in->unresponsive = 0;
    }
}

int purr_rt_tick(purr_rt_t *rt)
{
    int stopped = 0;
    uint32_t now = now_ms(rt);
    uint32_t free = free_bytes(rt);

    /* Memory pressure, with hysteresis so it does not flap at a threshold. */
    uint8_t lvl = 0;
    if (rt->cfg.low && (free < rt->cfg.low || (rt->pressure >= 1 && free < rt->cfg.low + rt->cfg.margin))) {
        lvl = 1;
    }
    if (rt->cfg.critical &&
        (free < rt->cfg.critical || (rt->pressure == 2 && free < rt->cfg.critical + rt->cfg.margin))) {
        lvl = 2;
    }
    if (lvl > rt->pressure) {
        emitf(rt, "lowmemory", "%d", lvl);
        tell_low_memory(rt, lvl);
    }
    rt->pressure = lvl;

    if (lvl == 2) {
        while (free_bytes(rt) < rt->cfg.critical + rt->cfg.margin) {
            if (rt->kills > 0 && now - rt->last_kill_ms < rt->cfg.min_kill_ms) {
                break;                      /* the minimum interval between kills */
            }
            purr_rt_inst_t *v = pick_victim(rt);
            if (v == NULL) {
                break;                      /* only the foreground app (or system) is left */
            }
            rt->kills++;
            rt->last_kill_ms = now;
            purr_rt_stop(rt, v->id, PURR_APP_DESTROY_MEMORY);
            stopped++;
            if (rt->cfg.min_kill_ms > 0) {
                break;
            }
        }
    }

    /* An unresponsive foreground app is reported once; the user (or a rule) decides. */
    purr_rt_inst_t *fg = purr_rt_get(rt, rt->fg_id);
    if (fg && rt->cfg.unresponsive_ms && !fg->unresponsive &&
        now - fg->last_beat_ms > rt->cfg.unresponsive_ms) {
        fg->unresponsive = 1;
        emitf(rt, "unresponsive", "%d,%s", fg->id, fg->name);
    }
    return stopped;
}

/* ------------------------------------------------------------------ queries */

static void json_str(ob_t *o, const char *s)
{
    ob_printf(o, "\"");
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') {
            ob_printf(o, "\\%c", *s);
        } else if ((unsigned char)*s < 0x20) {
            ob_printf(o, "?");
        } else {
            ob_printf(o, "%c", *s);
        }
    }
    ob_printf(o, "\"");
}

static void flags_text(ob_t *o, uint8_t f)
{
    int any = 0;
    static const struct { uint8_t bit; const char *name; } names[] = {
        {PURR_APP_F_BACKGROUND_OK, "background-ok"}, {PURR_APP_F_SAVES_STATE, "saves-state"},
        {PURR_APP_F_PERSISTENT, "persistent"}, {PURR_APP_F_PRIVILEGED, "privileged"},
        {PURR_APP_F_SUSPEND, "suspend"}, {PURR_APP_F_NOFOCUS, "nofocus"},
        {PURR_APP_F_FOCUS, "focus"},
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (f & names[i].bit) {
            ob_printf(o, "%s%s", any ? " " : "", names[i].name);
            any = 1;
        }
    }
    if (!any) {
        ob_printf(o, "-");
    }
}

static void app_text(purr_rt_t *rt, ob_t *o, const purr_rt_inst_t *in)
{
    ob_printf(o, "app %d: %s\n", in->id, in->name);
    ob_printf(o, "\tstate: %s%s\n", purr_rt_state_name(in->state),
              in->unresponsive ? " (unresponsive)" : "");
    ob_printf(o, "\tkind: %s\n", purr_rt_kind_name(in->app.kind));
    ob_printf(o, "\tmemory: %u\n", (unsigned)in->memory);
    ob_printf(o, "\tflags: ");
    flags_text(o, in->flags);
    ob_printf(o, "%s\n", in->pinned ? " (pinned)" : "");
    ob_printf(o, "\tuptime: %u\n", (unsigned)(now_ms(rt) - in->started_ms));
}

static void app_json(purr_rt_t *rt, ob_t *o, const purr_rt_inst_t *in)
{
    ob_printf(o, "{\"id\":%d,\"name\":", in->id);
    json_str(o, in->name);
    ob_printf(o, ",\"state\":\"%s\",\"kind\":\"%s\",\"memory\":%u,\"flags\":%u,\"pinned\":%s,"
                 "\"unresponsive\":%s,\"uptime_ms\":%u}",
              purr_rt_state_name(in->state), purr_rt_kind_name(in->app.kind), (unsigned)in->memory,
              (unsigned)in->flags, in->pinned ? "true" : "false",
              in->unresponsive ? "true" : "false", (unsigned)(now_ms(rt) - in->started_ms));
}

static const char *effect_name(int e)
{
    switch (e) {
    case RULE_BACKGROUND: return "background";
    case RULE_PERSISTENT: return "persistent";
    case RULE_SUSPEND:    return "suspend";
    case RULE_MEMORY:     return "memory";
    case RULE_FOCUS:      return "focus";
    case RULE_NOFOCUS:    return "nofocus";
    }
    return "?";
}

int purr_rt_query(purr_rt_t *rt, const char *line, char *out, size_t cap)
{
    ob_t o;
    ob_init(&o, out, cap);
    char buf[PURR_RT_LINE_LEN];
    copy_str(buf, sizeof(buf), line ? line : "");
    char *cmd = trim(buf);
    int json = 0;
    char *sp = strchr(cmd, ' ');
    if (sp) {
        *sp = '\0';
        char *rest = trim(sp + 1);
        if (strcmp(rest, "-j") == 0) {
            json = 1;
        }
    }

    if (strcmp(cmd, "apps") == 0) {
        if (json) {
            ob_printf(&o, "[");
        }
        int n = 0;
        for (int i = 0; i < PURR_RT_MAX_APPS; i++) {
            if (rt->inst[i].id == 0) {
                continue;
            }
            if (json) {
                ob_printf(&o, "%s", n ? "," : "");
                app_json(rt, &o, &rt->inst[i]);
            } else {
                app_text(rt, &o, &rt->inst[i]);
            }
            n++;
        }
        if (json) {
            ob_printf(&o, "]");
        } else if (n == 0) {
            ob_printf(&o, "no apps running\n");
        }
        return 0;
    }
    if (strcmp(cmd, "activeapp") == 0) {
        purr_rt_inst_t *fg = purr_rt_get(rt, rt->fg_id);
        if (fg == NULL) {
            ob_printf(&o, json ? "null" : "none\n");
        } else if (json) {
            app_json(rt, &o, fg);
        } else {
            app_text(rt, &o, fg);
        }
        return 0;
    }
    if (strcmp(cmd, "memory") == 0) {
        uint32_t held = 0;
        for (int i = 0; i < PURR_RT_MAX_APPS; i++) {
            held += rt->inst[i].id ? rt->inst[i].memory : 0;
        }
        const char *lv[] = {"ok", "low", "critical"};
        if (json) {
            ob_printf(&o, "{\"free\":%u,\"held_by_apps\":%u,\"reserve\":%u,\"low\":%u,"
                          "\"critical\":%u,\"pressure\":\"%s\",\"oom_kills\":%d}",
                      (unsigned)free_bytes(rt), (unsigned)held, (unsigned)rt->cfg.reserve,
                      (unsigned)rt->cfg.low, (unsigned)rt->cfg.critical, lv[rt->pressure], rt->kills);
        } else {
            ob_printf(&o, "free: %u\nheld by apps: %u\nreserve: %u\nlow: %u\ncritical: %u\n"
                          "pressure: %s\noom kills: %d\n",
                      (unsigned)free_bytes(rt), (unsigned)held, (unsigned)rt->cfg.reserve,
                      (unsigned)rt->cfg.low, (unsigned)rt->cfg.critical, lv[rt->pressure], rt->kills);
        }
        return 0;
    }
    if (strcmp(cmd, "rules") == 0) {
        if (json) {
            ob_printf(&o, "[");
        }
        for (int i = 0; i < rt->nrules; i++) {
            const purr_rt_rule_t *r = &rt->rules[i];
            if (json) {
                ob_printf(&o, "%s{\"effect\":\"%s\",\"arg\":%u,\"name\":", i ? "," : "",
                          effect_name(r->effect), (unsigned)r->arg);
                json_str(&o, r->name);
                ob_printf(&o, ",\"kind\":%d}", r->kind);
            } else {
                ob_printf(&o, "rule %d: %s", i, effect_name(r->effect));
                if (r->effect == RULE_MEMORY) {
                    ob_printf(&o, " %u", (unsigned)r->arg);
                }
                if (r->name[0]) {
                    ob_printf(&o, ", name:%s", r->name);
                }
                if (r->kind >= 0) {
                    ob_printf(&o, ", class:%s", purr_rt_kind_name(r->kind));
                }
                ob_printf(&o, "\n");
            }
        }
        if (json) {
            ob_printf(&o, "]");
        }
        return 0;
    }
    if (strcmp(cmd, "binds") == 0) {
        for (int i = 0; i < rt->nbinds; i++) {
            const purr_rt_bind_t *b = &rt->binds[i];
            ob_printf(&o, "bind %d: %s%s%s%s%s -> %s\n", i, (b->mods & PURR_RT_MOD_SUPER) ? "SUPER " : "",
                      (b->mods & PURR_RT_MOD_CTRL) ? "CTRL " : "", (b->mods & PURR_RT_MOD_ALT) ? "ALT " : "",
                      (b->mods & PURR_RT_MOD_SHIFT) ? "SHIFT " : "", b->key, b->dispatch);
        }
        return 0;
    }
    if (strcmp(cmd, "autostart") == 0) {
        for (int i = 0; i < rt->nexec_boot; i++) {
            ob_printf(&o, "boot: %s\n", rt->exec_boot[i]);
        }
        for (int i = 0; i < rt->nexec; i++) {
            ob_printf(&o, "login: %s\n", rt->exec_once[i]);
        }
        return 0;
    }
    if (strcmp(cmd, "version") == 0) {
        ob_printf(&o, json ? "{\"runtime\":1,\"app_abi\":%u}" : "purr-runtime 1 (app abi %u)\n",
                  (unsigned)PURR_APP_ABI_VERSION_2);
        return 0;
    }
    ob_printf(&o, "unknown query: %s\n", cmd);
    return -1;
}

/* ---------------------------------------------------------------- dispatchers */

static const char *error_text(int e)
{
    switch (e) {
    case PURR_RT_E_LOAD:   return "could not load that app";
    case PURR_RT_E_FULL:   return "no room for another app";
    case PURR_RT_E_MEMORY: return "not enough memory";
    case PURR_RT_E_CREATE: return "the app failed to start";
    case PURR_RT_E_NOAPP:  return "no such app";
    case PURR_RT_E_STATE:  return "not allowed in the app's state";
    case PURR_RT_E_ARG:    return "bad argument";
    case PURR_RT_E_UNTRUSTED: return "refused: the app is not signed by a trusted key";
    }
    return "error";
}

static int reply(ob_t *o, int rc, const char *ok_fmt, const char *arg)
{
    if (rc < 0) {
        ob_printf(o, "error: %s\n", error_text(rc));
        return -1;
    }
    ob_printf(o, ok_fmt, arg);
    return 0;
}

int purr_rt_dispatch(purr_rt_t *rt, const char *line, char *out, size_t cap)
{
    ob_t o;
    ob_init(&o, out, cap);
    char buf[PURR_RT_LINE_LEN];
    copy_str(buf, sizeof(buf), line ? line : "");
    char *cmd = trim(buf);
    char *arg = NULL, *rest = NULL;
    char *sp = strchr(cmd, ' ');
    if (sp) {
        *sp = '\0';
        arg = trim(sp + 1);
        char *sp2 = strchr(arg, ' ');
        if (sp2) {
            *sp2 = '\0';
            rest = trim(sp2 + 1);
        }
    }
    if (cmd[0] == '\0') {
        ob_printf(&o, "error: empty dispatcher\n");
        return -1;
    }

    if (strcmp(cmd, "open") == 0 || strcmp(cmd, "exec") == 0) {
        if (arg == NULL || arg[0] == '\0') {
            ob_printf(&o, "error: open needs an app name\n");
            return -1;
        }
        int id = purr_rt_open(rt, arg);
        if (id < 0) {
            return reply(&o, id, "", "");
        }
        ob_printf(&o, "opened %s as %d\n", arg, id);
        return 0;
    }
    if (strcmp(cmd, "cycle") == 0) {
        int id = purr_rt_ref(rt, "next");
        if (id == 0) {
            ob_printf(&o, "error: no apps\n");
            return -1;
        }
        return reply(&o, purr_rt_focus(rt, id), "ok\n", "");
    }

    /* Everything else takes an app reference. */
    int id = arg ? purr_rt_ref(rt, arg) : 0;
    if (arg == NULL) {
        ob_printf(&o, "error: %s needs an app (id, name, active, next or prev)\n", cmd);
        return -1;
    }
    if (id == 0) {
        ob_printf(&o, "error: no such app: %s\n", arg);
        return -1;
    }
    if (strcmp(cmd, "close") == 0) {
        return reply(&o, purr_rt_stop(rt, id, PURR_APP_DESTROY_USER), "ok\n", "");
    }
    if (strcmp(cmd, "kill") == 0) {
        return reply(&o, purr_rt_stop(rt, id, REASON_KILLED), "ok\n", "");
    }
    if (strcmp(cmd, "focus") == 0 || strcmp(cmd, "foreground") == 0) {
        return reply(&o, purr_rt_focus(rt, id), "ok\n", "");
    }
    if (strcmp(cmd, "background") == 0) {
        return reply(&o, purr_rt_background(rt, id), "ok\n", "");
    }
    if (strcmp(cmd, "suspend") == 0) {
        return reply(&o, purr_rt_suspend(rt, id), "ok\n", "");
    }
    if (strcmp(cmd, "resume") == 0) {
        return reply(&o, purr_rt_resume(rt, id), "ok\n", "");
    }
    if (strcmp(cmd, "pin") == 0 || strcmp(cmd, "unpin") == 0) {
        purr_rt_inst_t *in = purr_rt_get(rt, id);
        in->pinned = (strcmp(cmd, "pin") == 0);
        ob_printf(&o, "ok\n");
        return 0;
    }
    if (strcmp(cmd, "post") == 0) {
        if (rest == NULL || rest[0] == '\0') {
            ob_printf(&o, "error: post needs <app> <event> [data]\n");
            return -1;
        }
        char *data = strchr(rest, ' ');
        if (data) {
            *data++ = '\0';
            data = trim(data);
        }
        return reply(&o, purr_rt_post(rt, id, rest, data), "ok\n", "");
    }
    ob_printf(&o, "error: unknown dispatcher: %s\n", cmd);
    return -1;
}

/* ---------------------------------------------------------------------- binds */

int purr_rt_key(purr_rt_t *rt, unsigned mods, const char *key)
{
    for (int i = 0; i < rt->nbinds; i++) {
        if (rt->binds[i].mods == (uint8_t)mods && ieq(rt->binds[i].key, key)) {
            char out[PURR_RT_LINE_LEN];
            purr_rt_dispatch(rt, rt->binds[i].dispatch, out, sizeof(out));
            return 1;
        }
    }
    return 0;
}

/* --------------------------------------------------------------------- config */

static int parse_uint(const char *s, uint32_t *out)
{
    if (*s == '\0') {
        return 0;
    }
    uint32_t v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') {
            return 0;
        }
        v = v * 10 + (uint32_t)(*s - '0');
    }
    *out = v;
    return 1;
}

static int kind_from_name(const char *s)
{
    if (ieq(s, "daemon")) {
        return PURR_APP_KIND_DAEMON;
    }
    if (ieq(s, "command-line")) {
        return PURR_APP_KIND_COMMAND_LINE;
    }
    if (ieq(s, "ui")) {
        return PURR_APP_KIND_UI;
    }
    return -1;
}

static unsigned parse_mods(char *s, int *ok)
{
    unsigned m = 0;
    *ok = 1;
    for (char *tok = strtok(s, " +_"); tok; tok = strtok(NULL, " +_")) {
        if (ieq(tok, "SUPER")) {
            m |= PURR_RT_MOD_SUPER;
        } else if (ieq(tok, "CTRL")) {
            m |= PURR_RT_MOD_CTRL;
        } else if (ieq(tok, "ALT")) {
            m |= PURR_RT_MOD_ALT;
        } else if (ieq(tok, "SHIFT")) {
            m |= PURR_RT_MOD_SHIFT;
        } else if (!ieq(tok, "NONE")) {
            *ok = 0;
        }
    }
    return m;
}

static void config_error(char *err, size_t errcap, size_t *used, int lineno, const char *msg)
{
    if (err == NULL || errcap == 0) {
        return;
    }
    int n = snprintf(err + *used, errcap - *used, "line %d: %s\n", lineno, msg);
    if (n > 0) {
        *used += ((size_t)n < errcap - *used) ? (size_t)n : errcap - *used - 1;
    }
}

int purr_rt_config_load(purr_rt_t *rt, const char *text, char *err, size_t errcap)
{
    rt->nrules = 0;
    rt->nbinds = 0;
    rt->nexec = 0;
    rt->nexec_boot = 0;
    rt->cfg = rt->cfg_defaults;               /* options the file omits revert, as in Hyprland */
    if (err && errcap) {
        err[0] = '\0';
    }
    size_t used = 0;
    int bad = 0, lineno = 0;
    const char *p = text ? text : "";
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char line[PURR_RT_LINE_LEN * 2];
        if (len >= sizeof(line)) {
            len = sizeof(line) - 1;
        }
        memcpy(line, p, len);
        line[len] = '\0';
        p += eol ? (size_t)(eol - p) + 1 : strlen(p);
        lineno++;

        char *hash = strchr(line, '#');
        if (hash) {
            *hash = '\0';
        }
        char *s = trim(line);
        if (*s == '\0') {
            continue;
        }
        char *eq = strchr(s, '=');
        if (eq == NULL) {
            config_error(err, errcap, &used, lineno, "expected keyword = value");
            bad++;
            continue;
        }
        *eq = '\0';
        char *key = trim(s);
        char *val = trim(eq + 1);

        if (strncmp(key, "general:", 8) == 0) {
            const char *opt = key + 8;
            uint32_t v;
            if (!parse_uint(val, &v)) {
                config_error(err, errcap, &used, lineno, "a number was expected");
                bad++;
            } else if (strcmp(opt, "max_apps") == 0) {
                rt->cfg.max_apps = v > PURR_RT_MAX_APPS ? PURR_RT_MAX_APPS : v;
            } else if (strcmp(opt, "reserve_kb") == 0) {
                rt->cfg.reserve = v * 1024u;
            } else if (strcmp(opt, "low_kb") == 0) {
                rt->cfg.low = v * 1024u;
            } else if (strcmp(opt, "critical_kb") == 0) {
                rt->cfg.critical = v * 1024u;
            } else if (strcmp(opt, "margin_kb") == 0) {
                rt->cfg.margin = v * 1024u;
            } else if (strcmp(opt, "min_kill_ms") == 0) {
                rt->cfg.min_kill_ms = v;
            } else if (strcmp(opt, "unresponsive_ms") == 0) {
                rt->cfg.unresponsive_ms = v;
            } else {
                config_error(err, errcap, &used, lineno, "unknown general option");
                bad++;
            }
        } else if (strcmp(key, "exec-once") == 0) {
            if (rt->nexec >= PURR_RT_MAX_EXEC || *val == '\0') {
                config_error(err, errcap, &used, lineno, "exec-once: too many, or empty");
                bad++;
            } else {
                copy_str(rt->exec_once[rt->nexec++], PURR_RT_NAME_LEN, val);
            }
        } else if (strcmp(key, "exec-boot") == 0) {
            if (rt->nexec_boot >= PURR_RT_MAX_EXEC || *val == '\0') {
                config_error(err, errcap, &used, lineno, "exec-boot: too many, or empty");
                bad++;
            } else {
                copy_str(rt->exec_boot[rt->nexec_boot++], PURR_RT_NAME_LEN, val);
            }
        } else if (strcmp(key, "rule") == 0) {
            char *comma = strchr(val, ',');
            if (comma == NULL) {
                config_error(err, errcap, &used, lineno, "rule = effect, matcher");
                bad++;
                continue;
            }
            *comma = '\0';
            char *eff = trim(val);
            char *match = trim(comma + 1);
            purr_rt_rule_t r;
            memset(&r, 0, sizeof(r));
            r.kind = -1;
            char *arg = strchr(eff, ' ');
            if (arg) {
                *arg++ = '\0';
                arg = trim(arg);
            }
            if (strcmp(eff, "background") == 0) {
                r.effect = RULE_BACKGROUND;
            } else if (strcmp(eff, "persistent") == 0) {
                r.effect = RULE_PERSISTENT;
            } else if (strcmp(eff, "suspend") == 0) {
                r.effect = RULE_SUSPEND;
            } else if (strcmp(eff, "focus") == 0) {
                r.effect = RULE_FOCUS;
            } else if (strcmp(eff, "nofocus") == 0) {
                r.effect = RULE_NOFOCUS;
            } else if (strcmp(eff, "memory") == 0) {
                uint32_t kb;
                if (arg == NULL || !parse_uint(arg, &kb)) {
                    config_error(err, errcap, &used, lineno, "memory needs a size in KB");
                    bad++;
                    continue;
                }
                r.effect = RULE_MEMORY;
                r.arg = kb * 1024u;
            } else {
                config_error(err, errcap, &used, lineno, "unknown rule effect");
                bad++;
                continue;
            }
            if (strncmp(match, "name:", 5) == 0) {
                copy_str(r.name, sizeof(r.name), match + 5);
            } else if (strncmp(match, "class:", 6) == 0) {
                r.kind = (int8_t)kind_from_name(match + 6);
                if (r.kind < 0) {
                    config_error(err, errcap, &used, lineno, "unknown class");
                    bad++;
                    continue;
                }
            } else {
                config_error(err, errcap, &used, lineno, "matcher must be name: or class:");
                bad++;
                continue;
            }
            if (rt->nrules >= PURR_RT_MAX_RULES) {
                config_error(err, errcap, &used, lineno, "too many rules");
                bad++;
                continue;
            }
            rt->rules[rt->nrules++] = r;
        } else if (strcmp(key, "bind") == 0) {
            char *c1 = strchr(val, ',');
            char *c2 = c1 ? strchr(c1 + 1, ',') : NULL;
            if (c1 == NULL || c2 == NULL) {
                config_error(err, errcap, &used, lineno, "bind = MODS, key, dispatcher[, args]");
                bad++;
                continue;
            }
            *c1 = '\0';
            *c2 = '\0';
            char *mods = trim(val);
            char *kname = trim(c1 + 1);
            char *disp = trim(c2 + 1);
            /* Hyprland writes `dispatcher, params`; our own form is `dispatcher params`. */
            {
                char *w = disp;
                int space = 0;
                for (char *r = disp; *r; r++) {
                    if (*r == ',' || *r == ' ') {
                        space = 1;
                    } else {
                        if (space && w != disp) {
                            *w++ = ' ';
                        }
                        space = 0;
                        *w++ = *r;
                    }
                }
                *w = '\0';
            }
            int ok;
            unsigned m = parse_mods(mods, &ok);
            if (!ok || *kname == '\0' || *disp == '\0') {
                config_error(err, errcap, &used, lineno, "bad modifiers, key or dispatcher");
                bad++;
                continue;
            }
            if (rt->nbinds >= PURR_RT_MAX_BINDS) {
                config_error(err, errcap, &used, lineno, "too many binds");
                bad++;
                continue;
            }
            purr_rt_bind_t *b = &rt->binds[rt->nbinds++];
            b->mods = (uint8_t)m;
            copy_str(b->key, sizeof(b->key), kname);
            copy_str(b->dispatch, sizeof(b->dispatch), disp);
        } else {
            config_error(err, errcap, &used, lineno, "unknown keyword");
            bad++;
        }
    }
    emitf(rt, "config", "reloaded");
    return bad;
}

int purr_rt_exec_boot(purr_rt_t *rt)
{
    int opened = 0;
    for (int i = 0; i < rt->nexec_boot; i++) {
        if (purr_rt_open(rt, rt->exec_boot[i]) > 0) {
            opened++;
        } else {
            log_line(rt, "exec-boot: could not open %s", rt->exec_boot[i]);
        }
    }
    return opened;
}

int purr_rt_exec_once(purr_rt_t *rt)
{
    int opened = 0;
    for (int i = 0; i < rt->nexec; i++) {
        if (purr_rt_open(rt, rt->exec_once[i]) > 0) {
            opened++;
        } else {
            log_line(rt, "exec-once: could not open %s", rt->exec_once[i]);
        }
    }
    return opened;
}
