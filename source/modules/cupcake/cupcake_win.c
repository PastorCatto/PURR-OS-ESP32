// cupcake_win.c — catcall_ui_t backend for Cupcake (LVGL)
//
// Forked from cardstack_win.c (window/label/button/textarea/layout/keyboard),
// plus the list-widget implementation ported from kittenui_win.c (cardstack's
// own backend doesn't have list support yet). Registers via
// purr_kernel_register_ui() during cupcake module init.
//
// App windows are created hidden, parented directly to lv_scr_act() — same
// layer as the home screen/drawer, so win_show() pops the app full-screen on
// top of everything. A close button is added to every window's title bar
// that just hides the window, returning to the home screen (no dim-overlay
// hookup needed here — Cupcake has no card stack to restore into).

#include "lvgl.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "../../kernel/catcalls/catcall_ui.h"
#include "../../kernel/catcalls/catcall_input.h"
#include "../../kernel/core/purr_kernel.h"
#include "cupcake.h"

static const char *TAG = "cupcake_win";

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
// Registration (ck_win_on_close() below) has to stay a valid catcall
// implementation regardless — app_manager.c calls purr_win_on_close()
// generically for every window on every UI backend, MiniWin's own title-bar
// close icon still relies on this exact mechanism firing — but nothing in
// this file ever invokes a stored callback anymore now that Cupcake's own
// per-window close button is gone (ck_win_create() no longer creates one;
// the Lollipop nav bar's Back button calls app_manager_stop() directly
// instead, bypassing this hook entirely). Left in place as inert-but-
// necessary infrastructure, not dead code to clean up.
typedef struct { purr_win_cb_t cb; void *user; } close_hook_t;
static close_hook_t s_close_hooks[MAX_WINS];

// Single on-screen keyboard shared by every window (see ck_kb_show()/
// ck_kb_hide() below) — s_keyboard_owner_win tracks which window's textarea
// it's currently bound to, so ck_win_destroy() can unbind before that
// window's textarea is freed out from under it (see its comment).
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

// Breadcrumbed wrapper around the deferred window delete queued by
// ck_win_destroy() — lv_obj_del_async() itself is vendored LVGL and can't
// carry a breadcrumb, but it still runs inside the same lv_timer_handler()
// pass (just outside the originating click callback's stack), so if the
// actual lv_obj_del() teardown is where a hang lives, this is the only way
// to tell that apart from "something else in that frame hung instead."
static void win_del_async_cb(void *obj) {
    purr_kernel_ui_breadcrumb("win_del_async:begin");
    lv_obj_del((lv_obj_t *)obj);
    purr_kernel_ui_breadcrumb("win_del_async:end");
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
// No title bar, no per-window minimize/close buttons — the Lollipop nav bar
// (cupcake_ui.c) now owns both of those system-wide: Home minimizes the
// foreground app, Back closes it (app_manager_stop(), same call app_manager_
// on_win_close() below already makes when a window closes some other way).
// header_height=0 in lv_win_create() below means lv_win reserves no header
// row at all, so the whole window is content — true full screen, and no
// button row left to conflict with the status bar's drag hotzone at the top
// (that hotzone has to stay touch-live even while visually hidden, to catch
// its own reveal gesture — a real title-bar button sitting under it would
// have been unreachable, which is exactly why windows used to start below
// PEEK_H instead of at y=0).

static purr_win_t ck_win_create(const char *title) {
    lv_obj_t *scr = lv_scr_act();
    lv_obj_t *win = lv_win_create(scr, 0);
    ESP_LOGI(TAG, "win_create '%s': scr=%p win=%p scr_children_before=%d core=%d",
             title, (void *)scr, (void *)win, (int)lv_obj_get_child_cnt(scr), xPortGetCoreID());

    purr_win_t handle = alloc_win(win);

    // Genuinely full screen now — no status bar / nav bar space reserved.
    // Both are lv_layer_top() overlays that auto-hide while an app is
    // foreground (see cupcake_ui.c), so nothing permanent is lost underneath.
    lv_obj_set_size(win, cupcake_hal_width(), cupcake_hal_height());
    lv_obj_set_pos(win, 0, 0);
    lv_obj_add_flag(win, LV_OBJ_FLAG_HIDDEN);

    // See cardstack_win.c's cw_win_create for why the content area needs an
    // explicit column flex — lv_win's content container has none by default,
    // and apps assume top-level widgets stack vertically in creation order.
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

static void ck_win_destroy(purr_win_t h) {
    lv_obj_t *w = get_win(h);
    if (s_keyboard && s_keyboard_owner_win == h) {
        // This window owns the textarea the on-screen keyboard is currently
        // bound to — unbind before lv_obj_del() frees it below, otherwise
        // the keyboard keeps a dangling lv_obj_t* and the next keypress
        // anywhere crashes (LoadProhibited inside lv_obj_get_parent(),
        // reached via lv_keyboard_def_event_cb -> lv_textarea_add_text on
        // the freed textarea) — confirmed live via a real crash after
        // closing a dialog (e.g. fileman's "New Folder" prompt) while its
        // textarea was still the keyboard's target.
        lv_keyboard_set_textarea(s_keyboard, NULL);
        lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
        s_keyboard_owner_win = 0;
    }
    // Deferred, not lv_obj_del(w) directly — this runs synchronously inside
    // app_manager_stop(), which itself runs from inside an LVGL click event
    // (the nav bar's Back button, the Running Apps panel's Kill button,
    // etc.). Deleting a window's whole object tree mid-dispatch, before
    // lv_timer_handler()'s own redraw/anim passes for this same frame have
    // run, is exactly what LVGL's own docs warn against for lv_obj_del()
    // called from an event callback — use lv_obj_del_async() (via
    // win_del_async_cb, above) instead, which defers the actual teardown
    // to the start of the next lv_timer_handler() tick, outside any
    // event's call stack. Confirmed live: closing an app (e.g. Terminal)
    // could hang cupcake_task forever partway through the very next
    // lv_timer_handler() call after app_manager_stop() itself had already
    // returned and logged "stopped: <app>" — i.e. the hang was downstream
    // of this synchronous delete, not inside app_manager_stop() itself.
    if (w) lv_async_call(win_del_async_cb, w);
    if (h >= 1 && h <= MAX_WINS) {
        s_active_layout[h - 1] = NULL;
        s_close_hooks[h - 1].cb = NULL;
        s_close_hooks[h - 1].user = NULL;
    }
    free_win(h);
}

static void ck_win_on_close(purr_win_t h, purr_win_cb_t cb, void *user) {
    if (h < 1 || h > MAX_WINS) return;
    s_close_hooks[h - 1].cb = cb;
    s_close_hooks[h - 1].user = user;
}

static void ck_win_show(purr_win_t h) {
    lv_obj_t *w = get_win(h);
    if (!w) { ESP_LOGW(TAG, "win_show: handle=%u has no lv_obj!", (unsigned)h); return; }
    lv_obj_clear_flag(w, LV_OBJ_FLAG_HIDDEN);
    // Every app window is a same-parent (lv_scr_act()) sibling of the home
    // screen, the drawer, and every other app's window — move_foreground()
    // makes this one the last child, i.e. painted last/on top of all of
    // them. The status bar/panels live on lv_layer_top(), a separate LVGL
    // layer always composited above lv_scr_act() regardless of sibling
    // order, so it's structurally never covered by this.
    lv_obj_move_foreground(w);
    lv_area_t coords;
    lv_obj_get_coords(w, &coords);
    ESP_LOGI(TAG, "win_show: handle=%u obj=%p hidden=%d abs=(%d,%d)-(%d,%d) idx=%d/%d core=%d",
             (unsigned)h, (void *)w, (int)lv_obj_has_flag(w, LV_OBJ_FLAG_HIDDEN),
             (int)coords.x1, (int)coords.y1, (int)coords.x2, (int)coords.y2,
             (int)lv_obj_get_index(w), (int)lv_obj_get_child_cnt(lv_obj_get_parent(w)),
             xPortGetCoreID());
}

static void ck_win_hide(purr_win_t h) {
    lv_obj_t *w = get_win(h);
    if (w) lv_obj_add_flag(w, LV_OBJ_FLAG_HIDDEN);
}

static void ck_win_clear(purr_win_t h) {
    lv_obj_t *w = get_win(h);
    if (!w) return;
    lv_obj_t *content = lv_win_get_content(w);
    if (content) lv_obj_clean(content);
}

// ── Labels ────────────────────────────────────────────────────────────────────

static purr_wid_t ck_label_create(purr_win_t h, const char *text) {
    lv_obj_t *parent = content_parent(h);
    if (!parent) return 0;
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, LV_PCT(100));
    return alloc_wid(lbl);
}

static void ck_label_set(purr_wid_t wid, const char *text) {
    lv_obj_t *o = get_wid(wid);
    if (o) lv_label_set_text(o, text);
}

// Banner-style big text (Milk Bottle's incoming-message display) — same
// widget label_create() returned, just re-styled bigger+centered on set.
// lv_font_montserrat_32 is already linked into every build (confirmed
// available alongside 8-48 tonight) so no new font asset is needed.
static void ck_label_set_big(purr_wid_t wid, const char *text) {
    lv_obj_t *o = get_wid(wid);
    if (!o) return;
    lv_label_set_text(o, text);
    lv_obj_set_style_text_font(o, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_align(o, LV_TEXT_ALIGN_CENTER, 0);
}

static void ck_label_align(purr_wid_t wid, purr_align_t align) {
    lv_obj_t *o = get_wid(wid);
    if (!o) return;
    lv_text_align_t a = (align == PURR_ALIGN_CENTER) ? LV_TEXT_ALIGN_CENTER :
                        (align == PURR_ALIGN_RIGHT)  ? LV_TEXT_ALIGN_RIGHT  :
                                                        LV_TEXT_ALIGN_LEFT;
    lv_obj_set_style_text_align(o, a, 0);
}

// ── Buttons ───────────────────────────────────────────────────────────────────

static purr_wid_t ck_btn_create(purr_win_t h, const char *label,
                                  purr_win_cb_t cb, void *user) {
    lv_obj_t *parent = content_parent(h);
    if (!parent) return 0;
    lv_obj_t *btn = lv_btn_create(parent);
    // LVGL's default button size (~100px wide) was harmless while every
    // widget stacked one-per-line, but now that row/col grouping actually
    // renders side by side (calculator's 4-wide grid, settings' button
    // rows), it overflowed a 320px-class screen. Fixed compact height,
    // width shrinks to fit each label instead.
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

static void ck_btn_enable(purr_wid_t wid, bool enabled) {
    lv_obj_t *o = get_wid(wid);
    if (!o) return;
    if (enabled) lv_obj_clear_state(o, LV_STATE_DISABLED);
    else         lv_obj_add_state(o, LV_STATE_DISABLED);
}

// ── Textarea ──────────────────────────────────────────────────────────────────

static purr_wid_t ck_ta_create(purr_win_t h, uint16_t w_pct, uint16_t h_pct) {
    lv_obj_t *parent = content_parent(h);
    if (!parent) return 0;
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_obj_set_size(ta, LV_PCT(w_pct), LV_PCT(h_pct));
    lv_textarea_set_one_line(ta, false);
    // Membership in the physical-keyboard group, not focus — every textarea
    // joins so ck_ta_focus() below can pick the right one later. NULL group
    // (no input catcall registered) just means this app is touch/on-screen-
    // keyboard-only, same as before this bridge existed.
    lv_group_t *g = cupcake_hal_keypad_group();
    if (g) lv_group_add_obj(g, ta);
    return alloc_wid(ta);
}

static void ck_ta_append(purr_wid_t wid, const char *text) {
    lv_obj_t *o = get_wid(wid);
    if (o) lv_textarea_add_text(o, text);
}

static void ck_ta_set(purr_wid_t wid, const char *text) {
    lv_obj_t *o = get_wid(wid);
    if (o) lv_textarea_set_text(o, text);
}

static void ck_ta_clear(purr_wid_t wid) {
    lv_obj_t *o = get_wid(wid);
    if (o) lv_textarea_set_text(o, "");
}

static const char *ck_ta_get(purr_wid_t wid) {
    lv_obj_t *o = get_wid(wid);
    return o ? lv_textarea_get_text(o) : NULL;
}

static void ck_ta_focus(purr_wid_t wid) {
    lv_obj_t *o = get_wid(wid);
    if (!o) return;
    lv_group_t *g = cupcake_hal_keypad_group();
    // lv_group_focus_obj() applies LV_STATE_FOCUSED itself and, more
    // importantly, retargets the keypad indev's group so BBQ20 keystrokes
    // land on this textarea instead of whichever one last had focus.
    if (g) lv_group_focus_obj(o);
    else   lv_obj_add_state(o, LV_STATE_FOCUSED);
}

static void ck_ta_cb(purr_wid_t wid, purr_win_cb_t cb, void *user) {
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
// Ported from kittenui_win.c's kw_list_* — cardstack_win.c never grew this,
// so this is the first Cupcake-side widget to fork from KittenUI rather than
// Cardstack. Same caveat as there: no lv_group/encoder navigation yet, so a
// tap fires SELECTED immediately followed by ACTIVATED.

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

// Diagnostic only — pinpoints whether a hang is inside LVGL's own scroll/
// gesture processing itself (as opposed to anything this file's other
// breadcrumbs already cover: window teardown, list rebuild, MSN's refresh
// path). Confirmed useful live: a hang reproduced with none of those
// breadcrumbs set, meaning it never left plain "timer_handler" — this
// narrows it down further without needing to patch vendored LVGL.
static void list_scroll_breadcrumb_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_SCROLL_BEGIN)      purr_kernel_ui_breadcrumb("list:scroll_begin");
    else if (code == LV_EVENT_SCROLL)       purr_kernel_ui_breadcrumb("list:scroll");
    else if (code == LV_EVENT_SCROLL_END)   purr_kernel_ui_breadcrumb("list:scroll_end");
    else if (code == LV_EVENT_PRESSED)      purr_kernel_ui_breadcrumb("list:pressed");
    else if (code == LV_EVENT_RELEASED)     purr_kernel_ui_breadcrumb("list:released");
}

static purr_wid_t ck_list_create(purr_win_t h, uint16_t w_pct, uint16_t h_pct) {
    lv_obj_t *parent = content_parent(h);
    if (!parent) return 0;
    lv_obj_t *list = lv_list_create(parent);
    lv_obj_set_size(list, LV_PCT(w_pct), LV_PCT(h_pct));
    lv_obj_add_event_cb(list, list_scroll_breadcrumb_cb, LV_EVENT_SCROLL_BEGIN, NULL);
    lv_obj_add_event_cb(list, list_scroll_breadcrumb_cb, LV_EVENT_SCROLL, NULL);
    lv_obj_add_event_cb(list, list_scroll_breadcrumb_cb, LV_EVENT_SCROLL_END, NULL);
    lv_obj_add_event_cb(list, list_scroll_breadcrumb_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(list, list_scroll_breadcrumb_cb, LV_EVENT_RELEASED, NULL);
    purr_wid_t wid = alloc_wid(list);
    if (wid >= 1 && wid <= MAX_WIDS) {
        s_list_meta[wid - 1].cb = NULL;
        s_list_meta[wid - 1].user = NULL;
        s_list_meta[wid - 1].selected_idx = -1;
    }
    return wid;
}

// Rebuilding a list's rows means lv_obj_clean()-ing every existing row —
// synchronous, immediate deletion of whatever's there right now. Several
// callers (e.g. msn.c's buddy_refresh_task()) call purr_win_list_set_items()
// on a periodic timer from their own background task, not from cupcake_task
// — if that lands while the user is mid-touch/mid-scroll on the very list
// being rebuilt, cupcake_task's own indev processing for that same list is
// running concurrently on another core, serialized only by the ui_lock
// between individual LVGL calls, not across the whole gesture. Confirmed
// live: scrolling MSN's buddy list while its background refresh timer fires
// hung cupcake_task. Deferring the actual rebuild onto cupcake_task's own
// tick (lv_async_call, same pattern ck_win_destroy() uses) means list
// mutation always happens from the one task already driving lv_timer_handler()
// and its indev reads, never interleaved with an in-flight gesture on the
// same object from a different task.
typedef struct { purr_wid_t wid; const char **items; const char **icons; int count; } list_set_ctx_t;

static void ck_list_set_items_async_cb(void *user) {
    list_set_ctx_t *sctx = (list_set_ctx_t *)user;
    purr_kernel_ui_breadcrumb("list_set_items:begin");
    lv_obj_t *list = get_wid(sctx->wid);
    if (list && sctx->wid >= 1 && sctx->wid <= MAX_WIDS) {
        lv_obj_clean(list);
        s_list_meta[sctx->wid - 1].selected_idx = -1;
        for (int i = 0; i < sctx->count; i++) {
            const char *icon = (sctx->icons && sctx->icons[i]) ? sctx->icons[i] : NULL;
            lv_obj_t *btn = lv_list_add_btn(list, icon, (sctx->items && sctx->items[i]) ? sctx->items[i] : "");
            cb_ctx_t *ctx = heap_caps_malloc(sizeof(cb_ctx_t), MALLOC_CAP_DEFAULT);
            if (ctx) {
                ctx->cb = NULL;
                ctx->user = NULL;
                ctx->wid = sctx->wid;
                lv_obj_add_event_cb(btn, list_btn_event_cb, LV_EVENT_CLICKED, ctx);
            }
        }
    }
    purr_kernel_ui_breadcrumb("list_set_items:end");
    heap_caps_free(sctx);
}

static void ck_list_set_items_ex(purr_wid_t wid, const char **items, const char **icons, int count) {
    if (wid < 1 || wid > MAX_WIDS) return;
    list_set_ctx_t *sctx = heap_caps_malloc(sizeof(list_set_ctx_t), MALLOC_CAP_DEFAULT);
    if (!sctx) return;
    sctx->wid = wid;
    sctx->items = items;
    sctx->icons = icons;
    sctx->count = count;
    lv_async_call(ck_list_set_items_async_cb, sctx);
}

static void ck_list_set_items(purr_wid_t wid, const char **items, int count) {
    ck_list_set_items_ex(wid, items, NULL, count);
}

// icons[i] is an LV_SYMBOL_* string constant (a font glyph baked into
// LVGL's own symbol font, not a bitmap asset) or NULL for no icon, matching
// lv_list_add_btn()'s own (list, icon, txt) shape. See cupcake.h's doc
// comment on the caller-facing contract, including the same deferred-
// rebuild lifetime requirement as items — both arrays must stay valid
// until the next lv_timer_handler() tick actually consumes them.
void cupcake_win_list_set_items_icon(purr_wid_t wid, const char **items, const char **icons, int count) {
    ck_list_set_items_ex(wid, items, icons, count);
}

static void ck_list_clear(purr_wid_t wid) {
    ck_list_set_items(wid, NULL, 0);
}

static int ck_list_get_selected(purr_wid_t wid) {
    if (wid < 1 || wid > MAX_WIDS) return -1;
    return s_list_meta[wid - 1].selected_idx;
}

static void ck_list_set_selected(purr_wid_t wid, int index) {
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

static void ck_list_cb(purr_wid_t wid, purr_win_cb_t cb, void *user) {
    if (wid < 1 || wid > MAX_WIDS) return;
    s_list_meta[wid - 1].cb = cb;
    s_list_meta[wid - 1].user = user;
}

// ── Layout ────────────────────────────────────────────────────────────────────

static purr_wid_t ck_layout_begin(purr_win_t h, purr_layout_t dir, uint8_t pad, bool grow) {
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
    // Overrides the main-axis size of `cont` within its own parent's flex
    // layout (LVGL's grow, not the flex_align above, which only affects how
    // *this* container's own children are placed) — lets a row/col fill the
    // rest of the window's vertical space instead of hugging its content, so
    // percentage-sized children (lists, textareas) resolve against a real size.
    if (grow) lv_obj_set_flex_grow(cont, 1);

    purr_wid_t wid = alloc_wid(cont);
    if (h >= 1 && h <= MAX_WINS) s_active_layout[h - 1] = cont;
    if (wid >= 1 && wid <= MAX_WIDS) s_layout_owner_win[wid - 1] = (int)(h - 1);
    return wid;
}

static void ck_layout_end(purr_wid_t wid) {
    if (wid < 1 || wid > MAX_WIDS) return;
    int owner = s_layout_owner_win[wid - 1];
    if (owner >= 0 && owner < MAX_WINS) s_active_layout[owner] = NULL;
}

// ── Keyboard ──────────────────────────────────────────────────────────────────
// s_keyboard/s_keyboard_owner_win are declared near the top of this file
// (alongside s_close_hooks) since ck_win_destroy() needs them too.

// Devices with a real physical keyboard (T-Deck's bbq20) don't need the
// LVGL on-screen one taking up screen space too — set_backlight is only
// ever implemented by keyboard-class input drivers in this codebase (see
// catcall_input.h's own doc comment on that field), so its presence among
// registered inputs is what this checks instead of hardcoding a driver
// name here.
static bool ck_has_physical_keyboard(void) {
    int n = purr_kernel_input_count();
    for (int i = 0; i < n; i++) {
        const catcall_input_t *inp = purr_kernel_input_at(i);
        if (inp && inp->set_backlight) return true;
    }
    return false;
}

static void ck_kb_show(purr_win_t h, purr_wid_t target) {
    if (ck_has_physical_keyboard()) return;
    lv_obj_t *w = get_win(h);
    lv_obj_t *ta = get_wid(target);
    if (!w || !ta) return;
    if (!s_keyboard) {
        s_keyboard = lv_keyboard_create(lv_scr_act());
    }
    lv_keyboard_set_textarea(s_keyboard, ta);
    s_keyboard_owner_win = h;
    lv_obj_clear_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_keyboard);
}

static void ck_kb_hide(purr_win_t h) {
    (void)h;
    if (s_keyboard) lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
}

// ── Registration ──────────────────────────────────────────────────────────────

static const catcall_ui_t s_cupcake_win = {
    .name            = "cupcake",
    .catcall_version = CATCALL_UI_VERSION,
    .win_create      = ck_win_create,
    .win_destroy     = ck_win_destroy,
    .win_show        = ck_win_show,
    .win_hide        = ck_win_hide,
    .win_clear       = ck_win_clear,
    .win_on_close    = ck_win_on_close,
    .label_create    = ck_label_create,
    .label_set       = ck_label_set,
    .label_align     = ck_label_align,
    .label_set_big   = ck_label_set_big,
    .btn_create      = ck_btn_create,
    .btn_enable      = ck_btn_enable,
    .textarea_create = ck_ta_create,
    .textarea_append = ck_ta_append,
    .textarea_set    = ck_ta_set,
    .textarea_clear  = ck_ta_clear,
    .textarea_get    = ck_ta_get,
    .textarea_focus  = ck_ta_focus,
    .textarea_cb     = ck_ta_cb,
    .list_create       = ck_list_create,
    .list_set_items    = ck_list_set_items,
    .list_clear        = ck_list_clear,
    .list_get_selected = ck_list_get_selected,
    .list_set_selected = ck_list_set_selected,
    .list_cb           = ck_list_cb,
    .layout_begin    = ck_layout_begin,
    .layout_end      = ck_layout_end,
    .kb_show         = ck_kb_show,
    .kb_hide         = ck_kb_hide,
};

void cupcake_win_register(void) {
    purr_kernel_register_ui(&s_cupcake_win);
}
