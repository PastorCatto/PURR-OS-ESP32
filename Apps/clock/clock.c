/*
 * clock.c - Clock, a sideloaded UI app: the time, a stopwatch and a timer. No libc: only the catcalls it is handed.
 *
 * The home page shows the time and two rows (Stopwatch, Timer). The stopwatch counts up in tenths with laps; the timer counts down from a preset or a number of
 * minutes typed in. Everything runs while the app is open: a stopped-in-the-background timer that alarms is for a later version (it needs the `power` wake alarm).
 *
 * The time shown is local when the OS offers `system` version 2 (a local-time call, OS 1.0.23); on an older OS it is UTC and says so.
 */
#include <stddef.h>
#include <stdint.h>

#include "purr_app_abi.h"
#include "purr_catcalls.h"

#define NO_OPT __attribute__((noinline, optimize("no-tree-loop-distribute-patterns")))

static const purr_cc_ui_t *ui;
static const purr_cc_sched_t *sc;
static const purr_cc_system_t *sys;
static const purr_cc_system_t *sys2;        /* the same table at version 2 (local_time), or NULL on an older OS */
static const purr_cc_app_t *app;
static const purr_cc_console_t *con;

enum { PAGE_HOME, PAGE_WATCH, PAGE_TIMER_SET, PAGE_TIMER_RUN };
static int s_page;
static int s_tick = -1;                     /* the sched timer id for the page on show */

/* ---------------------------------------------------------------- tiny helpers (no libc) */

NO_OPT static int same(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

NO_OPT static char *put_str(char *d, const char *s)
{
    while (*s) {
        *d++ = *s++;
    }
    return d;
}

/* `digits` decimal digits of v, zero padded. */
NO_OPT static char *put_num(char *d, uint32_t v, int digits)
{
    char tmp[10];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v != 0 && n < 10);
    while (n < digits) {
        tmp[n++] = '0';
    }
    while (n > 0) {
        *d++ = tmp[--n];
    }
    return d;
}

NO_OPT static uint32_t parse_uint(const char *s)
{
    uint32_t v = 0;
    while (*s == ' ') {
        s++;
    }
    while (*s >= '0' && *s <= '9' && v < 100000) {
        v = v * 10 + (uint32_t)(*s - '0');
        s++;
    }
    return v;
}

/* ---------------------------------------------------------------- the time of day */

typedef struct {
    int year, mon, mday, hour, min, sec, wday;
    int local;                              /* 1 = local time, 0 = UTC */
} tod_t;

/* Days since 1970-01-01 to a civil date (Howard Hinnant's algorithm). */
NO_OPT static void civil(int64_t days, int *y, int *m, int *d)
{
    days += 719468;
    int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    int64_t doe = days - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t yy = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yy + (*m <= 2));
}

NO_OPT static int time_of_day(tod_t *t)
{
    if (sys2 != NULL && sys2->local_time != NULL) {
        purr_cc_tm_t tm;
        if (sys2->local_time(&tm) == 0) {
            t->year = tm.year; t->mon = tm.mon; t->mday = tm.mday; t->hour = tm.hour; t->min = tm.min; t->sec = tm.sec; t->wday = tm.wday;
            t->local = 1;
            return 0;
        }
    }
    int64_t now = sys != NULL ? sys->unix_time() : 0;
    if (now <= 0) {
        return -1;                          /* the clock has not been set */
    }
    int64_t days = now / 86400;
    int64_t rem = now % 86400;
    civil(days, &t->year, &t->mon, &t->mday);
    t->hour = (int)(rem / 3600);
    t->min = (int)((rem % 3600) / 60);
    t->sec = (int)(rem % 60);
    t->wday = (int)((days + 4) % 7);        /* 1970-01-01 was a Thursday */
    t->local = 0;
    return 0;
}

static const char *const k_days[7] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
static const char *const k_mons[12] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};

/* ---------------------------------------------------------------- pages */

static int h_time, h_date, h_a, h_b, h_c;   /* widget handles of the page on show */
static char s_last[24];                     /* what the time label shows, so it is only redrawn when it changes */

static void arm(uint32_t ms)
{
    if (s_tick > 0) {
        sc->timer_cancel(s_tick);
        s_tick = -1;
    }
    if (ms != 0) {
        s_tick = sc->timer_start(ms, 1);
    }
}

NO_OPT static void draw_home_time(void)
{
    tod_t t;
    char a[24], b[48];
    if (time_of_day(&t) != 0) {
        char *p = put_str(a, "--:--:--");
        *p = 0;
        p = put_str(b, "The clock is not set");
        *p = 0;
    } else {
        char *p = put_num(a, (uint32_t)t.hour, 2);
        *p++ = ':';
        p = put_num(p, (uint32_t)t.min, 2);
        *p++ = ':';
        p = put_num(p, (uint32_t)t.sec, 2);
        *p = 0;
        p = put_str(b, k_days[t.wday % 7]);
        *p++ = ',';
        *p++ = ' ';
        p = put_str(p, k_mons[(t.mon - 1) % 12]);
        *p++ = ' ';
        p = put_num(p, (uint32_t)t.mday, 1);
        if (!t.local) {
            p = put_str(p, "  (UTC)");
        }
        *p = 0;
    }
    if (!same(a, s_last)) {
        put_str(s_last, a)[0] = 0;
        ui->set_text(h_time, a);
        ui->set_text(h_date, b);
    }
}

static void show_home(void)
{
    s_page = PAGE_HOME;
    ui->clear();
    ui->win_title("Clock");
    ui->win_back(0);                        /* the back arrow closes the app from here */
    s_last[0] = 0;
    h_time = ui->label("--:--:--", PURR_UI_ROLE_TITLE);
    h_date = ui->label("", PURR_UI_ROLE_DIM);
    ui->spacer(8);
    h_a = ui->row("Stopwatch", "count up, with laps");
    h_b = ui->row("Timer", "count down");
    draw_home_time();
    arm(1000);
}

/* ---- stopwatch: elapsed time is kept as milliseconds, with the uptime at the moment it last started */

static uint32_t s_sw_acc;                   /* ms counted before the last start */
static uint32_t s_sw_t0;                    /* uptime when it last started */
static int s_sw_run;
static int s_sw_laps;
static int s_lap_h[5];
#define MAX_SHOWN_LAPS 5

NO_OPT static uint32_t sw_elapsed(void)
{
    return s_sw_acc + (s_sw_run ? sc->uptime_ms() - s_sw_t0 : 0);
}

/* mm:ss.t (or h:mm:ss.t past an hour) */
NO_OPT static void fmt_watch(char *d, uint32_t ms)
{
    uint32_t tenths = (ms / 100) % 10, s = ms / 1000, m = s / 60, h = m / 60;
    char *p = d;
    if (h > 0) {
        p = put_num(p, h, 1);
        *p++ = ':';
        p = put_num(p, m % 60, 2);
    } else {
        p = put_num(p, m, 2);
    }
    *p++ = ':';
    p = put_num(p, s % 60, 2);
    *p++ = '.';
    p = put_num(p, tenths, 1);
    *p = 0;
}

static void draw_watch(void)
{
    char a[24];
    fmt_watch(a, sw_elapsed());
    if (!same(a, s_last)) {
        put_str(s_last, a)[0] = 0;
        ui->set_text(h_time, a);
    }
}

static void show_watch(void)
{
    s_page = PAGE_WATCH;
    ui->clear();
    ui->win_title("Stopwatch");
    ui->win_back(1);
    s_last[0] = 0;
    h_time = ui->label("00:00.0", PURR_UI_ROLE_TITLE);
    ui->spacer(6);
    h_a = ui->button(s_sw_run ? "Stop" : "Start");
    h_b = ui->button("Lap");
    h_c = ui->button("Reset");
    for (int i = 0; i < MAX_SHOWN_LAPS; i++) {
        s_lap_h[i] = -1;
    }
    draw_watch();
    arm(s_sw_run ? 100 : 0);
}

NO_OPT static void add_lap(void)
{
    if (!s_sw_run || s_sw_laps >= 99) {
        return;
    }
    char line[40], t[24];
    fmt_watch(t, sw_elapsed());
    char *p = put_str(line, "Lap ");
    p = put_num(p, (uint32_t)++s_sw_laps, 1);
    p = put_str(p, "   ");
    p = put_str(p, t);
    *p = 0;
    int slot = (s_sw_laps - 1) % MAX_SHOWN_LAPS;
    if (s_lap_h[slot] > 0) {
        ui->remove(s_lap_h[slot]);          /* only the last few are kept on screen */
    }
    s_lap_h[slot] = ui->label(line, PURR_UI_ROLE_DIM);
}

/* ---- timer */

static uint32_t s_tm_total;                 /* ms the timer was set to */
static uint32_t s_tm_left;                  /* ms left when it last started or was paused */
static uint32_t s_tm_t0;
static int s_tm_run;
static int s_tm_done;
static int h_custom, h_preset[6];
static const uint32_t k_presets[6] = {1, 5, 10, 15, 30, 60};   /* minutes */
static const char *const k_preset_name[6] = {"1 minute", "5 minutes", "10 minutes", "15 minutes", "30 minutes", "1 hour"};

NO_OPT static uint32_t tm_left(void)
{
    if (!s_tm_run) {
        return s_tm_left;
    }
    uint32_t used = sc->uptime_ms() - s_tm_t0;
    return used >= s_tm_left ? 0 : s_tm_left - used;
}

NO_OPT static void fmt_timer(char *d, uint32_t ms)
{
    uint32_t s = (ms + 999) / 1000, m = s / 60, h = m / 60;
    char *p = d;
    if (h > 0) {
        p = put_num(p, h, 1);
        *p++ = ':';
        p = put_num(p, m % 60, 2);
    } else {
        p = put_num(p, m, 2);
    }
    *p++ = ':';
    p = put_num(p, s % 60, 2);
    *p = 0;
}

static void draw_timer(void)
{
    char a[24];
    uint32_t left = tm_left();
    if (left == 0 && !s_tm_done) {
        s_tm_done = 1;
        s_tm_run = 0;
        s_tm_left = 0;
        ui->status("Timer finished", 0);
        ui->set_text(h_a, "Again");
        arm(0);
    }
    if (s_tm_done) {
        put_str(a, "Time's up")[0] = 0;
    } else {
        fmt_timer(a, left);
    }
    if (!same(a, s_last)) {
        put_str(s_last, a)[0] = 0;
        ui->set_text(h_time, a);
    }
}

static void show_timer_set(void)
{
    s_page = PAGE_TIMER_SET;
    ui->clear();
    ui->win_title("Timer");
    ui->win_back(1);
    arm(0);
    s_tm_run = 0;
    s_tm_done = 0;
    for (int i = 0; i < 6; i++) {
        h_preset[i] = ui->row(k_preset_name[i], NULL);
    }
    ui->spacer(6);
    h_custom = ui->textarea(4, "or type minutes", 0);
    h_a = ui->button("Start");
}

static void show_timer_run(uint32_t ms)
{
    s_page = PAGE_TIMER_RUN;
    s_tm_total = ms;
    s_tm_left = ms;
    s_tm_t0 = sc->uptime_ms();
    s_tm_run = 1;
    s_tm_done = 0;
    ui->clear();
    ui->win_title("Timer");
    ui->win_back(1);
    s_last[0] = 0;
    h_time = ui->label("", PURR_UI_ROLE_TITLE);
    ui->spacer(6);
    h_a = ui->button("Pause");
    h_b = ui->button("Reset");
    draw_timer();
    arm(250);
}

/* ---------------------------------------------------------------- the app */

static int on_create(const purr_app_env_t *env, const void *state, uint32_t len)
{
    (void)state;
    (void)len;
    if (env == NULL || env->catcall_get == NULL) {
        return -1;
    }
    con = (const purr_cc_console_t *)env->catcall_get("console", 1);
    ui = (const purr_cc_ui_t *)env->catcall_get("ui", 1);
    sc = (const purr_cc_sched_t *)env->catcall_get("sched", 1);
    sys = (const purr_cc_system_t *)env->catcall_get("system", 1);
    sys2 = (const purr_cc_system_t *)env->catcall_get("system", 2);
    app = (const purr_cc_app_t *)env->catcall_get("app", 1);
    if (ui == NULL || sc == NULL || sys == NULL) {
        if (con) {
            con->print("clock: needs the ui, sched and system catcalls\n");
        }
        return -1;
    }
    show_home();
    return 0;
}

NO_OPT static void click(int h)
{
    if (s_page == PAGE_HOME) {
        if (h == h_a) {
            show_watch();
        } else if (h == h_b) {
            show_timer_set();
        }
    } else if (s_page == PAGE_WATCH) {
        if (h == h_a) {
            if (s_sw_run) {
                s_sw_acc += sc->uptime_ms() - s_sw_t0;
                s_sw_run = 0;
                arm(0);
                ui->set_text(h_a, "Start");
                draw_watch();
            } else {
                s_sw_t0 = sc->uptime_ms();
                s_sw_run = 1;
                arm(100);
                ui->set_text(h_a, "Stop");
            }
        } else if (h == h_b) {
            add_lap();
        } else if (h == h_c) {
            s_sw_acc = 0;
            s_sw_laps = 0;
            if (s_sw_run) {
                s_sw_t0 = sc->uptime_ms();
            }
            show_watch();
        }
    } else if (s_page == PAGE_TIMER_SET) {
        for (int i = 0; i < 6; i++) {
            if (h == h_preset[i]) {
                show_timer_run(k_presets[i] * 60000u);
                return;
            }
        }
        if (h == h_a) {
            char buf[8];
            ui->get_text(h_custom, buf, sizeof(buf));
            uint32_t m = parse_uint(buf);
            if (m == 0 || m > 999) {
                ui->status("Type a number of minutes, 1 to 999", 1);
            } else {
                show_timer_run(m * 60000u);
            }
        }
    } else if (s_page == PAGE_TIMER_RUN) {
        if (h == h_a) {
            if (s_tm_done) {
                show_timer_run(s_tm_total);
            } else if (s_tm_run) {
                s_tm_left = tm_left();
                s_tm_run = 0;
                arm(0);
                ui->set_text(h_a, "Resume");
            } else {
                s_tm_t0 = sc->uptime_ms();
                s_tm_run = 1;
                arm(250);
                ui->set_text(h_a, "Pause");
            }
        } else if (h == h_b) {
            show_timer_set();
        }
    }
}

static void on_event(const purr_app_event_t *ev)
{
    if (ui == NULL) {
        return;
    }
    if (ev->type == PURR_APPEV_TIMER) {
        if (ev->arg != s_tick) {
            return;
        }
        if (s_page == PAGE_HOME) {
            draw_home_time();
        } else if (s_page == PAGE_WATCH) {
            draw_watch();
        } else if (s_page == PAGE_TIMER_RUN) {
            draw_timer();
        }
    } else if (ev->type == PURR_APPEV_UI && ev->name != NULL) {
        if (same(ev->name, "click")) {
            click(ev->arg);
        } else if (same(ev->name, "back")) {
            if (s_page == PAGE_TIMER_RUN) {
                show_timer_set();
            } else if (s_page == PAGE_HOME) {
                if (app != NULL) {
                    app->close_self();
                }
            } else {
                show_home();
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
    .memory_needed = 16384, .kind = 2, .flags = 0,
};

__attribute__((used))
const purr_app_table_t *clock_entry(void)
{
    return &s_table.base;
}
