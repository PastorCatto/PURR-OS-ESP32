#pragma once
// pounce.h — internal data model + cross-file API for the Pounce UI backend.
// See pounce_plan.md at the repo root for the full design. Nothing outside
// this module includes this header — apps only ever see catcall_ui.h/
// purr_win.h (via pounce_win_register()'s catcall_ui_t).

#include "catcall_ui.h"
#include "font6x8.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ── Palette (A5 — hacker-terminal aesthetic) ────────────────────────────────
// RGB888 — converted to RGB565 at the draw-primitive layer (pounce_render.c),
// same convention as WCE_* in miniwin_wince_desktop.c.
#define PW_COL_BG        0x000000u   // black
#define PW_COL_FG        0x00FF41u   // phosphor/"matrix" green — text & borders
#define PW_COL_DIM       0x008022u   // dimmed green — disabled/secondary text
#define PW_COL_ACCENT    0xFFB000u   // amber — warnings, low signal, alerts
#define PW_COL_FOCUS_BG  PW_COL_FG   // focused widget: inverted video
#define PW_COL_FOCUS_FG  PW_COL_BG

// Status strip (A6) — reserved at the top of every window's layout.
#define PW_STATUS_H  (PW_CHAR_H + 2)

// ── Pools ────────────────────────────────────────────────────────────────────

#define PW_MAX_WINS 16
#define PW_MAX_WIDS 256

typedef enum { PW_LABEL, PW_BUTTON, PW_TEXTAREA, PW_LIST, PW_LAYOUT } pw_kind_t;

typedef struct {
    bool       alive;
    pw_kind_t  kind;
    purr_win_t win;                      // owning window handle
    int16_t    parent;                   // enclosing PW_LAYOUT widget's index, -1 = top-level
    int16_t    x, y, w, h;               // computed by pw_layout_compute()
    bool       focusable, focused;
    int16_t    tab_prev, tab_next;       // creation-order tab ring (-1 = none)
    int16_t    group_id;                 // enclosing layout_begin() group, -1 = none
    int16_t    index_in_group;
    union {
        struct { char *text; purr_align_t align; } label;
        struct { char *text; bool enabled; purr_win_cb_t cb; void *user; } button;
        struct { char *buf; size_t buf_cap, len; bool editing; uint16_t w_pct, h_pct;
                 purr_win_cb_t cb; void *user; } textarea;
        struct { char **items; int count, selected, top_visible; uint16_t w_pct, h_pct;
                 purr_win_cb_t cb; void *user; } list;
        struct { purr_layout_t dir; uint8_t pad; bool grow; } layout;
    };
} pw_widget_t;

typedef struct {
    bool          alive, visible;
    char          title[32];
    purr_win_cb_t on_close_cb;
    void         *on_close_user;
    int16_t       tab_head, tab_tail;    // creation-order focus ring, -1 = empty
    int16_t       focus_wid;             // currently focused widget index, -1 = none
    int16_t       active_layout;         // currently-open row/col widget index, -1 = none
    int16_t       edit_target;           // widget index in text-edit mode, -1 = none
} pw_window_t;

extern pw_window_t s_pw_wins[PW_MAX_WINS];
extern pw_widget_t s_pw_wids[PW_MAX_WIDS];

// Handle <-> index. Handles are 1-based (0 = invalid), matching the
// KittenUI/MiniWin convention this contract already assumes.
static inline int pw_win_idx(purr_win_t win)  { return (win >= 1 && win <= PW_MAX_WINS) ? (int)win - 1 : -1; }
static inline int pw_wid_idx(purr_wid_t wid)  { return (wid >= 1 && wid <= PW_MAX_WIDS) ? (int)wid - 1 : -1; }
static inline purr_win_t pw_win_handle(int idx) { return (purr_win_t)(idx + 1); }
static inline purr_wid_t pw_wid_handle(int idx) { return (purr_wid_t)(idx + 1); }

// NULL if the handle is out of range or the slot isn't alive.
static inline pw_window_t *pw_window(purr_win_t win) {
    int i = pw_win_idx(win);
    if (i < 0 || !s_pw_wins[i].alive) return NULL;
    return &s_pw_wins[i];
}
static inline pw_widget_t *pw_widget(purr_wid_t wid) {
    int i = pw_wid_idx(wid);
    if (i < 0 || !s_pw_wids[i].alive) return NULL;
    return &s_pw_wids[i];
}

// ── pounce_win.c ─────────────────────────────────────────────────────────────

const catcall_ui_t *pounce_win_register(void);

// The topmost (focused/visible) window in the modal stack, 0 if none.
purr_win_t pw_top_window(void);

// ── pounce_render.c ──────────────────────────────────────────────────────────

void pw_fill_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint32_t rgb888);
void pw_draw_string(int16_t x, int16_t y, const char *s, uint32_t fg, uint32_t bg);
void pw_draw_string_clipped(int16_t x, int16_t y, const char *s, int max_chars,
                             uint32_t fg888, uint32_t bg888);

// Calls visit(idx, user) for every alive widget owned by `win` with the
// given `parent` (-1 = top-level), in creation order. Widget-pool indices
// aren't guaranteed contiguous per-parent, so tree walks always go through
// this rather than assuming a child-index range.
typedef void (*pw_visit_fn)(int wid_idx, void *user);
void pw_for_each_child(purr_win_t win, int16_t parent, pw_visit_fn visit, void *user);

// Computes (x,y,w,h) for every widget in `win` from its layout tree. Called
// once per win_show()/window-stack-top-change (A3) — not per widget-add.
void pw_layout_compute(purr_win_t win);

// Full redraw of `win`: background + every widget. Called on win_show() and
// whenever the modal stack's top changes (A3 — no offscreen buffer, so a
// revealed window's pixels are genuinely stale otherwise).
void pw_win_full_repaint(purr_win_t win);

// Redraws exactly one widget's rect — the fast path every catcall_ui_t
// mutator uses (A3's "synchronous redraw at the mutation call site").
void pw_redraw_widget(purr_wid_t wid);

// 1px border rect around a widget's cached rect, on/off — the only visual
// side effect of a focus change (A4's "never fire a callback for focus").
void pw_draw_focus_border(purr_wid_t wid, bool on);

// ── pounce_focus.c ───────────────────────────────────────────────────────────

// Rebuilds the focus-group table for `win` and focuses its first focusable
// widget. Called from win_show() once layout is computed.
void pw_focus_rebuild(purr_win_t win);

// Drains every registered catcall_input_t (trackball POINTER deltas +
// keyboard KEY_DOWN/UP), routes to focus-nav, text-edit, or the A6 hotkey.
// Called once per pounce_module.c task tick.
void pw_focus_input_tick(void);

// ── pounce_launcher.c ────────────────────────────────────────────────────────
// Home screen shown whenever the modal window stack is empty (pw_top_window()
// == 0) — an app grid, built on BlackPurr's proven pattern. Not a purr_win_t
// window; shell-level chrome exactly like the status strip.

void pw_launcher_draw(void);
void pw_launcher_move(int ddx, int ddy);
void pw_launcher_activate(void);

// ── pounce_status.c ──────────────────────────────────────────────────────────

void pw_status_init(void);

// Returns true if this keycode was consumed as the global compose hotkey
// (A6) — only ever called from nav mode (pounce_focus.c never calls this
// while a textarea has edit_target, so a bare letter is always safe here).
bool pw_status_hotkey_check(uint16_t keycode);

// Redraws the status strip if its data has changed enough to be worth a
// repaint (A6 — timer-driven, not per-tick).
void pw_status_tick(void);

#ifdef __cplusplus
}
#endif
