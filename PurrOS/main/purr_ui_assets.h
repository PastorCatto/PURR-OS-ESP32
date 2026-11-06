/*
 * purr_ui_assets.h - the UI's pictures: a pack for the shell, and an optional theme on top (plan1.md "Flash map v2", plan7.md "UI packages").
 *
 * Each shell has its own pack, a file in LittleFS (/ui/<shell>/pack.bin; `purrstrap uiassets` writes the format). A theme chosen in Settings
 * (/ui/active.conf, /ui/themes/<id>/pack.bin) is looked at first, and a name it does not have comes from the shell's pack, so a theme can change a few
 * pictures. Not executable code, not signed itself (the release component that carries a shell's pack is), and each pack carries a CRC32 over the directory
 * and the pixel data, checked in the background after start so a damaged or partly written file is "no pictures", never garbage. The packs are read into
 * PSRAM once at start; purr_asset_t.pixels points into that buffer.
 */#ifndef PURR_UI_ASSETS_H
#define PURR_UI_ASSETS_H

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PURR_ASSET_RGB565    0   /* w*h RGB565 pixels */
#define PURR_ASSET_RGB565A8  1   /* the RGB565 plane, then a w*h 8-bit alpha plane (LVGL's RGB565A8) */

typedef struct {
    char name[24];
    const uint16_t *pixels;   /* host-native RGB565, straight from the mapped partition; for
                               * PURR_ASSET_RGB565A8 the alpha plane follows the pixels directly */
    uint16_t width, height;
    uint8_t color_format;     /* PURR_ASSET_* */
} purr_asset_t;

/* Loads the shell's pack and the active theme from LittleFS and validates them (magic, bounds, CRC32). Safe to call with no file system, or with missing or
 * damaged files: it returns an error, purr_ui_assets_find() then returns NULL for what is missing, and nothing else about LVGL start-up is affected. A theme that
 * was chosen but cannot be used is dropped (the shell's own pack is used) and named by purr_ui_assets_theme_failed(). Call it once, after the file system is up. */
esp_err_t purr_ui_assets_init(void);

/* The theme in use ("" if none), the theme that was chosen but could not be used ("" if none), and this build's shell id ("w10m" or "android"). */
const char *purr_ui_assets_theme(void);
const char *purr_ui_assets_theme_failed(void);
const char *purr_ui_assets_shell(void);

/* Themes installed in LittleFS (/ui/themes/<id>/): at most PURR_UI_THEME_SLOTS at a time. A theme is chosen with purr_ui_theme_select() and takes effect at the next start. */
#define PURR_UI_THEME_SLOTS 3

typedef struct {
    char id[40];            /* the folder name */
    char name[32];          /* theme.conf name=, else the id */
    char author[32];
    char version[12];
    char shell[10];         /* "w10m", "android" or "any" */
    uint8_t on_sd;          /* 1 if it is only on the SD card (choosing it copies it into a slot) */
} purr_theme_info_t;

/* Fills `out` with up to `max` installed themes; returns how many (0 if there are none or no file system). */
int purr_ui_theme_list(purr_theme_info_t *out, int max);
/* Chooses a theme for the next start ("" or NULL: none, the shell's own pictures). 0 on success, -1 if there is no such theme or it could not be saved, -2 if the
 * theme is only on the SD card and all PURR_UI_THEME_SLOTS slots are taken (remove one first). A theme that is only on the SD card is copied into a slot. */
int purr_ui_theme_select(const char *id);
/* How many themes are in the slots in LittleFS, not counting `except` (NULL: count all). */
int purr_ui_theme_slots_used(const char *except);
/* Removes an installed theme's files (and chooses none if it was the chosen one). 0 on success. */
int purr_ui_theme_remove(const char *id);
/* The id chosen for the next start (not necessarily the one in use now), "" if none. */
int purr_ui_theme_chosen(char *id, size_t cap);

/* The active theme's picture of this name, else the shell's pack's. NULL if neither has one. */
const purr_asset_t *purr_ui_assets_find(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* PURR_UI_ASSETS_H */
