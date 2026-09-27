// cardstack_win.c — catcall_ui_t backend for Cardstack (LVGL)
//
// Ported near-verbatim from kittenui_win.c — both are plain LVGL (lv_win/
// lv_label/lv_btn/lv_textarea/lv_keyboard), nothing KittenUI-specific in it,
// so the same widget mapping works unchanged here. Registers via
// purr_kernel_register_ui() during cardstack module init.
//
// App windows are created hidden, parented directly to lv_scr_act() — same
// layer as the card stack itself, so win_show() pops the app full-screen on
// top of the stack rather than living inside any one card. A close button
// is added to every window's title bar (kittenui's caller added one from
// the desktop shell instead; cardstack has no separate desktop shell, so it
// has to live here) that just hides the window, returning to the stack.

#include "lvgl.h"
#include "esp_heap_caps.h"
#include "../../kernel/catcalls/catcall_ui.h"
#include "../../kernel/core/purr_kernel.h"
#include "cardstack.h"

#define MAX_WINS  16
#define MAX_WIDS  128

static lv_obj_t *s_wins[MAX_WINS];
static lv_obj_t *s_wids[MAX_WIDS];

// Active purr_win_row()/purr_win_col() container per window, if any — see
// content_parent()'s comment for why this exists.
static lv_obj_t *s_active_layout[MAX_WINS];
// For a layout container's wid, which window index owns it, so layout_end()
// (which only receives the container's wid) can clear the right slot above.
static int s_layout_owner_win[MAX_WIDS];

// Optional extra callback fired when a window's native close button is
// clicked (see win_on_close in catcall_ui.h) — app_manager uses this to
// actually stop the app on Close, distinct from Minimize which just hides.
typedef struct { purr_win_cb_t cb; void *user; } close_hook_t;
static close_hook_t s_close_hooks[MAX_WINS];

static purr_win_t alloc_win(lv_obj_t *obj) {
    for (int i = 0; i < MAX_WINS; i++) {
        if (!s_wins[i]) { s_wins[i] = obj; return (purr_win_t)(i + 1); }
    }
    return 0;
}
static lv_obj_t *get_win(purr_win_t h) {
    if (h < 1 || h > MAX_WINS) return NULL;
    return s_wins[h - 1];
}
static void free_win(purr_win_t h) {
    if (h >= 1 && h <= MAX_WINS) s_wins[h - 1] = NULL;
}

static void free_wid(purr_wid_t h) {
    if (h >= 1 && h <= MAX_WIDS) s_wids[h - 1] = NULL;
}

// Every alloc_wid'd object gets this attached on LV_EVENT_DELETE, so its
// slot is reclaimed whenever LVGL actually destroys the widget (win_destroy's
// lv_obj_del or win_clear's lv_obj_clean both cascade DELETE to every child)
// instead of relying on call sites to remember to free it themselves —
// previously nothing ever called free_wid() and slots were never reclaimed.
static void wid_delete_cb(lv_event_t *e) {
    purr_wid_t wid = (purr_wid_t)(intptr_t)lv_event_get_user_data(e);
    free_wid(wid);
}

static purr_wid_t alloc_wid(lv_obj_t *obj) {
    for (int i = 0; i < MAX_WIDS; i++) {
        if (!s_wids[i]) {
            s_wids[i] = obj;
            purr_wid_t wid = (purr_wid_t)(i + 1);
            lv_obj_add_event_cb(obj, wid_delete_cb, LV_EVENT_DELETE, (void *)(intptr_t)wid);
            return wid;
        }
    }
    return 0;
}
static lv_obj_t *get_wid(purr_wid_t h) {
    if (h < 1 || h > MAX_WIDS) return NULL;
    return s_wids[h - 1];
}

// Widgets must land inside the currently-active purr_win_row()/purr_win_col()
// container, if the app has one open — not always the window's own content
// area. Without this, every app that groups widgets into a row/col (the
// calculator's button grid, settings' button rows, fileman's list+preview
// split) gets every widget flattened into one top-to-bottom stack instead,
// since label/button/textarea/list creation had no way to reach the active
// container.
static lv_obj_t *content_parent(purr_win_t h) {
    if (h >= 1 && h <= MAX_WINS && s_active_layout[h - 1]) return s_active_layout[h - 1];
    lv_obj_t *w = get_win(h);
    return w ? lv_win_get_content(w) : NULL;
}

typedef struct { purr_win_cb_t cb; void *user; purr_wid_t wid; } cb_ctx_t;

// Mirrors wid_delete_cb but for the heap_caps_malloc'd cb_ctx_t a button or
// textarea callback attaches — without this, every cb_ctx_t allocated in
// cw_btn_create/cw_ta_cb leaked on every window destroy/clear, which on a
// device that opens and closes apps repeatedly (the normal Cardstack usage
// pattern) eventually starves the heap.
static void ctx_delete_cb(lv_event_t *e) {
    cb_ctx_t *ctx = (cb_ctx_t *)lv_event_get_user_data(e);
    if (ctx) heap_caps_free(ctx);
}

static void btn_event_cb(lv_event_t *e) {
    cb_ctx_t *ctx = (cb_ctx_t *)lv_event_get_user_data(e);
    if (ctx && ctx->cb) ctx->cb(ctx->wid, PURR_EVENT_CLICKED, ctx->user);
}

static void ta_event_cb(lv_event_t *e) {
    cb_ctx_t *ctx = (cb_ctx_t *)lv_event_get_user_data(e);
    if (ctx && ctx->cb) ctx->cb(ctx->wid, PURR_EVENT_CHANGED, ctx->user);
}

// Minimize just hides the window (and clears the dim overlay, same as Close
// used to) — the app keeps running in the background.
static void minimize_btn_event_cb(lv_event_t *e) {
    purr_win_t h = (purr_win_t)(intptr_t)lv_event_get_user_data(e);
    lv_obj_t *win = get_win(h);
    if (win) lv_obj_add_flag(win, LV_OBJ_FLAG_HIDDEN);
    cardstack_ui_on_window_closed();
}

// Close hides too (and clears the dim overlay), but also fires the
// registered win_on_close hook (if any) — app_manager wires this to
// app_manager_stop(), so Close now actually stops the app instead of just
// hiding it.
static void close_btn_event_cb(lv_event_t *e) {
    purr_win_t h = (purr_win_t)(intptr_t)lv_event_get_user_data(e);
    lv_obj_t *win = get_win(h);
    if (win) lv_obj_add_flag(win, LV_OBJ_FLAG_HIDDEN);
    cardstack_ui_on_window_closed();
    if (h >= 1 && h <= MAX_WINS && s_close_hooks[h - 1].cb) {
        s_close_hooks[h - 1].cb(h, PURR_EVENT_CLICKED, s_close_hooks[h - 1].user);
    }
}

// ── Window ────────────────────────────────────────────────────────────────────

static purr_win_t cw_win_create(const char *title) {
    lv_obj_t *win = lv_win_create(lv_scr_act(), 32);
    lv_win_add_title(win, title);

    // Allocated early so button callbacks get the purr_win_t handle (not the
    // raw lv_obj_t*) as user-data — close_btn_event_cb needs the handle to
    // look up s_close_hooks.
    purr_win_t handle = alloc_win(win);

    lv_obj_t *min_btn = lv_win_add_btn(win, LV_SYMBOL_MINUS, 32);
    lv_obj_add_event_cb(min_btn, minimize_btn_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)handle);

    lv_obj_t *close_btn = lv_win_add_btn(win, LV_SYMBOL_CLOSE, 32);
    lv_obj_add_event_cb(close_btn, close_btn_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)handle);

    // Keep the window (and its close/minimize buttons, in the title row)
    // entirely below the status bar's drag hotzone — that hotzone lives on
    // lv_layer_top(), which LVGL always hit-tests above lv_scr_act()'s tree
    // regardless of z-order, so a full-screen window at (0,0) permanently
    // lost its buttons to the hotzone's y=0..PEEK_H band.
    lv_obj_set_size(win, cardstack_hal_width(),
                     (lv_coord_t)(cardstack_hal_height() - CARDSTACK_STATUS_PEEK_H));
    lv_obj_set_pos(win, 0, CARDSTACK_STATUS_PEEK_H);
    lv_obj_add_flag(win, LV_OBJ_FLAG_HIDDEN);

    // lv_win's content area (what lv_win_get_content() returns below) has no
    // layout of its own — LVGL's lv_win constructor only sets a column flex
    // on the *outer* win object, to stack header above content; the content
    // container itself is a plain lv_obj_create() with no flex_flow set
    // (see managed_components/lvgl__lvgl/.../lv_win.c). Every widget an app
    // adds directly to the window — i.e. anything not wrapped in
    // purr_win_row()/purr_win_col() — therefore landed at the same default
    // position and overlapped. Apps overwhelmingly assume top-level widgets
    // stack vertically in creation order (settings.c, about.c, etc. all do
    // this), so give the content area that behavior here instead of making
    // every app wrap every widget in an explicit column.
    lv_obj_t *content = lv_win_get_content(win);
    lv_obj_set_style_pad_all(content, 6, 0);
    lv_obj_set_style_pad_row(content, 6, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    // Compact default text size so app content fits a small screen — a
    // widget can still override this per-instance if it needs to.
    lv_obj_set_style_text_font(content, &lv_font_montserrat_14, 0);

    return handle;
}

static void cw_win_destroy(purr_win_t h) {
    lv_obj_t *w = get_win(h);
    if (w) lv_obj_del(w);
    if (h >= 1 && h <= MAX_WINS) {
        s_active_layout[h - 1] = NULL;
        s_close_hooks[h - 1].cb = NULL;
        s_close_hooks[h - 1].user = NULL;
    }
    free_win(h);
}

static void cw_win_on_close(purr_win_t h, purr_win_cb_t cb, void *user) {
    if (h < 1 || h > MAX_WINS) return;
    s_close_hooks[h - 1].cb = cb;
    s_close_hooks[h - 1].user = user;
}

static void cw_win_show(purr_win_t h) {
    lv_obj_t *w = get_win(h);
    if (w) { lv_obj_clear_flag(w, LV_OBJ_FLAG_HIDDEN); lv_obj_move_foreground(w); }
}

static void cw_win_hide(purr_win_t h) {
    lv_obj_t *w = get_win(h);
    if (w) lv_obj_add_flag(w, LV_OBJ_FLAG_HIDDEN);
}

static void cw_win_clear(purr_win_t h) {
    lv_obj_t *w = get_win(h);
    if (!w) return;
    lv_obj_t *content = lv_win_get_content(w);
    if (content) lv_obj_clean(content);
}

// ── Labels ────────────────────────────────────────────────────────────────────

static purr_wid_t cw_label_create(purr_win_t h, const char *text) {
    lv_obj_t *parent = content_parent(h);
    if (!parent) return 0;
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, LV_PCT(100));
    return alloc_wid(lbl);
}

static void cw_label_set(purr_wid_t wid, const char *text) {
    lv_obj_t *o = get_wid(wid);
    if (o) lv_label_set_text(o, text);
}

static void cw_label_align(purr_wid_t wid, purr_align_t align) {
    lv_obj_t *o = get_wid(wid);
    if (!o) return;
    lv_text_align_t a = (align == PURR_ALIGN_CENTER) ? LV_TEXT_ALIGN_CENTER :
                        (align == PURR_ALIGN_RIGHT)  ? LV_TEXT_ALIGN_RIGHT  :
                                                        LV_TEXT_ALIGN_LEFT;
    lv_obj_set_style_text_align(o, a, 0);
}

// ── Buttons ───────────────────────────────────────────────────────────────────

static purr_wid_t cw_btn_create(purr_win_t h, const char *label,
                                  purr_win_cb_t cb, void *user) {
    lv_obj_t *parent = content_parent(h);
    if (!parent) return 0;
    lv_obj_t *btn = lv_btn_create(parent);
    // LVGL's default button size (~100px wide) was harmless while every
    // widget stacked one-per-line, but now that row/col grouping actually
    // renders side by side, it overflowed a 320px-class screen. Fixed
    // compact height, width shrinks to fit each label.
    lv_obj_set_height(btn, 32);
    lv_obj_set_width(btn, LV_SIZE_CONTENT);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, label);
    lv_obj_center(lbl);

    purr_wid_t wid = alloc_wid(btn);
    if (cb) {
        cb_ctx_t *ctx = heap_caps_malloc(sizeof(cb_ctx_t), MALLOC_CAP_DEFAULT);
        if (ctx) {
            ctx->cb = cb; ctx->user = user; ctx->wid = wid;
            lv_obj_add_event_cb(btn, btn_event_cb, LV_EVENT_CLICKED, ctx);
            lv_obj_add_event_cb(btn, ctx_delete_cb, LV_EVENT_DELETE, ctx);
        }
    }
    return wid;
}

static void cw_btn_enable(purr_wid_t wid, bool enabled) {
    lv_obj_t *o = get_wid(wid);
    if (!o) return;
    if (enabled) lv_obj_clear_state(o, LV_STATE_DISABLED);
    else         lv_obj_add_state(o, LV_STATE_DISABLED);
}

// ── Textarea ──────────────────────────────────────────────────────────────────

static purr_wid_t cw_ta_create(purr_win_t h, uint16_t w_pct, uint16_t h_pct) {
    lv_obj_t *parent = content_parent(h);
    if (!parent) return 0;
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_obj_set_size(ta, LV_PCT(w_pct), LV_PCT(h_pct));
    lv_textarea_set_one_line(ta, false);
    return alloc_wid(ta);
}

static void cw_ta_append(purr_wid_t wid, const char *text) {
    lv_obj_t *o = get_wid(wid);
    if (o) lv_textarea_add_text(o, text);
}

static void cw_ta_set(purr_wid_t wid, const char *text) {
    lv_obj_t *o = get_wid(wid);
    if (o) lv_textarea_set_text(o, text);
}

static void cw_ta_clear(purr_wid_t wid) {
    lv_obj_t *o = get_wid(wid);
    if (o) lv_textarea_set_text(o, "");
}

static const char *cw_ta_get(purr_wid_t wid) {
    lv_obj_t *o = get_wid(wid);
    return o ? lv_textarea_get_text(o) : NULL;
}

static void cw_ta_focus(purr_wid_t wid) {
    lv_obj_t *o = get_wid(wid);
    if (o) lv_obj_add_state(o, LV_STATE_FOCUSED);
}

static void cw_ta_cb(purr_wid_t wid, purr_win_cb_t cb, void *user) {
    lv_obj_t *o = get_wid(wid);
    if (!o || !cb) return;
    cb_ctx_t *ctx = heap_caps_malloc(sizeof(cb_ctx_t), MALLOC_CAP_DEFAULT);
    if (ctx) {
        ctx->cb = cb; ctx->user = user; ctx->wid = wid;
        lv_obj_add_event_cb(o, ta_event_cb, LV_EVENT_VALUE_CHANGED, ctx);
        lv_obj_add_event_cb(o, ctx_delete_cb, LV_EVENT_DELETE, ctx);
    }
}

// ── Layout ────────────────────────────────────────────────────────────────────

static purr_wid_t cw_layout_begin(purr_win_t h, purr_layout_t dir, uint8_t pad, bool grow) {
    lv_obj_t *parent = content_parent(h);
    if (!parent) return 0;
    lv_obj_t *cont = lv_obj_create(parent);
    lv_obj_set_size(cont, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(cont, pad, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_layout(cont, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(cont,
        (dir == PURR_LAYOUT_ROW) ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    if (grow) lv_obj_set_flex_grow(cont, 1);

    purr_wid_t wid = alloc_wid(cont);
    if (h >= 1 && h <= MAX_WINS) s_active_layout[h - 1] = cont;
    if (wid >= 1 && wid <= MAX_WIDS) s_layout_owner_win[wid - 1] = (int)(h - 1);
    return wid;
}

static void cw_layout_end(purr_wid_t wid) {
    if (wid < 1 || wid > MAX_WIDS) return;
    int owner = s_layout_owner_win[wid - 1];
    if (owner >= 0 && owner < MAX_WINS) s_active_layout[owner] = NULL;
}

// ── Keyboard ──────────────────────────────────────────────────────────────────

static lv_obj_t *s_keyboard = NULL;

static void cw_kb_show(purr_win_t h, purr_wid_t target) {
    lv_obj_t *w = get_win(h);
    lv_obj_t *ta = get_wid(target);
    if (!w || !ta) return;
    if (!s_keyboard) {
        s_keyboard = lv_keyboard_create(lv_scr_act());
    }
    lv_keyboard_set_textarea(s_keyboard, ta);
    lv_obj_clear_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_keyboard);
}

static void cw_kb_hide(purr_win_t h) {
    (void)h;
    if (s_keyboard) lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
}

// ── Registration ──────────────────────────────────────────────────────────────

static const catcall_ui_t s_cardstack_win = {
    .name            = "cardstack",
    .catcall_version = CATCALL_UI_VERSION,
    .win_create      = cw_win_create,
    .win_destroy     = cw_win_destroy,
    .win_show        = cw_win_show,
    .win_hide        = cw_win_hide,
    .win_clear       = cw_win_clear,
    .win_on_close    = cw_win_on_close,
    .label_create    = cw_label_create,
    .label_set       = cw_label_set,
    .label_align     = cw_label_align,
    .btn_create      = cw_btn_create,
    .btn_enable      = cw_btn_enable,
    .textarea_create = cw_ta_create,
    .textarea_append = cw_ta_append,
    .textarea_set    = cw_ta_set,
    .textarea_clear  = cw_ta_clear,
    .textarea_get    = cw_ta_get,
    .textarea_focus  = cw_ta_focus,
    .textarea_cb     = cw_ta_cb,
    .layout_begin    = cw_layout_begin,
    .layout_end      = cw_layout_end,
    .kb_show         = cw_kb_show,
    .kb_hide         = cw_kb_hide,
};

void cardstack_win_register(void) {
    purr_kernel_register_ui(&s_cardstack_win);
}
