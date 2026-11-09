/*
 * storesvc.c - the Store's companion, a background daemon with no screen. No libc.
 *
 * It does the Store's work on a schedule: a first look for updates about a minute and a half after it starts, then every `store.check_hours` hours (12 by
 * default). A check refreshes every trusted repository's signed index and, if something newer is offered, posts a notification. If the owner switched on
 * `store.auto_update`, it then installs the updates itself (apps, and assets and extras on flash or an SD card). That includes the Store app and this
 * companion: they are items in the official repository like any other, and an update takes effect the next time they start.
 *
 * A background app cannot be asked for a permission, so "install-apps" is granted to this app when the Store is set up (and Settings > Apps shows it).
 * The work itself is done by the OS (the `store` catcall); this app only decides when. When the OS grows a service runtime it becomes the second entry of the
 * Store's package ([service]), with a wake window on an alarm in place of these timers.
 */
#include <stddef.h>
#include <stdint.h>

#include "purr_app_abi.h"
#include "purr_catcalls.h"

static const purr_cc_sched_t *sc;
static const purr_cc_store_t *st;
static const purr_cc_console_t *con;

static int s_timer = -1;
static int s_phase;                         /* 0 first check pending, 1 steady */
static int s_after_check;                   /* the job that is running is a check: act on its answer */
static int s_denied;

#define FIRST_CHECK_MS 90000u

static int same(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static void say(const char *t)
{
    if (con != NULL) {
        con->print(t);
    }
}

static void arm(uint32_t ms)
{
    if (s_timer > 0) {
        sc->timer_cancel(s_timer);
    }
    s_timer = sc->timer_start(ms, 0);
}

static uint32_t period_ms(void)
{
    int h = st->setting("store.check_hours", 12);
    if (h < 1) {
        h = 1;
    }
    if (h > 168) {
        h = 168;
    }
    return (uint32_t)h * 3600000u;
}

static void check_now(void)
{
    int rc = st->start("check", "", "");
    if (rc == PURR_STORE_OK) {
        s_after_check = 1;
    } else if (rc == PURR_STORE_DENIED || rc == PURR_STORE_ASK) {
        s_denied = 1;                                       /* not granted: do not keep trying */
        say("storesvc: not allowed to install apps (Settings > Apps)\n");
    }
}

static int on_create(const purr_app_env_t *env, const void *state, uint32_t len)
{
    (void)state;
    (void)len;
    if (env == NULL || env->catcall_get == NULL) {
        return -1;
    }
    con = (const purr_cc_console_t *)env->catcall_get("console", 1);
    sc = (const purr_cc_sched_t *)env->catcall_get("sched", 1);
    st = (const purr_cc_store_t *)env->catcall_get("store", 1);
    if (sc == NULL || st == NULL) {
        say("storesvc: needs the sched and store catcalls\n");
        return -1;
    }
    arm(FIRST_CHECK_MS);
    return 0;
}

static void on_event(const purr_app_event_t *ev)
{
    if (st == NULL || s_denied) {
        return;
    }
    if (ev->type == PURR_APPEV_TIMER && (int)ev->arg == s_timer) {
        s_timer = -1;
        if (s_phase == 0) {
            s_phase = 1;
        }
        check_now();
        arm(period_ms());
    } else if (ev->type == PURR_APPEV_UI && ev->name != NULL && same(ev->name, "job")) {
        if (s_after_check) {
            s_after_check = 0;
            if (ev->arg == 0 && st->setting("store.auto_update", 0) && st->updates() > 0) {
                st->start("update-all", "", "");
            }
        }
    }
}

static void on_destroy(int reason)
{
    (void)reason;
}

static const purr_app_table2_t s_table = {
    .base = {.abi_version = PURR_APP_ABI_VERSION_2, .on_create = on_create, .on_destroy = on_destroy},
    .on_event = on_event,
    .memory_needed = 4096, .kind = 1 /* daemon */, .flags = 0x05 /* background, persistent */,
};

__attribute__((used))
const purr_app_table_t *storesvc_entry(void)
{
    return &s_table.base;
}
