/* purr_wallpaper.c - see purr_wallpaper.h. */
#include "purr_wallpaper.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "purr_settings.h"

#define KEY "display.wallpaper"

static void (*s_listener)(void);

int purr_wallpaper_get(void)
{
    int v = purr_settings_kv_int(KEY, 0);
    return v >= 0 && v <= PURR_WALLPAPER_BUILTIN ? v : 0;
}

int purr_wallpaper_set(int choice)
{
    if (choice < 0 || choice > PURR_WALLPAPER_BUILTIN) {
        return -1;
    }
    char v[8];
    snprintf(v, sizeof(v), "%d", choice);
    if (purr_settings_kv_set(KEY, v) != 0) {
        return -1;
    }
    if (s_listener != NULL) {
        s_listener();
    }
    return 0;
}

/* The designed-in pictures of the default choice: the start screen, the lock screen and the sign-in screen each had their own. */
static const char *const k_default[] = {"img0", "img1", "img3"};

const char *purr_wallpaper_asset(purr_wp_surface_t surface)
{
    static const char *const k_pic[PURR_WALLPAPER_BUILTIN] = {"img0", "img1", "img2", "img3", "img4", "img5", "img6", "img7", "img8", "img9"};
    int c = purr_wallpaper_get();
    if (c == 0) {
        return k_default[surface >= PURR_WP_START && surface <= PURR_WP_SIGNIN ? surface : PURR_WP_START];
    }
    return k_pic[c - 1];
}

const char *purr_wallpaper_name(int choice)
{
    static const char *const k_name[PURR_WALLPAPER_BUILTIN + 1] = {
        "Default", "Picture 1", "Picture 2", "Picture 3", "Picture 4", "Picture 5", "Picture 6", "Picture 7", "Picture 8", "Picture 9", "Picture 10",
    };
    return choice >= 0 && choice <= PURR_WALLPAPER_BUILTIN ? k_name[choice] : "?";
}

void purr_wallpaper_set_listener(void (*changed)(void))
{
    s_listener = changed;
}

static int wp_get(char *out, size_t cap)
{
    snprintf(out, cap, "%d", purr_wallpaper_get());
    return 0;
}

static int wp_set(const char *value, char *err, size_t cap)
{
    char *end = NULL;
    long v = strtol(value, &end, 10);
    if (end == value || *end != '\0' || v < 0 || v > PURR_WALLPAPER_BUILTIN) {
        snprintf(err, cap, "expected 0 (default) or 1-%d", PURR_WALLPAPER_BUILTIN);
        return -1;
    }
    if (purr_wallpaper_set((int)v) != 0) {
        snprintf(err, cap, "could not save");
        return -1;
    }
    return 0;
}

void purr_wallpaper_register(void)
{
    static const purr_setting_t s = {KEY, "wallpaper picture", "0-10 (0 = default)", 0, wp_get, wp_set};
    purr_settings_register(&s);
}
