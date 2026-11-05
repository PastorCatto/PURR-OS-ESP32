/*
 * purr_battery.h - the battery, as the OS sees it: whatever driver is bound to the board's "battery" device (battery-adc on the T-Deck Plus).
 *
 * The driver gives a raw reading; this smooths it (the voltage sags under load and wobbles), keeps the last good value so the status strip and
 * Settings never wait on the converter, and refreshes it at most every 20 seconds. Calls are cheap and safe from any task.
 */
#ifndef PURR_BATTERY_H
#define PURR_BATTERY_H

#ifdef __cplusplus
extern "C" {
#endif

/* Fills the smoothed percent (0..100) and the cell voltage in millivolts. Returns 1 if there is a battery reading, else 0 (no driver, or no cell). */
int purr_battery_get(int *percent, int *millivolts);

/* 1 while the charger or USB power is holding the cell's line up (the driver's flag; an older driver never sets it). */
int purr_battery_charging(void);

/* The percent, or -1 when there is no reading (the `system` catcall's battery_percent). */
int purr_battery_percent(void);

#ifdef __cplusplus
}
#endif

#endif
