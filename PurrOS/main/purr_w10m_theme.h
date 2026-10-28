/*
 * purr_w10m_theme.h - the Windows 10 Mobile look as data (plan7.md, "Theme and Settings").
 *
 * One place says what the UI looks like: light or dark, the accent colour, whether the sheets are
 * see-through, how opaque the tiles are. The shell and every widget an app makes draw from it:
 *
 *  - the shell adds the shared styles below to its objects (so a change restyles them all at once);
 *  - an LVGL theme layer styles standard widgets (button, switch, slider, dropdown, text area) the
 *    same way, so an app that makes a button through the `ui` catcall gets a W10M button with no
 *    styling code of its own.
 *
 * Everything here runs on the UI service task, the only place LVGL may be called from.
 */
#ifndef PURR_W10M_THEME_H
#define PURR_W10M_THEME_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;
    uint32_t color;
} purr_accent_t;

typedef enum {
    PURR_TH_SHEET,          /* a full-screen sheet: control center, app list, settings (opaque or see-through) */
    PURR_TH_STRIP,          /* the status strip (see-through over the wallpaper, or solid) */
    PURR_TH_TILE,           /* a start tile: accent fill at the tile opacity, darker when pressed */
    PURR_TH_ACCENT_BG,      /* a solid accent fill, darker when pressed */
    PURR_TH_ACCENT_TEXT,    /* text in the accent colour */
    PURR_TH_ACCENT_BORDER,  /* a left edge in the accent colour */
    PURR_TH_TEXT,           /* normal text on a sheet */
    PURR_TH_DIM,            /* secondary text on a sheet */
    PURR_TH_SURFACE,        /* a raised block on a sheet (a notification, a settings row) */
    PURR_TH_ROW_PRESSED,    /* a list row while it is touched */
    PURR_TH_ICON,           /* a line icon (white art with alpha) recoloured to the accent */
    PURR_TH_COUNT
} purr_th_style_t;

/* Builds the styles and installs the LVGL theme layer on the display. Call once, after
 * purr_lvgl_init(), before any widget is made. Then loads the system default settings. */
void purr_theme_init(void);

/* Adds one of the shared styles to an object (and its pressed-state twin where it has one). */
void purr_theme_apply(lv_obj_t *obj, purr_th_style_t which);

int purr_theme_accent_count(void);
const purr_accent_t *purr_theme_accent_at(int index);
int purr_theme_accent(void);                    /* index of the current accent */
int purr_theme_dark(void);                      /* 1 = dark base, 0 = light */
int purr_theme_transparency(void);              /* 1 = see-through sheets, 0 = solid */
int purr_theme_tile_opacity(void);              /* 0..255 */

lv_color_t purr_theme_accent_color(void);
lv_color_t purr_theme_text_color(void);
lv_color_t purr_theme_dim_color(void);
lv_color_t purr_theme_surface_color(void);

/* Each applies at once (every object restyles), saves the settings, and runs the change hook. */
void purr_theme_set_accent(int index);
void purr_theme_set_dark(int dark);
void purr_theme_set_transparency(int on);
void purr_theme_set_tile_opacity(int opa);

/* The shell registers one function to refresh the parts it coloured by hand. */
void purr_theme_on_change(void (*cb)(void));

/* Loads the signed-in user's own settings (/home/<name>/.config/theme.conf) over the system ones.
 * The sign-in screen itself uses the system file, which holds whatever was saved last. */
void purr_theme_load_user(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* PURR_W10M_THEME_H */
