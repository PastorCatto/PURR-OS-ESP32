/*
 * app_common.h - one small v2 app, built several ways for testing the app runtime (plan6.md).
 * Each of clock.c, notes.c and daemon.c sets APP_NAME, APP_KIND, APP_FLAGS and APP_MEMORY and
 * then includes this. It uses the catcalls a real app would: console, app, sched, event.
 *
 * It prints what the runtime does to it (create, resume, pause, save, low memory, event, destroy)
 * so a log of a run shows the whole lifecycle, in order.
 */
#include <stddef.h>
#include <stdint.h>

#include "purr_app_abi.h"
#include "purr_catcalls.h"

#ifndef APP_NAME
#error "define APP_NAME before including app_common.h"
#endif

/* No libc in an app yet (AppSDK/SPEC.md section 2, the mini-library, is still to be built). */
__attribute__((noinline, optimize("no-tree-loop-distribute-patterns")))
static void copy_bytes(void *dst, const void *src, uint32_t n)
{
    volatile uint8_t *d = (volatile uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) {
        *d++ = *s++;
    }
}

static uint32_t str_len(const char *s)
{
    uint32_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

static const purr_cc_console_t *s_con;
static const purr_cc_app_t *s_app;
static const purr_cc_sched_t *s_sched;
static const purr_cc_event_t *s_evt;
static uint32_t s_runs;          /* the saved state: how many times this app has run */
static int s_timer;

static void say(const char *s)
{
    if (s_con != NULL && s_con->print != NULL) {
        s_con->print(APP_NAME ": ");
        s_con->print(s);
        s_con->print("\n");
    }
}

static void say_num(const char *label, unsigned n)
{
    char buf[48];
    uint32_t l = str_len(label);
    copy_bytes(buf, label, l);
    int i = (int)l;
    char digits[12];
    int d = 0;
    if (n == 0) {
        digits[d++] = '0';
    }
    while (n > 0 && d < 11) {
        digits[d++] = (char)('0' + n % 10);
        n /= 10;
    }
    while (d > 0) {
        buf[i++] = digits[--d];
    }
    buf[i] = '\0';
    say(buf);
}

static int on_create(const purr_app_env_t *env, const void *state, uint32_t len)
{
    if (env == NULL || env->catcall_get == NULL) {
        return -1;
    }
    s_con = (const purr_cc_console_t *)env->catcall_get("console", 1);
    s_app = (const purr_cc_app_t *)env->catcall_get("app", 1);
    s_sched = (const purr_cc_sched_t *)env->catcall_get("sched", 1);
    s_evt = (const purr_cc_event_t *)env->catcall_get("event", 1);
    s_runs = 0;
    if (state != NULL && len >= sizeof(s_runs)) {
        copy_bytes(&s_runs, state, sizeof(s_runs));
    }
    s_runs++;
    say_num("create, run number ", s_runs);
    if (s_evt != NULL && s_evt->subscribe != NULL) {
        s_evt->subscribe("custom.");
    }
    if (s_sched != NULL && s_sched->timer_start != NULL) {
        s_timer = s_sched->timer_start(500, 1);
    }
    return 0;
}

static void on_resume(void) { say("resume"); }
static void on_pause(void) { say("pause"); }

static uint32_t on_save_state(void *buf, uint32_t cap)
{
    if (cap < sizeof(s_runs)) {
        return 0;
    }
    copy_bytes(buf, &s_runs, sizeof(s_runs));
    say("save state");
    return (uint32_t)sizeof(s_runs);
}

static void on_low_memory(int level) { say_num("low memory, level ", (unsigned)level); }

static void on_event(const purr_app_event_t *ev)
{
    if (ev->type == PURR_APPEV_TIMER) {
        static int ticks;
        if (++ticks == 2) {                       /* once is enough to see timers work */
            say("timer fired");
            if (s_sched != NULL && s_sched->timer_cancel != NULL) {
                s_sched->timer_cancel(s_timer);
            }
        }
        return;
    }
    say("event");
    if (ev->name != NULL) {
        say(ev->name);
    }
    if (ev->data != NULL) {
        say(ev->data);
    }
}

static void on_destroy(int reason) { say_num("destroy, reason ", (unsigned)reason); }

static const purr_app_table2_t s_table = {
    .base = {.abi_version = PURR_APP_ABI_VERSION_2, .on_create = on_create,
             .on_destroy = on_destroy},
    .on_resume = on_resume, .on_pause = on_pause, .on_save_state = on_save_state,
    .on_low_memory = on_low_memory, .on_event = on_event,
    .memory_needed = APP_MEMORY, .kind = APP_KIND, .flags = APP_FLAGS,
};

__attribute__((used))
const purr_app_table_t *APP_ENTRY(void)
{
    return &s_table.base;
}
