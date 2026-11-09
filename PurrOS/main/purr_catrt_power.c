/* purr_catrt_power.c - see purr_catrt_power.h and plan10.md. */
#include "purr_catrt_power.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "commands.h"
#include "login.h"
#include "purr_app_abi.h"
#include "purr_catcalls.h"
#include "purr_catrt.h"
#include "purr_cli.h"
#include "purr_fs.h"
#include "purr_perm.h"
#include "purr_power.h"
#include "purr_power_plat.h"

static const char *TAG = "power-cc";

_Static_assert(PURR_CC_WAKE_SCREEN == PURR_WAKE_SCREEN && PURR_CC_WAKE_CPU == PURR_WAKE_CPU &&
               PURR_CC_ALARM_ALIGN == PURR_ALARM_ALIGN, "the app-facing constants must equal the policy's");

#define MAX_PENDING 4

typedef struct {
    char app[32];
    int perm;
} pending_t;

static purr_perm_t s_perm;
static char s_user[32];                         /* whose grants s_perm holds ("" = nobody yet) */
static pending_t s_pend[MAX_PENDING];
static int s_npend;                             /* the head is the prompt on screen */
static SemaphoreHandle_t s_mu;
static purr_perm_show_fn s_show;

static void lock(void) { xSemaphoreTakeRecursive(s_mu, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGiveRecursive(s_mu); }

/* ------------------------------------------------------------------ the grants file */

typedef struct {
    char *buf;
    size_t cap, len;
} acc_t;

static int accumulate(void *vctx, const void *data, uint32_t n)
{
    acc_t *a = vctx;
    if (a->len + n >= a->cap) {
        return -1;
    }
    memcpy(a->buf + a->len, data, n);
    a->len += n;
    a->buf[a->len] = '\0';
    return 0;
}

static int path_for(char *p, size_t cap)
{
    const char *u = purr_login_current()->name;
    if (u[0] == '\0') {
        return -1;
    }
    snprintf(p, cap, "/home/%.24s/permissions.conf", u);
    return 0;
}

/* The table follows the signed-in user: it is re-read when the user changes. Fails closed: with nobody signed in,
 * or an unreadable file, the table is empty and everything dangerous asks (or is refused, for a background app). */
static void ensure_loaded(void)
{
    const char *u = purr_login_current()->name;
    if (strcmp(u, s_user) == 0) {
        return;
    }
    memset(&s_perm, 0, sizeof(s_perm));
    snprintf(s_user, sizeof(s_user), "%s", u);
    char path[64], text[1024];
    acc_t a = {text, sizeof(text), 0};
    text[0] = '\0';
    if (path_for(path, sizeof(path)) == 0 && purr_fs_read(purr_login_fs(), path, accumulate, &a) >= 0) {
        purr_perm_parse(&s_perm, text);
    }
}

static void save(void)
{
    char path[64], text[1024];
    if (path_for(path, sizeof(path)) != 0) {
        return;
    }
    size_t n = purr_perm_format(&s_perm, text, sizeof(text));
    if (purr_fs_write(purr_login_fs(), path, text, (uint32_t)n) < 0) {
        ESP_LOGW(TAG, "could not save %s", path);
    }
}

/* ------------------------------------------------------------------ asking */

/* May the calling app do this? 0 yes; PURR_POWER_DENIED; PURR_POWER_ASK (a prompt is up or queued). */
static int gate(int perm, int *slot_out)
{
    char name[32];
    purr_catrt_slot_info_t si;
    int slot = purr_catrt_caller(name, sizeof(name));
    if (slot < 0 || purr_catrt_slot_info(slot, &si) != 0) {
        return PURR_POWER_BAD;
    }
    *slot_out = slot;
    lock();
    ensure_loaded();
    int d = purr_perm_decide(&s_perm, name, perm, si.privileged);
    int rc;
    if (d == PURR_PERM_YES) {
        rc = 0;
    } else if (d == PURR_PERM_NO || !si.foreground || s_show == NULL) {
        if (d == PURR_PERM_PROMPT) {
            ESP_LOGW(TAG, "%s: %s refused (%s)", name, purr_perm_name(perm),
                     si.foreground ? "no prompt available" : "a background app cannot ask");
        }
        rc = PURR_POWER_DENIED;
    } else {
        int queued = 0, first = 0;
        for (int i = 0; i < s_npend; i++) {
            queued |= s_pend[i].perm == perm && strcmp(s_pend[i].app, name) == 0;
        }
        if (!queued && s_npend < MAX_PENDING) {
            snprintf(s_pend[s_npend].app, sizeof(s_pend[0].app), "%s", name);
            s_pend[s_npend].perm = perm;
            first = s_npend == 0;
            s_npend++;
            queued = 1;
        }
        int denials = purr_perm_denials(&s_perm, name, perm);
        unlock();
        if (first) {
            s_show(name, perm, denials);
        }
        return queued ? PURR_POWER_ASK : PURR_POWER_DENIED;      /* the queue is full: refuse rather than wait */
    }
    unlock();
    return rc;
}

int purr_catrt_perm_gate(int perm)
{
    int slot;
    return gate(perm, &slot);
}

int purr_catrt_perm_answer(int answer, char *next_app, size_t cap, int *next_perm, int *next_denials)
{
    lock();
    int more = 0;
    if (s_npend > 0) {
        pending_t p = s_pend[0];
        memmove(&s_pend[0], &s_pend[1], (size_t)(s_npend - 1) * sizeof(s_pend[0]));
        s_npend--;
        int allowed = answer == PURR_ANSWER_ONCE || answer == PURR_ANSWER_ALWAYS;
        ensure_loaded();
        if (answer == PURR_ANSWER_ALWAYS) {
            purr_perm_set(&s_perm, p.app, p.perm, PURR_PERM_ALWAYS);
            save();
        } else if (answer == PURR_ANSWER_ONCE) {
            purr_perm_set(&s_perm, p.app, p.perm, PURR_PERM_ONCE);
        } else if (answer == PURR_ANSWER_NEVER) {
            purr_perm_set(&s_perm, p.app, p.perm, PURR_PERM_NEVER);
            save();
        } else {
            purr_perm_deny(&s_perm, p.app, p.perm);
        }
        purr_catrt_post_event(purr_catrt_slot_of(p.app), PURR_APPEV_PERM, allowed, purr_perm_name(p.perm));
        if (s_npend > 0) {
            snprintf(next_app, cap, "%s", s_pend[0].app);
            *next_perm = s_pend[0].perm;
            *next_denials = purr_perm_denials(&s_perm, s_pend[0].app, s_pend[0].perm);
            more = 1;
        }
    }
    unlock();
    return more;
}

void purr_catrt_perm_set_ui(purr_perm_show_fn show) { s_show = show; }

int purr_catrt_perm_get(const char *app, int perm)
{
    lock();
    ensure_loaded();
    int s = purr_perm_state(&s_perm, app, perm);
    unlock();
    return s;
}

static void revoke_running(const char *app, int perm)
{
    int slot = purr_catrt_slot_of(app);
    if (slot >= 0) {
        if (perm == PURR_PERM_KEEP_AWAKE) {
            purr_power_sys_release(slot, PURR_WAKE_SCREEN);
            purr_power_sys_release(slot, PURR_WAKE_CPU);
        } else if (perm == PURR_PERM_WAKE_ALARM) {
            purr_power_sys_alarm_cancel(slot, 0);
            purr_power_sys_alarm_cancel(slot, 1);
        }
    }
}

int purr_catrt_perm_put(const char *app, int perm, int state)
{
    lock();
    ensure_loaded();
    int rc = purr_perm_set(&s_perm, app, perm, state);
    if (rc == 0) {
        save();
        if (state != PURR_PERM_ALWAYS && state != PURR_PERM_ONCE) {
            revoke_running(app, perm);                  /* takes effect at once */
        }
    }
    unlock();
    return rc;
}

void purr_catrt_perm_reset(const char *app)
{
    lock();
    ensure_loaded();
    purr_perm_reset(&s_perm, app);
    save();
    for (int p = 0; p < PURR_PERM_COUNT; p++) {
        revoke_running(app, p);
    }
    unlock();
}

int purr_catrt_perm_apps(char names[][32], int max)
{
    lock();
    ensure_loaded();
    int n = 0;
    for (int i = 0; i < s_perm.count && n < max; i++) {
        snprintf(names[n++], 32, "%s", s_perm.e[i].app);
    }
    unlock();
    return n;
}

/* ------------------------------------------------------------------ the catcall */

static int cc_hold(int kind, uint32_t lease_ms)
{
    int slot, rc = gate(PURR_PERM_KEEP_AWAKE, &slot);
    if (rc != 0) {
        return rc;
    }
    purr_catrt_slot_info_t si;
    if (kind == PURR_CC_WAKE_SCREEN && (purr_catrt_slot_info(slot, &si) != 0 || !si.foreground)) {
        return PURR_POWER_BAD;                          /* a screen lock is for the app in front */
    }
    return purr_power_sys_hold(slot, kind, lease_ms) == 0 ? 0 : PURR_POWER_BAD;
}

static int cc_renew(int kind, uint32_t lease_ms)
{
    int slot, rc = gate(PURR_PERM_KEEP_AWAKE, &slot);
    if (rc != 0) {
        return rc;
    }
    return purr_power_sys_renew(slot, kind, lease_ms) == 0 ? 0 : PURR_POWER_BAD;
}

static void cc_release(int kind)
{
    int slot = purr_catrt_caller(NULL, 0);
    if (slot >= 0) {
        purr_power_sys_release(slot, kind);
    }
}

static int cc_alarm_set(uint32_t interval_ms, uint32_t window_ms, int flags)
{
    int slot, rc = gate(PURR_PERM_WAKE_ALARM, &slot);
    if (rc != 0) {
        return rc;
    }
    purr_catrt_slot_info_t si;
    if (purr_catrt_slot_info(slot, &si) != 0 || !si.background_ok) {
        return PURR_POWER_BAD;                          /* only an app that may run in the background */
    }
    int idx = purr_power_sys_alarm_set(slot, -1, interval_ms, window_ms, flags);
    return idx >= 0 ? idx : (idx == -2 ? PURR_POWER_NO_ALARM : PURR_POWER_BAD);
}

static void cc_alarm_cancel(int index)
{
    int slot = purr_catrt_caller(NULL, 0);
    if (slot >= 0) {
        purr_power_sys_alarm_cancel(slot, index);
    }
}

static void cc_done(void)
{
    int slot = purr_catrt_caller(NULL, 0);
    if (slot >= 0) {
        purr_power_sys_done(slot);
    }
}

static int cc_screen_is_on(void) { return purr_power_sys_screen_is_on(); }

static const purr_cc_power_t s_cc_power = {
    .version = 1, .hold = cc_hold, .renew = cc_renew, .release = cc_release, .alarm_set = cc_alarm_set,
    .alarm_cancel = cc_alarm_cancel, .done = cc_done, .screen_is_on = cc_screen_is_on,
};

/* ------------------------------------------------------------------ hooks */

static void on_wake(int owner, int alarm, const char *why)
{
    purr_catrt_post_event(owner, PURR_APPEV_WAKE, alarm, why);
}

static void on_notify(int owner, const char *what)
{
    if (strcmp(what, "alarm-off") == 0) {
        purr_catrt_post_event(owner, PURR_APPEV_WAKE, -1, "alarm-off");
    }
}

static void on_stopped(int slot, const char *name)
{
    purr_power_sys_release_owner(slot);
    lock();
    purr_perm_app_stopped(&s_perm, name);
    unlock();
}

static void on_paused(int slot) { purr_power_sys_release(slot, PURR_WAKE_SCREEN); }

/* The app's task is stopped: nothing it holds may keep the device awake, and a stopped app cannot answer an alarm. */
static void on_frozen(int slot)
{
    purr_power_sys_release(slot, PURR_WAKE_SCREEN);
    purr_power_sys_release(slot, PURR_WAKE_CPU);
    purr_power_sys_alarm_cancel(slot, 0);
    purr_power_sys_alarm_cancel(slot, 1);
}

void purr_catrt_power_init(void)
{
    if (s_mu == NULL) {
        s_mu = xSemaphoreCreateRecursiveMutex();
    }
    purr_power_sys_set_hooks(on_wake, on_notify);
    purr_catrt_set_power(&s_cc_power, on_stopped, on_paused);
    purr_catrt_set_power_frozen(on_frozen);
}

/* ------------------------------------------------------------------ the shell commands */

static const char *state_word(int s)
{
    return s == PURR_PERM_ALWAYS ? "always" : s == PURR_PERM_ONCE ? "once" : s == PURR_PERM_NEVER ? "never" : "ask";
}

int purr_perms_cmd(purr_cli_t *cli, int argc, char **argv)
{
    const char *cmd = argv[0];
    int change = strcmp(cmd, "grant") == 0 || strcmp(cmd, "revoke") == 0;
    int reset = argc >= 3 && strcmp(argv[1], "reset") == 0;
    if ((change || reset) && !(purr_login_is_root() || purr_login_current()->role == PURR_ROLE_USER_ADMIN)) {
        purr_cli_printf(cli, "only an administrator can change another app's permissions\n");
        return 1;
    }
    if (change) {
        int perm = argc >= 4 ? purr_perm_by_name(argv[3]) : -1;
        if (perm < 0 && argc >= 4) {
            purr_cli_printf(cli, "unknown permission %s (keep-awake, wake-alarm)\n", argv[3]);
            return 1;
        }
        if (argc < 4) {
            purr_cli_printf(cli, "usage: %s <app> <permission>\n", cmd);
            return 1;
        }
        int rc = purr_catrt_perm_put(argv[2], perm, strcmp(cmd, "grant") == 0 ? PURR_PERM_ALWAYS : PURR_PERM_ASK);
        purr_cli_printf(cli, rc == 0 ? "%s: %s %s\n" : "could not change %s %s %s\n", argv[2], argv[3],
                        strcmp(cmd, "grant") == 0 ? "allowed" : "will ask again");
        return rc == 0 ? 0 : 1;
    }
    if (reset) {
        purr_catrt_perm_reset(argv[2]);
        purr_cli_printf(cli, "%s: every permission will ask again\n", argv[2]);
        return 0;
    }
    char names[PURR_PERM_MAX_APPS][32];
    int n = purr_catrt_perm_apps(names, PURR_PERM_MAX_APPS);
    int shown = 0;
    for (int i = 0; i < n; i++) {
        if (argc >= 2 && strcmp(argv[1], names[i]) != 0) {
            continue;
        }
        shown++;
        purr_cli_printf(cli, "%s:", names[i]);
        for (int p = 0; p < PURR_PERM_COUNT; p++) {
            purr_cli_printf(cli, " %s=%s", purr_perm_name(p), state_word(purr_catrt_perm_get(names[i], p)));
        }
        purr_cli_printf(cli, "\n");
    }
    if (!shown) {
        purr_cli_printf(cli, "%s\n", argc >= 2 ? "no decisions on file for that app" : "no app has asked for a permission yet");
    }
    return 0;
}
