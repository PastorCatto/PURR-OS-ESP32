/* purr_power_plat.c - the device's power manager. See purr_power_plat.h and plan10.md. */
#include "purr_power_plat.h"
#include "purr_kbdlight.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "purr_display.h"
#include "purr_kernel.h"
#include "purr_power.h"
#include "purr_settings.h"

static const char *TAG = "power";

#define TICK_MS 250
#define DEFAULT_TIMEOUT_S 60

static purr_power_t s_p;
static SemaphoreHandle_t s_mu;
static int s_started;
static purr_power_wake_fn s_wake_hook;
static purr_power_notify_fn s_notify_hook;
static int s_lightsleep;                        /* power.lightsleep: stored now, acted on in plan10 phase 5 */

static uint32_t plat_now(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* The brightness the user chose lives in the display.brightness setting (the shell owns it); read it back
 * rather than keeping a second copy. Without a shell (the recovery profile) it is full. */
static int user_brightness(void)
{
    int b = purr_settings_kv_int("display.brightness", 255);
    return b < 1 ? 255 : (b > 255 ? 255 : b);
}

static void (*s_repaint)(void);
static void (*s_screen_hook)(int on);

void purr_power_sys_set_screen_hook(void (*hook)(int on))
{
    s_screen_hook = hook;
}

void purr_power_sys_set_repaint(void (*repaint)(void))
{
    s_repaint = repaint;
}

/* Off: the backlight goes dark first (so nothing is seen while the panel is told to stop), then the panel sleeps: it stops driving the glass, which is what
 * a long idle needs. On: the panel wakes (its driver waits the 120 ms the controller needs), the shell draws the current picture (anything drawn while the
 * panel slept was dropped), and only then the backlight comes up, so a stale frame is never shown. A display with no power control (the e-paper) only has
 * the backlight step. */
static void apply_screen(int on)
{
    const purr_display_v2_t *d = purr_kernel_display();
    int panel = d != NULL && d->set_power != NULL && (d->features & PURR_DISPLAY_F_POWER) != 0;
    if (!on && s_screen_hook != NULL) {
        s_screen_hook(0);                        /* the shell locks while the picture is still being shown, so wake finds the lock screen */
    }
    if (!on && d != NULL && d->set_brightness != NULL) {
        d->set_brightness(0);
    }
    if (panel) {
        esp_err_t e = d->set_power(on != 0);
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "panel %s failed (%d)", on ? "wake" : "sleep", (int)e);
        }
    }
    if (on) {
        if (panel && s_repaint != NULL) {
            s_repaint();
        }
        if (d != NULL && d->set_brightness != NULL) {
            d->set_brightness((uint8_t)user_brightness());
        }
    }
    purr_kbdlight_screen(on);                   /* the keys follow the screen */
    ESP_LOGI(TAG, "screen %s%s", on ? "on" : "off", panel ? " (panel)" : "");
}

/* The power manager asks for the screen while it holds its lock, and waking it waits on the UI task (which may be blocked on that same lock in its input
 * callback). So the request is only written down here and the power task carries it out after the lock is dropped, as soon as it is nudged. */
static volatile int s_screen_want = -1;
static TaskHandle_t s_tick_task;

static void plat_screen(int on)
{
    __atomic_store_n(&s_screen_want, on ? 1 : 0, __ATOMIC_RELEASE);
    if (s_tick_task != NULL) {
        xTaskNotifyGive(s_tick_task);
    } else {
        apply_screen(on);                        /* before the task exists (early boot): nothing to deadlock with */
        __atomic_store_n(&s_screen_want, -1, __ATOMIC_RELEASE);
    }
}

static void plat_cpu(int ok)
{
    /* Light sleep itself comes with the hardware phase (esp_pm); the decision is already made and logged. */
    ESP_LOGI(TAG, "cpu may sleep: %s%s", ok ? "yes" : "no", ok && !s_lightsleep ? " (light sleep is off)" : "");
}

/* The hooks run outside the lock: they post to an app's queue and must never be able to deadlock against a
 * call that came in from that app. So the tick collects what fired and runs it after unlocking. */
typedef struct {
    int owner, alarm;
    char why[12];
} fired_t;
static fired_t s_fired[8];
static int s_nfired;

static void plat_wake(int owner, int alarm, const char *why)
{
    ESP_LOGI(TAG, "alarm: app %d alarm %d", owner, alarm);
    if (s_nfired < (int)(sizeof(s_fired) / sizeof(s_fired[0]))) {       /* delivered after the lock is dropped */
        s_fired[s_nfired].owner = owner;
        s_fired[s_nfired].alarm = alarm;
        snprintf(s_fired[s_nfired].why, sizeof(s_fired[0].why), "%s", why);
        s_nfired++;
    }
}

static void plat_notify(int owner, const char *what)
{
    ESP_LOGW(TAG, "app %d: %s", owner, what);
    if (s_notify_hook) {
        s_notify_hook(owner, what);
    }
}

static const purr_power_plat_t s_plat = {plat_now, plat_screen, plat_cpu, plat_wake, plat_notify};

static void lock(void)
{
    if (s_mu) {
        xSemaphoreTakeRecursive(s_mu, portMAX_DELAY);
    }
}

static void unlock(void)
{
    if (s_mu) {
        xSemaphoreGiveRecursive(s_mu);
    }
}

static void tick_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(TICK_MS));          /* a tick every TICK_MS, or sooner when the screen is to change */
        int want = __atomic_exchange_n(&s_screen_want, -1, __ATOMIC_ACQ_REL);
        if (want >= 0) {
            apply_screen(want);
        }
        fired_t out[8];
        int n;
        lock();
        s_nfired = 0;
        purr_power_tick(&s_p);
        n = s_nfired;
        memcpy(out, s_fired, sizeof(out));
        unlock();
        for (int i = 0; i < n; i++) {
            if (s_wake_hook) {
                s_wake_hook(out[i].owner, out[i].alarm, out[i].why);
            }
        }
    }
}

/* --------------------------------------------------------------------------- settings */

static int timeout_get(char *o, size_t c)
{
    lock();
    unsigned t = s_p.timeout_ms / 1000;
    unlock();
    if (t == 0) {
        snprintf(o, c, "never");
    } else {
        snprintf(o, c, "%u", t);
    }
    return 0;
}

static int timeout_set(const char *v, char *e, size_t c)
{
    int s = -1;
    if (strcmp(v, "never") == 0 || strcmp(v, "0") == 0) {
        s = 0;
    } else {
        char *end = NULL;
        long n = strtol(v, &end, 10);
        if (end != v && *end == '\0' && n >= 5 && n <= 3600) {
            s = (int)n;
        }
    }
    if (s < 0) {
        snprintf(e, c, "expected 5-3600 seconds, or never");
        return -1;
    }
    purr_power_sys_set_timeout_s(s);
    char buf[12];
    snprintf(buf, sizeof(buf), "%d", s);
    return purr_settings_kv_set("power.screen_timeout", buf) == 0 ? 0 : (snprintf(e, c, "could not save"), -1);
}

static int light_get(char *o, size_t c) { snprintf(o, c, "%s", s_lightsleep ? "on" : "off"); return 0; }

static int light_set(const char *v, char *e, size_t c)
{
    int on;
    if (purr_settings_parse_bool(v, &on) != 0) {
        snprintf(e, c, "expected on|off");
        return -1;
    }
    s_lightsleep = on;
    return purr_settings_kv_set("power.lightsleep", on ? "1" : "0") == 0 ? 0 : (snprintf(e, c, "could not save"), -1);
}

static const purr_setting_t s_keys[] = {
    {"power.screen_timeout", "seconds of no input before the screen turns off", "5-3600|never", 0, timeout_get,
     timeout_set},
    {"power.lightsleep", "let the CPU light-sleep while the screen is off (needs real hardware)", "on|off", 1,
     light_get, light_set},
};

/* --------------------------------------------------------------------------- the API */

void purr_power_sys_start(void)
{
    if (s_started) {
        return;
    }
    s_mu = xSemaphoreCreateRecursiveMutex();
    s_p.plat = NULL;
    purr_power_init(&s_p, &s_plat);
    int t = purr_settings_kv_int("power.screen_timeout", DEFAULT_TIMEOUT_S);
    purr_power_set_timeout(&s_p, t <= 0 ? 0 : (uint32_t)t * 1000u);
    s_lightsleep = purr_settings_kv_int("power.lightsleep", 0);
    for (size_t i = 0; i < sizeof(s_keys) / sizeof(s_keys[0]); i++) {
        purr_settings_register(&s_keys[i]);
    }
    s_started = 1;
    xTaskCreate(tick_task, "power", 4096, NULL, 4, &s_tick_task);
    ESP_LOGI(TAG, "up: screen timeout %s s, light sleep %s", t <= 0 ? "never" : "set", s_lightsleep ? "on" : "off");
}

void purr_power_sys_set_hooks(purr_power_wake_fn wake, purr_power_notify_fn notify)
{
    s_wake_hook = wake;
    s_notify_hook = notify;
}

int purr_power_sys_activity(void)
{
    if (!s_started) {
        return 0;
    }
    lock();
    int woke = purr_power_activity(&s_p);
    unlock();
    return woke;
}

void purr_power_sys_screen(int on)
{
    lock();
    purr_power_screen_request(&s_p, on);
    unlock();
}

int purr_power_sys_screen_is_on(void)
{
    lock();
    int on = purr_power_screen_is_on(&s_p);
    unlock();
    return on;
}

void purr_power_sys_set_timeout_s(int seconds)
{
    lock();
    purr_power_set_timeout(&s_p, seconds <= 0 ? 0 : (uint32_t)seconds * 1000u);
    unlock();
}

#define LOCKED(expr)          \
    do {                      \
        lock();               \
        r = (expr);           \
        unlock();             \
    } while (0)

int purr_power_sys_hold(int owner, int kind, uint32_t lease_ms)
{
    int r;
    LOCKED(purr_power_hold(&s_p, owner, kind, lease_ms));
    return r;
}

int purr_power_sys_renew(int owner, int kind, uint32_t lease_ms)
{
    int r;
    LOCKED(purr_power_renew(&s_p, owner, kind, lease_ms));
    return r;
}

void purr_power_sys_release(int owner, int kind)
{
    lock();
    purr_power_release(&s_p, owner, kind);
    unlock();
}

int purr_power_sys_alarm_set(int owner, int idx, uint32_t interval_ms, uint32_t window_ms, int flags)
{
    int r;
    LOCKED(purr_power_alarm_set(&s_p, owner, idx, interval_ms, window_ms, flags));
    return r;
}

void purr_power_sys_alarm_cancel(int owner, int idx)
{
    lock();
    purr_power_alarm_cancel(&s_p, owner, idx);
    unlock();
}

void purr_power_sys_done(int owner)
{
    lock();
    purr_power_done(&s_p, owner);
    unlock();
}

void purr_power_sys_release_owner(int owner)
{
    lock();
    purr_power_release_owner(&s_p, owner);
    unlock();
}

int purr_power_sys_status(char *out, size_t cap)
{
    int r;
    LOCKED(purr_power_status(&s_p, out, cap));
    return r;
}

uint32_t purr_power_sys_next_alarm_in(void)
{
    lock();
    uint32_t r = purr_power_next_alarm_in(&s_p);
    unlock();
    return r;
}

/* --------------------------------------------------------------------------- the command */

int purr_power_cmd(purr_cli_t *cli, int argc, char **argv)
{
    char buf[512];
    if (argc >= 2 && strcmp(argv[1], "screen") == 0) {
        int on;
        if (argc < 3 || purr_settings_parse_bool(argv[2], &on) != 0) {
            purr_cli_printf(cli, "usage: power screen on|off\n");
            return 1;
        }
        purr_power_sys_screen(on);
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "timeout") == 0) {
        char err[48] = "";
        if (argc < 3 || timeout_set(argv[2], err, sizeof(err)) != 0) {
            purr_cli_printf(cli, "%s\n", err[0] ? err : "usage: power timeout <seconds|never>");
            return 1;
        }
        return 0;
    }
    if (argc >= 2) {
        purr_cli_printf(cli, "usage: power [screen on|off | timeout <seconds|never>]\n");
        return 1;
    }
    purr_power_sys_status(buf, sizeof(buf));
    purr_cli_printf(cli, "%s", buf);
    return 0;
}
