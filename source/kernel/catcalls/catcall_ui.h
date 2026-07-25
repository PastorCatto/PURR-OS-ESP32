#pragma once
// catcall_ui.h — unified UI catcall contract
//
// UI modules (kittenui, miniwin) register an implementation of this struct.
// Apps call purr_win_*() helpers (purr_win.h) which dispatch through the
// registered implementation — apps never reference LVGL or MiniWin directly.
//
// Handle types are opaque uint32_t tokens. 0 = invalid/NULL handle.
// The backend maps tokens to its internal widget pointers.

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#define CATCALL_UI_VERSION 7

typedef uint32_t purr_win_t;   // window handle
typedef uint32_t purr_wid_t;   // widget handle (label, button, textarea, etc.)

// Widget event types passed to callbacks
typedef enum {
    PURR_EVENT_CLICKED   = 0,
    PURR_EVENT_CHANGED   = 1,   // textarea text changed
    PURR_EVENT_FOCUSED   = 2,
    PURR_EVENT_DEFOCUS   = 3,
    PURR_EVENT_SELECTED  = 4,   // list: highlight moved (no confirm)
    PURR_EVENT_ACTIVATED = 5,   // list: entry confirmed/entered
} purr_event_t;

typedef void (*purr_win_cb_t)(purr_wid_t wid, purr_event_t event, void *user);

// Raw canvas drawing + touch — for widget-dense UIs (e.g. a calculator
// keypad) that draw their own controls as flat colour/text rather than
// creating one native widget per button. PURR-OS-0.11's app model always
// worked this way (see PURR-OS-0.11/devices/apps/app_lua_window.cpp's
// l_win_rect/l_win_button/l_win_wait_touch — even its "buttons" were just
// entries in an app-owned draw list, never real widget-manager objects) and
// never hit a hang class confirmed live in the *current* widget-manager
// backend: closing a window with ~20 native button controls can leave the
// UI task permanently stuck inside the backend's own control-teardown code,
// for reasons that traced clean through every bounded loop involved and
// don't reproduce with a low control count. Until that's understood, apps
// that need many buttons (calculators, keypads, grids) should draw them
// with this API instead of one native button widget each.
typedef void (*purr_win_paint_cb_t)(purr_win_t win, void *user);
typedef void (*purr_win_touch_cb_t)(purr_win_t win, int16_t x, int16_t y,
                                     bool pressed, void *user);

// Text alignment
typedef enum {
    PURR_ALIGN_LEFT   = 0,
    PURR_ALIGN_CENTER = 1,
    PURR_ALIGN_RIGHT  = 2,
} purr_align_t;

// Layout direction for purr_win_begin_row / purr_win_begin_col
typedef enum {
    PURR_LAYOUT_ROW = 0,
    PURR_LAYOUT_COL = 1,
} purr_layout_t;

typedef struct {
    const char  *name;
    uint8_t      catcall_version;   // must equal CATCALL_UI_VERSION

    // ── Window lifecycle ───────────────────────────────────────────────────
    purr_win_t (*win_create)  (const char *title);
    void       (*win_destroy) (purr_win_t win);
    void       (*win_show)    (purr_win_t win);
    void       (*win_hide)    (purr_win_t win);
    void       (*win_clear)   (purr_win_t win);   // remove all child widgets

    // Fires cb when the window's native title-bar close button is clicked,
    // in addition to the backend's own default hide-on-close behavior — lets
    // something outside the app (app_manager) know the user asked to fully
    // close this window, without the backend needing to know what that
    // means. Optional: backends that don't implement it can leave this NULL.
    void       (*win_on_close) (purr_win_t win, purr_win_cb_t cb, void *user);

    // ── Labels ─────────────────────────────────────────────────────────────
    purr_wid_t (*label_create) (purr_win_t win, const char *text);
    void       (*label_set)    (purr_wid_t wid, const char *text);
    void       (*label_align)  (purr_wid_t wid, purr_align_t align);

    // Re-renders an existing label's text in a large, banner-style font
    // (e.g. Milk Bottle's big incoming-message display) — same wid a plain
    // label_create() returned, just a different visual treatment on set.
    // Optional: purr_win_label_set_big() in purr_win.h falls back to
    // label_set() (normal size) for any backend that leaves this NULL, so
    // it's always safe to call, just not always literally "big" everywhere.
    void       (*label_set_big) (purr_wid_t wid, const char *text);

    // ── Buttons ────────────────────────────────────────────────────────────
    purr_wid_t (*btn_create)   (purr_win_t win, const char *label,
                                purr_win_cb_t cb, void *user);
    void       (*btn_enable)   (purr_wid_t wid, bool enabled);

    // ── Text area (scrollable multi-line display + optional input) ─────────
    purr_wid_t (*textarea_create) (purr_win_t win, uint16_t w_pct, uint16_t h_pct);
    void       (*textarea_append)  (purr_wid_t wid, const char *text);
    void       (*textarea_set)     (purr_wid_t wid, const char *text);
    void       (*textarea_clear)   (purr_wid_t wid);
    const char *(*textarea_get)    (purr_wid_t wid);    // current text (backend-owned)
    void       (*textarea_focus)   (purr_wid_t wid);    // show keyboard / cursor
    void       (*textarea_cb)      (purr_wid_t wid, purr_win_cb_t cb, void *user);

    // ── List (flat, non-nested selectable list) ─────────────────────────────
    purr_wid_t (*list_create)       (purr_win_t win, uint16_t w_pct, uint16_t h_pct);
    void       (*list_set_items)    (purr_wid_t wid, const char **items, int count);
    void       (*list_clear)        (purr_wid_t wid);
    int        (*list_get_selected) (purr_wid_t wid);   // -1 if none
    void       (*list_set_selected) (purr_wid_t wid, int index);
    void       (*list_cb)           (purr_wid_t wid, purr_win_cb_t cb, void *user);

    // Icon-per-row variant of list_set_items. icons[i] is an LV_SYMBOL_*-style
    // glyph string (a font codepoint, not a bitmap asset — so no shared image
    // resources are needed) or NULL for that row.
    //
    // Optional, like label_set_big above: purr_win_list_set_items_icon() falls
    // back to plain list_set_items() when a backend leaves this NULL, so it is
    // always safe to call and rows simply lose their glyphs.
    //
    // This began life as cupcake_win_list_set_items_icon(), a Cupcake-only
    // extern that callers reached behind #ifdef CONFIG_PURR_UI_BACKEND_CUPCAKE.
    // That silently dropped every row icon in Milkbar and MSN the moment the
    // device moved to any other backend — the bug that motivated promoting it
    // here. Anything backend-specific that apps actually want ends up like
    // this; better in the contract than behind a preprocessor guard.
    void       (*list_set_items_icon)(purr_wid_t wid, const char **items,
                                       const char **icons, int count);

    // ── Tile grid (scrollable grid of icon+label tiles, e.g. an app's own
    // internal navigation menu) ─────────────────────────────────────────────
    // Optional, like label_set_big above: a backend that doesn't implement
    // this leaves both NULL. purr_win_tile_grid_create() in purr_win.h
    // returns 0 in that case — the CALLER (not this layer) is responsible
    // for falling back to something else (e.g. a plain list), since only
    // the caller knows what a sane fallback looks like for its own content.
    // symbols are LV_SYMBOL_*-style built-in glyph strings, not bitmap
    // assets — keeps this implementable by any LVGL-based backend without
    // needing shared icon resources. colors are backend-interpreted (a
    // backend that can't do per-tile color may just ignore that array).
    purr_wid_t (*tile_grid_create)     (purr_win_t win, uint16_t w_pct, uint16_t h_pct);
    void       (*tile_grid_set_items)  (purr_wid_t grid_wid,
                                         const char **labels, const char **symbols,
                                         const uint32_t *colors,
                                         purr_win_cb_t *cbs, void **users, int count);

    // ── Layout helpers ─────────────────────────────────────────────────────
    // Begin a row or column container inside win. Returns container widget.
    // grow: when true, the container expands to fill the remaining space in
    // its parent's flex layout instead of hugging its own content — needed
    // for any row/col holding percentage-sized children (e.g. a list+preview
    // split), which otherwise collapse to 0 size inside a content-sized
    // parent. false preserves the original hug-content behavior.
    purr_wid_t (*layout_begin) (purr_win_t win, purr_layout_t dir, uint8_t pad, bool grow);
    void       (*layout_end)   (purr_wid_t container);

    // ── Keyboard ───────────────────────────────────────────────────────────
    void (*kb_show) (purr_win_t win, purr_wid_t target_textarea);
    void (*kb_hide) (purr_win_t win);

    // ── Canvas (raw draw + touch, see purr_win_paint_cb_t's doc above) ──────
    // canvas_on_paint's cb is invoked by the backend whenever win's client
    // area needs (re)drawing (creation, restore, overlap uncovering) — call
    // canvas_rect/canvas_text only from inside it, where the backend has a
    // valid paint context set up. canvas_repaint() requests another such
    // call asynchronously (e.g. after app state changes) rather than drawing
    // immediately, since there may be no valid paint context at the call
    // site (a button-style tap handled via canvas_on_touch, for instance).
    void (*canvas_on_paint) (purr_win_t win, purr_win_paint_cb_t cb, void *user);
    void (*canvas_on_touch) (purr_win_t win, purr_win_touch_cb_t cb, void *user);
    void (*canvas_rect)     (purr_win_t win, int16_t x, int16_t y,
                              int16_t w, int16_t h, uint32_t color);
    void (*canvas_text)     (purr_win_t win, int16_t x, int16_t y,
                              const char *text, uint32_t color);
    void (*canvas_repaint)  (purr_win_t win);
    void (*canvas_size)     (purr_win_t win, int16_t *w, int16_t *h);   // client area size

} catcall_ui_t;
