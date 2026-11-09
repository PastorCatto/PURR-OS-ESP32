/*
 * purr_kbdlight.h - the keyboard backlight (the board's "kbd-backlight" device). The level is a setting (keyboard.backlight: 0 off, 1 low, 2 medium,
 * 3 high), applied at start-up, switched off while the screen is off and restored with it.
 */
#ifndef PURR_KBDLIGHT_H
#define PURR_KBDLIGHT_H

#ifdef __cplusplus
extern "C" {
#endif

int purr_kbdlight_present(void);            /* 1 if the backlight driver is bound */
int purr_kbdlight_get(void);                /* the saved level, 0..3 */
int purr_kbdlight_set(int level);           /* applies and saves it; 0, or -1 if there is no backlight */
const char *purr_kbdlight_name(int level);  /* "Off", "Low", "Medium", "High" */
void purr_kbdlight_boot(void);              /* applies the saved level once drivers and settings are up */
void purr_kbdlight_screen(int on);          /* the screen went off or on: the keys follow */

#ifdef __cplusplus
}
#endif

#endif
