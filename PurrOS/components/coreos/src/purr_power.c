/* purr_power.c - see purr_power.h and plan10.md. Plain C, no ESP-IDF. */
#include "purr_power.h"

#include <stdio.h>
#include <string.h>

/* Wrap-safe: true once now has reached t. Times are uint32 milliseconds that wrap after ~49 days. */
static int reached(uint32_t now, uint32_t t) { return (int32_t)(now - t) >= 0; }

static uint32_t now(const purr_power_t *p) { return p->plat->now_ms(); }

static purr_power_owner_t *own(purr_power_t *p, int owner)
{
    if (owner < 0 || owner >= PURR_POWER_MAX_OWNERS) {
        return NULL;
    }
    p->owner[owner].used = 1;
    return &p->owner[owner];
}

static int any_window(const purr_power_t *p)
{
    for (int o = 0; o < PURR_POWER_MAX_OWNERS; o++) {
        for (int a = 0; a < PURR_POWER_MAX_ALARMS; a++) {
            if (p->owner[o].alarm[a].active && p->owner[o].alarm[a].in_window) {
                return 1;
            }
        }
    }
    return 0;
}

static int any_lock(const purr_power_t *p, int kind)
{
    for (int o = 0; o < PURR_POWER_MAX_OWNERS; o++) {
        if (kind == PURR_WAKE_SCREEN ? p->owner[o].screen_lock : p->owner[o].cpu_lock) {
            return 1;
        }
    }
    return 0;
}

static void set_screen(purr_power_t *p, int on)
{
    if (p->screen_on != on) {
        p->screen_on = on;
        if (p->plat->screen) {
            p->plat->screen(on);
        }
    }
    if (on) {
        p->last_activity = now(p);
    }
}

void purr_power_init(purr_power_t *p, const purr_power_plat_t *plat)
{
    memset(p, 0, sizeof(*p));
    p->plat = plat;
    p->screen_on = 1;
    p->cpu_sleep_ok = -1;
    p->timeout_ms = 0;
    p->last_activity = now(p);
}

void purr_power_set_timeout(purr_power_t *p, uint32_t ms)
{
    p->timeout_ms = ms;
    p->last_activity = now(p);
}

int purr_power_activity(purr_power_t *p)
{
    int was_off = !p->screen_on;
    set_screen(p, 1);
    p->last_activity = now(p);
    return was_off;
}

void purr_power_screen_request(purr_power_t *p, int on)
{
    set_screen(p, on != 0);
}

int purr_power_hold(purr_power_t *p, int owner, int kind, uint32_t lease_ms)
{
    purr_power_owner_t *o = own(p, owner);
    if (o == NULL) {
        return -1;
    }
    if (kind == PURR_WAKE_SCREEN) {
        o->screen_lock = 1;
        set_screen(p, 1);
        return 0;
    }
    if (kind == PURR_WAKE_CPU) {
        if (lease_ms == 0) {
            lease_ms = PURR_POWER_LEASE_DEFAULT_MS;
        }
        if (lease_ms > PURR_POWER_LEASE_MAX_MS) {
            lease_ms = PURR_POWER_LEASE_MAX_MS;
        }
        o->cpu_lock = 1;
        o->cpu_until = now(p) + lease_ms;
        return 0;
    }
    return -1;
}

int purr_power_renew(purr_power_t *p, int owner, int kind, uint32_t lease_ms)
{
    purr_power_owner_t *o = own(p, owner);
    if (o == NULL || kind != PURR_WAKE_CPU || !o->cpu_lock) {
        return -1;
    }
    return purr_power_hold(p, owner, kind, lease_ms);
}

void purr_power_release(purr_power_t *p, int owner, int kind)
{
    purr_power_owner_t *o = own(p, owner);
    if (o == NULL) {
        return;
    }
    if (kind == PURR_WAKE_SCREEN) {
        o->screen_lock = 0;
        p->last_activity = now(p);                  /* the timeout counts from the release, not from the last tap */
    } else if (kind == PURR_WAKE_CPU) {
        o->cpu_lock = 0;
    }
}

int purr_power_alarm_set(purr_power_t *p, int owner, int idx, uint32_t interval_ms, uint32_t window_ms, int flags)
{
    purr_power_owner_t *o = own(p, owner);
    if (o == NULL || interval_ms < PURR_POWER_MIN_INTERVAL_MS) {
        return -1;
    }
    if (idx < 0) {
        for (int i = 0; i < PURR_POWER_MAX_ALARMS; i++) {
            if (!o->alarm[i].active) {
                idx = i;
                break;
            }
        }
        if (idx < 0) {
            return -2;
        }
    } else if (idx >= PURR_POWER_MAX_ALARMS) {
        return -1;
    }
    if (window_ms == 0) {
        window_ms = PURR_POWER_WINDOW_DEFAULT_MS;
    }
    if (window_ms > PURR_POWER_WINDOW_MAX_MS) {
        window_ms = PURR_POWER_WINDOW_MAX_MS;
    }
    purr_alarm_t *a = &o->alarm[idx];
    memset(a, 0, sizeof(*a));
    a->interval = interval_ms;
    a->window_ms = window_ms;
    a->flags = (uint8_t)flags;
    a->active = 1;
    a->next_due = now(p) + interval_ms;
    return idx;
}

void purr_power_alarm_cancel(purr_power_t *p, int owner, int idx)
{
    purr_power_owner_t *o = own(p, owner);
    if (o != NULL && idx >= 0 && idx < PURR_POWER_MAX_ALARMS) {
        memset(&o->alarm[idx], 0, sizeof(o->alarm[idx]));
    }
}

void purr_power_done(purr_power_t *p, int owner)
{
    purr_power_owner_t *o = own(p, owner);
    if (o == NULL) {
        return;
    }
    for (int i = 0; i < PURR_POWER_MAX_ALARMS; i++) {
        if (o->alarm[i].active && o->alarm[i].in_window) {
            o->alarm[i].in_window = 0;
            o->alarm[i].overruns = 0;               /* it finished in time: the streak is broken */
        }
    }
}

void purr_power_release_owner(purr_power_t *p, int owner)
{
    if (owner >= 0 && owner < PURR_POWER_MAX_OWNERS) {
        memset(&p->owner[owner], 0, sizeof(p->owner[owner]));
        p->last_activity = now(p);
    }
}

static void fire(purr_power_t *p, int owner, int idx, uint32_t t)
{
    purr_alarm_t *a = &p->owner[owner].alarm[idx];
    a->next_due = t + a->interval;
    a->in_window = 1;
    a->window_end = t + a->window_ms;
    p->wakes++;
    if (p->plat->wake) {
        p->plat->wake(owner, idx, "alarm");
    }
}

void purr_power_tick(purr_power_t *p)
{
    uint32_t t = now(p);

    /* Leases and windows first, so a wake that was due in the same instant starts clean. */
    for (int o = 0; o < PURR_POWER_MAX_OWNERS; o++) {
        purr_power_owner_t *ow = &p->owner[o];
        if (ow->cpu_lock && reached(t, ow->cpu_until)) {
            ow->cpu_lock = 0;
            p->expired++;
            if (p->plat->notify) {
                p->plat->notify(o, "lease-expired");
            }
        }
        for (int a = 0; a < PURR_POWER_MAX_ALARMS; a++) {
            purr_alarm_t *al = &ow->alarm[a];
            if (al->active && al->in_window && reached(t, al->window_end)) {
                al->in_window = 0;
                p->overruns++;
                if (++al->overruns >= PURR_POWER_MAX_OVERRUNS) {
                    memset(al, 0, sizeof(*al));
                    if (p->plat->notify) {
                        p->plat->notify(o, "alarm-off");
                    }
                }
            }
        }
    }

    /* Due alarms. One due alarm brings along every ALIGN alarm that is due within a fifth of its own interval. */
    int due_any = 0;
    for (int o = 0; o < PURR_POWER_MAX_OWNERS && !due_any; o++) {
        for (int a = 0; a < PURR_POWER_MAX_ALARMS; a++) {
            if (p->owner[o].alarm[a].active && reached(t, p->owner[o].alarm[a].next_due)) {
                due_any = 1;
                break;
            }
        }
    }
    if (due_any) {
        for (int o = 0; o < PURR_POWER_MAX_OWNERS; o++) {
            for (int a = 0; a < PURR_POWER_MAX_ALARMS; a++) {
                purr_alarm_t *al = &p->owner[o].alarm[a];
                if (!al->active) {
                    continue;
                }
                int early = (al->flags & PURR_ALARM_ALIGN) &&
                            reached(t + al->interval / 5, al->next_due);
                if (reached(t, al->next_due) || early) {
                    fire(p, o, a, t);
                }
            }
        }
    }

    /* The idle timeout. A screen lock, or a window that is open, keeps the screen as it is. */
    if (p->screen_on && p->timeout_ms != 0 && !any_lock(p, PURR_WAKE_SCREEN) &&
        reached(t, p->last_activity + p->timeout_ms)) {
        set_screen(p, 0);
    }

    int ok = !p->screen_on && !any_lock(p, PURR_WAKE_CPU) && !any_window(p);
    if (ok != p->cpu_sleep_ok) {
        p->cpu_sleep_ok = ok;
        if (p->plat->cpu_sleep_allowed) {
            p->plat->cpu_sleep_allowed(ok);
        }
    }
}

int purr_power_screen_is_on(const purr_power_t *p) { return p->screen_on; }

int purr_power_cpu_may_sleep(const purr_power_t *p)
{
    return !p->screen_on && !any_lock(p, PURR_WAKE_CPU) && !any_window(p);
}

uint32_t purr_power_next_alarm_in(const purr_power_t *p)
{
    uint32_t t = now(p), best = UINT32_MAX;
    for (int o = 0; o < PURR_POWER_MAX_OWNERS; o++) {
        for (int a = 0; a < PURR_POWER_MAX_ALARMS; a++) {
            const purr_alarm_t *al = &p->owner[o].alarm[a];
            if (al->active) {
                uint32_t in = reached(t, al->next_due) ? 0 : al->next_due - t;
                if (in < best) {
                    best = in;
                }
            }
        }
    }
    return best;
}

int purr_power_status(const purr_power_t *p, char *out, size_t cap)
{
    if (cap == 0) {
        return 0;
    }
    uint32_t t = now(p);
    int n = snprintf(out, cap, "screen: %s   timeout: ", p->screen_on ? "on" : "off");
    if (p->timeout_ms) {
        n += snprintf(out + n, cap - (size_t)n, "%u s\n", (unsigned)(p->timeout_ms / 1000));
    } else {
        n += snprintf(out + n, cap - (size_t)n, "never\n");
    }
    n += snprintf(out + n, cap - (size_t)n, "cpu may sleep: %s   wakes %u, overruns %u, leases expired %u\n",
                  purr_power_cpu_may_sleep(p) ? "yes" : "no", (unsigned)p->wakes, (unsigned)p->overruns,
                  (unsigned)p->expired);
    for (int o = 0; o < PURR_POWER_MAX_OWNERS && (size_t)n < cap; o++) {
        const purr_power_owner_t *ow = &p->owner[o];
        if (ow->screen_lock) {
            n += snprintf(out + n, cap - (size_t)n, "  app %d holds the screen\n", o);
        }
        if (ow->cpu_lock) {
            n += snprintf(out + n, cap - (size_t)n, "  app %d holds the cpu (%u ms left)\n", o,
                          reached(t, ow->cpu_until) ? 0u : (unsigned)(ow->cpu_until - t));
        }
        for (int a = 0; a < PURR_POWER_MAX_ALARMS && (size_t)n < cap; a++) {
            const purr_alarm_t *al = &ow->alarm[a];
            if (al->active) {
                n += snprintf(out + n, cap - (size_t)n, "  app %d alarm %d: every %u s, next in %u s%s%s\n", o, a,
                              (unsigned)(al->interval / 1000),
                              reached(t, al->next_due) ? 0u : (unsigned)((al->next_due - t) / 1000),
                              al->flags & PURR_ALARM_ALIGN ? ", aligned" : "", al->in_window ? ", awake now" : "");
            }
        }
    }
    return n < (int)cap ? n : (int)cap - 1;
}
