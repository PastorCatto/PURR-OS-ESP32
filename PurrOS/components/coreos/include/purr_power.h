/*
 * purr_power.h - who may keep the device awake, who wakes it, and when it may sleep (plan10.md).
 *
 * This is policy only: it has no ESP-IDF in it. The things that actually happen (the screen going dark,
 * the CPU being let into light sleep, an app being told to wake) are done through a small platform
 * struct, so the same code runs on the device, in the VM, and in a host test with a fake clock.
 *
 * Owners are small integers (the app instance id; 0 is the system). Everything an owner holds or sets is
 * dropped by purr_power_release_owner() when the app stops.
 *
 *   Screen locks   keep the screen on until released. The runtime drops them when the app leaves the front.
 *   CPU locks      keep the CPU out of light sleep. They carry a lease and expire unless renewed.
 *   Alarms         wake the owner every interval. Each wake opens a window; the owner calls done() when it has
 *                  finished (or the window runs out). Three windows in a row that ran out switch the alarm off.
 *
 * The caller drives time: purr_power_tick() every few hundred ms (and after any wake), purr_power_activity()
 * on a key or touch.
 */
#ifndef PURR_POWER_H
#define PURR_POWER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PURR_WAKE_SCREEN 1
#define PURR_WAKE_CPU    2

#define PURR_ALARM_ALIGN 1                  /* may be moved up to a fifth of its interval early, to share a wake */

#define PURR_POWER_MAX_OWNERS       8
#define PURR_POWER_MAX_ALARMS       2       /* per owner */
#define PURR_POWER_MIN_INTERVAL_MS  5000u
#define PURR_POWER_LEASE_DEFAULT_MS 30000u
#define PURR_POWER_LEASE_MAX_MS     120000u
#define PURR_POWER_WINDOW_DEFAULT_MS 10000u
#define PURR_POWER_WINDOW_MAX_MS    30000u
#define PURR_POWER_MAX_OVERRUNS     3

typedef struct {
    uint32_t (*now_ms)(void);
    void (*screen)(int on);                             /* backlight and panel; called only on a change */
    void (*cpu_sleep_allowed)(int ok);                  /* called only on a change */
    /* An alarm fired for owner. why is "alarm". Called from purr_power_tick. */
    void (*wake)(int owner, int alarm, const char *why);
    /* Something the user should hear about: "alarm-off" (overruns), "lease-expired". May be NULL. */
    void (*notify)(int owner, const char *what);
} purr_power_plat_t;

typedef struct {
    uint32_t interval, next_due, window_ms, window_end;
    uint8_t flags, active, in_window, overruns;
} purr_alarm_t;

typedef struct {
    uint8_t used;
    uint8_t screen_lock;
    uint8_t cpu_lock;
    uint32_t cpu_until;
    purr_alarm_t alarm[PURR_POWER_MAX_ALARMS];
} purr_power_owner_t;

typedef struct {
    const purr_power_plat_t *plat;
    purr_power_owner_t owner[PURR_POWER_MAX_OWNERS];
    int screen_on;
    int cpu_sleep_ok;               /* last value handed to the platform; -1 before the first */
    uint32_t timeout_ms;            /* 0 = never */
    uint32_t last_activity;
    uint32_t wakes, overruns, expired;     /* counters, for the status text */
} purr_power_t;

void purr_power_init(purr_power_t *p, const purr_power_plat_t *plat);
void purr_power_set_timeout(purr_power_t *p, uint32_t ms);

/* A key or touch. Returns 1 if the screen was off and this woke it: the caller then discards the input, so a
 * press that only wakes the screen does not also click something. */
int  purr_power_activity(purr_power_t *p);
/* The user asks for the screen on or off (a sleep key, the lock button). */
void purr_power_screen_request(purr_power_t *p, int on);

/* kind is PURR_WAKE_SCREEN or PURR_WAKE_CPU. lease_ms applies to CPU locks (0 = default, capped); a screen
 * lock lasts until released. 0 on success, -1 for a bad owner or kind. */
int  purr_power_hold(purr_power_t *p, int owner, int kind, uint32_t lease_ms);
int  purr_power_renew(purr_power_t *p, int owner, int kind, uint32_t lease_ms);   /* -1 if not held */
void purr_power_release(purr_power_t *p, int owner, int kind);

/* Sets alarm 0 or 1 (index returned), replacing one with the same index if idx >= 0, else taking a free slot.
 * -1: bad owner or interval shorter than the minimum; -2: no free alarm. window_ms 0 = default. */
int  purr_power_alarm_set(purr_power_t *p, int owner, int idx, uint32_t interval_ms, uint32_t window_ms, int flags);
void purr_power_alarm_cancel(purr_power_t *p, int owner, int idx);
/* The owner has finished the work of its current wake. Closes every open window it has. */
void purr_power_done(purr_power_t *p, int owner);

void purr_power_release_owner(purr_power_t *p, int owner);

/* Expires leases and windows, fires due alarms (and any ALIGN alarm close behind), applies the idle timeout,
 * and tells the platform about the screen and the CPU gate. */
void purr_power_tick(purr_power_t *p);

int  purr_power_screen_is_on(const purr_power_t *p);
int  purr_power_cpu_may_sleep(const purr_power_t *p);
/* Milliseconds until the next alarm is due, or UINT32_MAX if none. The platform arms its wake timer with this. */
uint32_t purr_power_next_alarm_in(const purr_power_t *p);
/* Text for the `power` command and Settings: state, holders, alarms. Returns the length. */
int  purr_power_status(const purr_power_t *p, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* PURR_POWER_H */
