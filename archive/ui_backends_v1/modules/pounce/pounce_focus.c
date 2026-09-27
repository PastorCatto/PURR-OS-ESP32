// pounce_focus.c — keyboard + trackball widget-to-widget focus navigation
// for the Pounce UI backend (pounce_plan.md A4). The genuinely new part:
// nothing else in this codebase has real focus-nav between widgets.
#include "sdkconfig.h"

#ifdef CONFIG_PURR_UI_BACKEND_POUNCE

#include "pounce.h"
#include "../../kernel/core/purr_kernel.h"
#include "esp_heap_caps.h"
#include <string.h>

// ── Focus groups (A4) ────────────────────────────────────────────────────────
// Each contiguous run of focusable widgets sharing an enclosing
// layout_begin() is one group; a standalone (ungrouped) focusable widget is
// its own singleton group. Rebuilt whenever a window is shown — only the
// currently-focused window's groups are ever relevant (full-screen modal
// stack, A3), so this is one shared table, not per-window storage.
#define PW_GROUP_MAX_MEMBERS 16
#define PW_MAX_GROUPS 32

typedef struct { int16_t members[PW_GROUP_MAX_MEMBERS]; int16_t count; } pw_group_t;

static pw_group_t s_groups[PW_MAX_GROUPS];
static int        s_group_count = 0;

void pw_focus_rebuild(purr_win_t win) {
    pw_window_t *pwin = pw_window(win);
    if (!pwin) return;

    s_group_count = 0;
    int16_t prev_tab = -1;
    pwin->tab_head = -1;
    pwin->tab_tail = -1;

    int16_t last_parent = -999;   // sentinel distinct from -1 and any real widget index
    int     cur_group   = -1;

    for (int i = 0; i < PW_MAX_WIDS; i++) {
        pw_widget_t *w = &s_pw_wids[i];
        if (!w->alive || w->win != win) continue;

        w->focusable = (w->kind == PW_BUTTON || w->kind == PW_LIST || w->kind == PW_TEXTAREA);
        w->focused = false;
        w->group_id = -1;
        w->index_in_group = -1;
        if (!w->focusable) continue;

        // Flat creation-order tab ring (Tab key — separate from grouping).
        w->tab_prev = prev_tab;
        w->tab_next = -1;
        if (prev_tab >= 0) s_pw_wids[prev_tab].tab_next = (int16_t)i;
        else pwin->tab_head = (int16_t)i;
        pwin->tab_tail = (int16_t)i;
        prev_tab = (int16_t)i;

        // Group assignment.
        if (w->parent == -1) {
            cur_group = s_group_count++;
            last_parent = -999;   // any later same-container run still opens fresh
        } else if (w->parent != last_parent) {
            cur_group = s_group_count++;
            last_parent = w->parent;
        }
        if (cur_group >= 0 && cur_group < PW_MAX_GROUPS) {
            pw_group_t *g = &s_groups[cur_group];
            if (g->count == 0) g->count = 0;   // (no-op — clarity: freshly opened group starts empty)
            if (g->count < PW_GROUP_MAX_MEMBERS) {
                w->group_id = (int16_t)cur_group;
                w->index_in_group = g->count;
                g->members[g->count++] = (int16_t)i;
            }
        }
    }

    pwin->focus_wid = pwin->tab_head;
    pwin->edit_target = -1;
    if (pwin->focus_wid >= 0) s_pw_wids[pwin->focus_wid].focused = true;
}

// ── Navigation ───────────────────────────────────────────────────────────────

static void pw_focus_set(purr_win_t win, int new_idx) {
    pw_window_t *pwin = pw_window(win);
    if (!pwin) return;
    if (pwin->focus_wid >= 0 && pwin->focus_wid != new_idx) {
        s_pw_wids[pwin->focus_wid].focused = false;
        pw_draw_focus_border(pw_wid_handle(pwin->focus_wid), false);
    }
    pwin->focus_wid = (int16_t)new_idx;
    if (new_idx >= 0) {
        s_pw_wids[new_idx].focused = true;
        pw_draw_focus_border(pw_wid_handle(new_idx), true);
    }
}

// Left/Right move within the current group, falling through to the
// adjacent group's edge member at a boundary. Up/Down move to the prev/next
// group at the same index_in_group, clamped. Both wrap around rather than
// dead-stopping at either end (A4).
static void move_focus(purr_win_t win, int ddx, int ddy) {
    pw_window_t *pwin = pw_window(win);
    if (!pwin || pwin->focus_wid < 0 || s_group_count == 0) return;
    pw_widget_t *cur = &s_pw_wids[pwin->focus_wid];
    int g = cur->group_id;
    int ig = cur->index_in_group;
    if (g < 0 || g >= s_group_count) return;
    int new_wid = -1;

    if (ddx != 0) {
        int target = ig + ddx;
        if (target >= 0 && target < s_groups[g].count) {
            new_wid = s_groups[g].members[target];
        } else if (target < 0) {
            int pg = (g - 1 + s_group_count) % s_group_count;
            new_wid = s_groups[pg].members[s_groups[pg].count - 1];
        } else {
            int ng = (g + 1) % s_group_count;
            new_wid = s_groups[ng].members[0];
        }
    } else if (ddy != 0) {
        int ng = (g + ddy + s_group_count) % s_group_count;
        int clamped = ig;
        if (clamped >= s_groups[ng].count) clamped = s_groups[ng].count - 1;
        if (clamped < 0) clamped = 0;
        new_wid = s_groups[ng].members[clamped];
    }

    if (new_wid >= 0) pw_focus_set(win, new_wid);
}

// List: genuine two-stage model (A4). While a list has widget-focus, Up/Down
// browses its own internal selection directly rather than leaving the list
// (a list is normally its own singleton group, so Left/Right already falls
// through via move_focus() above without any special-casing needed here).
static void list_move_selection(int idx, int ddy) {
    pw_widget_t *w = &s_pw_wids[idx];
    if (!w->list.items || w->list.count == 0) return;
    int sel = w->list.selected + ddy;
    if (sel < 0) sel = 0;
    if (sel >= w->list.count) sel = w->list.count - 1;
    if (sel == w->list.selected) return;
    w->list.selected = sel;

    int max_lines = (w->h - 4) / PW_CHAR_H;
    if (max_lines < 1) max_lines = 1;
    if (sel < w->list.top_visible) w->list.top_visible = sel;
    if (sel >= w->list.top_visible + max_lines) w->list.top_visible = sel - max_lines + 1;

    pw_redraw_widget(pw_wid_handle(idx));
    if (w->list.cb) w->list.cb(pw_wid_handle(idx), PURR_EVENT_SELECTED, w->list.user);
}

static void nav_step(purr_win_t win, int ddx, int ddy) {
    pw_window_t *pwin = pw_window(win);
    if (!pwin || pwin->focus_wid < 0 || pwin->edit_target >= 0) return;
    pw_widget_t *cur = &s_pw_wids[pwin->focus_wid];
    if (ddy != 0 && cur->kind == PW_LIST) {
        list_move_selection(pwin->focus_wid, ddy);
        return;
    }
    move_focus(win, ddx, ddy);
}

// ── Activate (A4) — never fires through a widget's own click callback for a
// focus change, only for an explicit Enter/click (see pounce_plan.md A4's
// "critical correctness rule") ──────────────────────────────────────────────

static void activate_focused(purr_win_t win) {
    pw_window_t *pwin = pw_window(win);
    if (!pwin || pwin->focus_wid < 0) return;
    int idx = pwin->focus_wid;
    pw_widget_t *w = &s_pw_wids[idx];
    switch (w->kind) {
    case PW_BUTTON:
        if (w->button.enabled && w->button.cb)
            w->button.cb(pw_wid_handle(idx), PURR_EVENT_CLICKED, w->button.user);
        break;
    case PW_LIST:
        if (w->list.cb && w->list.selected >= 0)
            w->list.cb(pw_wid_handle(idx), PURR_EVENT_ACTIVATED, w->list.user);
        break;
    case PW_TEXTAREA:
        pwin->edit_target = (int16_t)idx;
        w->textarea.editing = true;
        pw_redraw_widget(pw_wid_handle(idx));
        break;
    default:
        break;
    }
}

// ── Text-edit mode ───────────────────────────────────────────────────────────

static void ta_insert(int idx, char ch) {
    pw_widget_t *w = &s_pw_wids[idx];
    if (w->textarea.len + 2 > w->textarea.buf_cap) {
        size_t new_cap = w->textarea.buf_cap ? w->textarea.buf_cap * 2 : 64;
        char *nb = heap_caps_realloc(w->textarea.buf, new_cap, MALLOC_CAP_SPIRAM);
        if (!nb) return;
        w->textarea.buf = nb;
        w->textarea.buf_cap = new_cap;
    }
    w->textarea.buf[w->textarea.len++] = ch;
    w->textarea.buf[w->textarea.len] = '\0';
    pw_redraw_widget(pw_wid_handle(idx));
    if (w->textarea.cb) w->textarea.cb(pw_wid_handle(idx), PURR_EVENT_CHANGED, w->textarea.user);
}

static void ta_backspace(int idx) {
    pw_widget_t *w = &s_pw_wids[idx];
    if (w->textarea.len == 0) return;
    w->textarea.buf[--w->textarea.len] = '\0';
    pw_redraw_widget(pw_wid_handle(idx));
    if (w->textarea.cb) w->textarea.cb(pw_wid_handle(idx), PURR_EVENT_CHANGED, w->textarea.user);
}

static void ta_exit_edit(purr_win_t win) {
    pw_window_t *pwin = pw_window(win);
    if (!pwin || pwin->edit_target < 0) return;
    int idx = pwin->edit_target;
    s_pw_wids[idx].textarea.editing = false;
    pwin->edit_target = -1;
    pw_redraw_widget(pw_wid_handle(idx));
}

// ── Keyboard dispatch ─────────────────────────────────────────────────────────
// BBQ20 emits raw ASCII only, KEY_DOWN only, no modifier bits (confirmed in
// bbq20.c) — matches this codebase's existing cupcake_hal.c convention for
// backspace (0x08/0x7F) and enter (0x0D/0x0A), reused here for consistency.

static void handle_key_down(purr_win_t win, uint16_t keycode) {
    pw_window_t *pwin = pw_window(win);
    if (!pwin) return;

    if (pwin->edit_target >= 0) {
        int idx = pwin->edit_target;
        if (keycode == 0x1B) { ta_exit_edit(win); return; }
        if (keycode == 0x0D || keycode == 0x0A) { ta_insert(idx, '\n'); return; }
        if (keycode == 0x08 || keycode == 0x7F) { ta_backspace(idx); return; }
        if (keycode >= 0x20 && keycode < 0x7F) { ta_insert(idx, (char)keycode); }
        return;
    }

    // Nav mode only from here — safe for the A6 global hotkey to consume a
    // bare letter (see pw_status_hotkey_check's doc comment).
    if (pw_status_hotkey_check(keycode)) return;

    if (keycode == 0x09) {
        pw_widget_t *cur = (pwin->focus_wid >= 0) ? &s_pw_wids[pwin->focus_wid] : NULL;
        int next = cur ? cur->tab_next : -1;
        if (next < 0) next = pwin->tab_head;
        if (next >= 0) pw_focus_set(win, next);
    } else if (keycode == 0x0D || keycode == 0x0A) {
        activate_focused(win);
    }
}

// ── Input tick (called once per pounce_module.c task iteration) ─────────────

#define PW_NAV_THRESHOLD 3
#define PW_TRACKBALL_CLICK_KEYCODE 0x0028

static int16_t s_acc_dx = 0, s_acc_dy = 0;
static bool    s_trackball_click_down = false;

static void handle_pointer(int16_t dx, int16_t dy) {
    purr_win_t win = pw_top_window();
    s_acc_dx = (int16_t)(s_acc_dx + dx);
    s_acc_dy = (int16_t)(s_acc_dy + dy);
    if (!win) {
        // No window open — trackball drives the launcher grid instead.
        while (s_acc_dx >= PW_NAV_THRESHOLD)  { pw_launcher_move( 1, 0); s_acc_dx = (int16_t)(s_acc_dx - PW_NAV_THRESHOLD); }
        while (s_acc_dx <= -PW_NAV_THRESHOLD) { pw_launcher_move(-1, 0); s_acc_dx = (int16_t)(s_acc_dx + PW_NAV_THRESHOLD); }
        while (s_acc_dy >= PW_NAV_THRESHOLD)  { pw_launcher_move(0,  1); s_acc_dy = (int16_t)(s_acc_dy - PW_NAV_THRESHOLD); }
        while (s_acc_dy <= -PW_NAV_THRESHOLD) { pw_launcher_move(0, -1); s_acc_dy = (int16_t)(s_acc_dy + PW_NAV_THRESHOLD); }
        return;
    }
    while (s_acc_dx >= PW_NAV_THRESHOLD)  { nav_step(win,  1, 0); s_acc_dx = (int16_t)(s_acc_dx - PW_NAV_THRESHOLD); }
    while (s_acc_dx <= -PW_NAV_THRESHOLD) { nav_step(win, -1, 0); s_acc_dx = (int16_t)(s_acc_dx + PW_NAV_THRESHOLD); }
    while (s_acc_dy >= PW_NAV_THRESHOLD)  { nav_step(win, 0,  1); s_acc_dy = (int16_t)(s_acc_dy - PW_NAV_THRESHOLD); }
    while (s_acc_dy <= -PW_NAV_THRESHOLD) { nav_step(win, 0, -1); s_acc_dy = (int16_t)(s_acc_dy + PW_NAV_THRESHOLD); }
}

void pw_focus_input_tick(void) {
    int n = purr_kernel_input_count();
    for (int i = 0; i < n; i++) {
        const catcall_input_t *dev = purr_kernel_input_at(i);
        if (!dev || !dev->poll_event) continue;
        input_event_t ev;
        while (dev->poll_event(&ev)) {
            switch (ev.type) {
            case INPUT_EVENT_POINTER:
                handle_pointer(ev.delta_x, ev.delta_y);
                break;
            case INPUT_EVENT_KEY_DOWN:
                if (ev.keycode == PW_TRACKBALL_CLICK_KEYCODE) {
                    if (!s_trackball_click_down) {
                        s_trackball_click_down = true;
                        purr_win_t win = pw_top_window();
                        if (!win) {
                            pw_launcher_activate();
                        } else {
                            pw_window_t *pwin = pw_window(win);
                            if (pwin && pwin->edit_target < 0) activate_focused(win);
                        }
                    }
                } else {
                    purr_win_t win = pw_top_window();
                    if (win) {
                        handle_key_down(win, ev.keycode);
                    } else if (!pw_status_hotkey_check(ev.keycode) &&
                               (ev.keycode == 0x0D || ev.keycode == 0x0A)) {
                        pw_launcher_activate();
                    }
                }
                break;
            case INPUT_EVENT_KEY_UP:
                if (ev.keycode == PW_TRACKBALL_CLICK_KEYCODE) s_trackball_click_down = false;
                break;
            default:
                break;
            }
        }
    }
}

#endif  // CONFIG_PURR_UI_BACKEND_POUNCE
