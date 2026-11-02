/* Tests for the app manager (purr_runtime.c): states, launch and admission, focus, saved state,
 * the out-of-memory killer, rules, binds, the config parser, dispatchers, queries and events.
 * Everything runs against fakes: no tasks, no heap, no device. */
#include <stdio.h>
#include <string.h>

#include "purr_runtime.h"
#include "testkit.h"

/* ---------------------------------------------------------------- the fake world */

#define MAX_FAKE 8
#define INTERNAL_PER_APP 10000u   /* internal RAM one started app takes in the fake machine */
#define LOG_LINES 64
#define LOG_LEN 96

typedef struct {
    const char *name;
    uint8_t kind, flags;
    uint32_t memory;
    int create_result;
} fake_app_t;

static fake_app_t s_apps[] = {
    {"notes",  PURR_APP_KIND_UI,           PURR_APP_F_SAVES_STATE, 2000, 0},
    {"clock",  PURR_APP_KIND_UI,           PURR_APP_F_BACKGROUND_OK, 1000, 0},
    {"sync",   PURR_APP_KIND_DAEMON,       0, 500, 0},
    {"shell",  PURR_APP_KIND_COMMAND_LINE, 0, 800, 0},
    {"big",    PURR_APP_KIND_UI,           0, 6000, 0},
    {"broken", PURR_APP_KIND_UI,           0, 100, -1},
    {"cfg1",   PURR_APP_KIND_UI,           0, 100, 0},
    {"cfg2",   PURR_APP_KIND_UI,           0, 100, 0},
    {"cfg3",   PURR_APP_KIND_UI,           0, 100, 0},
    {"sync2",  PURR_APP_KIND_DAEMON,       0, 500, 0},
};
#define NAPPS ((int)(sizeof(s_apps) / sizeof(s_apps[0])))

static uint32_t s_now;
static uint32_t s_total;                  /* total memory of the fake machine */
static uint32_t s_used;                   /* bytes held by started instances */
static char s_log[LOG_LINES][LOG_LEN];
static int s_nlog;
static char s_events[LOG_LINES][LOG_LEN];
static int s_nev;
static uint8_t s_saved[PURR_RT_MAX_STATE];
static uint32_t s_saved_len;
static char s_saved_name[PURR_RT_NAME_LEN];

/* What each app callback saw, so tests can check order and arguments. */
static char s_calls[512];
static int s_destroy_reason = -99, s_low_level = -99;
static const void *s_created_state;
static uint32_t s_created_state_len;
static char s_last_event_name[32], s_last_event_data[32];
static int s_event_calls;

static void call(const char *s)
{
    size_t n = strlen(s_calls);
    snprintf(s_calls + n, sizeof(s_calls) - n, "%s;", s);
}

static int app_create(const purr_app_env_t *env, const void *state, uint32_t len)
{
    (void)env;
    s_created_state = state;
    s_created_state_len = len;
    call("create");
    return 0;
}
static void app_destroy(int reason) { s_destroy_reason = reason; call("destroy"); }
static void app_resume(void) { call("resume"); }
static void app_pause(void) { call("pause"); }
static uint32_t app_save(void *buf, uint32_t cap)
{
    (void)cap;
    memcpy(buf, "SAVED", 5);
    call("save");
    return 5;
}
static void app_low(int level) { s_low_level = level; call("low"); }
static void app_event(const purr_app_event_t *ev)
{
    snprintf(s_last_event_name, sizeof(s_last_event_name), "%s", ev->name ? ev->name : "");
    snprintf(s_last_event_data, sizeof(s_last_event_data), "%s", ev->data ? ev->data : "");
    s_event_calls++;
    call("event");
}

static const purr_app_table2_t s_table = {
    .base = {.abi_version = PURR_APP_ABI_VERSION_2, .on_create = app_create,
             .on_destroy = app_destroy},
    .on_resume = app_resume, .on_pause = app_pause, .on_save_state = app_save,
    .on_low_memory = app_low, .on_event = app_event,
};
static const purr_app_env_t s_env = {0};

static int fake_load(void *ctx, const char *name, purr_rt_app_t *out)
{
    (void)ctx;
    if (strcmp(name, "rogue") == 0) {
        return PURR_RT_HOST_UNTRUSTED;        /* unsigned or tampered: the platform refuses it */
    }
    for (int i = 0; i < NAPPS; i++) {
        if (strcmp(s_apps[i].name, name) == 0) {
            memset(out, 0, sizeof(*out));
            out->table = &s_table;
            out->env = &s_env;
            out->handle = (uint32_t)(i + 1);
            out->memory_needed = s_apps[i].memory;
            out->internal_needed = INTERNAL_PER_APP;
            out->kind = s_apps[i].kind;
            out->flags = s_apps[i].flags;
            return 0;
        }
    }
    return -1;
}

static uint32_t s_int_free;               /* the fake machine's internal RAM, in bytes; each started app takes INTERNAL_PER_APP of it */
static uint8_t s_int_held[16];            /* by handle: this instance holds its share */
static uint32_t fake_internal_free(void *ctx, uint32_t *largest)
{
    (void)ctx;
    *largest = s_int_free;
    return s_int_free;
}
static uint8_t s_frozen[16];              /* by app handle: the host has stopped its task */
static int s_freeze_calls, s_delivered_frozen;

/* Instances start their blocks at CREATE and release them at unload, like the device layer. */
static int fake_deliver(void *ctx, int id, const purr_rt_app_t *app, const purr_rt_event_t *ev)
{
    (void)ctx;
    (void)id;
    if (s_frozen[app->handle]) {
        s_delivered_frozen++;                 /* the manager told a stopped task something: the device would hang */
    }
    if (ev->type == PURR_RT_EV_CREATE) {
        int idx = (int)app->handle - 1;
        if (s_apps[idx].create_result < 0) {
            return -1;
        }
        s_used += app->memory_needed;
        s_int_free -= app->internal_needed;
        s_int_held[app->handle] = 1;
    }
    return purr_rt_invoke(app, ev);
}

static uint32_t s_unloaded_mem;
static void fake_freeze(void *ctx, uint32_t handle, int frozen)
{
    (void)ctx;
    s_frozen[handle] = (uint8_t)frozen;
    s_freeze_calls++;
}
static void fake_unload(void *ctx, uint32_t handle)
{
    (void)ctx;
    int idx = (int)handle - 1;
    if (s_int_held[handle]) {
        s_int_held[handle] = 0;
        s_int_free += INTERNAL_PER_APP;
    }
    /* Only instances that were created hold memory; the fake tracks it by app. */
    if (s_used >= s_apps[idx].memory) {
        s_used -= s_apps[idx].memory;
    }
    s_unloaded_mem += s_apps[idx].memory;
}

static uint32_t fake_now(void *ctx) { (void)ctx; return s_now; }
static uint32_t fake_free(void *ctx) { (void)ctx; return s_total - s_used; }
static int fake_save(void *ctx, const char *name, const void *buf, uint32_t len)
{
    (void)ctx;
    memcpy(s_saved, buf, len);
    s_saved_len = len;
    snprintf(s_saved_name, sizeof(s_saved_name), "%s", name);
    return 0;
}
static int fake_load_state(void *ctx, const char *name, void *buf, uint32_t cap)
{
    (void)ctx;
    if (s_saved_len == 0 || strcmp(name, s_saved_name) != 0 || s_saved_len > cap) {
        return 0;
    }
    memcpy(buf, s_saved, s_saved_len);
    return (int)s_saved_len;
}
static void fake_log(void *ctx, const char *line)
{
    (void)ctx;
    if (s_nlog < LOG_LINES) {
        snprintf(s_log[s_nlog++], LOG_LEN, "%s", line);
    }
}

static void sink(void *ctx, const char *line)
{
    (void)ctx;
    if (s_nev < LOG_LINES) {
        snprintf(s_events[s_nev++], LOG_LEN, "%s", line);
    }
}

static purr_rt_t rt;

static void reset(uint32_t total)
{
    s_now = 1000;
    s_total = total;
    s_used = 0;
    s_nlog = 0;
    s_nev = 0;
    s_saved_len = 0;
    s_saved_name[0] = '\0';
    s_calls[0] = '\0';
    s_destroy_reason = s_low_level = -99;
    s_created_state = NULL;
    s_created_state_len = 0;
    s_event_calls = 0;
    s_unloaded_mem = 0;
    memset(s_frozen, 0, sizeof(s_frozen));
    memset(s_int_held, 0, sizeof(s_int_held));
    s_int_free = 0x40000000u;                 /* plenty, so tests that do not care about it are unaffected */
    s_freeze_calls = s_delivered_frozen = 0;
    purr_rt_host_t host = {
        .load = fake_load, .unload = fake_unload, .deliver = fake_deliver, .now_ms = fake_now,
        .free_bytes = fake_free, .save_state = fake_save, .load_state = fake_load_state,
        .log = fake_log, .freeze = fake_freeze, .internal_free = fake_internal_free,
    };
    purr_rt_init(&rt, &host);
    purr_rt_add_sink(&rt, sink, NULL);
}

static int saw_event(const char *line)
{
    for (int i = 0; i < s_nev; i++) {
        if (strcmp(s_events[i], line) == 0) {
            return 1;
        }
    }
    return 0;
}

static int event_index(const char *line)
{
    for (int i = 0; i < s_nev; i++) {
        if (strcmp(s_events[i], line) == 0) {
            return i;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------ launching */

static void test_open_and_lifecycle(void)
{
    reset(100000);
    int id = purr_rt_open(&rt, "notes");
    CHECK_EQ(id, 1);
    CHECK_EQ(rt.fg_id, 1);
    CHECK_EQ(purr_rt_get(&rt, 1)->state, PURR_RT_FOREGROUND);
    /* create first, then resume once it is in front */
    CHECK(strcmp(s_calls, "create;resume;") == 0);
    CHECK(saw_event("openapp>>1,notes"));
    CHECK(saw_event("focus>>1,notes"));
    CHECK(saw_event("state>>1,notes,starting,foreground"));
    CHECK_EQ(purr_rt_open(&rt, "nosuchapp"), PURR_RT_E_LOAD);
    CHECK_EQ(purr_rt_open(&rt, ""), PURR_RT_E_ARG);
}

static void test_an_unsigned_app_never_runs(void)
{
    reset(100000);
    char out[128];
    CHECK_EQ(purr_rt_open(&rt, "rogue"), PURR_RT_E_UNTRUSTED);
    CHECK_EQ(purr_rt_count(&rt), 0);
    CHECK_EQ(s_used, 0);
    CHECK(saw_event("refused>>rogue,untrusted"));
    CHECK(!saw_event("openapp>>1,rogue"));
    CHECK_EQ(purr_rt_dispatch(&rt, "open rogue", out, sizeof(out)), -1);
    CHECK(strstr(out, "not signed by a trusted key") != NULL);
    /* not even as an exec-once app at boot */
    char err[64];
    purr_rt_config_load(&rt, "exec-once = rogue\n", err, sizeof(err));
    CHECK_EQ(purr_rt_exec_once(&rt), 0);
    CHECK_EQ(purr_rt_count(&rt), 0);
    /* and a normal app still opens afterwards */
    CHECK(purr_rt_open(&rt, "clock") > 0);
}

static void test_failed_create_leaves_nothing(void)
{
    reset(100000);
    CHECK_EQ(purr_rt_open(&rt, "broken"), PURR_RT_E_CREATE);
    CHECK_EQ(purr_rt_count(&rt), 0);
    CHECK_EQ(rt.fg_id, 0);
    CHECK(!saw_event("openapp>>1,broken"));
    CHECK_EQ(s_used, 0);
}

static void test_launch_moves_the_old_foreground_back(void)
{
    reset(100000);
    purr_rt_open(&rt, "clock");               /* may run in the background */
    s_calls[0] = '\0';
    purr_rt_open(&rt, "notes");               /* does not */
    CHECK_EQ(purr_rt_get(&rt, 1)->state, PURR_RT_BACKGROUND);
    CHECK_EQ(purr_rt_get(&rt, 2)->state, PURR_RT_FOREGROUND);
    CHECK(strstr(s_calls, "pause;") != NULL);

    purr_rt_open(&rt, "shell");               /* notes does not allow the background: suspended */
    CHECK_EQ(purr_rt_get(&rt, 2)->state, PURR_RT_SUSPENDED);
    CHECK_EQ(rt.fg_id, 3);
}

static void test_daemons_start_in_the_background(void)
{
    reset(100000);
    purr_rt_open(&rt, "notes");
    int d = purr_rt_open(&rt, "sync");
    CHECK_EQ(purr_rt_get(&rt, d)->state, PURR_RT_BACKGROUND);
    CHECK_EQ(rt.fg_id, 1);                    /* the foreground app was left alone */
}

static void test_focus_switches_and_resumes(void)
{
    reset(100000);
    purr_rt_open(&rt, "notes");
    purr_rt_open(&rt, "shell");               /* notes suspended */
    s_calls[0] = '\0';
    CHECK_EQ(purr_rt_focus(&rt, 1), 0);
    CHECK_EQ(rt.fg_id, 1);
    CHECK_EQ(purr_rt_get(&rt, 2)->state, PURR_RT_SUSPENDED);
    CHECK(strstr(s_calls, "resume;") != NULL);
    CHECK_EQ(purr_rt_focus(&rt, 99), PURR_RT_E_NOAPP);
}

/* ------------------------------------------------------------------ saved state */

static void test_state_is_saved_on_leaving_and_restored_on_create(void)
{
    reset(100000);
    purr_rt_open(&rt, "notes");
    purr_rt_open(&rt, "shell");               /* notes leaves the foreground: saved */
    CHECK_EQ(s_saved_len, 5);
    CHECK(strcmp(s_saved_name, "notes") == 0);
    CHECK(memcmp(s_saved, "SAVED", 5) == 0);

    purr_rt_stop(&rt, 1, PURR_APP_DESTROY_USER);
    s_created_state = NULL;
    purr_rt_open(&rt, "notes");               /* the next run is handed it */
    CHECK(s_created_state != NULL);
    CHECK_EQ(s_created_state_len, 5);
    CHECK(memcmp(s_created_state, "SAVED", 5) == 0);
}

static void test_an_app_without_the_flag_saves_nothing(void)
{
    reset(100000);
    purr_rt_open(&rt, "shell");
    purr_rt_open(&rt, "clock");
    CHECK_EQ(s_saved_len, 0);
}

/* ------------------------------------------------------------------ closing */

static void test_close_runs_destroy_and_promotes_the_next_app(void)
{
    reset(100000);
    purr_rt_open(&rt, "clock");
    purr_rt_open(&rt, "shell");
    s_now += 10;
    CHECK_EQ(purr_rt_stop(&rt, 2, PURR_APP_DESTROY_USER), 0);
    CHECK_EQ(s_destroy_reason, PURR_APP_DESTROY_USER);
    CHECK(saw_event("closeapp>>2,shell,closed"));
    CHECK_EQ(purr_rt_get(&rt, 2) == NULL, 1);
    CHECK_EQ(rt.fg_id, 1);                    /* clock comes back */
    CHECK_EQ(purr_rt_get(&rt, 1)->state, PURR_RT_FOREGROUND);
    CHECK_EQ(purr_rt_stop(&rt, 2, PURR_APP_DESTROY_USER), PURR_RT_E_NOAPP);
}

static void test_kill_skips_destroy(void)
{
    reset(100000);
    char out[128];
    purr_rt_open(&rt, "shell");
    s_destroy_reason = -99;
    CHECK_EQ(purr_rt_dispatch(&rt, "kill active", out, sizeof(out)), 0);
    CHECK_EQ(s_destroy_reason, -99);
    CHECK(saw_event("closeapp>>1,shell,killed"));
    CHECK(saw_event("killed>>1,shell,killed"));
}

static void test_closing_releases_memory(void)
{
    reset(100000);
    purr_rt_open(&rt, "notes");
    CHECK_EQ(s_used, 2000);
    purr_rt_stop(&rt, 1, PURR_APP_DESTROY_USER);
    CHECK_EQ(s_used, 0);
}

static void test_ids_are_not_reused(void)
{
    reset(100000);
    int a = purr_rt_open(&rt, "shell");
    purr_rt_stop(&rt, a, PURR_APP_DESTROY_USER);
    int b = purr_rt_open(&rt, "shell");
    CHECK(b > a);
}

/* ------------------------------------------------------------------ admission */

static void test_admission_stops_the_least_important_app(void)
{
    reset(7000);
    purr_rt_open(&rt, "clock");               /* 1000, background-ok */
    purr_rt_open(&rt, "notes");               /* 2000; clock to background */
    purr_rt_open(&rt, "shell");               /* 800; notes suspended */
    CHECK_EQ(s_used, 3800);
    int id = purr_rt_open(&rt, "big");        /* 6000: needs 2200 more freed */
    CHECK(id > 0);
    /* The suspended app goes first (notes), then the background one (clock). */
    CHECK(purr_rt_get(&rt, 2) == NULL);
    CHECK(purr_rt_get(&rt, 1) == NULL);
    CHECK(saw_event("closeapp>>2,notes,memory"));
    CHECK(event_index("closeapp>>2,notes,memory") < event_index("closeapp>>1,clock,memory"));
    CHECK_EQ(purr_rt_get(&rt, id)->state, PURR_RT_FOREGROUND);
}

static void test_admission_never_stops_the_foreground_app(void)
{
    reset(6500);
    purr_rt_open(&rt, "shell");               /* the foreground app, 800 */
    CHECK_EQ(purr_rt_open(&rt, "big"), PURR_RT_E_MEMORY);   /* 6000 needed, 5700 free */
    CHECK(purr_rt_get(&rt, 1) != NULL);
    CHECK_EQ(rt.fg_id, 1);
    CHECK_EQ(purr_rt_count(&rt), 1);
}

static void test_admission_keeps_the_system_reserve(void)
{
    reset(3000);
    rt.cfg.reserve = 1500;
    CHECK_EQ(purr_rt_open(&rt, "notes"), PURR_RT_E_MEMORY);  /* 2000 + 1500 > 3000 */
    CHECK_EQ(purr_rt_open(&rt, "clock") > 0, 1);              /* 1000 + 1500 <= 3000 */
}

static void test_the_app_count_cap(void)
{
    reset(1000000);
    rt.cfg.max_apps = 2;
    int a = purr_rt_open(&rt, "clock");
    purr_rt_open(&rt, "shell");
    /* Full: the oldest suspended/background app is stopped to make room. */
    int c = purr_rt_open(&rt, "notes");
    CHECK(c > 0);
    CHECK_EQ(purr_rt_count(&rt), 2);
    CHECK(purr_rt_get(&rt, a) == NULL);
}

static void test_persistent_apps_survive_admission(void)
{
    reset(5000);
    char err[64];
    purr_rt_config_load(&rt, "rule = persistent, name:clock\n", err, sizeof(err));
    purr_rt_open(&rt, "clock");               /* persistent, background after the next launch */
    purr_rt_open(&rt, "shell");
    CHECK_EQ(purr_rt_open(&rt, "big"), PURR_RT_E_MEMORY);
    CHECK(purr_rt_get(&rt, 1) != NULL);
}

/* ------------------------------------------------------------------ memory pressure */

static void configure_pressure(void)
{
    rt.cfg.low = 3000;
    rt.cfg.critical = 1500;
    rt.cfg.margin = 500;
    rt.cfg.min_kill_ms = 0;
}

static void test_low_memory_is_announced_once(void)
{
    reset(5000);
    configure_pressure();
    purr_rt_open(&rt, "notes");               /* used 2000, free 3000: not yet low */
    purr_rt_tick(&rt);
    CHECK_EQ(rt.pressure, 0);
    purr_rt_open(&rt, "shell");               /* used 2800, free 2200: low */
    s_calls[0] = '\0';
    purr_rt_tick(&rt);
    CHECK_EQ(rt.pressure, 1);
    CHECK(saw_event("lowmemory>>1"));
    CHECK_EQ(s_low_level, PURR_APP_MEM_LOW);
    int first = s_nev;
    purr_rt_tick(&rt);
    CHECK_EQ(s_nev, first);                   /* no repeat while it stays low */
}

static void test_critical_memory_stops_victims_in_order(void)
{
    reset(5000);
    configure_pressure();
    purr_rt_open(&rt, "clock");               /* 1000 */
    purr_rt_open(&rt, "notes");               /* 2000 */
    purr_rt_open(&rt, "shell");               /* 800 -> used 3800, free 1200: critical */
    int stopped = purr_rt_tick(&rt);
    CHECK(stopped >= 1);
    CHECK(rt.kills >= 1);
    /* notes (suspended) goes before clock (background) */
    CHECK(saw_event("killed>>2,notes,memory"));
    CHECK(purr_rt_get(&rt, 3) != NULL);       /* the foreground app is never the victim */
    CHECK(saw_event("lowmemory>>2"));
}

static void test_the_minimum_interval_between_kills(void)
{
    reset(5000);
    configure_pressure();
    rt.cfg.min_kill_ms = 1000;
    purr_rt_open(&rt, "clock");
    purr_rt_open(&rt, "notes");
    purr_rt_open(&rt, "shell");
    s_now = 5000;
    purr_rt_tick(&rt);
    int after_first = rt.kills;
    CHECK_EQ(after_first, 1);
    s_used += 2500;                           /* something leaks: critical again */
    purr_rt_tick(&rt);                        /* same instant: too soon for another */
    CHECK_EQ(rt.kills, after_first);
    s_now += 1500;
    purr_rt_tick(&rt);
    CHECK_EQ(rt.kills, after_first + 1);
}

static void test_only_the_foreground_app_left_is_reported_not_killed(void)
{
    reset(1500);
    configure_pressure();
    purr_rt_open(&rt, "shell");               /* 800 of 1500: free 700 < critical */
    CHECK_EQ(purr_rt_tick(&rt), 0);
    CHECK(purr_rt_get(&rt, 1) != NULL);
    CHECK_EQ(rt.pressure, 2);
}

static void test_pressure_has_hysteresis(void)
{
    reset(5000);
    configure_pressure();
    purr_rt_open(&rt, "notes");
    purr_rt_open(&rt, "shell");               /* free 2200 -> low */
    purr_rt_tick(&rt);
    CHECK_EQ(rt.pressure, 1);
    s_used -= 1000;                           /* free 3200: above low but within the margin */
    purr_rt_tick(&rt);
    CHECK_EQ(rt.pressure, 1);
    s_used -= 400;                            /* free 3600: clear of low + margin */
    purr_rt_tick(&rt);
    CHECK_EQ(rt.pressure, 0);
}

/* ------------------------------------------------------------------ unresponsive */

static void test_an_unresponsive_foreground_app_is_reported_once(void)
{
    reset(100000);
    rt.cfg.unresponsive_ms = 5000;
    purr_rt_open(&rt, "notes");
    s_now += 4000;
    purr_rt_tick(&rt);
    CHECK(!saw_event("unresponsive>>1,notes"));
    s_now += 2000;
    purr_rt_tick(&rt);
    CHECK(saw_event("unresponsive>>1,notes"));
    int n = s_nev;
    purr_rt_tick(&rt);
    CHECK_EQ(s_nev, n);
    purr_rt_heartbeat(&rt, 1);                /* it answers again */
    CHECK_EQ(purr_rt_get(&rt, 1)->unresponsive, 0);
}

/* ------------------------------------------------------------------ the bus */

static void test_events_reach_subscribed_apps_only(void)
{
    reset(100000);
    purr_rt_open(&rt, "clock");
    purr_rt_open(&rt, "shell");               /* clock now in the background */
    purr_rt_subscribe(&rt, 1, "custom.");
    s_event_calls = 0;
    purr_rt_emit(&rt, "custom.weather", "rain");
    CHECK_EQ(s_event_calls, 1);
    CHECK(strcmp(s_last_event_name, "custom.weather") == 0);
    CHECK(strcmp(s_last_event_data, "rain") == 0);
    purr_rt_emit(&rt, "other.thing", "x");
    CHECK_EQ(s_event_calls, 1);               /* the prefix did not match */
    purr_rt_unsubscribe(&rt, 1, "custom.");
    purr_rt_emit(&rt, "custom.weather", "sun");
    CHECK_EQ(s_event_calls, 1);
    CHECK(saw_event("custom.weather>>sun"));  /* sinks still saw it */
}

static void test_a_suspended_app_gets_no_events(void)
{
    reset(100000);
    purr_rt_open(&rt, "notes");
    purr_rt_subscribe(&rt, 1, "x");
    purr_rt_open(&rt, "shell");               /* notes suspended */
    s_event_calls = 0;
    purr_rt_emit(&rt, "x.y", "z");
    CHECK_EQ(s_event_calls, 0);
    CHECK_EQ(purr_rt_post(&rt, 1, "x.y", "z"), PURR_RT_E_STATE);
}

static void test_sinks_can_be_removed(void)
{
    reset(100000);
    purr_rt_remove_sink(&rt, sink, NULL);
    purr_rt_open(&rt, "shell");
    CHECK_EQ(s_nev, 0);
}

/* ------------------------------------------------------------------ dispatchers */

static void test_dispatchers(void)
{
    reset(100000);
    char out[256];
    CHECK_EQ(purr_rt_dispatch(&rt, "open clock", out, sizeof(out)), 0);
    CHECK(strstr(out, "opened clock as 1") != NULL);
    CHECK_EQ(purr_rt_dispatch(&rt, "exec shell", out, sizeof(out)), 0);
    CHECK_EQ(rt.fg_id, 2);
    CHECK_EQ(purr_rt_dispatch(&rt, "focus clock", out, sizeof(out)), 0);
    CHECK_EQ(rt.fg_id, 1);
    CHECK_EQ(purr_rt_dispatch(&rt, "cycle", out, sizeof(out)), 0);
    CHECK_EQ(rt.fg_id, 2);
    CHECK_EQ(purr_rt_dispatch(&rt, "cycle", out, sizeof(out)), 0);
    CHECK_EQ(rt.fg_id, 1);                    /* wraps */
    CHECK_EQ(purr_rt_dispatch(&rt, "suspend active", out, sizeof(out)), 0);
    CHECK_EQ(purr_rt_get(&rt, 1)->state, PURR_RT_SUSPENDED);
    CHECK_EQ(purr_rt_dispatch(&rt, "resume clock", out, sizeof(out)), 0);
    CHECK_EQ(purr_rt_get(&rt, 1)->state, PURR_RT_FOREGROUND);
    CHECK_EQ(purr_rt_dispatch(&rt, "background active", out, sizeof(out)), 0);
    CHECK_EQ(purr_rt_get(&rt, 1)->state, PURR_RT_BACKGROUND);
    CHECK_EQ(rt.fg_id, 0);
    CHECK_EQ(purr_rt_dispatch(&rt, "pin shell", out, sizeof(out)), 0);
    CHECK_EQ(purr_rt_get(&rt, 2)->pinned, 1);
    CHECK_EQ(purr_rt_dispatch(&rt, "post clock tick now", out, sizeof(out)), 0);
    CHECK(strcmp(s_last_event_name, "tick") == 0);
    CHECK(strcmp(s_last_event_data, "now") == 0);
    CHECK_EQ(purr_rt_dispatch(&rt, "close 2", out, sizeof(out)), 0);
    CHECK(purr_rt_get(&rt, 2) == NULL);
}

static void test_dispatcher_errors_are_clear(void)
{
    reset(100000);
    char out[256];
    CHECK_EQ(purr_rt_dispatch(&rt, "", out, sizeof(out)), -1);
    CHECK_EQ(purr_rt_dispatch(&rt, "dance", out, sizeof(out)), -1);
    CHECK_EQ(purr_rt_dispatch(&rt, "focus", out, sizeof(out)), -1);
    CHECK_EQ(purr_rt_dispatch(&rt, "focus ghost", out, sizeof(out)), -1);
    CHECK(strstr(out, "no such app: ghost") != NULL);
    CHECK_EQ(purr_rt_dispatch(&rt, "open", out, sizeof(out)), -1);
    CHECK_EQ(purr_rt_dispatch(&rt, "open nosuchapp", out, sizeof(out)), -1);
    CHECK(strstr(out, "could not load") != NULL);
    CHECK_EQ(purr_rt_dispatch(&rt, "cycle", out, sizeof(out)), -1);   /* nothing running */
}

static void test_refs(void)
{
    reset(100000);
    purr_rt_open(&rt, "clock");
    purr_rt_open(&rt, "shell");
    purr_rt_open(&rt, "notes");
    CHECK_EQ(purr_rt_ref(&rt, "2"), 2);
    CHECK_EQ(purr_rt_ref(&rt, "shell"), 2);
    CHECK_EQ(purr_rt_ref(&rt, "active"), 3);
    CHECK_EQ(purr_rt_ref(&rt, "next"), 1);    /* wraps */
    CHECK_EQ(purr_rt_ref(&rt, "prev"), 2);
    CHECK_EQ(purr_rt_ref(&rt, "9"), 0);
    CHECK_EQ(purr_rt_ref(&rt, "12abc"), 0);
    CHECK_EQ(purr_rt_ref(&rt, "ghost"), 0);
}

/* ------------------------------------------------------------------ queries */

static void test_queries(void)
{
    reset(100000);
    char out[1024];
    purr_rt_query(&rt, "apps", out, sizeof(out));
    CHECK(strstr(out, "no apps running") != NULL);
    purr_rt_open(&rt, "notes");
    purr_rt_open(&rt, "sync");
    purr_rt_query(&rt, "apps", out, sizeof(out));
    CHECK(strstr(out, "app 1: notes") != NULL);
    CHECK(strstr(out, "state: foreground") != NULL);
    CHECK(strstr(out, "app 2: sync") != NULL);
    CHECK(strstr(out, "kind: daemon") != NULL);

    purr_rt_query(&rt, "apps -j", out, sizeof(out));
    CHECK(out[0] == '[');
    CHECK(strstr(out, "\"name\":\"notes\"") != NULL);
    CHECK(strstr(out, "\"state\":\"background\"") != NULL);
    CHECK(out[strlen(out) - 1] == ']');

    purr_rt_query(&rt, "activeapp", out, sizeof(out));
    CHECK(strstr(out, "app 1: notes") != NULL);
    purr_rt_query(&rt, "activeapp -j", out, sizeof(out));
    CHECK(out[0] == '{');

    purr_rt_query(&rt, "memory", out, sizeof(out));
    CHECK(strstr(out, "held by apps: 2500") != NULL);
    purr_rt_query(&rt, "version", out, sizeof(out));
    CHECK(strstr(out, "purr-runtime 1") != NULL);
    CHECK_EQ(purr_rt_query(&rt, "bogus", out, sizeof(out)), -1);

    /* A buffer too small for the answer is cut off, never overrun. */
    char tiny[16];
    purr_rt_query(&rt, "apps", tiny, sizeof(tiny));
    CHECK(strlen(tiny) < sizeof(tiny));
}

/* ------------------------------------------------------------------ config */

static const char *s_conf =
    "# the apps config\n"
    "general:max_apps = 4\n"
    "general:reserve_kb = 2\n"
    "general:low_kb = 4\n"
    "general:critical_kb = 1\n"
    "general:margin_kb = 1\n"
    "general:min_kill_ms = 500\n"
    "general:unresponsive_ms = 8000\n"
    "\n"
    "rule = background, name:sy*\n"
    "rule = persistent, class:daemon\n"
    "rule = memory 4, name:notes      # a comment after a value\n"
    "rule = nofocus, name:cfg2\n"
    "rule = focus, name:sync\n"
    "bind = SUPER, Tab, cycle\n"
    "bind = SUPER SHIFT, q, close active\n"
    "bind = CTRL, d, post, active, ping\n"
    "exec-once = shell\n"
    "exec-once = clock\n";

static void test_config_parses_a_clean_file(void)
{
    reset(100000);
    char err[256];
    int bad = purr_rt_config_load(&rt, s_conf, err, sizeof(err));
    CHECK_EQ(bad, 0);
    CHECK_EQ(rt.cfg.max_apps, 4);
    CHECK_EQ(rt.cfg.reserve, 2048);
    CHECK_EQ(rt.cfg.low, 4096);
    CHECK_EQ(rt.cfg.critical, 1024);
    CHECK_EQ(rt.cfg.margin, 1024);
    CHECK_EQ(rt.cfg.min_kill_ms, 500);
    CHECK_EQ(rt.cfg.unresponsive_ms, 8000);
    CHECK_EQ(rt.nrules, 5);
    CHECK_EQ(rt.nbinds, 3);
    CHECK_EQ(rt.nexec, 2);
    CHECK(saw_event("config>>reloaded"));
    char out[512];
    purr_rt_query(&rt, "rules", out, sizeof(out));
    CHECK(strstr(out, "rule 2: memory 4096, name:notes") != NULL);
    purr_rt_query(&rt, "binds", out, sizeof(out));
    CHECK(strstr(out, "SUPER SHIFT q -> close active") != NULL);
    CHECK(strstr(out, "CTRL d -> post active ping") != NULL);   /* Hyprland's comma form folds */
}

static void test_config_reports_bad_lines_and_keeps_going(void)
{
    reset(100000);
    char err[512];
    const char *text =
        "general:max_apps = lots\n"
        "general:nothing = 1\n"
        "rule = frobnicate, name:x\n"
        "rule = background\n"
        "rule = memory, name:x\n"
        "rule = background, color:red\n"
        "rule = background, class:window\n"
        "bind = SUPER, Tab\n"
        "bind = HYPER, Tab, cycle\n"
        "wallpaper = none\n"
        "just text\n"
        "rule = background, name:good\n";
    int bad = purr_rt_config_load(&rt, text, err, sizeof(err));
    CHECK_EQ(bad, 11);
    CHECK_EQ(rt.nrules, 1);                   /* the good line still took effect */
    CHECK(strstr(err, "line 1: a number was expected") != NULL);
    CHECK(strstr(err, "line 3: unknown rule effect") != NULL);
    CHECK(strstr(err, "line 11: expected keyword = value") != NULL);
}

static void test_a_reload_replaces_the_old_rules(void)
{
    reset(100000);
    char err[64];
    purr_rt_config_load(&rt, s_conf, err, sizeof(err));
    purr_rt_config_load(&rt, "rule = background, name:one\n", err, sizeof(err));
    CHECK_EQ(rt.nrules, 1);
    CHECK_EQ(rt.nbinds, 0);
    CHECK_EQ(rt.nexec, 0);
}

static void test_options_the_file_omits_revert_to_defaults(void)
{
    reset(100000);
    rt.cfg.reserve = 1000;
    purr_rt_set_defaults(&rt);
    char err[64];
    purr_rt_config_load(&rt, "general:reserve_kb = 50\n", err, sizeof(err));
    CHECK_EQ(rt.cfg.reserve, 50 * 1024);
    purr_rt_config_load(&rt, "rule = background, name:x\n", err, sizeof(err));
    CHECK_EQ(rt.cfg.reserve, 1000);           /* the line is gone, so the default is back */
}

static void test_rules_shape_what_opens(void)
{
    reset(100000);
    char err[256];
    purr_rt_config_load(&rt, s_conf, err, sizeof(err));
    /* memory override: notes holds 4 KB under the rule, not its 2000-byte header value */
    purr_rt_open(&rt, "notes");
    CHECK_EQ(purr_rt_get(&rt, 1)->memory, 4096);
    /* nofocus: cfg2 opens but does not take the foreground */
    purr_rt_open(&rt, "cfg2");
    CHECK_EQ(rt.fg_id, 1);
    CHECK_EQ(purr_rt_get(&rt, 2)->state, PURR_RT_BACKGROUND);
    /* focus: a daemon with the rule takes the foreground */
    int s = purr_rt_open(&rt, "sync");
    CHECK_EQ(rt.fg_id, s);
    /* background (name glob): sync may keep running when it leaves */
    purr_rt_open(&rt, "shell");
    CHECK_EQ(purr_rt_get(&rt, s)->state, PURR_RT_BACKGROUND);
    /* persistent (class daemon): sync is never a victim */
    CHECK((purr_rt_get(&rt, s)->flags & PURR_APP_F_PERSISTENT) != 0);
}

static void test_a_suspend_rule_beats_background_ok(void)
{
    reset(100000);
    char err[64];
    purr_rt_config_load(&rt, "rule = suspend, name:clock\n", err, sizeof(err));
    purr_rt_open(&rt, "clock");
    purr_rt_open(&rt, "shell");
    CHECK_EQ(purr_rt_get(&rt, 1)->state, PURR_RT_SUSPENDED);
}

static void test_binds_run_dispatchers(void)
{
    reset(100000);
    char err[256];
    purr_rt_config_load(&rt, s_conf, err, sizeof(err));
    purr_rt_open(&rt, "clock");
    purr_rt_open(&rt, "shell");
    CHECK_EQ(rt.fg_id, 2);
    CHECK_EQ(purr_rt_key(&rt, PURR_RT_MOD_SUPER, "Tab"), 1);   /* cycle */
    CHECK_EQ(rt.fg_id, 1);
    CHECK_EQ(purr_rt_key(&rt, PURR_RT_MOD_SUPER, "tab"), 1);   /* key names ignore case */
    CHECK_EQ(rt.fg_id, 2);
    CHECK_EQ(purr_rt_key(&rt, 0, "Tab"), 0);                   /* the mods must match */
    CHECK_EQ(purr_rt_key(&rt, PURR_RT_MOD_SUPER, "x"), 0);
    CHECK_EQ(purr_rt_key(&rt, PURR_RT_MOD_SUPER | PURR_RT_MOD_SHIFT, "q"), 1);   /* close active */
    CHECK(purr_rt_get(&rt, 2) == NULL);
}

static void test_exec_once_opens_in_order(void)
{
    reset(100000);
    char err[256];
    purr_rt_config_load(&rt, s_conf, err, sizeof(err));
    CHECK_EQ(purr_rt_exec_once(&rt), 2);
    CHECK(strcmp(purr_rt_get(&rt, 1)->name, "shell") == 0);
    CHECK(strcmp(purr_rt_get(&rt, 2)->name, "clock") == 0);
    CHECK_EQ(rt.fg_id, 2);
    /* one that cannot open is logged and skipped */
    purr_rt_config_load(&rt, "exec-once = ghost\nexec-once = notes\n", err, sizeof(err));
    int before = purr_rt_count(&rt);
    CHECK_EQ(purr_rt_exec_once(&rt), 1);
    CHECK_EQ(purr_rt_count(&rt), before + 1);
    CHECK(s_nlog > 0);
}

static void test_boot_apps_start_at_boot_and_the_rest_wait_for_a_login(void)
{
    reset(100000);
    char err[128], out[256];
    purr_rt_config_load(&rt, "exec-boot = sync\nexec-once = notes\nexec-once = clock\n", err, sizeof(err));
    CHECK_EQ(rt.nexec_boot, 1);
    CHECK_EQ(rt.nexec, 2);
    purr_rt_query(&rt, "autostart", out, sizeof(out));
    CHECK(strstr(out, "boot: sync") != NULL);
    CHECK(strstr(out, "login: notes") != NULL);

    /* at boot only the essential app opens; nothing else runs with nobody logged in */
    CHECK_EQ(purr_rt_exec_boot(&rt), 1);
    CHECK_EQ(purr_rt_count(&rt), 1);
    CHECK(strcmp(purr_rt_get(&rt, 1)->name, "sync") == 0);
    CHECK(purr_rt_ref(&rt, "notes") == 0);

    /* after the login the rest start, in order */
    CHECK_EQ(purr_rt_exec_once(&rt), 2);
    CHECK_EQ(purr_rt_count(&rt), 3);
    CHECK(purr_rt_ref(&rt, "notes") > 0);
    CHECK(purr_rt_ref(&rt, "clock") > 0);
}

static void test_an_exec_boot_app_that_cannot_open_is_logged_and_skipped(void)
{
    reset(100000);
    char err[64];
    purr_rt_config_load(&rt, "exec-boot = ghost\nexec-boot = rogue\nexec-boot = sync\n", err, sizeof(err));
    CHECK_EQ(purr_rt_exec_boot(&rt), 1);          /* a missing and an unsigned app are skipped */
    CHECK_EQ(purr_rt_count(&rt), 1);
    CHECK(s_nlog >= 2);
}

static void test_the_event_stream_tells_the_whole_story(void)
{
    reset(100000);
    purr_rt_open(&rt, "clock");
    purr_rt_open(&rt, "shell");
    purr_rt_stop(&rt, 2, PURR_APP_DESTROY_USER);
    CHECK(event_index("openapp>>1,clock") < event_index("focus>>1,clock"));
    CHECK(event_index("focus>>1,clock") < event_index("openapp>>2,shell"));
    CHECK(event_index("openapp>>2,shell") < event_index("focus>>2,shell"));
    CHECK(saw_event("state>>1,clock,foreground,background"));
    CHECK(saw_event("state>>2,shell,foreground,stopping"));
    CHECK(event_index("closeapp>>2,shell,closed") > event_index("state>>2,shell,foreground,stopping"));
    /* and it hands focus back */
    CHECK(event_index("focus>>1,clock") != event_index("focus>>1,clock") + 1);
}

/* ------------------------------------------------------------ resident apps: frozen, evicted, services */

static void test_leaving_the_screen_freezes_and_coming_back_thaws(void)
{
    reset(100000);
    int n = purr_rt_open(&rt, "notes");
    purr_rt_open(&rt, "shell");               /* notes leaves the screen: not background-ok, so suspended */
    CHECK_EQ(purr_rt_get(&rt, n)->state, PURR_RT_SUSPENDED);
    CHECK_EQ(s_frozen[1], 1);                 /* notes is handle 1 */
    CHECK_EQ(s_delivered_frozen, 0);
    purr_rt_focus(&rt, n);                    /* RESUME must reach a running task */
    CHECK_EQ(s_frozen[1], 0);
    CHECK_EQ(s_delivered_frozen, 0);
    purr_rt_focus(&rt, 2);                    /* notes frozen again */
    CHECK_EQ(s_frozen[1], 1);
    purr_rt_stop(&rt, n, PURR_APP_DESTROY_USER);   /* its DESTROY must reach a running task too */
    CHECK_EQ(s_delivered_frozen, 0);
}

static void test_a_background_ok_app_is_not_frozen(void)
{
    reset(100000);
    purr_rt_open(&rt, "clock");
    purr_rt_open(&rt, "shell");
    CHECK_EQ(purr_rt_get(&rt, 1)->state, PURR_RT_BACKGROUND);
    CHECK_EQ(s_frozen[2], 0);                 /* clock is handle 2 */
}

static void test_over_the_frozen_cap_the_oldest_is_closed(void)
{
    reset(100000);
    rt.cfg.max_frozen = 2;
    int a = purr_rt_open(&rt, "notes");
    s_now += 10;
    int b = purr_rt_open(&rt, "shell");
    s_now += 10;
    int c = purr_rt_open(&rt, "cfg1");
    s_now += 10;
    CHECK(purr_rt_get(&rt, a) != NULL && purr_rt_get(&rt, b) != NULL);   /* two frozen: at the cap */
    int d = purr_rt_open(&rt, "cfg2");        /* a third would be frozen: notes, the longest unused, goes */
    CHECK(d > 0);
    CHECK(purr_rt_get(&rt, a) == NULL);
    CHECK(purr_rt_get(&rt, b) != NULL && purr_rt_get(&rt, c) != NULL);
    CHECK(saw_event("evicted>>1,notes"));
    CHECK_EQ(s_destroy_reason, PURR_APP_DESTROY_MEMORY);
    CHECK_EQ(s_delivered_frozen, 0);
    CHECK_EQ(rt.fg_id, d);                    /* the app in front is never the one closed */
}

static void test_the_app_just_brought_forward_is_never_evicted(void)
{
    reset(100000);
    rt.cfg.max_frozen = 1;
    int a = purr_rt_open(&rt, "notes");
    s_now += 10;
    int b = purr_rt_open(&rt, "shell");       /* notes frozen: at the cap */
    s_now += 10;
    int c = purr_rt_open(&rt, "cfg1");        /* shell frozen too: notes (older) is closed */
    CHECK(purr_rt_get(&rt, a) == NULL);
    s_now += 10;
    CHECK_EQ(purr_rt_focus(&rt, b), 0);       /* shell back in front, cfg1 frozen: one frozen, fine */
    CHECK(purr_rt_get(&rt, b) != NULL && purr_rt_get(&rt, c) != NULL);
    CHECK_EQ(rt.fg_id, b);
}

static void test_persistent_frozen_apps_are_not_evicted(void)
{
    reset(100000);
    char err[64];
    purr_rt_config_load(&rt, "rule = persistent, name:notes\n", err, sizeof(err));
    rt.cfg.max_frozen = 1;                    /* after the load: a load starts from the defaults */
    int a = purr_rt_open(&rt, "notes");
    s_now += 10;
    int b = purr_rt_open(&rt, "shell");
    s_now += 10;
    purr_rt_open(&rt, "cfg1");                /* two frozen over a cap of one: shell goes, notes is protected */
    CHECK(purr_rt_get(&rt, a) != NULL);
    CHECK(purr_rt_get(&rt, b) == NULL);
}

static void test_a_full_set_of_services_refuses_another(void)
{
    reset(100000);
    rt.cfg.max_services = 1;
    purr_rt_open(&rt, "notes");
    int s1 = purr_rt_open(&rt, "sync");
    CHECK_EQ(purr_rt_get(&rt, s1)->state, PURR_RT_BACKGROUND);
    CHECK_EQ(purr_rt_open(&rt, "sync2"), PURR_RT_E_SERVICES);
    CHECK(saw_event("refused>>sync2,service-slots"));
    CHECK_EQ(purr_rt_count(&rt), 2);
}

static void test_a_background_app_over_the_service_cap_is_frozen_instead(void)
{
    reset(100000);
    rt.cfg.max_services = 1;
    purr_rt_open(&rt, "sync");                /* takes the one service slot */
    int c = purr_rt_open(&rt, "clock");       /* background-ok, in front */
    purr_rt_open(&rt, "shell");               /* clock leaves: no slot left, so suspended and frozen */
    CHECK_EQ(purr_rt_get(&rt, c)->state, PURR_RT_SUSPENDED);
    CHECK_EQ(s_frozen[2], 1);
}

static void test_the_psram_table(void)
{
    uint32_t f, s;
    purr_rt_caps_for_psram(8u * 1024 * 1024 - 100000, &f, &s);
    CHECK_EQ(f, 3);
    CHECK_EQ(s, 2);
    purr_rt_caps_for_psram(4u * 1024 * 1024 - 100000, &f, &s);
    CHECK_EQ(f, 2);
    CHECK_EQ(s, 1);
    purr_rt_caps_for_psram(2u * 1024 * 1024, &f, &s);
    CHECK_EQ(f, 1);
    CHECK_EQ(s, 1);
    purr_rt_caps_for_psram(0, &f, &s);
    CHECK_EQ(f, 1);
    CHECK_EQ(s, 1);
}

static void test_a_config_file_can_lower_the_caps_but_not_raise_them(void)
{
    reset(100000);
    rt.cfg.max_frozen = 3;
    rt.cfg.max_services = 2;
    purr_rt_set_defaults(&rt);
    char err[64];
    purr_rt_config_load(&rt, "general:max_frozen = 1\ngeneral:max_services = 9\n", err, sizeof(err));
    CHECK_EQ(rt.cfg.max_frozen, 1);
    CHECK_EQ(rt.cfg.max_services, 2);         /* 9 is more than the machine allows */
    purr_rt_config_load(&rt, "general:max_frozen = 0\n", err, sizeof(err));
    CHECK_EQ(rt.cfg.max_frozen, 3);           /* 0 means "no cap": not on a machine that has one */
}


/* ------------------------------------------------------------ internal RAM admission */

static void test_internal_ram_is_what_limits_apps_not_the_total(void)
{
    reset(10000000);                          /* the total (PSRAM included) is huge */
    rt.cfg.internal_reserve = 24000;
    s_int_free = 70000;
    const char *names[] = {"notes", "shell", "cfg1", "cfg2", "cfg3"};
    int ids[5];
    for (int i = 0; i < 5; i++) {
        s_now += 10;
        ids[i] = purr_rt_open(&rt, names[i]);
        CHECK(ids[i] > 0);
    }
    /* 70000 -> four apps fit with 24000 left (30000); the fifth needs 34000, so the oldest frozen app is closed to make room */
    CHECK(purr_rt_get(&rt, ids[0]) == NULL);
    CHECK(saw_event("evicted>>1,notes"));
    CHECK(s_int_free >= 24000);               /* the system's reserve is never eaten */
    CHECK_EQ(rt.fg_id, ids[4]);
}

static void test_no_victim_means_a_clear_refusal(void)
{
    reset(10000000);
    rt.cfg.internal_reserve = 24000;
    s_int_free = 30000;                       /* not enough for even one app plus the reserve */
    CHECK_EQ(purr_rt_open(&rt, "notes"), PURR_RT_E_INTERNAL);
    CHECK_EQ(purr_rt_count(&rt), 0);
    CHECK(saw_event("refused>>notes,memory"));
    s_int_free = 100000;                      /* room again: the same app opens */
    CHECK(purr_rt_open(&rt, "notes") > 0);
}

static void test_the_foreground_app_is_never_the_price_of_internal_ram(void)
{
    reset(10000000);
    rt.cfg.internal_reserve = 24000;
    s_int_free = 40000;
    int a = purr_rt_open(&rt, "notes");       /* 30000 left after it: not enough for another plus the reserve */
    CHECK(a > 0);
    CHECK_EQ(purr_rt_open(&rt, "shell"), PURR_RT_E_INTERNAL);   /* the only candidate is in front: refused, not closed */
    CHECK(purr_rt_get(&rt, a) != NULL);
}

static void test_internal_ram_emergency_closes_one_app_per_tick(void)
{
    reset(10000000);
    rt.cfg.internal_reserve = 24000;
    rt.cfg.min_kill_ms = 0;
    s_int_free = 200000;
    int a = purr_rt_open(&rt, "notes");
    s_now += 10;
    int b = purr_rt_open(&rt, "shell");       /* notes is frozen */
    s_int_free = 8000;                        /* something else (a job task) took almost everything: below half the reserve */
    purr_rt_tick(&rt);
    CHECK(purr_rt_get(&rt, a) == NULL);
    CHECK(purr_rt_get(&rt, b) != NULL);       /* the one in front stays */
    CHECK(saw_event("evicted>>1,notes"));
}

static void test_the_internal_reserve_is_a_config_option(void)
{
    reset(100000);
    char err[64];
    purr_rt_config_load(&rt, "general:internal_reserve_kb = 16\n", err, sizeof(err));
    CHECK_EQ(rt.cfg.internal_reserve, 16 * 1024);
}


int main(void)
{
    test_open_and_lifecycle();
    test_an_unsigned_app_never_runs();
    test_failed_create_leaves_nothing();
    test_launch_moves_the_old_foreground_back();
    test_daemons_start_in_the_background();
    test_focus_switches_and_resumes();
    test_state_is_saved_on_leaving_and_restored_on_create();
    test_an_app_without_the_flag_saves_nothing();
    test_close_runs_destroy_and_promotes_the_next_app();
    test_kill_skips_destroy();
    test_closing_releases_memory();
    test_ids_are_not_reused();
    test_admission_stops_the_least_important_app();
    test_admission_never_stops_the_foreground_app();
    test_admission_keeps_the_system_reserve();
    test_the_app_count_cap();
    test_persistent_apps_survive_admission();
    test_low_memory_is_announced_once();
    test_critical_memory_stops_victims_in_order();
    test_the_minimum_interval_between_kills();
    test_only_the_foreground_app_left_is_reported_not_killed();
    test_pressure_has_hysteresis();
    test_an_unresponsive_foreground_app_is_reported_once();
    test_events_reach_subscribed_apps_only();
    test_a_suspended_app_gets_no_events();
    test_sinks_can_be_removed();
    test_dispatchers();
    test_dispatcher_errors_are_clear();
    test_refs();
    test_queries();
    test_config_parses_a_clean_file();
    test_config_reports_bad_lines_and_keeps_going();
    test_a_reload_replaces_the_old_rules();
    test_options_the_file_omits_revert_to_defaults();
    test_rules_shape_what_opens();
    test_a_suspend_rule_beats_background_ok();
    test_binds_run_dispatchers();
    test_exec_once_opens_in_order();
    test_boot_apps_start_at_boot_and_the_rest_wait_for_a_login();
    test_an_exec_boot_app_that_cannot_open_is_logged_and_skipped();
    test_the_event_stream_tells_the_whole_story();
    test_leaving_the_screen_freezes_and_coming_back_thaws();
    test_a_background_ok_app_is_not_frozen();
    test_over_the_frozen_cap_the_oldest_is_closed();
    test_the_app_just_brought_forward_is_never_evicted();
    test_persistent_frozen_apps_are_not_evicted();
    test_a_full_set_of_services_refuses_another();
    test_a_background_app_over_the_service_cap_is_frozen_instead();
    test_the_psram_table();
    test_a_config_file_can_lower_the_caps_but_not_raise_them();
    test_internal_ram_is_what_limits_apps_not_the_total();
    test_no_victim_means_a_clear_refusal();
    test_the_foreground_app_is_never_the_price_of_internal_ram();
    test_internal_ram_emergency_closes_one_app_per_tick();
    test_the_internal_reserve_is_a_config_option();
    TK_DONE("test_runtime");
}
