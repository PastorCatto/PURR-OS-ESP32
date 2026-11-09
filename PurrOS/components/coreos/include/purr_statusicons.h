/*
 * purr_statusicons.h - which status-bar picture to show, the way Android 2.2 (Froyo) chose them (plan7.md, "Status icons, Froyo").
 *
 * The rules are read from the Froyo sources (frameworks/base, branch froyo-release): the battery's level list
 * (stat_sys_battery.xml, stat_sys_battery_charge.xml) and WifiManager.calculateSignalLevel. They are plain C so a host test covers
 * them; the shell (purr_android.c) only turns a number into a picture and draws it. The names are the ui_assets names of the
 * icon pack in UI/android-assets/.
 */
#ifndef PURR_STATUSICONS_H
#define PURR_STATUSICONS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PURR_SI_BATTERY_LEVELS 7        /* 0, 10, 20, 40, 60, 80, 100 */
#define PURR_SI_CHARGE_FRAMES  6        /* the charging animation, frames 0 to 5 */

/* Which of the seven battery pictures a percentage gets: 0-4, 5-14, 15-29, 30-49, 50-69, 70-89, 90-100 give 0 to 6.
 * Out of range clamps. */
int purr_si_battery_level(int percent);
const char *purr_si_battery_asset(int percent);        /* "st_b0" ... "st_b100" */
const char *purr_si_battery_unknown_asset(void);       /* "st_bunk": no reading */

/* Charging: the animation starts at the frame for the level (0 up to 14 percent, 1 up to 29, 2 up to 49, 3 up to 69, 4 up to 89, 5
 * above), shows each frame in turn to the last (5) and begins again at the start frame. The start frame is held 2000 ms, the others
 * 1000 ms (stat_sys_battery_charge.xml). At 90 percent and over there is one frame: it stays. */
int purr_si_charge_start(int percent);
int purr_si_charge_next(int frame, int start);         /* the frame after this one */
int purr_si_charge_ms(int frame, int start);           /* how long this frame stays */
const char *purr_si_charge_asset(int frame);           /* "st_bc0" ... "st_bc5" */

/* Wi-Fi: below -100 dBm 0 bars, above -55 dBm 4 bars, in between (dBm + 100) / 11 rounded down (WifiManager.calculateSignalLevel with five
 * levels). */
int purr_si_wifi_level(int rssi_dbm);
const char *purr_si_wifi_asset(int level);             /* "st_w0" ... "st_w4" */

/* Service bars from the LoRa radio: how well the last packet was heard (any packet, not a particular network), 1 to 4 bars; 0 when none was heard
 * in the last ten minutes. Thresholds in dBm: -85 and up 4, -100 and up 3, -112 and up 2, below that 1. */
#define PURR_SI_LORA_STALE_MS (10u * 60u * 1000u)
int purr_si_lora_level(int heard, int rssi_dbm, uint32_t age_ms);

/* Service (cell) bars, 0 to 4; a negative number is "no service" ("st_snull"). */
const char *purr_si_service_asset(int bars);

#ifdef __cplusplus
}
#endif

#endif /* PURR_STATUSICONS_H */
