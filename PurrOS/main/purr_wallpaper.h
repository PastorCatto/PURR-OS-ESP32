/*
 * purr_wallpaper.h - which wallpaper the device shows. Not part of any shell: the choice is the device-wide setting `display.wallpaper`
 * in /etc/system.conf, so it survives a reboot and stays when the user switches the shell (the Android and Windows 10 Mobile shells, navigation).
 * A shell asks which picture to draw on each surface and draws it; it listens with purr_wallpaper_set_listener() to redraw when the
 * choice changes.
 *
 *   0         the default: the start screen, lock screen and sign-in screen each keep the picture they were designed with
 *   1 .. 10   one of the pictures (img0 .. img9) the shell's picture pack, or a theme layered over it, carries, on every surface
 *
 * The official packs carry one picture (img0). A picture that is not in the packs is replaced by img0, and the menus list only the pictures there are
 * (purr_wallpaper_count()), so a theme or a second pack can add img1, img2 ... and the choices grow with them.
 */
#ifndef PURR_WALLPAPER_H
#define PURR_WALLPAPER_H

#ifdef __cplusplus
extern "C" {
#endif

#define PURR_WALLPAPER_BUILTIN 10

typedef enum {
    PURR_WP_START,
    PURR_WP_LOCK,
    PURR_WP_SIGNIN,
} purr_wp_surface_t;

/* The current choice, 0 .. PURR_WALLPAPER_BUILTIN. A missing or bad stored value reads as 0. */
int purr_wallpaper_get(void);

/* Saves the choice and tells the listener. 0 on success, -1 if choice is out of range or could not be saved. */
int purr_wallpaper_set(int choice);

/* The ui_assets name to draw on a surface for the current choice. */
const char *purr_wallpaper_asset(purr_wp_surface_t surface);

/* How many choices a menu offers: 1 (the default) plus the pictures present, counted from img0 without a gap. At least 1. */
int purr_wallpaper_count(void);

/* The name of choice n for a menu: "Default", "Picture 1" ... */
const char *purr_wallpaper_name(int choice);

/* Called after every change, on whichever task changed it (a shell hops to its own UI task if it needs to). */
void purr_wallpaper_set_listener(void (*changed)(void));

/* Registers the `display.wallpaper` setting so the Settings app, the `settings` command and scripts can change it. */
void purr_wallpaper_register(void);

#ifdef __cplusplus
}
#endif

#endif /* PURR_WALLPAPER_H */
