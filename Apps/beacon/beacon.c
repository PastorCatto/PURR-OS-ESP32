/*
 * beacon.c - Beacon, a sideloaded sample for the `power` catcall (plan10.md). No libc.
 *
 * "Start" asks for the wake-alarm permission (the first time), then sets an aligned 30 second alarm. Each wake
 * the OS opens a short window: the app holds the CPU for a moment, counts the wake, and calls done() so the
 * device can sleep again. That is the loop a mesh radio app runs: wake, check messages, beacon, hear the reply,
 * sleep. The app keeps running when it leaves the front (flag 0x01, background ok), so the wakes go on with the
 * screen off.
 */
#include <stddef.h>
#include <stdint.h>

#include "purr_app_abi.h"
#include "purr_catcalls.h"

#define NO_OPT __attribute__((noinline, optimize("no-tree-loop-distribute-patterns")))

#define INTERVAL_MS 30000u
#define WINDOW_MS 8000u

static const purr_cc_ui_t *ui;
static const purr_cc_power_t *pw;
static const purr_cc_console_t *con;

static int h_status, h_count, h_button;
static int s_alarm = -1;                    /* the alarm index while running, else -1 */
static int s_wakes;
static int s_waiting;                       /* a permission prompt is up: retry when it is answered */

NO_OPT static int put_str(char *d, int at, const char *s)
{
    while (*s) {
        d[at++] = *s++;
    }
    d[at] = '\0';
    return at;
}

NO_OPT static int put_num(char *d, int at, int v)
{
    char t[12];
    int n = 0;
    if (v <= 0) {
        t[n++] = '0';
    }
    while (v > 0) {
        t[n++] = (char)('0' + v % 10);
        v /= 10;
    }
    while (n > 0) {
        d[at++] = t[--n];
    }
    d[at] = '\0';
    return at;
}

NO_OPT static int same(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static void show_status(const char *text)
{
    ui->set_text(h_status, text);
}

static void show_count(void)
{
    char b[40];
    int n = put_str(b, 0, "Wakes so far: ");
    put_num(b, n, s_wakes);
    ui->set_text(h_count, b);
}

static void start(void)
{
    int r = pw->alarm_set(INTERVAL_MS, WINDOW_MS, PURR_CC_ALARM_ALIGN);
    if (r == PURR_POWER_ASK) {
        s_waiting = 1;
        show_status("Waiting for your answer...");
    } else if (r >= 0) {
        s_alarm = r;
        s_waiting = 0;
        show_status("Running: wakes every 30 s, even with the screen off");
        ui->set_text(h_button, "Stop");
    } else if (r == PURR_POWER_DENIED) {
        show_status("Not allowed. Turn on Wake alarms for Beacon in Settings.");
    } else {
        show_status("Could not start the alarm.");
    }
}

static void stop(void)
{
    if (s_alarm >= 0) {
        pw->alarm_cancel(s_alarm);
    }
    s_alarm = -1;
    show_status("Stopped");
    ui->set_text(h_button, "Start");
}

static int on_create(const purr_app_env_t *env, const void *state, uint32_t state_len)
{
    (void)state;
    (void)state_len;
    con = (const purr_cc_console_t *)env->catcall_get("console", 1);
    ui = (const purr_cc_ui_t *)env->catcall_get("ui", 1);
    pw = (const purr_cc_power_t *)env->catcall_get("power", 1);
    if (ui == NULL || pw == NULL) {
        if (con) {
            con->print("beacon: needs the ui and power catcalls\n");
        }
        return -1;
    }
    ui->win_title("Beacon");
    ui->win_back(0);
    ui->clear();
    ui->label("A wake every 30 seconds, then back to sleep.", PURR_UI_ROLE_DIM);
    h_status = ui->label("Stopped", PURR_UI_ROLE_TEXT);
    h_count = ui->label("Wakes so far: 0", PURR_UI_ROLE_ACCENT);
    h_button = ui->button("Start");
    return 0;
}

static void on_event(const purr_app_event_t *ev)
{
    if (ev->type == PURR_APPEV_UI && ev->name != NULL && same(ev->name, "click") && ev->arg == h_button) {
        if (s_alarm >= 0) {
            stop();
        } else {
            start();
        }
    } else if (ev->type == PURR_APPEV_PERM) {
        if (ev->arg == 1 && s_waiting) {
            start();                                    /* allowed: ask again, it will work now */
        } else if (ev->arg == 0) {
            s_waiting = 0;
            show_status("Not allowed. The alarm was not set.");
        }
    } else if (ev->type == PURR_APPEV_WAKE && ev->name != NULL) {
        if (same(ev->name, "alarm-off")) {
            s_alarm = -1;
            show_status("Switched off: it kept the device awake too long");
            ui->set_text(h_button, "Start");
            return;
        }
        s_wakes++;
        show_count();
        if (con) {
            con->print("beacon: wake\n");
        }
        pw->done();                                     /* the work is finished: the device may sleep */
    }
}

static void on_destroy(int reason)
{
    (void)reason;
}

static const purr_app_table2_t s_table = {
    .base = {.abi_version = PURR_APP_ABI_VERSION_2, .on_create = on_create, .on_destroy = on_destroy},
    .on_event = on_event,
    .memory_needed = 12288, .kind = 2, .flags = 0x01 /* may keep running in the background */,
};

__attribute__((used))
const purr_app_table_t *beacon_entry(void)
{
    return &s_table.base;
}
