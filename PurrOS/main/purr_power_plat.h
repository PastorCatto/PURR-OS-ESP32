/*
 * purr_power_plat.h - the device's power manager (plan10.md): one purr_power_t, the thread that ticks it, the
 * backlight, the settings keys, and the `power` command. The policy itself is coreos/purr_power.c.
 *
 * Every function takes the manager's lock, so any task may call them.
 */
#ifndef PURR_POWER_PLAT_H
#define PURR_POWER_PLAT_H

#include <stddef.h>
#include <stdint.h>

#include "purr_cli.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the tick task and registers power.screen_timeout / power.lightsleep. After the settings store is up. */
void purr_power_sys_start(void);

/* The shell registers how to draw the whole screen again; the power manager calls it after the panel wakes, before the backlight comes up. It may be
 * called from any task (it must handle that). */
void purr_power_sys_set_repaint(void (*repaint)(void));

/* The shell hears the screen go off (on the power task, before the backlight goes dark), so it can lock itself: whatever is on the screen when it
 * wakes again is the lock screen. Called with on = 0 only. */
void purr_power_sys_set_screen_hook(void (*hook)(int on));

/* A key or touch. Returns 1 if the screen was off and this press only woke it: the input path then drops it. */
int  purr_power_sys_activity(void);

void purr_power_sys_screen(int on);
int  purr_power_sys_screen_is_on(void);
void purr_power_sys_set_timeout_s(int seconds);        /* 0 = never */

int  purr_power_sys_hold(int owner, int kind, uint32_t lease_ms);
int  purr_power_sys_renew(int owner, int kind, uint32_t lease_ms);
void purr_power_sys_release(int owner, int kind);
int  purr_power_sys_alarm_set(int owner, int idx, uint32_t interval_ms, uint32_t window_ms, int flags);
void purr_power_sys_alarm_cancel(int owner, int idx);
void purr_power_sys_done(int owner);
void purr_power_sys_release_owner(int owner);
int  purr_power_sys_status(char *out, size_t cap);
uint32_t purr_power_sys_next_alarm_in(void);

/* Called (on the power task) when an alarm fires. Set by the app runtime. */
typedef void (*purr_power_wake_fn)(int owner, int alarm, const char *why);
typedef void (*purr_power_notify_fn)(int owner, const char *what);
void purr_power_sys_set_hooks(purr_power_wake_fn wake, purr_power_notify_fn notify);

/* The `power` shell command: power | power screen on|off | power timeout <seconds|never>. */
int  purr_power_cmd(purr_cli_t *cli, int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* PURR_POWER_PLAT_H */
