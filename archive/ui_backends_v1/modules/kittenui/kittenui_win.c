// kittenui_win.c — catcall_ui_t backend for KittenUI (LVGL)
// Registers via purr_kernel_register_ui() during kittenui module init.

#include "lvgl.h"
#include "esp_heap_caps.h"
#include "../../kernel/catcalls/catcall_ui.h"
#include "../../kernel/core/purr_kernel.h"

// ── Handle pool ───────────────────────────────────────────────────────────────
// Map opaque uint32_t tokens → lv_obj_t*. Simple linear scan; MAX 32 windows
// + 256 widgets is more than enough for embedded apps.

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

static purr_wid_t alloc_wid(lv_obj_t *obj) {
    for (int i = 0; i < MAX_WIDS; i++) {
        if (!s_wids[i]) { s_wids[i] = obj; return (purr_wid_t)(i + 1); }
    }
    return 0;
}
static lv_obj_t *get_wid(purr_wid_t h) {
    if (h < 1 || h > MAX_WIDS) return NULL;
    return s_wids[h - 1];
}
static void free_wid(purr_wid_t h) {
    if (h >= 1 && h <= MAX_WIDS) s_wids[h - 1] = NULL;
}

// ── Callback trampoline ───────────────────────────────────────────────────────

typedef struct { purr_win_cb_t cb; void *user; purr_wid_t wid; } cb_ctx_t;

static void btn_event_cb(lv_event_t *e) {
    cb_ctx_t *ctx = (cb_ctx_t *)lv_event_get_user_data(e);
    if (ctx && ctx->cb) ctx->cb(ctx->wid, PURR_EVENT_CLICKED, ctx->user);
}

static void ta_event_cb(lv_event_t *e) {
    cb_ctx_t *ctx = (cb_ctx_t *)lv_event_get_user_data(e);
    if (ctx && ctx->cb) ctx->cb(ctx->wid, PURR_EVENT_CHANGED, ctx->user);
}

// ── Window ────────────────────────────────────────────────────────────────────

static purr_win_t kw_win_create(const char *title) {
    lv_obj_t *win = lv_win_create(lv_scr_act(), 40);
    lv_win_add_title(win, title);
    lv_obj_set_size(win, LV_PCT(100), LV_PCT(100));
    lv_obj_add_flag(win, LV_OBJ_FLAG_HIDDEN);

    // lv_win's content area has no layout of its own by default — only the
    // outer win object stacks header+content (see lv_win.c's constructor).
    // Without this, every widget an app adds directly to the window (not
    // wrapped in purr_win_row()/purr_win_col()) lands at the same default
    // position and overlaps. Same fix as cardstack_win.c's cw_win_create —
    // this is a property of the shared lv_win widget, not backend-specific.
    lv_obj_t *content = lv_win_get_content(win);
    lv_obj_set_style_pad_all(content, 6, 0);
    lv_obj_set_style_pad_row(content, 6, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    return alloc_win(win);
}

static void kw_win_destroy(purr_win_t h) {
    lv_obj_t *w = get_win(h);
    if (w) lv_obj_del(w);
    if (h >= 1 && h <= MAX_WINS) s_active_layout[h - 1] = NULL;
    free_win(h);
}

static void kw_win_show(purr_win_t h) {
    lv_obj_t *w = get_win(h);
    if (w) lv_obj_clear_flag(w, LV_OBJ_FLAG_HIDDEN);
}

static void kw_win_hide(purr_win_t h) {
    lv_obj_t *w = get_win(h);
    if (w) lv_obj_add_flag(w, LV_OBJ_FLAG_HIDDEN);
}

static void kw_win_clear(purr_win_t h) {
    lv_obj_t *w = get_win(h);
    if (!w) return;
    lv_obj_t *content = lv_win_get_content(w);
    if (content) lv_obj_clean(content);
    if (h >= 1 && h <= MAX_WINS) s_active_layout[h - 1] = NULL;
}

// ── Labels ────────────────────────────────────────────────────────────────────

// purr_win_row()/purr_win_col() create a flex container (see kw_layout_begin)
// but every widget-creation call below used to parent straight to the
// window's top-level content area regardless, so the container sat there
// empty and every "row" of buttons/labels just landed in the same vertical
// stack as everything else — every app that groups widgets into a row
// (calculator's keypad, settings'/about's/fileman's button rows, ...)
// rendered as one long vertical stack instead. Parent into whichever layout
// is currently active for this window, falling back to the window content.
static lv_obj_t *content_parent(purr_win_t h, lv_obj_t *w) {
    if (h >= 1 && h <= MAX_WINS && s_active_layout[h - 1]) return s_active_layout[h - 1];
    return lv_win_get_content(w);
}

static purr_wid_t kw_label_create(purr_win_t h, const char *text) {
    lv_obj_t *w = get_win(h);
    if (!w) return 0;
    lv_obj_t *lbl = lv_label_create(content_parent(h, w));
    lv_label_set_text(lbl, text);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, LV_PCT(100));
    return alloc_wid(lbl);
}

static void kw_label_set(purr_wid_t wid, const char *text) {
    lv_obj_t *o = get_wid(wid);
    if (o) lv_label_set_text(o, text);
}

static void kw_label_align(purr_wid_t wid, purr_align_t align) {
    lv_obj_t *o = get_wid(wid);
    if (!o) return;
    lv_text_align_t a = (align == PURR_ALIGN_CENTER) ? LV_TEXT_ALIGN_CENTER :
                        (align == PURR_ALIGN_RIGHT)  ? LV_TEXT_ALIGN_RIGHT  :
                                                        LV_TEXT_ALIGN_LEFT;
    lv_obj_set_style_text_align(o, a, 0);
}

// ── Buttons ───────────────────────────────────────────────────────────────────

static purr_wid_t kw_btn_create(purr_win_t h, const char *label,
                                  purr_win_cb_t cb, void *user) {
    lv_obj_t *w = get_win(h);
    if (!w) return 0;
    lv_obj_t *btn = lv_btn_create(content_parent(h, w));
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, label);
    lv_obj_center(lbl);

    purr_wid_t wid = alloc_wid(btn);
    if (cb) {
        cb_ctx_t *ctx = heap_caps_malloc(sizeof(cb_ctx_t), MALLOC_CAP_DEFAULT);
        if (ctx) { ctx->cb = cb; ctx->user = user; ctx->wid = wid; }
        lv_obj_add_event_cb(btn, btn_event_cb, LV_EVENT_CLICKED, ctx);
    }
    return wid;
}

static void kw_btn_enable(purr_wid_t wid, bool enabled) {
    lv_obj_t *o = get_wid(wid);
    if (!o) return;
    if (enabled) lv_obj_clear_state(o, LV_STATE_DISABLED);
    else         lv_obj_add_state(o, LV_STATE_DISABLED);
}

// ── Textarea ──────────────────────────────────────────────────────────────────

static purr_wid_t kw_ta_create(purr_win_t h, uint16_t w_pct, uint16_t h_pct) {
    lv_obj_t *w = get_win(h);
    if (!w) return 0;
    lv_obj_t *ta = lv_textarea_create(content_parent(h, w));
    lv_obj_set_size(ta, LV_PCT(w_pct), LV_PCT(h_pct));
    lv_textarea_set_one_line(ta, false);
    return alloc_wid(ta);
}

static void kw_ta_append(purr_wid_t wid, const char *text) {
    lv_obj_t *o = get_wid(wid);
    if (o) lv_textarea_add_text(o, text);
}

static void kw_ta_set(purr_wid_t wid, const char *text) {
    lv_obj_t *o = get_wid(wid);
    if (o) lv_textarea_set_text(o, text);
}

static void kw_ta_clear(purr_wid_t wid) {
    lv_obj_t *o = get_wid(wid);
    if (o) lv_textarea_set_text(o, "");
}

static const char *kw_ta_get(purr_wid_t wid) {
    lv_obj_t *o = get_wid(wid);
    return o ? lv_textarea_get_text(o) : NULL;
}

static void kw_ta_focus(purr_wid_t wid) {
    lv_obj_t *o = get_wid(wid);
    if (o) lv_obj_add_state(o, LV_STATE_FOCUSED);
}

static void kw_ta_cb(purr_wid_t wid, purr_win_cb_t cb, void *user) {
    lv_obj_t *o = get_wid(wid);
    if (!o || !cb) return;
    cb_ctx_t *ctx = heap_caps_malloc(sizeof(cb_ctx_t), MALLOC_CAP_DEFAULT);
    if (ctx) { ctx->cb = cb; ctx->user = user; ctx->wid = wid; }
    lv_obj_add_event_cb(o, ta_event_cb, LV_EVENT_VALUE_CHANGED, ctx);
}

// ── List (flat, non-nested selectable list) ──────────────────────────────────
// KittenUI has no lv_group/encoder-navigation infrastructure today, so — like
// MiniWin — there's no real "highlight without confirm" input path yet; a tap
// fires SELECTED immediately followed by ACTIVATED. The per-button ctx only
// carries the owning list's wid — cb/user are looked up from s_list_meta at
// click time so calling list_cb() after list_set_items() still works.

typedef struct { purr_win_cb_t cb; void *user; int selected_idx; } list_meta_t;
static list_meta_t s_list_meta[MAX_WIDS];

static void list_btn_event_cb(lv_event_t *e) {
    cb_ctx_t *ctx = (cb_ctx_t *)lv_event_get_user_data(e);
    if (!ctx) return;
    purr_wid_t list_wid = ctx->wid;
    lv_obj_t *list = get_wid(list_wid);
    lv_obj_t *btn = lv_event_get_target(e);
    if (!list || !btn || list_wid < 1 || list_wid > MAX_WIDS) return;

    list_meta_t *meta = &s_list_meta[list_wid - 1];
    if (meta->selected_idx >= 0) {
        lv_obj_t *prev_btn = lv_obj_get_child(list, meta->selected_idx);
        if (prev_btn) lv_obj_clear_state(prev_btn, LV_STATE_CHECKED);
    }
    lv_obj_add_state(btn, LV_STATE_CHECKED);
    meta->selected_idx = (int)lv_obj_get_index(btn);

    if (meta->cb) {
        meta->cb(list_wid, PURR_EVENT_SELECTED, meta->user);
        meta->cb(list_wid, PURR_EVENT_ACTIVATED, meta->user);
    }
}

static purr_wid_t kw_list_create(purr_win_t h, uint16_t w_pct, uint16_t h_pct) {
    lv_obj_t *w = get_win(h);
    if (!w) return 0;
    lv_obj_t *list = lv_list_create(lv_win_get_content(w));
    lv_obj_set_size(list, LV_PCT(w_pct), LV_PCT(h_pct));
    purr_wid_t wid = alloc_wid(list);
    if (wid >= 1 && wid <= MAX_WIDS) {
        s_list_meta[wid - 1].cb = NULL;
        s_list_meta[wid - 1].user = NULL;
        s_list_meta[wid - 1].selected_idx = -1;
    }
    return wid;
}

static void kw_list_set_items(purr_wid_t wid, const char **items, int count) {
    lv_obj_t *list = get_wid(wid);
    if (!list || wid < 1 || wid > MAX_WIDS) return;
    lv_obj_clean(list);
    s_list_meta[wid - 1].selected_idx = -1;
    for (int i = 0; i < count; i++) {
        lv_obj_t *btn = lv_list_add_btn(list, NULL, (items && items[i]) ? items[i] : "");
        cb_ctx_t *ctx = heap_caps_malloc(sizeof(cb_ctx_t), MALLOC_CAP_DEFAULT);
        if (ctx) {
            ctx->cb = NULL;
            ctx->user = NULL;
            ctx->wid = wid;
            lv_obj_add_event_cb(btn, list_btn_event_cb, LV_EVENT_CLICKED, ctx);
        }
    }
}

static void kw_list_clear(purr_wid_t wid) {
    kw_list_set_items(wid, NULL, 0);
}

static int kw_list_get_selected(purr_wid_t wid) {
    if (wid < 1 || wid > MAX_WIDS) return -1;
    return s_list_meta[wid - 1].selected_idx;
}

static void kw_list_set_selected(purr_wid_t wid, int index) {
    lv_obj_t *list = get_wid(wid);
    if (!list || wid < 1 || wid > MAX_WIDS) return;
    list_meta_t *meta = &s_list_meta[wid - 1];
    if (meta->selected_idx >= 0) {
        lv_obj_t *prev_btn = lv_obj_get_child(list, meta->selected_idx);
        if (prev_btn) lv_obj_clear_state(prev_btn, LV_STATE_CHECKED);
    }
    meta->selected_idx = index;
    if (index >= 0) {
        lv_obj_t *btn = lv_obj_get_child(list, index);
        if (btn) {
            lv_obj_add_state(btn, LV_STATE_CHECKED);
            lv_obj_scroll_to_view(btn, LV_ANIM_OFF);
        }
    }
}

static void kw_list_cb(purr_wid_t wid, purr_win_cb_t cb, void *user) {
    if (wid < 1 || wid > MAX_WIDS) return;
    s_list_meta[wid - 1].cb = cb;
    s_list_meta[wid - 1].user = user;
}

// ── Layout ────────────────────────────────────────────────────────────────────

static purr_wid_t kw_layout_begin(purr_win_t h, purr_layout_t dir, uint8_t pad, bool grow) {
    lv_obj_t *w = get_win(h);
    if (!w) return 0;
    lv_obj_t *cont = lv_obj_create(content_parent(h, w));
    lv_obj_set_size(cont, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(cont, pad, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_layout(cont, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(cont,
        (dir == PURR_LAYOUT_ROW) ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    if (grow) lv_obj_set_flex_grow(cont, 1);
    purr_wid_t wid = alloc_wid(cont);
    if (wid >= 1 && wid <= MAX_WIDS) s_layout_owner_win[wid - 1] = (int)(h - 1);
    if (h >= 1 && h <= MAX_WINS) s_active_layout[h - 1] = cont;
    return wid;
}

static void kw_layout_end(purr_wid_t wid) {
    if (wid < 1 || wid > MAX_WIDS) return;
    int win_idx = s_layout_owner_win[wid - 1];
    if (win_idx >= 0 && win_idx < MAX_WINS && s_active_layout[win_idx] == get_wid(wid)) {
        s_active_layout[win_idx] = NULL;
    }
}

// ── Keyboard ──────────────────────────────────────────────────────────────────

static lv_obj_t *s_keyboard = NULL;

static void kw_kb_show(purr_win_t h, purr_wid_t target) {
    lv_obj_t *w = get_win(h);
    lv_obj_t *ta = get_wid(target);
    if (!w || !ta) return;
    if (!s_keyboard) {
        s_keyboard = lv_keyboard_create(lv_scr_act());
    }
    lv_keyboard_set_textarea(s_keyboard, ta);
    lv_obj_clear_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
}

static void kw_kb_hide(purr_win_t h) {
    (void)h;
    if (s_keyboard) lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
}

// ── Registration ──────────────────────────────────────────────────────────────

static const catcall_ui_t s_kittenui_win = {
    .name            = "kittenui",
    .catcall_version = CATCALL_UI_VERSION,
    .win_create      = kw_win_create,
    .win_destroy     = kw_win_destroy,
    .win_show        = kw_win_show,
    .win_hide        = kw_win_hide,
    .win_clear       = kw_win_clear,
    .label_create    = kw_label_create,
    .label_set       = kw_label_set,
    .label_align     = kw_label_align,
    .btn_create      = kw_btn_create,
    .btn_enable      = kw_btn_enable,
    .textarea_create = kw_ta_create,
    .textarea_append = kw_ta_append,
    .textarea_set    = kw_ta_set,
    .textarea_clear  = kw_ta_clear,
    .textarea_get    = kw_ta_get,
    .textarea_focus  = kw_ta_focus,
    .textarea_cb     = kw_ta_cb,
    .list_create     = kw_list_create,
    .list_set_items  = kw_list_set_items,
    .list_clear      = kw_list_clear,
    .list_get_selected = kw_list_get_selected,
    .list_set_selected = kw_list_set_selected,
    .list_cb         = kw_list_cb,
    .layout_begin    = kw_layout_begin,
    .layout_end      = kw_layout_end,
    .kb_show         = kw_kb_show,
    .kb_hide         = kw_kb_hide,
};

void kittenui_win_register(void) {
    purr_kernel_register_ui(&s_kittenui_win);
}
