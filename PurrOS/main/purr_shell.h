/*
 * purr_shell.h - the official shell, whichever one this image was built with (plan7.md).
 *
 * One official shell is compiled per image (Kconfig choice PURR_OFFICIAL_SHELL): the Windows 10 Mobile
 * style shell (purr_w10m.c) or the Android style shell (purr_android.c). Everything outside the shell
 * calls these functions and never the shell's own. Apps never see a shell at all: they use the ui,
 * tile, system and permission catcalls and the OS draws the result.
 *
 * All of them run on the UI service task or hop onto it, so they are safe from any task.
 */
#ifndef PURR_SHELL_H
#define PURR_SHELL_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Builds the shell on the UI service task and returns once it is on screen. Call after
 * purr_lvgl_init() and purr_ui_assets_init(). */
esp_err_t purr_shell_start(void);

/* 0, 90, 180 or 270 degrees clockwise from the panel's own orientation. */
esp_err_t purr_shell_set_orientation(int degrees);

/* Shows the lock screen. It only locks the screen; the first unlock of a boot is the sign-in screen. */
esp_err_t purr_shell_lock(void);

/* Adds a notification to the shell's notification surface (newest first). */
esp_err_t purr_shell_notify(const char *title, const char *text);

/* Re-reads the signed-in user's installed apps and rebuilds the shell's app views. */
esp_err_t purr_shell_refresh_apps(void);

/* The compiled shell's short name: "w10m" or "android". */
const char *purr_shell_name(void);

#ifdef __cplusplus
}
#endif

#endif /* PURR_SHELL_H */
