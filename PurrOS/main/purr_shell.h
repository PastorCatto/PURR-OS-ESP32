/*
 * purr_shell.h - the official shell that is running (plan7.md).
 *
 * The Android style shell is built into every image (the one that always works); any other shell is a loadable
 * module installed under /ui/shells/<id>/ and chosen in Settings (purr_shell_select(), purr_shell_choose()).
 * Everything outside the shell calls these functions and never the shell's own. Apps never see a shell at all: they use the ui,
 * tile, system and permission catcalls and the OS draws the result.
 *
 * All of them run on the UI service task or hop onto it, so they are safe from any task.
 */
#ifndef PURR_SHELL_H
#define PURR_SHELL_H

#include <stddef.h>

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

/* The running shell's id: "android" for the built-in one, the module's id otherwise. */
const char *purr_shell_name(void);

/* Reads /ui/active-shell.conf and, if it names an installed shell, loads it (verifying its signature) and makes its skin the theme's. Call once at start-up,
 * after the file system is up and before the theme and the pictures are set up. If the shell cannot be loaded, or the last starts did not get as far as showing
 * it, the built-in shell is used and the choice is undone. */
void purr_shell_select(void);

/* Call after purr_shell_start() returned: the chosen shell is on screen, so the count of failed starts is cleared, and a notification says why the
 * built-in shell is in use if a chosen one had to be given up. */
void purr_shell_started(void);

/* 1 when the shared screens (Settings, windows) are drawn in the Android skin: the built-in shell, or a module that says so. */
int purr_shell_is_android(void);

/* The installed shells (a folder /ui/shells/<id>/ with shell.cat in it; shell.conf gives name, author, version, skin). Returns how many were put in out. */
#define PURR_SHELL_STATE_UNTESTED      0                /* compatible with this system, never started on it */
#define PURR_SHELL_STATE_GOOD          1                /* compatible, and has started here before (the system marked it good) */
#define PURR_SHELL_STATE_NEEDS_UPDATE  2                /* uses something this system does not have or lays things out differently: the built-in shell runs instead until the shell or the system is updated */
#define PURR_SHELL_STATE_BAD           3                /* unreadable or not a shell */
typedef struct {
    char id[24], name[32], author[32], version[16], built_for[17];
    int skin;                                           /* PURR_SHELL_SKIN_W10M or PURR_SHELL_SKIN_ANDROID (purr_shell_abi.h) */
    int state;                                          /* PURR_SHELL_STATE_*: the shell's own header against this kernel, now (a system update can change it) */
} purr_shell_info_t;
int purr_shell_list(purr_shell_info_t *out, int max);

/* The chosen shell's id, "builtin" for the compiled-in one. */
void purr_shell_chosen(char *id, size_t cap);

/* Chooses a shell for the next start: "builtin" or the id of an installed one. Saved; takes effect at the next restart. 0 on success, -1 if it is not installed. */
int purr_shell_choose(const char *id);

/* The `ui.shell` setting for every shell: "builtin" (also "android"), an installed id, "nav" or "card_shell". get gives what is chosen. */
int purr_shell_mode_get(char *out, size_t cap);
int purr_shell_mode_set(const char *v, char *err, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* PURR_SHELL_H */
