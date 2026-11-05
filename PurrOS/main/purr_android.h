/*
 * purr_android.h - an Android 2.2 (Froyo) style shell for PURR OS, with a QWERTY keyboard in mind (plan7.md,
 * "Android shell build-out").
 *
 * The same job as purr_w10m.h, with the same five entry points: home pages and a dock, an app drawer, a
 * notification shade, a key strip (Menu, Home, Back) and a lock and sign-in screen. It is a client of the app
 * runtime (purr_catrt.h) exactly as the Windows style shell is, and provides the same contract to the shared
 * Settings, Developer Portal, task manager and permission prompt (purr_w10m_priv.h). Callers outside the
 * shell use purr_shell.h; this file is for purr_shell.c.
 */
#ifndef PURR_ANDROID_H
#define PURR_ANDROID_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t purr_android_start(void);
esp_err_t purr_android_set_orientation(int degrees);
esp_err_t purr_android_lock(void);
esp_err_t purr_android_notify(const char *title, const char *text);
esp_err_t purr_android_refresh_apps(void);

#ifdef __cplusplus
}
#endif

#endif /* PURR_ANDROID_H */
