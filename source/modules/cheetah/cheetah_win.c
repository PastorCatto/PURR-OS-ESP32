// cheetah_win.c — catcall_ui_t backend for Cheetah (LVGL).
//
// Adapted directly from mochi_win.c (itself descended from tabby_win.c /
// cupcake_win.c), not written fresh: window management is generic — every
// app already targets the shared catcall_ui_t contract regardless of which
// UI backend is active — and this lineage already encodes several hard-won,
// live-confirmed fixes that are not obvious from the contract alone —
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
//     leaks one allocation per button/textarea/list;
//   * EVERY focusable widget joins the shared keyboard/encoder group, not
//     just textareas — keyboard/trackball can reach a button, not only a
//     text field, which matters on a keyboard-first device like T-Deck Plus;
//   * widgets are removed from the group on delete, so a destroyed window
//     doesn't leave dangling group entries the encoder can still step onto;
//   * the on-screen keyboard is suppressed whenever a physical keyboard
//     exists, reclaiming that screen area.
//
// The row/section/grouped-table rendering below (tb_btn_create's "iOS 7
// grouped table" styling in particular) is inherited scaffolding, not a
// deliberate BB10 choice — it is what makes menus/lists/buttons render
// correctly and cheaply from day one. A BB10 visual pass is explicitly a
// later phase (see the Cheetah plan's Polish step), not part of getting this
// backend booting.
//
// App windows are created hidden, parented to lv_scr_act(), full-screen, with
// no title bar: the system UI owns the status bar/panels system-wide (with
// its nav bar suppressed — see purr_systemui_host_t::suppress_navbar), and
// Cheetah's Home Screen owns launching + minimizing.

#include <string.h>
#include "lvgl.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "../../kernel/catcalls/catcall_ui.h"
#include "../../kernel/catcalls/catcall_input.h"
#include "../../kernel/core/purr_kernel.h"
#include "../systemui/systemui.h"   // purr_systemui_fx_bg_opa_keep()
#include "cheetah.h"

static const char *TAG = "cheetah_win";

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

// ── App-widget theme: iOS-7 grouped table, light + dark ─────────────────────
// Every colour used anywhere in this file goes through one of these, checked
// against purr_kernel_dark_mode_enabled() (purr_kernel.h) — same "read at
// CONSTRUCTION time, not per frame" caveat purr_kernel_ui_effects_enabled()
// already carries: toggling it doesn't retroactively repaint an
// already-built window, only ones created after. Values are real iOS system
// colours (systemGroupedBackground / secondarySystemGroupedBackground /
// separator / label / secondaryLabel) — iOS 7 itself predates Apple's own
// dark mode, but the grouped-table construction this file already uses is
// the same shape either era, so this is a palette swap, not a different
// rendering approach. Declared up here, ahead of tb_win_create() below,
// which needs theme_group_ground(); the "iOS 7 grouped table" section
// further down just references these via #define, not full definitions.
static inline lv_color_t theme_cell_bg(void) {
    return purr_kernel_dark_mode_enabled() ? lv_color_hex(0x1C1C1E) : lv_color_hex(0xFFFFFF);
}
static inline lv_color_t theme_cell_text(void) {
    return purr_kernel_dark_mode_enabled() ? lv_color_hex(0xFFFFFF) : lv_color_hex(0x000000);
}
static inline lv_color_t theme_separator(void) {
    return purr_kernel_dark_mode_enabled() ? lv_color_hex(0x38383A) : lv_color_hex(0xC8C7CC);
}
// The ground behind grouped cells — what makes a white/dark cell read as a
// "card" rather than a floating slab. secondarySystemGroupedBackground is
// the cell colour above; this is systemGroupedBackground, the level behind
// it (Apple's own dark grouped background is pure black).
static inline lv_color_t theme_group_ground(void) {
    return purr_kernel_dark_mode_enabled() ? lv_color_hex(0x000000) : lv_color_hex(0xEFEFF4);
}
// secondaryLabel — section headers and right-aligned detail text share this
// one role in real iOS Settings, so one function covers both call sites.
static inline lv_color_t theme_dim_text(void) {
    return purr_kernel_dark_mode_enabled() ? lv_color_hex(0x98989D) : lv_color_hex(0x6D6D72);
}

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

// Shared "currently shown" stack backing cheetah_win_hide_foreground(). Only one
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
    lv_group_t *g  = cheetah_hal_group();
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

// Ends the open iOS-style cell group for this window. Defined with the grouped
// table further down; called from clear/label/list/textarea/layout above it.
static void group_close(purr_win_t h);

// Make a widget reachable from keyboard/trackball.
static void group_add(lv_obj_t *obj) {
    lv_group_t *g = cheetah_hal_group();
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

// Per-window close button — a red square "X" in the top-right corner, added
// directly by this backend (not through catcall_ui_t: no other backend
// draws window chrome, so this is Cheetah-local, matching how the close-hook
// plumbing below was ALREADY there waiting for exactly this).
//
// app_manager.c already calls purr_win_on_close() for every app window it
// creates (app_manager_on_win_close(), which stops the app), and
// settings.c does the same for its own sub-windows — s_close_hooks[] below
// has stored those registrations since this file was forked from
// mochi_win.c, but nothing ever FIRED one, because no backend drew a
// clickable close affordance. This button is that trigger, generic across
// every caller of purr_win_on_close() — it doesn't know or care what the
// hook actually does.
//
// First cut landed this bottom-left instead of top-right — root cause:
// lv_win.c's own create_root() puts the win container itself in
// LV_FLEX_FLOW_COLUMN (see the vendored lv_win.c, ~line 96), so a plain
// child added after header+content becomes the flex layout's THIRD item
// and gets column-flowed below content at the default (left) cross-align,
// silently overriding the lv_obj_align() call below. LV_OBJ_FLAG_IGNORE_LAYOUT
// is the real fix — it exempts this object from its parent's layout engine
// so manual position/align actually takes effect.
//
// Also sized up ("way bigger" was the direct ask) — 20px was genuinely too
// small a touch target on this panel; 40px is close to the usual ~40-44px
// minimum-touch-target guidance.
#define WIN_CLOSE_BTN_SZ 40

static void win_close_click_cb(lv_event_t *e) {
    purr_win_t h = (purr_win_t)(intptr_t)lv_event_get_user_data(e);
    if (h < 1 || h > MAX_WINS) return;
    if (s_close_hooks[h - 1].cb) {
        s_close_hooks[h - 1].cb(0, PURR_EVENT_CLICKED, s_close_hooks[h - 1].user);
    }
    // The hook above (when one is registered) is what actually stops the
    // app/destroys the window; this always lands back on the desktop
    // afterward regardless, rather than leaving whatever was underneath.
    win_stack_remove(h);
    cheetah_home_go_home();
}

static purr_win_t tb_win_create(const char *title) {
    lv_obj_t *scr = lv_scr_act();
    // header_height 0 — no title bar. Back/Home/Recents are the system UI's
    // nav bar, system-wide, so a per-window button row would be redundant and
    // would collide with the status bar's drag hotzone at the top.
    lv_obj_t *win = lv_win_create(scr, 0);

    purr_win_t handle = alloc_win(win);

    lv_obj_set_size(win, cheetah_hal_width(), cheetah_hal_height());
    lv_obj_set_pos(win, 0, 0);
    lv_obj_add_flag(win, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *close_btn = lv_obj_create(win);
    lv_obj_remove_style_all(close_btn);
    lv_obj_add_flag(close_btn, LV_OBJ_FLAG_IGNORE_LAYOUT);   // exempt from win's own COLUMN flex — see comment above
    lv_obj_set_size(close_btn, WIN_CLOSE_BTN_SZ, WIN_CLOSE_BTN_SZ);
    lv_obj_align(close_btn, LV_ALIGN_TOP_RIGHT, -2, 2);
    lv_obj_set_style_radius(close_btn, 3, 0);
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(0xE81123), 0);   // Windows-close-button red
    lv_obj_set_style_bg_opa(close_btn, LV_OPA_COVER, 0);
    lv_obj_clear_flag(close_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(close_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(close_btn, win_close_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)handle);
    lv_obj_t *close_lbl = lv_label_create(close_btn);
    lv_label_set_text(close_lbl, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(close_lbl, lv_color_white(), 0);
    lv_obj_center(close_lbl);
    lv_obj_clear_flag(close_lbl, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *content = lv_win_get_content(win);
    lv_obj_set_style_pad_all(content, 6, 0);
    lv_obj_set_style_pad_row(content, 6, 0);
    // iOS 7 groupedTableViewBackground. The cells read as cells because of
    // what sits behind them; on the default dark background they would just
    // look like floating slabs. theme_group_ground() picks the light/dark
    // variant — see this file's theme_* functions, declared above.
    lv_obj_set_style_bg_color(content, theme_group_ground(), 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_COVER, 0);
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
        group_close(h);
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

void cheetah_win_hide_foreground(void) {
    while (s_win_stack_count > 0) {
        tb_win_hide(s_win_stack[s_win_stack_count - 1]);   // removes itself
    }
    // Nothing is covering the springboard any more — restore focus to it, and
    // with it the encoder's editing mode, or the trackball goes dead on return
    // from an app.
    cheetah_home_go_home();
}

static void tb_win_clear(purr_win_t h) {
    lv_obj_t *w = get_win(h);
    if (!w) return;
    lv_obj_t *content = lv_win_get_content(w);
    if (content) lv_obj_clean(content);
    // The group container was a child of `content` and has just been freed —
    // clearing the pointer as well, or the next button would be parented to
    // deleted memory.
    group_close(h);
    if (h >= 1 && h <= MAX_WINS) s_active_layout[h - 1] = NULL;
}

// ── Labels ──────────────────────────────────────────────────────────────────

static purr_wid_t tb_label_create(purr_win_t h, const char *text) {
    lv_obj_t *parent = content_parent(h);
    if (!parent) return 0;
    // A label ends any open group and reads as its section header, which is
    // how iOS Settings separates groups. Small, grey, indented to line up with
    // the cell text below it.
    group_close(h);
    lv_obj_t *lbl = lv_label_create(parent);
    lv_obj_set_style_text_color(lbl, theme_dim_text(), 0);
    lv_obj_set_style_pad_left(lbl, 12, 0);
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

// ── iOS 7 grouped table ─────────────────────────────────────────────────────
//
// Buttons in the default vertical cheetah render as ROWS OF A GROUPED LIST, not as
// individual buttons. This is both the look and a performance fix, and the two
// happen to want the same thing.
//
// lv_btn_create() gives LVGL's themed button: its own background, border,
// radius AND shadow, per instance. A settings page is twenty of those — twenty
// separately-styled rounded, shadowed objects, every one re-rendered on every
// scroll frame. Corners alone measured at roughly a third of a frame, and
// shadows are worse (LV_SHADOW_CACHE_SIZE is 0, so blur is recomputed each
// draw). Reported from the device as "scrolling the settings page is laggy as
// shit", which is exactly what that adds up to.
//
// A grouped table pays radius ONCE for the group container and renders each row
// as a flat fill with a one-pixel separator — no per-row radius, no shadow, no
// border. Same information, a fraction of the drawing.
//
// Rows still get their own object because each needs its own click target and
// its own place in the focus group; the saving is in what each object COSTS to
// draw, not in how many there are.
//
// Colour functions (theme_cell_bg() etc.) are declared near the top of this
// file, above tb_win_create() — that function needs theme_group_ground()
// too, before this section is reached.
#define IOS_CELL_BG      theme_cell_bg()
#define IOS_CELL_TEXT    theme_cell_text()
#define IOS_SEPARATOR    theme_separator()
#define IOS_GROUP_RADIUS 10
#define IOS_ROW_H        38

// The group a button lands in. Consecutive buttons share one; anything else
// (a label, list, textarea, or an explicit row/col) closes it, so a label acts
// as a section header exactly as it does in iOS Settings.
static lv_obj_t *s_group[MAX_WINS];

static void group_close(purr_win_t h) {
    if (h >= 1 && h <= MAX_WINS) s_group[h - 1] = NULL;
}

static lv_obj_t *group_open(purr_win_t h, lv_obj_t *parent) {
    if (h < 1 || h > MAX_WINS) return parent;
    if (s_group[h - 1]) return s_group[h - 1];

    lv_obj_t *g = lv_obj_create(parent);
    lv_obj_remove_style_all(g);
    lv_obj_set_width(g, LV_PCT(100));
    lv_obj_set_height(g, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(g, IOS_CELL_BG, 0);
    lv_obj_set_style_bg_opa(g, LV_OPA_COVER, 0);
    // Radius paid once here, for the whole group — the point of the exercise.
    lv_obj_set_style_radius(g, IOS_GROUP_RADIUS, 0);
    lv_obj_set_style_clip_corner(g, true, 0);
    lv_obj_set_flex_flow(g, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(g, LV_OBJ_FLAG_SCROLLABLE);
    s_group[h - 1] = g;
    return g;
}

// ── Menu (sectioned list of actions) ────────────────────────────────────────
//
// The native rendering of catcall_ui_t's menu primitive: an iOS 7 grouped
// table. Headers are grey and inset, each group is one rounded white container,
// rows are flat with a hairline separator and optional right-aligned value.
//
// Radius is paid once per GROUP, never per row — which is the performance point
// of having this in the contract at all. The alternative an app used to write,
// a run of btn_create() calls, produced one themed button per action with its
// own background, border, radius and shadow, all re-rendered on every scroll
// frame.

#define MAX_MENUS 8
typedef struct {
    purr_wid_t    wid;
    purr_win_cb_t cb;
    void         *user;
    int           selected;
} menu_state_t;
static menu_state_t s_menus[MAX_MENUS];

static menu_state_t *menu_find(purr_wid_t wid) {
    for (int i = 0; i < MAX_MENUS; i++) if (s_menus[i].wid == wid) return &s_menus[i];
    return NULL;
}

static void menu_row_cb(lv_event_t *e) {
    // user_data packs (menu wid << 16 | flat row index) so one callback serves
    // every row without allocating a context per row — rows are the thing this
    // primitive exists to make cheap.
    uint32_t packed = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    purr_wid_t mw   = (purr_wid_t)(packed >> 16);
    int        row  = (int)(packed & 0xFFFF);
    menu_state_t *m = menu_find(mw);
    if (!m) return;
    m->selected = row;
    if (m->cb) m->cb(mw, PURR_EVENT_ACTIVATED, m->user);
}

static purr_wid_t tb_menu_create(purr_win_t h) {
    lv_obj_t *parent = content_parent(h);
    if (!parent) return 0;
    group_close(h);

    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_width(root, LV_PCT(100));
    lv_obj_set_flex_grow(root, 1);          // fill the window; the menu IS the screen
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(root, 6, 0);
    lv_obj_set_scroll_dir(root, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(root, LV_SCROLLBAR_MODE_OFF);

    purr_wid_t wid = alloc_wid(root);
    for (int i = 0; i < MAX_MENUS; i++) {
        if (!s_menus[i].wid) {
            s_menus[i].wid = wid; s_menus[i].cb = NULL;
            s_menus[i].user = NULL; s_menus[i].selected = -1;
            break;
        }
    }
    return wid;
}

static void tb_menu_set_sections(purr_wid_t wid, const purr_menu_section_t *sections, int n) {
    lv_obj_t *root = get_wid(wid);
    if (!root || !sections) return;
    lv_obj_clean(root);

    int flat = 0;   // flat row index across sections — the contract's index space
    for (int s = 0; s < n; s++) {
        if (sections[s].header) {
            lv_obj_t *hd = lv_label_create(root);
            lv_label_set_text(hd, sections[s].header);
            lv_obj_set_style_text_color(hd, theme_dim_text(), 0);
            lv_obj_set_style_pad_left(hd, 12, 0);
        }

        lv_obj_t *g = lv_obj_create(root);
        lv_obj_remove_style_all(g);
        lv_obj_set_width(g, LV_PCT(100));
        lv_obj_set_height(g, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(g, IOS_CELL_BG, 0);
        lv_obj_set_style_bg_opa(g, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(g, IOS_GROUP_RADIUS, 0);
        lv_obj_set_style_clip_corner(g, true, 0);
        lv_obj_set_flex_flow(g, LV_FLEX_FLOW_COLUMN);
        lv_obj_clear_flag(g, LV_OBJ_FLAG_SCROLLABLE);

        for (int i = 0; i < sections[s].count; i++, flat++) {
            lv_obj_t *row = lv_obj_create(g);
            lv_obj_remove_style_all(row);
            lv_obj_set_width(row, LV_PCT(100));
            lv_obj_set_height(row, IOS_ROW_H);
            lv_obj_set_style_bg_color(row, IOS_CELL_BG, 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
            // Last row of a group has no separator — iOS draws the group edge
            // instead, and a trailing hairline reads as a broken cell.
            if (i + 1 < sections[s].count) {
                lv_obj_set_style_border_color(row, IOS_SEPARATOR, 0);
                lv_obj_set_style_border_width(row, 1, 0);
                lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
            }
            lv_obj_set_style_pad_hor(row, 12, 0);
            lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(row, menu_row_cb, LV_EVENT_CLICKED,
                                 (void *)(uintptr_t)(((uint32_t)wid << 16) | (uint32_t)flat));
            group_add(row);

            lv_obj_t *l = lv_label_create(row);
            lv_label_set_text(l, sections[s].items[i]);
            lv_obj_set_style_text_color(l, IOS_CELL_TEXT, 0);
            lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);

            const char *val = sections[s].values ? sections[s].values[i] : NULL;
            if (val) {
                lv_obj_t *v = lv_label_create(row);
                lv_label_set_text(v, val);
                lv_obj_set_style_text_color(v, theme_dim_text(), 0);
                lv_obj_align(v, LV_ALIGN_RIGHT_MID, 0, 0);
            }
        }
    }
}

static void tb_menu_cb(purr_wid_t wid, purr_win_cb_t cb, void *user) {
    menu_state_t *m = menu_find(wid);
    if (m) { m->cb = cb; m->user = user; }
}

static int tb_menu_get_selected(purr_wid_t wid) {
    menu_state_t *m = menu_find(wid);
    return m ? m->selected : -1;
}

// ── Buttons ─────────────────────────────────────────────────────────────────

static purr_wid_t tb_btn_create(purr_win_t h, const char *label,
                                 purr_win_cb_t cb, void *user) {
    lv_obj_t *parent = content_parent(h);
    if (!parent) return 0;

    // Inside an explicit purr_win_row()/col() the app has asked for side-by-side
    // placement — a grouped table row would fight that. Keep a compact button
    // there, and reserve the list style for the default vertical cheetah, which is
    // what nearly every settings-style screen actually uses.
    bool in_layout = (h >= 1 && h <= MAX_WINS && s_active_layout[h - 1] != NULL);

    lv_obj_t *btn;
    if (in_layout) {
        // FLAT, not raised. A genuine multi-choice row (Low/Mid/High brightness,
        // screen timeout) is what layouts are for, so these stay buttons — but
        // they do not need to look or cost like LVGL's default themed button.
        //
        // The shadow is the expensive part, not the corner:
        // CONFIG_LV_SHADOW_CACHE_SIZE is 0, so the blur is recomputed on EVERY
        // draw of every button, and a row of three redraws all three on any
        // frame that touches them. Radius at least scales with corner size;
        // an uncached shadow is a fixed per-draw cost for a visual effect that
        // reads as dated anyway.
        //
        // Result is a flat fill with a modest corner — "flat rounded rather
        // than full depth".
        btn = lv_btn_create(parent);
        lv_obj_set_height(btn, 32);
        lv_obj_set_width(btn, LV_SIZE_CONTENT);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_shadow_opa(btn, LV_OPA_TRANSP, 0);
        // Pressed state too — the theme sets its own shadow there, so clearing
        // only the default state leaves it reappearing on touch.
        lv_obj_set_style_shadow_width(btn, 0, LV_STATE_PRESSED);
        lv_obj_set_style_shadow_opa(btn, LV_OPA_TRANSP, LV_STATE_PRESSED);
        lv_obj_set_style_border_width(btn, 0, 0);
        // Flat fill: no gradient. The default theme ramps the background, which
        // is a per-pixel interpolation across the whole button for a gradient
        // nobody asked for.
        lv_obj_set_style_bg_grad_dir(btn, LV_GRAD_DIR_NONE, 0);
        lv_obj_set_style_radius(btn, 6, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x007AFF), 0);   // systemBlue
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);

        lv_obj_t *l = lv_label_create(btn);
        lv_label_set_text(l, label);
        lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), 0);
        lv_obj_center(l);
    } else {
        lv_obj_t *g = group_open(h, parent);
        btn = lv_obj_create(g);
        lv_obj_remove_style_all(btn);          // no theme bg/border/radius/shadow
        lv_obj_set_width(btn, LV_PCT(100));
        lv_obj_set_height(btn, IOS_ROW_H);
        lv_obj_set_style_bg_color(btn, IOS_CELL_BG, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        // Separator is a bottom border, not a child object — one less object
        // per row, and it renders as a straight fill rather than a widget.
        lv_obj_set_style_border_color(btn, IOS_SEPARATOR, 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_border_side(btn, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_pad_left(btn, 12, 0);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *l = lv_label_create(btn);
        lv_label_set_text(l, label);
        lv_obj_set_style_text_color(l, IOS_CELL_TEXT, 0);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
    }

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
    group_close(h);
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
    lv_group_t *g = cheetah_hal_group();
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
    group_close(h);
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
            purr_fx_radius(tile, 10);
            uint32_t color = (sctx->colors && sctx->colors[i]) ? sctx->colors[i] : 0x3A3A3Cu;
            lv_obj_set_style_bg_color(tile, lv_color_hex(color), 0);
            // _keep, not the accent variant: each tile's colour identifies the
            // app it belongs to (sctx->colors), so flooding them all with one
            // accent would make the grid unreadable. Opaque at their own colour.
            purr_systemui_fx_bg_opa_keep(tile, LV_OPA_80);
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
    group_close(h);   // an explicit layout is its own section
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
    if (cheetah_hal_has_physical_keyboard()) return;
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

static const catcall_ui_t s_cheetah_win = {
    .name            = "cheetah",
    .catcall_version = CATCALL_UI_VERSION,
    .menu_create       = tb_menu_create,
    .menu_set_sections = tb_menu_set_sections,
    .menu_cb           = tb_menu_cb,
    .menu_get_selected = tb_menu_get_selected,
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

void cheetah_win_register(void) {
    purr_kernel_register_ui(&s_cheetah_win);
}

// Release the screen so the next backend to load can claim it.
// Called from Cheetah's deinit() — see purr_kernel_unregister_ui() for why
// leaving the registration behind broke speed demon's restore.
void cheetah_win_unregister(void) {
    purr_kernel_unregister_ui(&s_cheetah_win);
}
