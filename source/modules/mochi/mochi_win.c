// mochi_win.c — catcall_ui_t backend for Mochi (LVGL).
//
// Derived from tabby_win.c, which was itself adapted from cupcake_win.c —
// deliberately, rather than written fresh, because that lineage already
// encodes several hard-won, live-confirmed fixes that any LVGL backend needs
// and that are not obvious from the contract —
//
//   * window teardown deferred via lv_async_call(), because destroying a
//     window's object tree synchronously from inside a click callback hangs
//     the render task partway through the next lv_timer_handler() pass;
//   * the on-screen keyboard unbound from a textarea before that textarea is
//     freed, or the next keypress dereferences freed memory;
//   * list/tile rebuilds deferred the same way, because apps refresh lists
//     from their own background tasks and a rebuild landing mid-gesture on
//     the same object hangs the render task;
//   * per-callback context freed on LV_EVENT_DELETE, or every window destroy
//     leaks one allocation per button/textarea/list.
//
// Re-deriving those would mean re-hitting them on hardware. What this backend
// on top:
//
//   * EVERY focusable widget joins the shared keyboard/encoder group, not just
//     textareas. Under Cupcake only textareas are group members, so a physical
//     keyboard can reach a text field but never a button — you have to touch
//     buttons. On a keyboard-first device that is the difference between an
//     app being usable one-handed and not.
//   * Widgets are removed from the group on delete, so a destroyed window
//     doesn't leave dangling group entries the encoder can still step onto.
//   * The on-screen keyboard is suppressed whenever a physical keyboard
//     exists (same capability test), reclaiming that screen area.
//
// App windows are created hidden, parented to lv_scr_act(), full-screen, with
// no title bar: the system UI's nav bar owns Back/Home/Recents system-wide,
// and the shell owns launching.

#include <string.h>
#include "lvgl.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "../../kernel/catcalls/catcall_ui.h"
#include "../../kernel/catcalls/catcall_input.h"
#include "../../kernel/core/purr_kernel.h"
#include "mochi.h"

static const char *TAG = "mochi_win";

#define MAX_WINS  16
#define MAX_WIDS  128

static lv_obj_t *s_wins[MAX_WINS];
static lv_obj_t *s_wids[MAX_WIDS];

static lv_obj_t *s_active_layout[MAX_WINS];
static int       s_layout_owner_win[MAX_WIDS];

typedef struct { purr_win_cb_t cb; void *user; } close_hook_t;
static close_hook_t s_close_hooks[MAX_WINS];

static lv_obj_t  *s_keyboard = NULL;
static purr_win_t s_keyboard_owner_win = 0;

// ── Handle tables ───────────────────────────────────────────────────────────

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

// Shared "currently shown" stack backing mochi_win_hide_foreground(). Only one
// app is foreground at a time, so a single stack is equivalent to "this app's
// open windows" — and it catches lazily-created sub-windows that app_manager
// never tracked, which a plain hide-app->window misses.
static purr_win_t s_win_stack[MAX_WINS];
static int        s_win_stack_count = 0;

static void win_stack_remove(purr_win_t h) {
    for (int i = 0; i < s_win_stack_count; i++) {
        if (s_win_stack[i] == h) {
            memmove(&s_win_stack[i], &s_win_stack[i + 1],
                    (size_t)(s_win_stack_count - i - 1) * sizeof(purr_win_t));
            s_win_stack_count--;
            return;
        }
    }
}
static void win_stack_push(purr_win_t h) {
    win_stack_remove(h);   // no duplicates if the same window is re-shown
    if (s_win_stack_count < MAX_WINS) s_win_stack[s_win_stack_count++] = h;
}

static void free_wid(purr_wid_t h) {
    if (h >= 1 && h <= MAX_WIDS) s_wids[h - 1] = NULL;
}

// Reclaims the handle slot AND drops the object from the focus group. The
// group removal is load-bearing here: because this backend adds
// every focusable widget to the group (unlike Cupcake, which only adds
// textareas), a destroyed window would otherwise leave stale entries that the
// encoder can still step onto — focusing freed memory.
static void wid_delete_cb(lv_event_t *e) {
    purr_wid_t wid = (purr_wid_t)(intptr_t)lv_event_get_user_data(e);
    lv_obj_t  *obj = lv_event_get_target(e);
    lv_group_t *g  = mochi_hal_group();
    if (g && obj) lv_group_remove_obj(obj);
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

// Make a widget reachable from keyboard/trackball.
static void group_add(lv_obj_t *obj) {
    lv_group_t *g = mochi_hal_group();
    if (g && obj) lv_group_add_obj(g, obj);
}

// Widgets must land inside the app's currently-open purr_win_row()/col()
// container when it has one, not always the window's own content area —
// otherwise every grouped widget flattens into one vertical stack.
static lv_obj_t *content_parent(purr_win_t h) {
    if (h >= 1 && h <= MAX_WINS && s_active_layout[h - 1]) return s_active_layout[h - 1];
    lv_obj_t *w = get_win(h);
    return w ? lv_win_get_content(w) : NULL;
}

typedef struct { purr_win_cb_t cb; void *user; purr_wid_t wid; } cb_ctx_t;

// Without this every heap-allocated callback context leaks on window destroy.
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

// ── Window ──────────────────────────────────────────────────────────────────

static void win_del_async_cb(void *obj) {
    purr_kernel_ui_breadcrumb("win_del_async:begin");
    lv_obj_del((lv_obj_t *)obj);
    purr_kernel_ui_breadcrumb("win_del_async:end");
}

static purr_win_t tb_win_create(const char *title) {
    lv_obj_t *scr = lv_scr_act();
    // header_height 0 — no title bar. Back/Home/Recents are the system UI's
    // nav bar, system-wide, so a per-window button row would be redundant and
    // would collide with the status bar's drag hotzone at the top.
    lv_obj_t *win = lv_win_create(scr, 0);

    purr_win_t handle = alloc_win(win);

    lv_obj_set_size(win, mochi_hal_width(), mochi_hal_height());
    lv_obj_set_pos(win, 0, 0);
    lv_obj_add_flag(win, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *content = lv_win_get_content(win);
    lv_obj_set_style_pad_all(content, 6, 0);
    lv_obj_set_style_pad_row(content, 6, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_text_font(content, &lv_font_montserrat_14, 0);

    ESP_LOGI(TAG, "win_create '%s' -> handle=%u", title ? title : "?", (unsigned)handle);
    return handle;
}

static void tb_win_destroy(purr_win_t h) {
    lv_obj_t *w = get_win(h);
    if (s_keyboard && s_keyboard_owner_win == h) {
        // Unbind before lv_obj_del() frees the textarea underneath, or the
        // next keypress anywhere dereferences a dangling pointer.
        lv_keyboard_set_textarea(s_keyboard, NULL);
        lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
        s_keyboard_owner_win = 0;
    }
    // Deferred: this runs synchronously inside app_manager_stop(), itself
    // called from an LVGL click event. Deleting a whole object tree mid-
    // dispatch is what LVGL's own docs warn against, and hangs the render task
    // in practice.
    if (w) lv_async_call(win_del_async_cb, w);
    if (h >= 1 && h <= MAX_WINS) {
        s_active_layout[h - 1] = NULL;
        s_close_hooks[h - 1].cb = NULL;
        s_close_hooks[h - 1].user = NULL;
    }
    win_stack_remove(h);
    free_win(h);
}

static void tb_win_on_close(purr_win_t h, purr_win_cb_t cb, void *user) {
    if (h < 1 || h > MAX_WINS) return;
    s_close_hooks[h - 1].cb = cb;
    s_close_hooks[h - 1].user = user;
}

static void tb_win_show(purr_win_t h) {
    lv_obj_t *w = get_win(h);
    if (!w) { ESP_LOGW(TAG, "win_show: handle=%u has no lv_obj", (unsigned)h); return; }
    lv_obj_clear_flag(w, LV_OBJ_FLAG_HIDDEN);
    win_stack_push(h);
    // Same-parent sibling of the shell root and every other app window;
    // move_foreground() makes this the last child, painted on top. The system
    // UI lives on lv_layer_top(), a separate layer always above this one.
    lv_obj_move_foreground(w);
}

static void tb_win_hide(purr_win_t h) {
    lv_obj_t *w = get_win(h);
    if (w) lv_obj_add_flag(w, LV_OBJ_FLAG_HIDDEN);
    win_stack_remove(h);
}

void mochi_win_hide_foreground(void) {
    while (s_win_stack_count > 0) {
        tb_win_hide(s_win_stack[s_win_stack_count - 1]);   // removes itself
    }
    // Nothing is covering the springboard any more — restore focus to it, and
    // with it the encoder's editing mode, or the trackball goes dead on return
    // from an app.
    mochi_springboard_go_home();
}

static void tb_win_clear(purr_win_t h) {
    lv_obj_t *w = get_win(h);
    if (!w) return;
    lv_obj_t *content = lv_win_get_content(w);
    if (content) lv_obj_clean(content);
    if (h >= 1 && h <= MAX_WINS) s_active_layout[h - 1] = NULL;
}

// ── Labels ──────────────────────────────────────────────────────────────────

static purr_wid_t tb_label_create(purr_win_t h, const char *text) {
    lv_obj_t *parent = content_parent(h);
    if (!parent) return 0;
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, LV_PCT(100));
    return alloc_wid(lbl);   // not group-added: labels aren't interactive
}

static void tb_label_set(purr_wid_t wid, const char *text) {
    lv_obj_t *o = get_wid(wid);
    if (o) lv_label_set_text(o, text);
}

static void tb_label_set_big(purr_wid_t wid, const char *text) {
    lv_obj_t *o = get_wid(wid);
    if (!o) return;
    lv_label_set_text(o, text);
    lv_obj_set_style_text_font(o, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_align(o, LV_TEXT_ALIGN_CENTER, 0);
}

static void tb_label_align(purr_wid_t wid, purr_align_t align) {
    lv_obj_t *o = get_wid(wid);
    if (!o) return;
    lv_text_align_t a = (align == PURR_ALIGN_CENTER) ? LV_TEXT_ALIGN_CENTER :
                        (align == PURR_ALIGN_RIGHT)  ? LV_TEXT_ALIGN_RIGHT  :
                                                        LV_TEXT_ALIGN_LEFT;
    lv_obj_set_style_text_align(o, a, 0);
}

// ── Buttons ─────────────────────────────────────────────────────────────────

static purr_wid_t tb_btn_create(purr_win_t h, const char *label,
                                 purr_win_cb_t cb, void *user) {
    lv_obj_t *parent = content_parent(h);
    if (!parent) return 0;
    lv_obj_t *btn = lv_btn_create(parent);
    // Fixed compact height, content-sized width — LVGL's ~100px default
    // overflows a 320px-class screen once row layouts actually place buttons
    // side by side.
    lv_obj_set_height(btn, 32);
    lv_obj_set_width(btn, LV_SIZE_CONTENT);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, label);
    lv_obj_center(lbl);

    purr_wid_t wid = alloc_wid(btn);
    group_add(btn);   // buttons are keyboard/trackball reachable
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

static void tb_btn_enable(purr_wid_t wid, bool enabled) {
    lv_obj_t *o = get_wid(wid);
    if (!o) return;
    if (enabled) lv_obj_clear_state(o, LV_STATE_DISABLED);
    else         lv_obj_add_state(o, LV_STATE_DISABLED);
}

// ── Textarea ────────────────────────────────────────────────────────────────

static purr_wid_t tb_ta_create(purr_win_t h, uint16_t w_pct, uint16_t h_pct) {
    lv_obj_t *parent = content_parent(h);
    if (!parent) return 0;
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_obj_set_size(ta, LV_PCT(w_pct), LV_PCT(h_pct));
    lv_textarea_set_one_line(ta, false);
    purr_wid_t wid = alloc_wid(ta);
    group_add(ta);
    return wid;
}

static void tb_ta_append(purr_wid_t wid, const char *text) {
    lv_obj_t *o = get_wid(wid);
    if (o) lv_textarea_add_text(o, text);
}
static void tb_ta_set(purr_wid_t wid, const char *text) {
    lv_obj_t *o = get_wid(wid);
    if (o) lv_textarea_set_text(o, text);
}
static void tb_ta_clear(purr_wid_t wid) {
    lv_obj_t *o = get_wid(wid);
    if (o) lv_textarea_set_text(o, "");
}
static const char *tb_ta_get(purr_wid_t wid) {
    lv_obj_t *o = get_wid(wid);
    return o ? lv_textarea_get_text(o) : NULL;
}

static void tb_ta_focus(purr_wid_t wid) {
    lv_obj_t *o = get_wid(wid);
    if (!o) return;
    lv_group_t *g = mochi_hal_group();
    if (g) {
        lv_group_focus_obj(o);
        // Put the encoder into edit mode on a focused textarea, otherwise a
        // trackball roll steps focus away instead of moving the cursor — the
        // ball is the natural way to reposition within text once you're in a
        // field, and stepping out of it mid-typing is the wrong default.
        lv_group_set_editing(g, true);
    } else {
        lv_obj_add_state(o, LV_STATE_FOCUSED);
    }
}

static void tb_ta_cb(purr_wid_t wid, purr_win_cb_t cb, void *user) {
    lv_obj_t *o = get_wid(wid);
    if (!o || !cb) return;
    cb_ctx_t *ctx = heap_caps_malloc(sizeof(cb_ctx_t), MALLOC_CAP_DEFAULT);
    if (ctx) {
        ctx->cb = cb; ctx->user = user; ctx->wid = wid;
        lv_obj_add_event_cb(o, ta_event_cb, LV_EVENT_VALUE_CHANGED, ctx);
        lv_obj_add_event_cb(o, ctx_delete_cb, LV_EVENT_DELETE, ctx);
    }
}

// ── List ────────────────────────────────────────────────────────────────────

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

static purr_wid_t tb_list_create(purr_win_t h, uint16_t w_pct, uint16_t h_pct) {
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

// Rebuilds must not run synchronously off a caller's background task: apps
// refresh lists from their own timers, and lv_obj_clean() landing mid-gesture
// on the very list being scrolled hangs the render task. Deferring onto the
// render task's own tick means list mutation is never interleaved with an
// in-flight gesture on the same object.
//
// Contract for callers: the items array must stay valid until the next
// lv_timer_handler() tick actually consumes it.
typedef struct { purr_wid_t wid; const char **items; const char **icons; int count; } list_set_ctx_t;

static void tb_list_set_items_async_cb(void *user) {
    list_set_ctx_t *sctx = (list_set_ctx_t *)user;
    purr_kernel_ui_breadcrumb("list_set_items:begin");
    lv_obj_t *list = get_wid(sctx->wid);
    if (list && sctx->wid >= 1 && sctx->wid <= MAX_WIDS) {
        lv_obj_clean(list);
        s_list_meta[sctx->wid - 1].selected_idx = -1;
        for (int i = 0; i < sctx->count; i++) {
            const char *icon = (sctx->icons && sctx->icons[i]) ? sctx->icons[i] : NULL;
            lv_obj_t *btn = lv_list_add_btn(list, icon,
                                             (sctx->items && sctx->items[i]) ? sctx->items[i] : "");
            group_add(btn);   // rows are keyboard/trackball reachable
            cb_ctx_t *ctx = heap_caps_malloc(sizeof(cb_ctx_t), MALLOC_CAP_DEFAULT);
            if (ctx) {
                ctx->cb = NULL; ctx->user = NULL; ctx->wid = sctx->wid;
                lv_obj_add_event_cb(btn, list_btn_event_cb, LV_EVENT_CLICKED, ctx);
                lv_obj_add_event_cb(btn, ctx_delete_cb, LV_EVENT_DELETE, ctx);
            }
        }
    }
    purr_kernel_ui_breadcrumb("list_set_items:end");
    heap_caps_free(sctx);
}

static void tb_list_set_items_ex(purr_wid_t wid, const char **items, const char **icons, int count) {
    if (wid < 1 || wid > MAX_WIDS) return;
    list_set_ctx_t *sctx = heap_caps_malloc(sizeof(list_set_ctx_t), MALLOC_CAP_DEFAULT);
    if (!sctx) return;
    sctx->wid = wid; sctx->items = items; sctx->icons = icons; sctx->count = count;
    lv_async_call(tb_list_set_items_async_cb, sctx);
}

static void tb_list_set_items(purr_wid_t wid, const char **items, int count) {
    tb_list_set_items_ex(wid, items, NULL, count);
}
static void tb_list_clear(purr_wid_t wid) {
    tb_list_set_items(wid, NULL, 0);
}
static int tb_list_get_selected(purr_wid_t wid) {
    if (wid < 1 || wid > MAX_WIDS) return -1;
    return s_list_meta[wid - 1].selected_idx;
}
static void tb_list_set_selected(purr_wid_t wid, int index) {
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
static void tb_list_cb(purr_wid_t wid, purr_win_cb_t cb, void *user) {
    if (wid < 1 || wid > MAX_WIDS) return;
    s_list_meta[wid - 1].cb = cb;
    s_list_meta[wid - 1].user = user;
}

// ── Tile grid ───────────────────────────────────────────────────────────────

#define TILE_W  64
#define TILE_H  84

static purr_wid_t tb_tile_grid_create(purr_win_t h, uint16_t w_pct, uint16_t h_pct) {
    lv_obj_t *parent = content_parent(h);
    if (!parent) return 0;
    lv_obj_t *grid = lv_obj_create(parent);
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, LV_PCT(w_pct), LV_PCT(h_pct));
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_scroll_dir(grid, LV_DIR_VER);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_layout(grid, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    // Fixed pad_row/column rather than SPACE_EVENLY's leftover-space gap,
    // which collapses to 0 once rows overflow the container.
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(grid, 8, 0);
    lv_obj_set_style_pad_row(grid, 10, 0);
    lv_obj_set_style_pad_column(grid, 10, 0);
    return alloc_wid(grid);
}

typedef struct { purr_win_cb_t cb; void *user; purr_wid_t wid; } tile_cb_ctx_t;

static void tile_event_cb(lv_event_t *e) {
    tile_cb_ctx_t *ctx = (tile_cb_ctx_t *)lv_event_get_user_data(e);
    if (ctx && ctx->cb) ctx->cb(ctx->wid, PURR_EVENT_CLICKED, ctx->user);
}
static void tile_ctx_delete_cb(lv_event_t *e) {
    tile_cb_ctx_t *ctx = (tile_cb_ctx_t *)lv_event_get_user_data(e);
    if (ctx) heap_caps_free(ctx);
}

typedef struct {
    purr_wid_t grid_wid;
    const char **labels;
    const char **symbols;
    const uint32_t *colors;
    purr_win_cb_t *cbs;
    void **users;
    int count;
} tile_set_ctx_t;

static void tb_tile_grid_set_items_async_cb(void *user) {
    tile_set_ctx_t *sctx = (tile_set_ctx_t *)user;
    lv_obj_t *grid = get_wid(sctx->grid_wid);
    if (grid) {
        lv_obj_clean(grid);
        for (int i = 0; i < sctx->count; i++) {
            lv_obj_t *tile = lv_obj_create(grid);
            lv_obj_remove_style_all(tile);
            lv_obj_set_size(tile, TILE_W, TILE_H);
            lv_obj_set_style_radius(tile, 10, 0);
            uint32_t color = (sctx->colors && sctx->colors[i]) ? sctx->colors[i] : 0x3A3A3Cu;
            lv_obj_set_style_bg_color(tile, lv_color_hex(color), 0);
            lv_obj_set_style_bg_opa(tile, LV_OPA_80, 0);
            lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
            group_add(tile);

            const char *symbol = (sctx->symbols && sctx->symbols[i]) ? sctx->symbols[i] : LV_SYMBOL_FILE;
            lv_obj_t *icon = lv_label_create(tile);
            lv_label_set_text(icon, symbol);
            lv_obj_set_style_text_font(icon, &lv_font_montserrat_32, 0);
            lv_obj_set_style_text_color(icon, lv_color_white(), 0);
            lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 4);
            lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE);

            lv_obj_t *lbl = lv_label_create(tile);
            lv_label_set_text(lbl, (sctx->labels && sctx->labels[i]) ? sctx->labels[i] : "");
            lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
            lv_obj_set_width(lbl, TILE_W - 6);
            lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
            lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, 0, -4);
            lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

            purr_win_cb_t cb     = sctx->cbs   ? sctx->cbs[i]   : NULL;
            void         *cbuser = sctx->users ? sctx->users[i] : NULL;
            if (cb) {
                tile_cb_ctx_t *ctx = heap_caps_malloc(sizeof(tile_cb_ctx_t), MALLOC_CAP_DEFAULT);
                if (ctx) {
                    ctx->cb = cb; ctx->user = cbuser; ctx->wid = sctx->grid_wid;
                    lv_obj_add_event_cb(tile, tile_event_cb, LV_EVENT_CLICKED, ctx);
                    lv_obj_add_event_cb(tile, tile_ctx_delete_cb, LV_EVENT_DELETE, ctx);
                }
            }
        }
    }
    heap_caps_free(sctx);
}

static void tb_tile_grid_set_items(purr_wid_t wid, const char **labels, const char **symbols,
                                    const uint32_t *colors, purr_win_cb_t *cbs, void **users, int count) {
    if (wid < 1 || wid > MAX_WIDS) return;
    tile_set_ctx_t *sctx = heap_caps_malloc(sizeof(tile_set_ctx_t), MALLOC_CAP_DEFAULT);
    if (!sctx) return;
    sctx->grid_wid = wid;
    sctx->labels = labels; sctx->symbols = symbols; sctx->colors = colors;
    sctx->cbs = cbs; sctx->users = users; sctx->count = count;
    lv_async_call(tb_tile_grid_set_items_async_cb, sctx);
}

// ── Layout ──────────────────────────────────────────────────────────────────

static purr_wid_t tb_layout_begin(purr_win_t h, purr_layout_t dir, uint8_t pad, bool grow) {
    lv_obj_t *parent = content_parent(h);
    if (!parent) return 0;
    lv_obj_t *cont = lv_obj_create(parent);
    lv_obj_set_size(cont, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(cont, pad, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_layout(cont, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(cont, (dir == PURR_LAYOUT_ROW) ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    // Lets a row/col fill remaining space instead of hugging content, so
    // percentage-sized children (lists, textareas) resolve against a real size.
    if (grow) lv_obj_set_flex_grow(cont, 1);

    purr_wid_t wid = alloc_wid(cont);
    if (h >= 1 && h <= MAX_WINS) s_active_layout[h - 1] = cont;
    if (wid >= 1 && wid <= MAX_WIDS) s_layout_owner_win[wid - 1] = (int)(h - 1);
    return wid;
}

static void tb_layout_end(purr_wid_t wid) {
    if (wid < 1 || wid > MAX_WIDS) return;
    int owner = s_layout_owner_win[wid - 1];
    if (owner >= 0 && owner < MAX_WINS) s_active_layout[owner] = NULL;
}

// ── Keyboard ────────────────────────────────────────────────────────────────

static void tb_kb_show(purr_win_t h, purr_wid_t target) {
    // A device with real keys doesn't need the on-screen keyboard eating a
    // third of a 240px-tall panel.
    if (mochi_hal_has_physical_keyboard()) return;
    lv_obj_t *w  = get_win(h);
    lv_obj_t *ta = get_wid(target);
    if (!w || !ta) return;
    if (!s_keyboard) s_keyboard = lv_keyboard_create(lv_scr_act());
    lv_keyboard_set_textarea(s_keyboard, ta);
    s_keyboard_owner_win = h;
    lv_obj_clear_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_keyboard);
}

static void tb_kb_hide(purr_win_t h) {
    (void)h;
    if (s_keyboard) lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
}

// ── Registration ────────────────────────────────────────────────────────────

static const catcall_ui_t s_mochi_win = {
    .name            = "mochi",
    .catcall_version = CATCALL_UI_VERSION,
    .win_create      = tb_win_create,
    .win_destroy     = tb_win_destroy,
    .win_show        = tb_win_show,
    .win_hide        = tb_win_hide,
    .win_clear       = tb_win_clear,
    .win_on_close    = tb_win_on_close,
    .label_create    = tb_label_create,
    .label_set       = tb_label_set,
    .label_align     = tb_label_align,
    .label_set_big   = tb_label_set_big,
    .btn_create      = tb_btn_create,
    .btn_enable      = tb_btn_enable,
    .textarea_create = tb_ta_create,
    .textarea_append = tb_ta_append,
    .textarea_set    = tb_ta_set,
    .textarea_clear  = tb_ta_clear,
    .textarea_get    = tb_ta_get,
    .textarea_focus  = tb_ta_focus,
    .textarea_cb     = tb_ta_cb,
    .list_create       = tb_list_create,
    .list_set_items    = tb_list_set_items,
    .list_clear        = tb_list_clear,
    .list_get_selected = tb_list_get_selected,
    .list_set_selected = tb_list_set_selected,
    .list_cb           = tb_list_cb,
    .list_set_items_icon = tb_list_set_items_ex,
    .tile_grid_create    = tb_tile_grid_create,
    .tile_grid_set_items = tb_tile_grid_set_items,
    .layout_begin    = tb_layout_begin,
    .layout_end      = tb_layout_end,
    .kb_show         = tb_kb_show,
    .kb_hide         = tb_kb_hide,
};

void mochi_win_register(void) {
    purr_kernel_register_ui(&s_mochi_win);
}
