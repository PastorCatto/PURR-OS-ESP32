#pragma once
// purr_win.h — unified windowing API for PURR OS apps
//
// Include this header and call purr_win_*() — never call LVGL or MiniWin
// directly from app code. The active UI module (kittenui or miniwin) registers
// a catcall_ui_t backend; these helpers dispatch through it.
//
// Usage:
//   #include "purr_win.h"
//
//   purr_win_t win = purr_win_create("My App");
//   purr_wid_t lbl = purr_win_label(win, "Hello!");
//   purr_win_show(win);

#include "catcall_ui.h"
#include "../core/purr_kernel.h"

// ── Internal dispatch macro ────────────────────────────────────────────────────
// Returns a default value if no UI is registered (graceful no-op).
//
// Both macros hold purr_kernel_ui_lock() for the duration of the call into
// the backend. This is what makes it safe for an app to update its UI from
// a background task (a periodic status refresh, a radio-rx callback, etc.)
// — without it, that task's call could run concurrently with the UI
// backend's own render task mid-frame and corrupt LVGL's internal state.
// _UI_CALL is a GNU statement-expression (supported by the GCC-based ESP-IDF
// toolchain) so the lock can wrap a value-producing call without forcing
// every call site to restructure around an early `return`.

#define _UI_CALL(type, ret, fn, ...) \
    ({ \
        purr_kernel_ui_lock(); \
        const catcall_ui_t *_ui = purr_kernel_ui(); \
        type _r = (_ui && _ui->fn) ? _ui->fn(__VA_ARGS__) : (ret); \
        purr_kernel_ui_unlock(); \
        _r; \
    })

#define _UI_VOID(fn, ...) \
    do { \
        purr_kernel_ui_lock(); \
        const catcall_ui_t *_ui = purr_kernel_ui(); \
        if (_ui && _ui->fn) _ui->fn(__VA_ARGS__); \
        purr_kernel_ui_unlock(); \
    } while(0)

// ── Window lifecycle ──────────────────────────────────────────────────────────

static inline purr_win_t purr_win_create(const char *title) {
    purr_win_t win = _UI_CALL(purr_win_t, 0, win_create, title);
    if (win) purr_kernel_notify_window_created(win);
    return win;
}
static inline void purr_win_destroy(purr_win_t win) {
    _UI_VOID(win_destroy, win);
}
static inline void purr_win_show(purr_win_t win) {
    _UI_VOID(win_show, win);
}
static inline void purr_win_hide(purr_win_t win) {
    _UI_VOID(win_hide, win);
}
static inline void purr_win_clear(purr_win_t win) {
    _UI_VOID(win_clear, win);
}
static inline void purr_win_on_close(purr_win_t win, purr_win_cb_t cb, void *user) {
    _UI_VOID(win_on_close, win, cb, user);
}

// ── Labels ────────────────────────────────────────────────────────────────────

static inline purr_wid_t purr_win_label(purr_win_t win, const char *text) {
    return _UI_CALL(purr_wid_t, 0, label_create, win, text);
}
static inline void purr_win_label_set(purr_wid_t wid, const char *text) {
    _UI_VOID(label_set, wid, text);
}
static inline void purr_win_label_align(purr_wid_t wid, purr_align_t align) {
    _UI_VOID(label_align, wid, align);
}
// Banner-style big text — falls back to a normal-size label_set() on any
// backend that leaves label_set_big NULL, so this is always safe to call
// (just not always literally "big" everywhere; today only Cupcake
// implements it — see catcall_ui.h's own comment on this field).
static inline void purr_win_label_set_big(purr_wid_t wid, const char *text) {
    purr_kernel_ui_lock();
    const catcall_ui_t *_ui = purr_kernel_ui();
    if (_ui && _ui->label_set_big) _ui->label_set_big(wid, text);
    else if (_ui && _ui->label_set) _ui->label_set(wid, text);
    purr_kernel_ui_unlock();
}

// ── Buttons ───────────────────────────────────────────────────────────────────

static inline purr_wid_t purr_win_button(purr_win_t win, const char *label,
                                          purr_win_cb_t cb, void *user) {
    return _UI_CALL(purr_wid_t, 0, btn_create, win, label, cb, user);
}
static inline void purr_win_button_enable(purr_wid_t wid, bool enabled) {
    _UI_VOID(btn_enable, wid, enabled);
}

// ── Textarea ──────────────────────────────────────────────────────────────────

static inline purr_wid_t purr_win_textarea(purr_win_t win,
                                            uint16_t w_pct, uint16_t h_pct) {
    return _UI_CALL(purr_wid_t, 0, textarea_create, win, w_pct, h_pct);
}
static inline void purr_win_textarea_append(purr_wid_t wid, const char *text) {
    _UI_VOID(textarea_append, wid, text);
}
static inline void purr_win_textarea_set(purr_wid_t wid, const char *text) {
    _UI_VOID(textarea_set, wid, text);
}
static inline void purr_win_textarea_clear(purr_wid_t wid) {
    _UI_VOID(textarea_clear, wid);
}
static inline const char *purr_win_textarea_get(purr_wid_t wid) {
    return _UI_CALL(const char *, NULL, textarea_get, wid);
}
static inline void purr_win_textarea_focus(purr_wid_t wid) {
    _UI_VOID(textarea_focus, wid);
}
static inline void purr_win_textarea_on_change(purr_wid_t wid,
                                                purr_win_cb_t cb, void *user) {
    _UI_VOID(textarea_cb, wid, cb, user);
}

// ── List ──────────────────────────────────────────────────────────────────────

static inline purr_wid_t purr_win_list(purr_win_t win,
                                        uint16_t w_pct, uint16_t h_pct) {
    return _UI_CALL(purr_wid_t, 0, list_create, win, w_pct, h_pct);
}
static inline void purr_win_list_set_items(purr_wid_t wid,
                                            const char **items, int count) {
    _UI_VOID(list_set_items, wid, items, count);
}
static inline void purr_win_list_clear(purr_wid_t wid) {
    _UI_VOID(list_clear, wid);
}
static inline int purr_win_list_get_selected(purr_wid_t wid) {
    return _UI_CALL(int, -1, list_get_selected, wid);
}
static inline void purr_win_list_set_selected(purr_wid_t wid, int index) {
    _UI_VOID(list_set_selected, wid, index);
}
static inline void purr_win_list_on_select(purr_wid_t wid,
                                           purr_win_cb_t cb, void *user) {
    _UI_VOID(list_cb, wid, cb, user);
}

// ── Layout ────────────────────────────────────────────────────────────────────

static inline purr_wid_t purr_win_row(purr_win_t win, uint8_t pad) {
    return _UI_CALL(purr_wid_t, 0, layout_begin, win, PURR_LAYOUT_ROW, pad, false);
}
static inline purr_wid_t purr_win_col(purr_win_t win, uint8_t pad) {
    return _UI_CALL(purr_wid_t, 0, layout_begin, win, PURR_LAYOUT_COL, pad, false);
}
// _grow variants: the container expands to fill the remaining space in its
// parent's flex layout instead of hugging its own content — required for a
// row/col whose children are percentage-sized (e.g. a list+preview split),
// since a hug-content parent can't resolve percentage-sized children.
static inline purr_wid_t purr_win_row_grow(purr_win_t win, uint8_t pad) {
    return _UI_CALL(purr_wid_t, 0, layout_begin, win, PURR_LAYOUT_ROW, pad, true);
}
static inline purr_wid_t purr_win_col_grow(purr_win_t win, uint8_t pad) {
    return _UI_CALL(purr_wid_t, 0, layout_begin, win, PURR_LAYOUT_COL, pad, true);
}
static inline void purr_win_layout_end(purr_wid_t container) {
    _UI_VOID(layout_end, container);
}

// ── Keyboard ──────────────────────────────────────────────────────────────────

static inline void purr_win_keyboard_show(purr_win_t win, purr_wid_t target) {
    _UI_VOID(kb_show, win, target);
}
static inline void purr_win_keyboard_hide(purr_win_t win) {
    _UI_VOID(kb_hide, win);
}

// ── Canvas (raw draw + touch) ────────────────────────────────────────────────
// See purr_win_paint_cb_t's doc comment in catcall_ui.h for when to use this
// instead of purr_win_button()/purr_win_label().

static inline void purr_win_canvas_on_paint(purr_win_t win, purr_win_paint_cb_t cb, void *user) {
    _UI_VOID(canvas_on_paint, win, cb, user);
}
static inline void purr_win_canvas_on_touch(purr_win_t win, purr_win_touch_cb_t cb, void *user) {
    _UI_VOID(canvas_on_touch, win, cb, user);
}
static inline void purr_win_canvas_rect(purr_win_t win, int16_t x, int16_t y,
                                         int16_t w, int16_t h, uint32_t color) {
    _UI_VOID(canvas_rect, win, x, y, w, h, color);
}
static inline void purr_win_canvas_text(purr_win_t win, int16_t x, int16_t y,
                                         const char *text, uint32_t color) {
    _UI_VOID(canvas_text, win, x, y, text, color);
}
static inline void purr_win_canvas_repaint(purr_win_t win) {
    _UI_VOID(canvas_repaint, win);
}
static inline void purr_win_canvas_size(purr_win_t win, int16_t *w, int16_t *h) {
    _UI_VOID(canvas_size, win, w, h);
}
