/*
 * purr_w10m_priv.h - what the shell (purr_w10m.c) shares with its Settings app
 * (purr_w10m_settings.c). Not for anything else; UI service task only.
 */
#ifndef PURR_W10M_PRIV_H
#define PURR_W10M_PRIV_H

#include "lvgl.h"

#define W10M_STATUS_H 18

const lv_image_dsc_t *w10m_img(const char *name);   /* NULL when the asset is missing */
int w10m_disp_w(void);
int w10m_disp_h(void);
void w10m_apply_orientation(int degrees);
void w10m_set_brightness(int level);                /* 1..255 */
int w10m_get_brightness(void);
void w10m_save_display(void);                       /* remember the brightness (system.conf) */

/* Shows or hides the DEV badge in the status strip (developer mode, unsigned apps). UI task only. */
void w10m_dev_badge_update(void);

/* Puts an app window (a sheet over the start screen) at the right depth: above the other sheets, beneath the lock
 * screen and the status strip. UI task only. */
void w10m_place_sheet(lv_obj_t *sheet);

/* The permission prompt (purr_w10m_perm.c): registers how the runtime asks the user. */
void w10m_perm_init(void);

/* Rebuilds the tiles and the app list from the installed apps. UI task only. */
void w10m_apps_changed(void);

/* The Developer Portal: its own app, a sheet over the start screen; opens only in developer mode. */
void w10m_devportal_build(void);
void w10m_devportal_layout(void);
void w10m_devportal_open(void);
int w10m_devportal_is_open(void);

/* The Settings app: a sheet over the start screen. */
void w10m_settings_build(void);
void w10m_settings_layout(void);
void w10m_settings_open(void);
int w10m_settings_is_open(void);

#endif /* PURR_W10M_PRIV_H */
