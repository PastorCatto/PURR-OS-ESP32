/*
 * purr_w10m.h - a Windows 10 Mobile style shell for PURR OS (plan7.md).
 *
 * The look and the structure come from fbiego/windows-10-mobile-lvgl (MIT, copyright (c) 2024
 * Felix Biego; see THIRD_PARTY.md): a start screen of tiles (some of them live), an app list, a
 * lock screen, a status bar, a notification panel, and a navigation bar that appears inside apps.
 * The code is not that project's: it is written for PURR OS's UI service and app runtime, for a
 * 320x240 landscape panel that can also run in any other orientation, and it reads the original
 * art unchanged from the ui_assets partition (purrstrap uiassets import-lvgl).
 *
 * It is a client of the app runtime (purr_catrt.h): tiles stand for apps, and what runs is
 * opened, focused and closed through the runtime's dispatchers and followed through its event
 * stream. Apps never see this file.
 */
#ifndef PURR_W10M_H
#define PURR_W10M_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Builds the shell on the UI service task and returns once it is on screen. Call after
 * purr_lvgl_init() and purr_ui_assets_init(). */
esp_err_t purr_w10m_start(void);

/* 0, 90, 180 or 270 degrees clockwise from the panel's own orientation. Safe from any task. */
esp_err_t purr_w10m_set_orientation(int degrees);

/* Shows the lock screen (Windows 10 Mobile style): swipe up to unlock. It only locks the screen; the
 * first unlock of a boot is the sign-in screen, which comes before anything else. */
esp_err_t purr_w10m_lock(void);

/* Adds a notification to the control center (newest first; tap to dismiss). Safe from any task. */
esp_err_t purr_w10m_notify(const char *title, const char *text);

/* Re-reads the signed-in user's installed apps and rebuilds the tiles and the app list. Call after
 * an app is installed or removed. Safe from any task. */
esp_err_t purr_w10m_refresh_apps(void);

#ifdef __cplusplus
}
#endif

#endif /* PURR_W10M_H */
