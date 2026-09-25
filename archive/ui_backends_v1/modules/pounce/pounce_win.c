// pounce_win.c — full catcall_ui_t implementation for the Pounce UI backend.
// Owns the widget/window pools (A2) and the modal window stack (A3). Every
// mutator redraws synchronously at the call site when its window is the
// visible top of the stack — see pounce_render.c's pw_redraw_widget().
#include "sdkconfig.h"

#ifdef CONFIG_PURR_UI_BACKEND_POUNCE

#include "pounce.h"
#include "../../kernel/core/purr_kernel.h"
#include "esp_heap_caps.h"
#include <string.h>

// ── Pools (declared extern in pounce.h) ──────────────────────────────────────

pw_window_t s_pw_wins[PW_MAX_WINS];
pw_widget_t s_pw_wids[PW_MAX_WIDS];

// ── Modal window stack (A3 — full-screen, no overlapping/floating chrome) ───

static int16_t s_win_stack[PW_MAX_WINS];
static int     s_win_stack_count = 0;

purr_win_t pw_top_window(void) {
    if (s_win_stack_count == 0) return 0;
    return pw_win_handle(s_win_stack[s_win_stack_count - 1]);
}

static void win_stack_remove(int idx) {
    for (int i = 0; i < s_win_stack_count; i++) {
        if (s_win_stack[i] == idx) {
            for (int j = i; j < s_win_stack_count - 1; j++) s_win_stack[j] = s_win_stack[j + 1];
            s_win_stack_count--;
            return;
        }
    }
}

static void win_stack_push(int idx) {
    win_stack_remove(idx);
    if (s_win_stack_count < PW_MAX_WINS) s_win_stack[s_win_stack_count++] = (int16_t)idx;
}

// Whatever's now on top (or the launcher home screen if nothing is) needs a
// full repaint — there's no offscreen buffer, so a revealed window's pixels
// are genuinely stale otherwise (A3).
static void repaint_new_top_or_blank(void) {
    purr_win_t top = pw_top_window();
    if (top) {
        pw_layout_compute(top);
        pw_focus_rebuild(top);
        pw_win_full_repaint(top);
        return;
    }
    pw_launcher_draw();
}

// ── Allocation ────────────────────────────────────────────────────────────────

static int alloc_win(void) {
    for (int i = 0; i < PW_MAX_WINS; i++) if (!s_pw_wins[i].alive) return i;
    return -1;
}
static int alloc_wid(void) {
    for (int i = 0; i < PW_MAX_WIDS; i++) if (!s_pw_wids[i].alive) return i;
    return -1;
}

static char *pw_strdup(const char *s) {
    if (!s) s = "";
    size_t n = strlen(s) + 1;
    char *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    if (p) memcpy(p, s, n);
    return p;
}

// Every widget's parent is either the window's currently-open layout
// container, or -1 (top-level). Layout containers themselves are always
// top-level (-1), even if one is opened while another is already active —
// real apps always close a layout before opening the next one (0 observed
// nested layout_begin() calls across all 8 system apps), so true recursive
// nesting isn't implemented; a would-be-nested container just flattens to a
// sibling instead of silently mispositioning.
static purr_wid_t new_widget(purr_win_t win, pw_kind_t kind) {
    pw_window_t *pwin = pw_window(win);
    if (!pwin) return 0;
    int idx = alloc_wid();
    if (idx < 0) return 0;
    pw_widget_t *w = &s_pw_wids[idx];
    memset(w, 0, sizeof(*w));
    w->alive  = true;
    w->kind   = kind;
    w->win    = win;
    w->parent = (kind == PW_LAYOUT) ? (int16_t)-1 : pwin->active_layout;
    w->group_id = -1;
    w->index_in_group = -1;
    w->tab_prev = -1;
    w->tab_next = -1;
    return pw_wid_handle(idx);
}

static void redraw_if_top(purr_wid_t wid) {
    pw_widget_t *w = pw_widget(wid);
    if (w && w->win == pw_top_window()) pw_redraw_widget(wid);
}

// ── Window lifecycle ──────────────────────────────────────────────────────────

static purr_win_t pw_ui_win_create(const char *title) {
    int idx = alloc_win();
    if (idx < 0) return 0;
    pw_window_t *w = &s_pw_wins[idx];
    memset(w, 0, sizeof(*w));
    w->alive = true;
    w->visible = false;
    strncpy(w->title, title ? title : "", sizeof(w->title) - 1);
    w->tab_head = w->tab_tail = -1;
    w->focus_wid = -1;
    w->active_layout = -1;
    w->edit_target = -1;
    return pw_win_handle(idx);
}

static void free_widget(int idx);   // fwd — defined below list helpers

static void pw_ui_win_destroy(purr_win_t win) {
    pw_window_t *pwin = pw_window(win);
    if (!pwin) return;
    for (int i = 0; i < PW_MAX_WIDS; i++)
        if (s_pw_wids[i].alive && s_pw_wids[i].win == win) free_widget(i);

    bool was_top = (pw_top_window() == win);
    win_stack_remove(pw_win_idx(win));
    pwin->alive = false;
    if (was_top) repaint_new_top_or_blank();
}

static void pw_ui_win_show(purr_win_t win) {
    pw_window_t *pwin = pw_window(win);
    if (!pwin) return;
    pwin->visible = true;
    win_stack_push(pw_win_idx(win));
    pw_layout_compute(win);
    pw_focus_rebuild(win);
    pw_win_full_repaint(win);
}

static void pw_ui_win_hide(purr_win_t win) {
    pw_window_t *pwin = pw_window(win);
    if (!pwin) return;
    pwin->visible = false;
    bool was_top = (pw_top_window() == win);
    win_stack_remove(pw_win_idx(win));
    if (was_top) repaint_new_top_or_blank();
}

static void pw_ui_win_clear(purr_win_t win) {
    pw_window_t *pwin = pw_window(win);
    if (!pwin) return;
    for (int i = 0; i < PW_MAX_WIDS; i++)
        if (s_pw_wids[i].alive && s_pw_wids[i].win == win) free_widget(i);
    pwin->active_layout = -1;
    pwin->focus_wid = -1;
    pwin->edit_target = -1;
    pwin->tab_head = pwin->tab_tail = -1;
    if (win == pw_top_window()) pw_win_full_repaint(win);
}

static void pw_ui_win_on_close(purr_win_t win, purr_win_cb_t cb, void *user) {
    pw_window_t *pwin = pw_window(win);
    if (!pwin) return;
    pwin->on_close_cb = cb;
    pwin->on_close_user = user;
    // Never fired today — Pounce has no title-bar chrome to click (A3: full-
    // screen modal stack, no floating window decorations). Stored for
    // contract completeness, same "optional, no-op is valid" allowance the
    // contract itself documents for this hook.
}

// ── Labels ────────────────────────────────────────────────────────────────────

static purr_wid_t pw_ui_label_create(purr_win_t win, const char *text) {
    purr_wid_t wid = new_widget(win, PW_LABEL);
    pw_widget_t *w = pw_widget(wid);
    if (!w) return 0;
    w->label.text = pw_strdup(text);
    w->label.align = PURR_ALIGN_LEFT;
    return wid;
}
static void pw_ui_label_set(purr_wid_t wid, const char *text) {
    pw_widget_t *w = pw_widget(wid);
    if (!w || w->kind != PW_LABEL) return;
    if (w->label.text) heap_caps_free(w->label.text);
    w->label.text = pw_strdup(text);
    redraw_if_top(wid);
}
static void pw_ui_label_align(purr_wid_t wid, purr_align_t align) {
    pw_widget_t *w = pw_widget(wid);
    if (!w || w->kind != PW_LABEL) return;
    w->label.align = align;
    redraw_if_top(wid);
}

// ── Buttons ───────────────────────────────────────────────────────────────────

static purr_wid_t pw_ui_btn_create(purr_win_t win, const char *label,
                                    purr_win_cb_t cb, void *user) {
    purr_wid_t wid = new_widget(win, PW_BUTTON);
    pw_widget_t *w = pw_widget(wid);
    if (!w) return 0;
    w->button.text = pw_strdup(label);
    w->button.enabled = true;
    w->button.cb = cb;
    w->button.user = user;
    return wid;
}
static void pw_ui_btn_enable(purr_wid_t wid, bool enabled) {
    pw_widget_t *w = pw_widget(wid);
    if (!w || w->kind != PW_BUTTON) return;
    w->button.enabled = enabled;
    redraw_if_top(wid);
}

// ── Textarea ──────────────────────────────────────────────────────────────────

static purr_wid_t pw_ui_textarea_create(purr_win_t win, uint16_t w_pct, uint16_t h_pct) {
    purr_wid_t wid = new_widget(win, PW_TEXTAREA);
    pw_widget_t *w = pw_widget(wid);
    if (!w) return 0;
    w->textarea.w_pct = w_pct;
    w->textarea.h_pct = h_pct;
    return wid;
}
static void pw_ui_textarea_set(purr_wid_t wid, const char *text) {
    pw_widget_t *w = pw_widget(wid);
    if (!w || w->kind != PW_TEXTAREA) return;
    if (w->textarea.buf) heap_caps_free(w->textarea.buf);
    size_t n = strlen(text ? text : "") + 1;
    w->textarea.buf = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    if (w->textarea.buf) {
        memcpy(w->textarea.buf, text ? text : "", n);
        w->textarea.buf_cap = n;
        w->textarea.len = n - 1;
    } else {
        w->textarea.buf_cap = 0;
        w->textarea.len = 0;
    }
    redraw_if_top(wid);
}
static void pw_ui_textarea_append(purr_wid_t wid, const char *text) {
    pw_widget_t *w = pw_widget(wid);
    if (!w || w->kind != PW_TEXTAREA || !text) return;
    size_t addlen = strlen(text);
    size_t need = w->textarea.len + addlen + 1;
    if (need > w->textarea.buf_cap) {
        size_t new_cap = w->textarea.buf_cap ? w->textarea.buf_cap * 2 : 64;
        while (new_cap < need) new_cap *= 2;
        char *nb = heap_caps_realloc(w->textarea.buf, new_cap, MALLOC_CAP_SPIRAM);
        if (!nb) return;
        w->textarea.buf = nb;
        w->textarea.buf_cap = new_cap;
    }
    memcpy(w->textarea.buf + w->textarea.len, text, addlen + 1);
    w->textarea.len += addlen;
    redraw_if_top(wid);
}
static void pw_ui_textarea_clear(purr_wid_t wid) {
    pw_widget_t *w = pw_widget(wid);
    if (!w || w->kind != PW_TEXTAREA) return;
    w->textarea.len = 0;
    if (w->textarea.buf) w->textarea.buf[0] = '\0';
    redraw_if_top(wid);
}
static const char *pw_ui_textarea_get(purr_wid_t wid) {
    pw_widget_t *w = pw_widget(wid);
    if (!w || w->kind != PW_TEXTAREA) return "";
    return w->textarea.buf ? w->textarea.buf : "";
}
static void pw_ui_textarea_focus(purr_wid_t wid) {
    pw_widget_t *w = pw_widget(wid);
    if (!w || w->kind != PW_TEXTAREA) return;
    pw_window_t *pwin = pw_window(w->win);
    if (!pwin) return;
    int idx = pw_wid_idx(wid);
    if (pwin->focus_wid != idx) {
        if (pwin->focus_wid >= 0) s_pw_wids[pwin->focus_wid].focused = false;
        pwin->focus_wid = (int16_t)idx;
        w->focused = true;
    }
    pwin->edit_target = (int16_t)idx;
    w->textarea.editing = true;
    redraw_if_top(wid);
}
static void pw_ui_textarea_cb(purr_wid_t wid, purr_win_cb_t cb, void *user) {
    pw_widget_t *w = pw_widget(wid);
    if (!w || w->kind != PW_TEXTAREA) return;
    w->textarea.cb = cb;
    w->textarea.user = user;
}

// ── List ──────────────────────────────────────────────────────────────────────

static void free_list_items(pw_widget_t *w) {
    if (w->list.items) {
        for (int i = 0; i < w->list.count; i++)
            if (w->list.items[i]) heap_caps_free(w->list.items[i]);
        heap_caps_free(w->list.items);
        w->list.items = NULL;
    }
    w->list.count = 0;
}

static purr_wid_t pw_ui_list_create(purr_win_t win, uint16_t w_pct, uint16_t h_pct) {
    purr_wid_t wid = new_widget(win, PW_LIST);
    pw_widget_t *w = pw_widget(wid);
    if (!w) return 0;
    w->list.w_pct = w_pct;
    w->list.h_pct = h_pct;
    w->list.selected = -1;
    return wid;
}
static void pw_ui_list_set_items(purr_wid_t wid, const char **items, int count) {
    pw_widget_t *w = pw_widget(wid);
    if (!w || w->kind != PW_LIST) return;
    free_list_items(w);
    if (count > 0 && items) {
        w->list.items = heap_caps_malloc(sizeof(char *) * (size_t)count, MALLOC_CAP_SPIRAM);
        if (w->list.items) {
            for (int i = 0; i < count; i++) w->list.items[i] = pw_strdup(items[i]);
            w->list.count = count;
        }
    }
    w->list.selected = (w->list.count > 0) ? 0 : -1;
    w->list.top_visible = 0;
    redraw_if_top(wid);
}
static void pw_ui_list_clear(purr_wid_t wid) {
    pw_widget_t *w = pw_widget(wid);
    if (!w || w->kind != PW_LIST) return;
    free_list_items(w);
    w->list.selected = -1;
    w->list.top_visible = 0;
    redraw_if_top(wid);
}
static int pw_ui_list_get_selected(purr_wid_t wid) {
    pw_widget_t *w = pw_widget(wid);
    if (!w || w->kind != PW_LIST) return -1;
    return w->list.selected;
}
static void pw_ui_list_set_selected(purr_wid_t wid, int index) {
    pw_widget_t *w = pw_widget(wid);
    if (!w || w->kind != PW_LIST) return;
    if (index < -1) index = -1;
    if (index >= w->list.count) index = w->list.count - 1;
    w->list.selected = index;
    redraw_if_top(wid);
}
static void pw_ui_list_cb(purr_wid_t wid, purr_win_cb_t cb, void *user) {
    pw_widget_t *w = pw_widget(wid);
    if (!w || w->kind != PW_LIST) return;
    w->list.cb = cb;
    w->list.user = user;
}

// ── Layout ────────────────────────────────────────────────────────────────────

static purr_wid_t pw_ui_layout_begin(purr_win_t win, purr_layout_t dir, uint8_t pad, bool grow) {
    purr_wid_t wid = new_widget(win, PW_LAYOUT);
    pw_widget_t *w = pw_widget(wid);
    if (!w) return 0;
    w->layout.dir = dir;
    w->layout.pad = pad;
    w->layout.grow = grow;
    pw_window_t *pwin = pw_window(win);
    if (pwin) pwin->active_layout = (int16_t)pw_wid_idx(wid);
    return wid;
}
static void pw_ui_layout_end(purr_wid_t container) {
    pw_widget_t *w = pw_widget(container);
    if (!w || w->kind != PW_LAYOUT) return;
    pw_window_t *pwin = pw_window(w->win);
    if (pwin && pwin->active_layout == pw_wid_idx(container)) pwin->active_layout = -1;
}

// ── Keyboard — no on-screen keyboard (A4/A8): physical BBQ20 is primary on
// the target device, text entry routes straight into the focused textarea's
// buffer via pounce_focus.c. Empty stubs, exactly mirroring MiniWin's own
// existing, already-shipping precedent for physical-keyboard devices. ──────

static void pw_ui_kb_show(purr_win_t win, purr_wid_t target) { (void)win; (void)target; }
static void pw_ui_kb_hide(purr_win_t win) { (void)win; }

// ── Widget teardown (used by win_destroy/win_clear) ──────────────────────────

static void free_widget(int idx) {
    pw_widget_t *w = &s_pw_wids[idx];
    switch (w->kind) {
    case PW_LABEL:    if (w->label.text) heap_caps_free(w->label.text); break;
    case PW_BUTTON:   if (w->button.text) heap_caps_free(w->button.text); break;
    case PW_TEXTAREA: if (w->textarea.buf) heap_caps_free(w->textarea.buf); break;
    case PW_LIST:     free_list_items(w); break;
    case PW_LAYOUT:   break;
    }
    w->alive = false;
}

// ── Registration ──────────────────────────────────────────────────────────────

static const catcall_ui_t s_pounce_ui = {
    .name              = "pounce",
    .catcall_version   = CATCALL_UI_VERSION,

    .win_create        = pw_ui_win_create,
    .win_destroy       = pw_ui_win_destroy,
    .win_show          = pw_ui_win_show,
    .win_hide          = pw_ui_win_hide,
    .win_clear         = pw_ui_win_clear,
    .win_on_close      = pw_ui_win_on_close,

    .label_create      = pw_ui_label_create,
    .label_set         = pw_ui_label_set,
    .label_align       = pw_ui_label_align,

    .btn_create        = pw_ui_btn_create,
    .btn_enable        = pw_ui_btn_enable,

    .textarea_create   = pw_ui_textarea_create,
    .textarea_append   = pw_ui_textarea_append,
    .textarea_set      = pw_ui_textarea_set,
    .textarea_clear    = pw_ui_textarea_clear,
    .textarea_get      = pw_ui_textarea_get,
    .textarea_focus    = pw_ui_textarea_focus,
    .textarea_cb       = pw_ui_textarea_cb,

    .list_create       = pw_ui_list_create,
    .list_set_items    = pw_ui_list_set_items,
    .list_clear        = pw_ui_list_clear,
    .list_get_selected = pw_ui_list_get_selected,
    .list_set_selected = pw_ui_list_set_selected,
    .list_cb           = pw_ui_list_cb,

    .layout_begin      = pw_ui_layout_begin,
    .layout_end        = pw_ui_layout_end,

    .kb_show           = pw_ui_kb_show,
    .kb_hide           = pw_ui_kb_hide,
};

const catcall_ui_t *pounce_win_register(void) {
    return &s_pounce_ui;
}

#endif  // CONFIG_PURR_UI_BACKEND_POUNCE
