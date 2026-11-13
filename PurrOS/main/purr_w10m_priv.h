/*
 * purr_w10m_priv.h - what the shell (purr_w10m.c) shares with its Settings app
 * (purr_w10m_settings.c). Not for anything else; UI service task only.
 */
#ifndef PURR_W10M_PRIV_H
#define PURR_W10M_PRIV_H

#include "lvgl.h"
#include "purr_w10m_theme.h"
#include "sdkconfig.h"

/* What the shell reserves at the top (the status strip) and the bottom (the Android shell's key strip: Menu, Home, Back). Every sheet and
 * window sizes itself between them. */
#define W10M_STATUS_H (purr_theme_skin() == PURR_SKIN_ANDROID ? 19 : 18)
#define W10M_NAV_H    (purr_theme_skin() == PURR_SKIN_ANDROID ? 28 : 0)

const lv_image_dsc_t *w10m_img(const char *name);   /* NULL when the asset is missing */
int w10m_disp_w(void);
int w10m_disp_h(void);
void w10m_apply_orientation(int degrees);
void w10m_set_brightness(int level);                /* 1..255 */
int w10m_get_brightness(void);
void w10m_save_display(void);                       /* remember the brightness (system.conf) */

/* Shows or hides the DEV badge in the status strip (developer mode, unsigned apps). UI task only. */
void w10m_dev_badge_update(void);

/* The status bar while something works (the Windows Phone look): the network and battery icons give way to a short message, five dots run along the
 * bar's lower edge, and when the work ends the message says "Done" for half a second and the icons come back. UI task only. A second begin while
 * one is running just changes the message; end without a begin does nothing. Long messages are cut off with "...". */
void w10m_busy_begin(const char *msg);
void w10m_busy_end(void);

/* Any status, error or new notification takes over the strip for 5 seconds (an error in amber); a second one replaces it, and work in progress
 * pauses behind it. w10m_banner is for the UI task, w10m_banner_post for any other task. */
void w10m_banner(const char *msg, int bad);
void w10m_banner_post(const char *msg, int bad);

/* A new notification (the notification shade and the strip) from any task. */
void w10m_note_post(const char *title, const char *text);

/* The button (from w10m_button) that was pressed last, or NULL if it has since been deleted. UI task only. */
lv_obj_t *w10m_last_button(void);

/* Puts an app window (a sheet over the start screen) at the right depth: above the other sheets, beneath the lock
 * screen and the status strip. UI task only. */
void w10m_place_sheet(lv_obj_t *sheet);

/* The permission prompt (purr_w10m_perm.c): registers how the runtime asks the user. */
void w10m_perm_init(void);
int w10m_perm_is_open(void);                        /* a question is on screen: it has to be answered first */
int w10m_perm_key(uint32_t key);                    /* its keys for a board with no touch (d-pad moves, Enter answers, Esc denies); 1 while it is open */

/* Rebuilds the tiles and the app list from the installed apps. UI task only. */
void w10m_apps_changed(void);
/* The same from any task. */
void w10m_apps_changed_post(void);

/* The Developer Portal: its own app, a sheet over the start screen; opens only in developer mode. */
void w10m_devportal_build(void);
void w10m_devportal_layout(void);
void w10m_devportal_open(void);
int w10m_devportal_is_open(void);
lv_obj_t *w10m_devportal_root(void);
void w10m_devportal_back(void);                     /* up one page, or closes it (the Back key) */

/* The task manager (purr_w10m_tasks.c): a list of every running app with its memory and an End button, opened from the control center's Tasks tile. UI task only. */
void w10m_tasks_open(void);                         /* a second call goes back */
void w10m_tasks_close(void);
int w10m_tasks_is_open(void);
lv_obj_t *w10m_tasks_root(void);
/* The lock screen or the sign-in screen is up: nothing behind it may be opened. */
int w10m_is_locked(void);

/* The Settings app: a sheet over the start screen. */
void w10m_settings_build(void);
void w10m_settings_layout(void);
void w10m_settings_open(void);
int w10m_settings_is_open(void);
lv_obj_t *w10m_settings_root(void);
void w10m_settings_back(void);                      /* up one page, or closes it from the first page (the Back key) */

#endif /* PURR_W10M_PRIV_H */
