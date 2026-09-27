// nougat_win.c — catcall_ui_t backend for Nougat (LVGL v9)
//
// Ported from cupcake_win.c (window/label/button/textarea/list/layout/
// keyboard) onto LVGL v9's real widget API — every renamed function below
// (lv_screen_active, lv_button_create, lv_obj_delete, lv_list_add_button,
// lv_obj_remove_flag/state, lv_obj_get_child_count, single-arg
// lv_win_create, lv_event_get_target returning void*) was confirmed against
// this project's actual resolved LVGL 9.5.0 headers
// (CoreOS/managed_components/lvgl__lvgl/src/...), not the v8-compat aliases
// LVGL also ships (src/lv_api_map_v8.h) — new code targets the real v9 API.
// Same widget-handle-pool pattern as cupcake_win.c (s_wins[]/s_wids[],
// alloc_win()/get_win()), same window-parenting model (app windows created
// hidden, parented directly to the active screen, popped full-screen on
// win_show()). Registers via purr_kernel_register_ui() during nougat module
// init.

#include "lvgl.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "../../kernel/catcalls/catcall_ui.h"
#include "../../kernel/catcalls/catcall_input.h"
#include "../../kernel/core/purr_kernel.h"
#include "nougat.h"

static const char *TAG = "nougat_win";

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
// clicked (see win_on_close in catcall_ui.h) — mirrors cupcake_win.c's own
// inert-but-necessary infrastructure: nothing in this file invokes a stored
// callback yet (Nougat has no nav bar of its own until Phase 2), but
// app_manager.c calls purr_win_on_close() generically for every window on
// every UI backend, so this has to stay a valid catcall implementation.
typedef struct { purr_win_cb_t cb; void *user; } close_hook_t;
static close_hook_t s_close_hooks[MAX_WINS];

// Single on-screen keyboard shared by every window — same shape as
// cupcake_win.c's s_keyboard/s_keyboard_owner_win.
static lv_obj_t *s_keyboard = NULL;
static purr_win_t s_keyboard_owner_win = 0;

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
// slot is reclaimed whenever LVGL actually destroys the widget.
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
// area. Mirrors cupcake_win.c's content_parent() exactly.
static lv_obj_t *content_parent(purr_win_t h) {
    if (h >= 1 && h <= MAX_WINS && s_active_layout[h - 1]) return s_active_layout[h - 1];
    lv_obj_t *w = get_win(h);
    return w ? lv_win_get_content(w) : NULL;
}

typedef struct { purr_win_cb_t cb; void *user; purr_wid_t wid; } cb_ctx_t;

// Mirrors wid_delete_cb but for the heap_caps_malloc'd cb_ctx_t a button,
// textarea, or list-item callback attaches — without this, every cb_ctx_t
// allocated below leaked on every window destroy/clear.
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

// ── Window ────────────────────────────────────────────────────────────────────
// No title bar, no per-window minimize/close buttons — same rationale as
// cupcake_win.c: a future nav bar (Nougat Phase 2) owns both system-wide.
// v9's lv_win_create() takes no header_height argument at all (unlike v8's
// two-arg form) — a window simply has no header until lv_win_add_title()/
// lv_win_add_button() are called, which this never does, giving the same
// "whole window is content" result cupcake_win.c got via header_height=0.

static purr_win_t ng_win_create(const char *title) {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_t *win = lv_win_create(scr);
    ESP_LOGI(TAG, "win_create '%s': scr=%p win=%p scr_children_before=%d core=%d",
             title, (void *)scr, (void *)win, (int)lv_obj_get_child_count(scr), xPortGetCoreID());

    purr_win_t handle = alloc_win(win);

    // Genuinely full screen — no status bar / nav bar space reserved. Both
    // will be lv_layer_top() overlays (Nougat Phase 2, matching cupcake_ui.c's
    // approach) that auto-hide while an app is foreground, so nothing
    // permanent is lost underneath.
    lv_obj_set_size(win, nougat_hal_width(), nougat_hal_height());
    lv_obj_set_pos(win, 0, 0);
    lv_obj_add_flag(win, LV_OBJ_FLAG_HIDDEN);

    // Same rationale as cupcake_win.c's cw_win_create/ck_win_create: the
    // content area needs an explicit column flex, since apps assume
    // top-level widgets stack vertically in creation order.
    lv_obj_t *content = lv_win_get_content(win);
    lv_obj_set_style_pad_all(content, 6, 0);
    lv_obj_set_style_pad_row(content, 6, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_text_font(content, &lv_font_montserrat_14, 0);

    return handle;
}

static void ng_win_destroy(purr_win_t h) {
    lv_obj_t *w = get_win(h);
    if (s_keyboard && s_keyboard_owner_win == h) {
        // Same UAF hazard cupcake_win.c's ck_win_destroy() documents and
        // guards against — unbind the keyboard before lv_obj_delete() frees
        // the textarea it's still targeting.
        lv_keyboard_set_textarea(s_keyboard, NULL);
        lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
        s_keyboard_owner_win = 0;
    }
    if (w) lv_obj_delete(w);
    if (h >= 1 && h <= MAX_WINS) {
        s_active_layout[h - 1] = NULL;
        s_close_hooks[h - 1].cb = NULL;
        s_close_hooks[h - 1].user = NULL;
    }
    free_win(h);
}

static void ng_win_on_close(purr_win_t h, purr_win_cb_t cb, void *user) {
    if (h < 1 || h > MAX_WINS) return;
    s_close_hooks[h - 1].cb = cb;
    s_close_hooks[h - 1].user = user;
}

static void ng_win_show(purr_win_t h) {
    lv_obj_t *w = get_win(h);
    if (!w) { ESP_LOGW(TAG, "win_show: handle=%u has no lv_obj!", (unsigned)h); return; }
    lv_obj_remove_flag(w, LV_OBJ_FLAG_HIDDEN);
    // Every app window is a same-parent (lv_screen_active()) sibling of
    // every other app's window — move to the top of the sibling stack, i.e.
    // painted last/on top of all of them. lv_layer_top() (the future status
    // bar/nav bar's home, Nougat Phase 2) is a separate LVGL layer always
    // composited above the active screen regardless of sibling order, so
    // it's structurally never covered by this.
    lv_obj_move_to_index(w, (int32_t)lv_obj_get_child_count(lv_obj_get_parent(w)) - 1);
    lv_area_t coords;
    lv_obj_get_coords(w, &coords);
    ESP_LOGI(TAG, "win_show: handle=%u obj=%p hidden=%d abs=(%d,%d)-(%d,%d) idx=%d/%d core=%d",
             (unsigned)h, (void *)w, (int)lv_obj_has_flag(w, LV_OBJ_FLAG_HIDDEN),
             (int)coords.x1, (int)coords.y1, (int)coords.x2, (int)coords.y2,
             (int)lv_obj_get_index(w), (int)lv_obj_get_child_count(lv_obj_get_parent(w)),
             xPortGetCoreID());
}

static void ng_win_hide(purr_win_t h) {
    lv_obj_t *w = get_win(h);
    if (w) lv_obj_add_flag(w, LV_OBJ_FLAG_HIDDEN);
}

static void ng_win_clear(purr_win_t h) {
    lv_obj_t *w = get_win(h);
    if (!w) return;
    lv_obj_t *content = lv_win_get_content(w);
    if (content) lv_obj_clean(content);
}

// ── Labels ────────────────────────────────────────────────────────────────────

static purr_wid_t ng_label_create(purr_win_t h, const char *text) {
    lv_obj_t *parent = content_parent(h);
    if (!parent) return 0;
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, LV_PCT(100));
    return alloc_wid(lbl);
}

static void ng_label_set(purr_wid_t wid, const char *text) {
    lv_obj_t *o = get_wid(wid);
    if (o) lv_label_set_text(o, text);
}

static void ng_label_align(purr_wid_t wid, purr_align_t align) {
    lv_obj_t *o = get_wid(wid);
    if (!o) return;
    lv_text_align_t a = (align == PURR_ALIGN_CENTER) ? LV_TEXT_ALIGN_CENTER :
                        (align == PURR_ALIGN_RIGHT)  ? LV_TEXT_ALIGN_RIGHT  :
                                                        LV_TEXT_ALIGN_LEFT;
    lv_obj_set_style_text_align(o, a, 0);
}

// ── Buttons ───────────────────────────────────────────────────────────────────

static purr_wid_t ng_btn_create(purr_win_t h, const char *label,
                                  purr_win_cb_t cb, void *user) {
    lv_obj_t *parent = content_parent(h);
    if (!parent) return 0;
    lv_obj_t *btn = lv_button_create(parent);
    // Same fixed-height/content-width fix cupcake_win.c's ck_btn_create
    // applies, for the same reason: LVGL's default button size overflows a
    // row/col grouping on a small screen.
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

static void ng_btn_enable(purr_wid_t wid, bool enabled) {
    lv_obj_t *o = get_wid(wid);
    if (!o) return;
    if (enabled) lv_obj_remove_state(o, LV_STATE_DISABLED);
    else         lv_obj_add_state(o, LV_STATE_DISABLED);
}

// ── Textarea ──────────────────────────────────────────────────────────────────

static purr_wid_t ng_ta_create(purr_win_t h, uint16_t w_pct, uint16_t h_pct) {
    lv_obj_t *parent = content_parent(h);
    if (!parent) return 0;
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_obj_set_size(ta, LV_PCT(w_pct), LV_PCT(h_pct));
    lv_textarea_set_one_line(ta, false);
    // Membership in the physical-keyboard group, not focus — mirrors
    // cupcake_win.c's ck_ta_create() exactly (see nougat_hal.c's
    // nougat_hal_keypad_group() doc comment for why this is inert on Tab5
    // today but kept for parity).
    lv_group_t *g = nougat_hal_keypad_group();
    if (g) lv_group_add_obj(g, ta);
    return alloc_wid(ta);
}

static void ng_ta_append(purr_wid_t wid, const char *text) {
    lv_obj_t *o = get_wid(wid);
    if (o) lv_textarea_add_text(o, text);
}

static void ng_ta_set(purr_wid_t wid, const char *text) {
    lv_obj_t *o = get_wid(wid);
    if (o) lv_textarea_set_text(o, text);
}

static void ng_ta_clear(purr_wid_t wid) {
    lv_obj_t *o = get_wid(wid);
    if (o) lv_textarea_set_text(o, "");
}

static const char *ng_ta_get(purr_wid_t wid) {
    lv_obj_t *o = get_wid(wid);
    return o ? lv_textarea_get_text(o) : NULL;
}

static void ng_ta_focus(purr_wid_t wid) {
    lv_obj_t *o = get_wid(wid);
    if (!o) return;
    lv_group_t *g = nougat_hal_keypad_group();
    if (g) lv_group_focus_obj(o);
    else   lv_obj_add_state(o, LV_STATE_FOCUSED);
}

static void ng_ta_cb(purr_wid_t wid, purr_win_cb_t cb, void *user) {
    lv_obj_t *o = get_wid(wid);
    if (!o || !cb) return;
    cb_ctx_t *ctx = heap_caps_malloc(sizeof(cb_ctx_t), MALLOC_CAP_DEFAULT);
    if (ctx) {
        ctx->cb = cb; ctx->user = user; ctx->wid = wid;
        lv_obj_add_event_cb(o, ta_event_cb, LV_EVENT_VALUE_CHANGED, ctx);
        lv_obj_add_event_cb(o, ctx_delete_cb, LV_EVENT_DELETE, ctx);
    }
}

// ── List (flat, non-nested selectable list) ──────────────────────────────────
// Ported from cupcake_win.c's ck_list_* — same caveat: no lv_group/encoder
// navigation yet, so a tap fires SELECTED immediately followed by ACTIVATED.

typedef struct { purr_win_cb_t cb; void *user; int selected_idx; } list_meta_t;
static list_meta_t s_list_meta[MAX_WIDS];

static void list_btn_event_cb(lv_event_t *e) {
    cb_ctx_t *ctx = (cb_ctx_t *)lv_event_get_user_data(e);
    if (!ctx) return;
    purr_wid_t list_wid = ctx->wid;
    lv_obj_t *list = get_wid(list_wid);
    lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
    if (!list || !btn || list_wid < 1 || list_wid > MAX_WIDS) return;

    list_meta_t *meta = &s_list_meta[list_wid - 1];
    if (meta->selected_idx >= 0) {
        lv_obj_t *prev_btn = lv_obj_get_child(list, meta->selected_idx);
        if (prev_btn) lv_obj_remove_state(prev_btn, LV_STATE_CHECKED);
    }
    lv_obj_add_state(btn, LV_STATE_CHECKED);
    meta->selected_idx = (int)lv_obj_get_index(btn);

    if (meta->cb) {
        meta->cb(list_wid, PURR_EVENT_SELECTED, meta->user);
        meta->cb(list_wid, PURR_EVENT_ACTIVATED, meta->user);
    }
}

static purr_wid_t ng_list_create(purr_win_t h, uint16_t w_pct, uint16_t h_pct) {
    lv_obj_t *parent = content_parent(h);
    if (!parent) return 0;
    lv_obj_t *list = lv_list_create(parent);
    lv_obj_set_size(list, LV_PCT(w_pct), LV_PCT(h_pct));
    purr_wid_t wid = alloc_wid(list);
    if (wid >= 1 && wid <= MAX_WIDS) {
        s_list_meta[wid - 1].cb = NULL;
        s_list_meta[wid - 1].user = NULL;
        s_list_meta[wid - 1].selected_idx = -1;
    }
    return wid;
}

static void ng_list_set_items(purr_wid_t wid, const char **items, int count) {
    lv_obj_t *list = get_wid(wid);
    if (!list || wid < 1 || wid > MAX_WIDS) return;
    lv_obj_clean(list);
    s_list_meta[wid - 1].selected_idx = -1;
    for (int i = 0; i < count; i++) {
        lv_obj_t *btn = lv_list_add_button(list, NULL, (items && items[i]) ? items[i] : "");
        cb_ctx_t *ctx = heap_caps_malloc(sizeof(cb_ctx_t), MALLOC_CAP_DEFAULT);
        if (ctx) {
            ctx->cb = NULL;
            ctx->user = NULL;
            ctx->wid = wid;
            lv_obj_add_event_cb(btn, list_btn_event_cb, LV_EVENT_CLICKED, ctx);
        }
    }
}

static void ng_list_clear(purr_wid_t wid) {
    ng_list_set_items(wid, NULL, 0);
}

static int ng_list_get_selected(purr_wid_t wid) {
    if (wid < 1 || wid > MAX_WIDS) return -1;
    return s_list_meta[wid - 1].selected_idx;
}

static void ng_list_set_selected(purr_wid_t wid, int index) {
    lv_obj_t *list = get_wid(wid);
    if (!list || wid < 1 || wid > MAX_WIDS) return;
    list_meta_t *meta = &s_list_meta[wid - 1];
    if (meta->selected_idx >= 0) {
        lv_obj_t *prev_btn = lv_obj_get_child(list, meta->selected_idx);
        if (prev_btn) lv_obj_remove_state(prev_btn, LV_STATE_CHECKED);
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

static void ng_list_cb(purr_wid_t wid, purr_win_cb_t cb, void *user) {
    if (wid < 1 || wid > MAX_WIDS) return;
    s_list_meta[wid - 1].cb = cb;
    s_list_meta[wid - 1].user = user;
}

// ── Layout ────────────────────────────────────────────────────────────────────

static purr_wid_t ng_layout_begin(purr_win_t h, purr_layout_t dir, uint8_t pad, bool grow) {
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

static void ng_layout_end(purr_wid_t wid) {
    if (wid < 1 || wid > MAX_WIDS) return;
    int owner = s_layout_owner_win[wid - 1];
    if (owner >= 0 && owner < MAX_WINS) s_active_layout[owner] = NULL;
}

// ── Keyboard ──────────────────────────────────────────────────────────────────
// s_keyboard/s_keyboard_owner_win are declared near the top of this file
// (alongside s_close_hooks) since ng_win_destroy() needs them too.

static bool ng_has_physical_keyboard(void) {
    int n = purr_kernel_input_count();
    for (int i = 0; i < n; i++) {
        const catcall_input_t *inp = purr_kernel_input_at(i);
        if (inp && inp->set_backlight) return true;
    }
    return false;
}

static void ng_kb_show(purr_win_t h, purr_wid_t target) {
    if (ng_has_physical_keyboard()) return;
    lv_obj_t *w = get_win(h);
    lv_obj_t *ta = get_wid(target);
    if (!w || !ta) return;
    if (!s_keyboard) {
        s_keyboard = lv_keyboard_create(lv_screen_active());
    }
    lv_keyboard_set_textarea(s_keyboard, ta);
    s_keyboard_owner_win = h;
    lv_obj_remove_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_to_index(s_keyboard,
        (int32_t)lv_obj_get_child_count(lv_obj_get_parent(s_keyboard)) - 1);
}

static void ng_kb_hide(purr_win_t h) {
    (void)h;
    if (s_keyboard) lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
}

// ── Registration ──────────────────────────────────────────────────────────────

static const catcall_ui_t s_nougat_win = {
    .name            = "nougat",
    .catcall_version = CATCALL_UI_VERSION,
    .win_create      = ng_win_create,
    .win_destroy     = ng_win_destroy,
    .win_show        = ng_win_show,
    .win_hide        = ng_win_hide,
    .win_clear       = ng_win_clear,
    .win_on_close    = ng_win_on_close,
    .label_create    = ng_label_create,
    .label_set       = ng_label_set,
    .label_align     = ng_label_align,
    .btn_create      = ng_btn_create,
    .btn_enable      = ng_btn_enable,
    .textarea_create = ng_ta_create,
    .textarea_append = ng_ta_append,
    .textarea_set    = ng_ta_set,
    .textarea_clear  = ng_ta_clear,
    .textarea_get    = ng_ta_get,
    .textarea_focus  = ng_ta_focus,
    .textarea_cb     = ng_ta_cb,
    .list_create       = ng_list_create,
    .list_set_items    = ng_list_set_items,
    .list_clear        = ng_list_clear,
    .list_get_selected = ng_list_get_selected,
    .list_set_selected = ng_list_set_selected,
    .list_cb           = ng_list_cb,
    .layout_begin    = ng_layout_begin,
    .layout_end      = ng_layout_end,
    .kb_show         = ng_kb_show,
    .kb_hide         = ng_kb_hide,
};

void nougat_win_register(void) {
    purr_kernel_register_ui(&s_nougat_win);
}
