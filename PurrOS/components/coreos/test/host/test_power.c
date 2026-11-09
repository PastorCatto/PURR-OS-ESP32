#include <string.h>

#include "purr_power.h"
#include "testkit.h"

static uint32_t s_now;
static int s_screen = 1, s_sleep_ok = -1, s_wakes, s_last_owner = -1, s_last_alarm = -1, s_notes;
static char s_note[32];

static uint32_t t_now(void) { return s_now; }
static void t_screen(int on) { s_screen = on; }
static void t_sleep(int ok) { s_sleep_ok = ok; }
static void t_wake(int owner, int alarm, const char *why)
{
    (void)why;
    s_wakes++;
    s_last_owner = owner;
    s_last_alarm = alarm;
}
static void t_notify(int owner, const char *what)
{
    (void)owner;
    s_notes++;
    snprintf(s_note, sizeof(s_note), "%s", what);
}

static const purr_power_plat_t PLAT = {t_now, t_screen, t_sleep, t_wake, t_notify};

static void fresh(purr_power_t *p, uint32_t start)
{
    s_now = start;
    s_screen = 1;
    s_sleep_ok = -1;
    s_wakes = 0;
    s_notes = 0;
    s_last_owner = s_last_alarm = -1;
    purr_power_init(p, &PLAT);
}

static void advance(purr_power_t *p, uint32_t ms)
{
    s_now += ms;
    purr_power_tick(p);
}

static void test_idle_timeout(void)
{
    purr_power_t p;
    fresh(&p, 1000);
    purr_power_set_timeout(&p, 30000);
    advance(&p, 29000);
    CHECK_EQ(s_screen, 1);
    advance(&p, 1000);
    CHECK_EQ(s_screen, 0);
    CHECK_EQ(s_sleep_ok, 1);                    /* off, nothing holding: light sleep allowed */

    /* The first press only wakes the screen and says so, so the caller can drop it. */
    CHECK_EQ(purr_power_activity(&p), 1);
    CHECK_EQ(s_screen, 1);
    CHECK_EQ(purr_power_activity(&p), 0);
    purr_power_tick(&p);
    CHECK_EQ(s_sleep_ok, 0);

    /* Activity restarts the countdown. */
    advance(&p, 20000);
    purr_power_activity(&p);
    advance(&p, 20000);
    CHECK_EQ(s_screen, 1);
    advance(&p, 10000);
    CHECK_EQ(s_screen, 0);

    /* Never. */
    purr_power_activity(&p);
    purr_power_set_timeout(&p, 0);
    advance(&p, 3600000);
    CHECK_EQ(s_screen, 1);
}

static void test_screen_lock(void)
{
    purr_power_t p;
    fresh(&p, 0);
    purr_power_set_timeout(&p, 10000);
    CHECK_EQ(purr_power_hold(&p, 2, PURR_WAKE_SCREEN, 0), 0);
    advance(&p, 60000);
    CHECK_EQ(s_screen, 1);                      /* held: no timeout */
    purr_power_release(&p, 2, PURR_WAKE_SCREEN);
    advance(&p, 9000);
    CHECK_EQ(s_screen, 1);                      /* counts from the release */
    advance(&p, 1000);
    CHECK_EQ(s_screen, 0);

    /* A lock taken with the screen off turns it on. */
    purr_power_hold(&p, 2, PURR_WAKE_SCREEN, 0);
    CHECK_EQ(s_screen, 1);
    purr_power_release_owner(&p, 2);            /* the app stopped */
    advance(&p, 10000);
    CHECK_EQ(s_screen, 0);

    CHECK_EQ(purr_power_hold(&p, 99, PURR_WAKE_SCREEN, 0), -1);
    CHECK_EQ(purr_power_hold(&p, 1, 77, 0), -1);
}

static void test_cpu_lease(void)
{
    purr_power_t p;
    fresh(&p, 0);
    purr_power_screen_request(&p, 0);
    purr_power_tick(&p);
    CHECK_EQ(s_sleep_ok, 1);

    purr_power_hold(&p, 3, PURR_WAKE_CPU, 0);   /* default 30 s */
    purr_power_tick(&p);
    CHECK_EQ(s_sleep_ok, 0);
    advance(&p, 29000);
    CHECK_EQ(s_sleep_ok, 0);
    CHECK_EQ(purr_power_renew(&p, 3, PURR_WAKE_CPU, 10000), 0);
    advance(&p, 9000);
    CHECK_EQ(s_sleep_ok, 0);                    /* renewed to 10 s from 29 s */
    advance(&p, 1000);
    CHECK_EQ(s_sleep_ok, 1);                    /* expired on its own */
    CHECK_EQ(s_notes, 1);
    CHECK(strcmp(s_note, "lease-expired") == 0);
    CHECK_EQ(purr_power_renew(&p, 3, PURR_WAKE_CPU, 10000), -1);   /* gone */

    /* A huge lease is capped. */
    purr_power_hold(&p, 3, PURR_WAKE_CPU, 10u * 60 * 60 * 1000);
    advance(&p, PURR_POWER_LEASE_MAX_MS - 1);
    CHECK_EQ(s_sleep_ok, 0);
    advance(&p, 1);
    CHECK_EQ(s_sleep_ok, 1);
}

static void test_alarm_window(void)
{
    purr_power_t p;
    fresh(&p, 5000);
    purr_power_screen_request(&p, 0);

    CHECK_EQ(purr_power_alarm_set(&p, 1, -1, 1000, 0, 0), -1);     /* under the floor */
    int a = purr_power_alarm_set(&p, 1, -1, 30000, 0, 0);
    CHECK_EQ(a, 0);
    CHECK_EQ(purr_power_next_alarm_in(&p), 30000u);
    advance(&p, 29999);
    CHECK_EQ(s_wakes, 0);
    CHECK_EQ(s_sleep_ok, 1);
    advance(&p, 1);
    CHECK_EQ(s_wakes, 1);
    CHECK_EQ(s_last_owner, 1);
    CHECK_EQ(s_sleep_ok, 0);                    /* the window keeps the cpu up */
    CHECK_EQ(purr_power_next_alarm_in(&p), 30000u);

    purr_power_done(&p, 1);
    purr_power_tick(&p);
    CHECK_EQ(s_sleep_ok, 1);                    /* back to sleep as soon as it says done */

    /* The window also ends on its own. */
    advance(&p, 30000);
    CHECK_EQ(s_wakes, 2);
    CHECK_EQ(s_sleep_ok, 0);
    advance(&p, PURR_POWER_WINDOW_DEFAULT_MS);
    CHECK_EQ(s_sleep_ok, 1);

    /* Cancel. */
    purr_power_alarm_cancel(&p, 1, 0);
    CHECK_EQ(purr_power_next_alarm_in(&p), UINT32_MAX);
    advance(&p, 120000);
    CHECK_EQ(s_wakes, 2);
}

static void test_overrun_cutoff(void)
{
    purr_power_t p;
    fresh(&p, 0);
    purr_power_screen_request(&p, 0);
    purr_power_alarm_set(&p, 4, -1, 10000, 5000, 0);
    for (int i = 0; i < 2; i++) {
        advance(&p, 10000);                     /* wake, never says done */
        advance(&p, 5000);                      /* window runs out */
    }
    CHECK_EQ(s_notes, 0);
    CHECK_EQ(s_wakes, 2);
    advance(&p, 5000);                          /* 3rd wake (t = 30 s) ... */
    advance(&p, 5000);                          /* ... overruns, and is switched off */
    CHECK_EQ(s_wakes, 3);
    CHECK_EQ(s_notes, 1);
    CHECK(strcmp(s_note, "alarm-off") == 0);
    CHECK_EQ(purr_power_next_alarm_in(&p), UINT32_MAX);

    /* Finishing in time resets the streak. */
    fresh(&p, 0);
    purr_power_screen_request(&p, 0);
    purr_power_alarm_set(&p, 4, -1, 10000, 5000, 0);
    /* expire, expire, done, expire, expire: never three in a row. */
    for (int i = 0; i < 5; i++) {
        advance(&p, 10000 * (uint32_t)(i + 1) - s_now);     /* the wake, on the 10 s grid */
        if (i == 2) {
            purr_power_done(&p, 4);
        } else {
            advance(&p, 5000);                              /* the window runs out */
        }
    }
    CHECK_EQ(s_notes, 0);
}

static void test_align_pulls_in(void)
{
    purr_power_t p;
    fresh(&p, 0);
    purr_power_screen_request(&p, 0);
    purr_power_alarm_set(&p, 1, -1, 60000, 0, PURR_ALARM_ALIGN);   /* due 60 s */
    advance(&p, 5000);
    purr_power_alarm_set(&p, 2, -1, 60000, 0, PURR_ALARM_ALIGN);   /* due 65 s: 5 s behind app 1 */
    purr_power_alarm_set(&p, 3, -1, 60000, 0, 0);                  /* due 65 s, will not move */
    advance(&p, 55000);
    CHECK_EQ(s_wakes, 2);                                          /* app 1 and the aligned app 2, one wake */
    advance(&p, 5000);
    CHECK_EQ(s_wakes, 3);                                          /* app 3 on its own time */
}

static void test_owner_cleanup_and_limits(void)
{
    purr_power_t p;
    fresh(&p, 0);
    purr_power_screen_request(&p, 0);
    CHECK_EQ(purr_power_alarm_set(&p, 5, -1, 10000, 0, 0), 0);
    CHECK_EQ(purr_power_alarm_set(&p, 5, -1, 10000, 0, 0), 1);
    CHECK_EQ(purr_power_alarm_set(&p, 5, -1, 10000, 0, 0), -2);    /* two per app */
    CHECK_EQ(purr_power_alarm_set(&p, 5, 0, 20000, 0, 0), 0);      /* replace in place */
    purr_power_hold(&p, 5, PURR_WAKE_CPU, 0);
    purr_power_release_owner(&p, 5);
    CHECK_EQ(purr_power_next_alarm_in(&p), UINT32_MAX);
    purr_power_tick(&p);
    CHECK_EQ(s_sleep_ok, 1);

    /* A window longer than the cap is cut to it. */
    purr_power_alarm_set(&p, 5, -1, 60000, 10u * 60 * 1000, 0);
    advance(&p, 60000);
    CHECK_EQ(s_sleep_ok, 0);
    advance(&p, PURR_POWER_WINDOW_MAX_MS);
    CHECK_EQ(s_sleep_ok, 1);
}

static void test_clock_wrap(void)
{
    purr_power_t p;
    fresh(&p, 0xFFFFFF00u);                     /* 256 ms before the counter wraps */
    purr_power_set_timeout(&p, 1000);
    advance(&p, 900);
    CHECK_EQ(s_screen, 1);
    advance(&p, 200);                           /* now past the wrap */
    CHECK_EQ(s_screen, 0);
    purr_power_screen_request(&p, 0);
    purr_power_alarm_set(&p, 1, -1, 10000, 0, 0);
    advance(&p, 10000);
    CHECK_EQ(s_wakes, 1);
}

static void test_status_text(void)
{
    purr_power_t p;
    char buf[400];
    fresh(&p, 0);
    purr_power_alarm_set(&p, 2, -1, 30000, 0, PURR_ALARM_ALIGN);
    purr_power_hold(&p, 2, PURR_WAKE_CPU, 5000);
    int n = purr_power_status(&p, buf, sizeof(buf));
    CHECK(n > 0 && n < (int)sizeof(buf));
    CHECK(strstr(buf, "app 2 alarm 0: every 30 s") != NULL);
    CHECK(strstr(buf, "app 2 holds the cpu") != NULL);
    char tiny[8];
    n = purr_power_status(&p, tiny, sizeof(tiny));
    CHECK(n < (int)sizeof(tiny));
}

int main(void)
{
    test_idle_timeout();
    test_screen_lock();
    test_cpu_lease();
    test_alarm_window();
    test_overrun_cutoff();
    test_align_pulls_in();
    test_owner_cleanup_and_limits();
    test_clock_wrap();
    test_status_text();
    TK_DONE("test_power");
}
