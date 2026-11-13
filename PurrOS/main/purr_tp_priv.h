/*
 * purr_tp_priv.h - what the pieces of the Type shell share (plan7.md, "The Type shell").
 *
 * The Type shell is the typographic launcher of the Playtime board: paper, ink and one accent colour (the user's choice in Settings, Personalization), a home
 * screen with an app list and three shortcut tiles, an iPod-style app list, a lock screen with an optional PIN, and a sign-in. It works with a d-pad and with
 * touch: every control is clickable, and a focus manager moves a highlight between them with the arrow keys. It is built into the image instead of the Android
 * (Froyo) shell when the build has CONFIG_PURR_SHELL_TYPE; the Android shell's files are not touched.
 *
 *   purr_tp.c        state, status strip, banners, the shell contract, orientation, brightness, the key hook, start
 *   purr_tp_kbd.c    the focus manager, the key grid (keyboard and PIN pad), small drawing helpers
 *   purr_tp_home.c   the app list, the home screen and its shortcut tiles, the app list screen, the quick menu
 *   purr_tp_lock.c   the lock screen, the PIN, the sign-in
 *   purr_tp_slot.c   the entry points the shell dispatcher (purr_shell.c) calls for the built-in shell
 *
 * UI service task only, unless a function says otherwise.
 */
#ifndef PURR_TP_PRIV_H
#define PURR_TP_PRIV_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

#include "purr_driver_class.h"
#include "purr_w10m_priv.h"
#include "purr_w10m_theme.h"

#define TP_INK    0x14120F
#define TP_PAPER  0xF4F1EA
#define TP_SAND   0xE7E2D5
#define TP_MUTED  0xBDB8AB

#define TP_STRIP_H W10M_STATUS_H            /* the shared sheets (Settings, app windows) start under this */

/* ---- purr_tp.c */
lv_color_t tp_accent(void);                 /* the theme's accent colour: the user's choice */
lv_color_t tp_on_accent(void);              /* ink or paper, whichever reads on the accent */
lv_color_t tp_ink(void);
lv_color_t tp_paper(void);
int tp_signed_in(void);
void tp_signed_in_set(void);
void tp_go_home(void);                      /* the Home key: everything open steps back to the home screen */
void tp_back(void);                         /* the Back key */
void tp_menu_key(void);                     /* the Menu key */
int tp_sheet_open(void);                    /* Settings, the portal, the task manager, a permission question or an app window is in front */
lv_obj_t *tp_strip(void);
void tp_strip_title(const char *t);         /* a title in the middle of the strip ("APPS"); NULL or "" for none */
void tp_clock_strings(char *tb, size_t tcap, char *db, size_t dcap);   /* "20:59" and "THU 12 NOV"; "--:--" until the clock is set */
void tp_note_add(const char *title, const char *text);
int tp_note_count(void);
int tp_note_peek(int i, char *title, size_t tcap, char *text, size_t xcap);   /* 0, or -1 */
void tp_rotate_next(void);
/* The gamepad, if the board has one: its ops (NULL when not), and its handle. */
const purr_gamepad_ops_t *tp_pad(void);
void *tp_pad_handle(void);
/* The lights under the buttons: lit for the buttons that do something on the screen in front (a bit mask of 1 << PURR_PAD_*), nothing happens without a gamepad. */
void tp_pad_hint(int mask);

/* ---- purr_tp_kbd.c: the focus manager. A screen registers what can be focused; arrows move the highlight to the nearest object in that direction. */
void tp_focus_clear(void);
void tp_focus_add(lv_obj_t *obj);
void tp_focus_set(lv_obj_t *obj);
lv_obj_t *tp_focus_get(void);
int tp_focus_key(uint32_t key);             /* an arrow moves the highlight, Enter clicks it: 1 if used */
/* A clickable box in the look of the shell: ink border, a fill (transparent, sand or ink), the accent fill and a thicker border while focused or pressed. */
lv_obj_t *tp_box(lv_obj_t *parent, int fill);
#define TP_FILL_NONE 0
#define TP_FILL_SAND 1
#define TP_FILL_INK  2
/* A label; a NULL colour inherits the text colour of its box (which is what makes the focused look read). */
lv_obj_t *tp_label(lv_obj_t *parent, const char *text, const lv_font_t *font, const lv_color_t *color);

/* The key grid: a keyboard (letters, capitals, symbols) or a PIN pad (digits), drawn in a given area, clickable and focusable. */
typedef enum { TP_KEY_CHAR, TP_KEY_DEL, TP_KEY_SHIFT, TP_KEY_OK, TP_KEY_SPACE } tp_key_kind_t;
typedef void (*tp_key_cb)(void *ud, tp_key_kind_t kind, char ch);
typedef struct tp_grid tp_grid_t;
tp_grid_t *tp_grid_keyboard(lv_obj_t *parent, int x, int y, int w, int h, tp_key_cb cb, void *ud);
tp_grid_t *tp_grid_pinpad(lv_obj_t *parent, int x, int y, int w, int h, tp_key_cb cb, void *ud);
void tp_grid_free(tp_grid_t *g);            /* the record only: the keys are deleted with the object they were made in */
void tp_grid_cycle(tp_grid_t *g);           /* abc, ABC, symbols, abc */
void tp_grid_show(tp_grid_t *g, int on);    /* shows or hides all its keys (hidden keys are not focusable) */
void tp_grid_add_focus(tp_grid_t *g);       /* puts its keys into the focus manager */
lv_obj_t *tp_grid_first(tp_grid_t *g);      /* a good key to start on */

/* ---- purr_tp_home.c */
void tp_home_build(lv_obj_t *screen);
void tp_home_layout(void);
void tp_home_reload(void);                  /* the apps, the tiles or the accent changed */
void tp_home_reset(void);                   /* close the drawer and the menus, show the home screen */
int tp_home_back(void);                     /* Back: closes the menu or the drawer; 1 if one was open */
int tp_home_key(uint32_t key);              /* arrows, Enter, Menu while the home screen is in front: 1 if used */
int tp_home_chord(int button);              /* FUNC held and a face button: the tile for it; 1 if there was one */
void tp_home_focus(void);                   /* take the focus (the screen in front is the home screen again) */
int tp_drawer_is_open(void);
void tp_drawer_open(void);
void tp_open_app(const char *name, int system);    /* system: 0 an installed app, 1 Settings, 2 the Developer Portal */
int tp_home_showing(void);
void tp_home_menu_toggle(void);             /* the quick menu */
/* What is playing, for the cover art in place of the clock: 0 while nothing is (no music app reports this yet). */
int tp_now_playing(char *title, size_t tcap, char *artist, size_t acap);

/* The picture for an app, from the icon pack in the image (the Froyo pictures today; a pack the owner swaps in later takes their place without a code change):
 * fr_<name>, an alias, or the generic one. The shared window code (purr_catrt_ui.c) asks for it too, in the Android skin. Same name and meaning as the Android
 * shell's function, which this image does not have. NULL only if the pack has no generic icon. */
const lv_image_dsc_t *and_app_icon(const char *app);

/* ---- the entry points purr_tp_slot.c gives the names the shell dispatcher calls */
esp_err_t purr_tp_start(void);
esp_err_t purr_tp_set_orientation(int degrees);
esp_err_t purr_tp_lock(void);
esp_err_t purr_tp_notify(const char *title, const char *text);
esp_err_t purr_tp_refresh_apps(void);

/* ---- purr_tp_lock.c */
void tp_lock_build(void);
void tp_lock_layout(void);
void tp_lock_show(void);
void tp_lock_unlock(void);
int tp_lock_is_on(void);
void tp_signin_build(void);
void tp_signin_layout(void);
void tp_signin_hidden_check(void);          /* a session already exists: hide the sign-in screen */
int tp_modal_key(uint32_t key);             /* the lock screen, the PIN pad or the sign-in is up and takes the keys: 1 if used */
int tp_modal_up(void);
void tp_pin_setup_open(void);               /* Set or remove the PIN (from the quick menu) */
int tp_pin_is_set(void);

#endif /* PURR_TP_PRIV_H */
