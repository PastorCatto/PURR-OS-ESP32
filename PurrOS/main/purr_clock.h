/*
 * purr_clock.h - the device's date and time. There is no battery-backed clock chip, so the time comes from the network (SNTP) whenever the device is
 * online, and the last known time is kept in a file so a start without a network is close to right, not 1970.
 *
 * Settings: time.ntp (on by default) and time.ntp_server (pool.ntp.org by default). The zone and 12/24 hour clock are purr_settings.c's.
 */
#ifndef PURR_CLOCK_H
#define PURR_CLOCK_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* After the file system and the settings are up: puts back the last known time and starts listening for the network. */
void purr_clock_boot(void);

int purr_clock_ntp_enabled(void);
void purr_clock_set_ntp_enabled(int on);                 /* saves it and acts on it now */

/* Asks for a sync right away (needs a network). Returns at once, 1 if a request went out (0: automatic time is off, or there is no network);
 * the result shows in purr_clock_status(). */
int purr_clock_sync_now(void);
/* 1 while that request has not been answered (at most about 8 seconds), so the status bar can show it working. */
int purr_clock_busy(void);

/* One line for the screen: "Synced 3 min ago (pool.ntp.org)", "Waiting for the network...", "Automatic time is off", "Not synced yet". */
void purr_clock_status(char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
