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
 *
 * There are two skins over the same roles, chosen at start-up by the shell that runs (purr_theme_set_skin(): the built-in Android shell, or a loaded shell's own skin): the Windows 10 Mobile
 * look (flat, square, accent fills) and the Android 2.2 look (grey bevelled buttons with small corners, black list rows
 * with a divider, orange when pressed or selected, a gradient title bar). Callers name the role (a button, a list row, a
 * title bar) and the skin decides how it looks, so shared code, apps' windows included, runs in either.
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
    PURR_TH_BUTTON,         /* a clickable object that acts as a button (apply to a plain object; lv_button already gets this) */
    PURR_TH_LIST_ROW,       /* a tappable list row: its fill, edge or divider, and the pressed look (the three W10M row roles in one) */
    PURR_TH_TITLE_BAR,      /* the header strip of a sheet or window; see purr_theme_title_bar() */
    PURR_TH_COUNT
} purr_th_style_t;

/* Builds the styles and installs the LVGL theme layer on the display. Call once, after
 * purr_lvgl_init(), before any widget is made. Then loads the system default settings. */
void purr_theme_init(void);

/* Adds one of the shared styles to an object (and its pressed-state twin where it has one). */
void purr_theme_apply(lv_obj_t *obj, purr_th_style_t which);

typedef enum {
    PURR_SKIN_W10M,
    PURR_SKIN_ANDROID,
} purr_skin_t;

/* Which skin this image was built with. */
purr_skin_t purr_theme_skin(void);

/* Chooses the skin (and so the shell whose pictures and layout are used). Call once at start-up, before purr_theme_init(): purr_shell_select() does. */
void purr_theme_set_skin(purr_skin_t skin);

/* Dresses a window or sheet header made of a container, a back arrow and a title: the title bar role on the
 * container, and in the Android skin the arrow is hidden (Back is a key there) and the title moves to the left edge.
 * The container stays clickable and goes back when tapped, in both skins. */
void purr_theme_title_bar(lv_obj_t *head, lv_obj_t *arrow, lv_obj_t *title);

/* The text colour that reads on a PURR_TH_BUTTON: white on the W10M accent fill, dark on the Android grey bevel. */
lv_color_t purr_theme_button_text_color(void);

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
