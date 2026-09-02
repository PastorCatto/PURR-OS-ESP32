// miniwin_win.c — catcall_ui_t backend for MiniWin
// Registers via purr_kernel_register_ui() during miniwin module init.

#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include "../../kernel/catcalls/catcall_ui.h"
#include "../../kernel/core/purr_kernel.h"
#include "MiniWin/miniwin.h"
#include "MiniWin/hal/hal_lcd.h"
#include "MiniWin/ui/ui_label.h"
#include "MiniWin/ui/ui_button.h"
#include "MiniWin/ui/ui_text_box.h"
#include "MiniWin/ui/ui_list_box.h"
#include "sdkconfig.h"
#include "esp_attr.h"   // EXT_RAM_BSS_ATTR — see s_label_data's own comment below
#ifdef CONFIG_PURR_MINIWIN_DESKTOP_WINCE
#include "miniwin_wince_desktop.h"
#endif

// mw_ui_text_box_add_new() hard-requires a non-NULL tt_font (see its null
// check in ui_text_box.c) — this widget apparently had no working consumer
// anywhere in the codebase before today, since nothing else sets it either.
extern const struct mf_rlefont_s mf_rlefont_DejaVuSans12;

// ── Handle pool ───────────────────────────────────────────────────────────────

#define MAX_WINS  16
#define MAX_WIDS  128

// Fixed column width used for a label placed inside an active row layout
// (labels have no natural width — MiniWin's label control always needs one).
#define MW_ROW_LABEL_WIDTH  100
#define MW_TA_BUF_SIZE      512

typedef struct {
    mw_handle_t    mw_win;
    bool           used;
    // Set once MiniWin's own window-removal path has run for this window —
    // either because we called mw_remove_window() ourselves (top-down, via
    // mw_win_destroy()) or because the user tapped the title-bar close icon
    // (bottom-up, MiniWin removes the window on its own before we hear about
    // it). Lets mw_win_destroy() and the MW_WINDOW_REMOVED_MESSAGE handler
    // below tell which of them is doing the tearing-down vs. just following
    // up, so neither re-removes already-gone controls (MW_ASSERT crash) nor
    // fires the close callback twice.
    bool           torn_down;
    purr_win_cb_t  close_cb;
    void          *close_user;
    // Canvas (raw draw + touch) — see purr_win_paint_cb_t's doc in
    // catcall_ui.h. NULL unless the app opted in via
    // purr_win_canvas_on_paint()/_on_touch().
    purr_win_paint_cb_t canvas_paint_cb;
    void                *canvas_paint_user;
    purr_win_touch_cb_t  canvas_touch_cb;
    void                *canvas_touch_user;
} win_slot_t;

typedef struct {
    mw_handle_t    mw_win;
    mw_handle_t    mw_ctrl;
    purr_win_cb_t  cb;
    void          *user;
    bool           used;
} wid_slot_t;

// Per-window layout cursor state, shared by every widget type so row/col
// placement and plain vertical stacking can't disagree about where the next
// widget goes (previously labels and buttons tracked independent Y-cursors).
typedef struct {
    bool     active;      // true while inside purr_win_row()/purr_win_col()
    int16_t  x, y;
    int16_t  row_h;
    uint8_t  pad;
} layout_state_t;

// s_wins deliberately NOT EXT_RAM_BSS_ATTR — get_win(h) walks it on every
// single purr_win_*() call from app code (not just redraw/interaction
// events like the widget-data pools below), the one MiniWin structure in
// this file frequent enough that PSRAM's per-access latency seemed worth
// not risking. s_wids is the same shape as those widget-data pools (only
// touched per-widget-operation, not per-purr_win_*()-call) — see s_list_
// data's own comment just below for the shared reasoning.
static win_slot_t     s_wins[MAX_WINS];
static EXT_RAM_BSS_ATTR wid_slot_t s_wids[MAX_WIDS];
static int16_t        s_cursor_y[MAX_WINS];
static layout_state_t s_layout[MAX_WINS];
static purr_wid_t     s_ta_focused[MAX_WINS];   // wid of focused textarea per window, 0 = none

// Windows whose close-icon was tapped this pump iteration, awaiting their
// close callback + mw_win_destroy() from a call site that ISN'T nested
// inside MiniWin's own mw_remove_window()/`reentered` re-entrancy guard —
// see win_message_func()'s MW_WINDOW_REMOVED_MESSAGE handler and
// miniwin_win_process_pending_closes()'s own comment for why. 4 slots is
// generous — closes don't happen faster than the ~5ms pump loop can drain
// this list.
#define MAX_PENDING_CLOSES 4
static purr_win_t s_pending_closes[MAX_PENDING_CLOSES];
static int        s_pending_close_count = 0;

// ── List storage ─────────────────────────────────────────────────────────
// Declared up here (rather than down by the other list functions) because
// win_message_func()'s MW_LIST_BOX_ITEM_PRESSED_MESSAGE handler needs it.
typedef struct {
    mw_ui_list_box_data_t box;
    // Authoritative last-pressed index, captured from
    // MW_LIST_BOX_ITEM_PRESSED_MESSAGE's message_data in win_message_func().
    // MiniWin's own box.line_is_selected is a transient "press animation"
    // flag that the list box widget itself clears (ui_list_box.c's
    // MW_TIMER_MESSAGE case) before this message is even posted — reading
    // it from mw_list_get_selected() always returned -1. -1 = none pressed
    // yet, or cleared by a content change.
    int last_pressed;
} list_slot_t;

// EXT_RAM_BSS_ATTR — PSRAM instead of internal DRAM. Same reasoning as
// MiniWin/miniwin.c's own mw_all_windows/mw_all_controls/mw_all_timers
// (see that file's comment): real, measured internal-DRAM pressure on
// tdeck_plus, only read on redraw/interaction (not a tight per-tick
// loop), safe and free on any PSRAM-less device via this attribute's own
// no-op fallback.
static EXT_RAM_BSS_ATTR list_slot_t s_list_data[MAX_WIDS];

// ── Textarea storage ─────────────────────────────────────────────────────
// Also declared up here — win_message_func()'s MW_KEY_PRESSED_MESSAGE
// handler needs to reach a focused textarea's buffer directly.
// s_ta_data: EXT_RAM_BSS_ATTR, same reasoning as s_list_data above.
// s_ta_buf is just an array of pointers (512 bytes total) to individually
// malloc'd buffers elsewhere — not worth moving on its own.
static EXT_RAM_BSS_ATTR mw_ui_text_box_data_t s_ta_data[MAX_WIDS];
static char *s_ta_buf[MAX_WIDS];

// Forward-declared — defined with the rest of the textarea widget functions
// further down, but win_message_func()'s MW_KEY_PRESSED_MESSAGE handler
// needs to call them.
static void mw_ta_recalc_height(int idx);
static void mw_ta_append(purr_wid_t wid, const char *text);

static purr_win_t alloc_win(mw_handle_t h) {
    for (int i = 0; i < MAX_WINS; i++)
        if (!s_wins[i].used) {
            s_wins[i].mw_win = h;
            s_wins[i].used = true;
            s_wins[i].torn_down = false;
            s_wins[i].close_cb = NULL;
            s_wins[i].close_user = NULL;
            s_wins[i].canvas_paint_cb = NULL;
            s_wins[i].canvas_paint_user = NULL;
            s_wins[i].canvas_touch_cb = NULL;
            s_wins[i].canvas_touch_user = NULL;
            s_cursor_y[i] = 0;
            s_layout[i].active = false;
            s_ta_focused[i] = 0;
            return (purr_win_t)(i+1);
        }
    return 0;
}
static mw_handle_t get_win(purr_win_t h) {
    if (h < 1 || h > MAX_WINS || !s_wins[h-1].used) return MW_INVALID_HANDLE;
    return s_wins[h-1].mw_win;
}
static void free_win(purr_win_t h) {
    if (h >= 1 && h <= MAX_WINS) s_wins[h-1].used = false;
}
static purr_win_t win_index_for_handle(mw_handle_t wh) {
    for (int i = 0; i < MAX_WINS; i++)
        if (s_wins[i].used && s_wins[i].mw_win == wh) return (purr_win_t)(i+1);
    return 0;
}

static purr_wid_t alloc_wid(mw_handle_t win, mw_handle_t ctrl,
                              purr_win_cb_t cb, void *user) {
    for (int i = 0; i < MAX_WIDS; i++) {
        if (!s_wids[i].used) {
            s_wids[i].mw_win  = win;
            s_wids[i].mw_ctrl = ctrl;
            s_wids[i].cb      = cb;
            s_wids[i].user    = user;
            s_wids[i].used    = true;
            return (purr_wid_t)(i+1);
        }
    }
    return 0;
}
static wid_slot_t *get_wid(purr_wid_t h) {
    if (h < 1 || h > MAX_WIDS || !s_wids[h-1].used) return NULL;
    return &s_wids[h-1];
}

// Place the next widget of size (width, height) inside win h: if a row/col
// layout is active, advance the row's x-cursor and track its tallest widget;
// otherwise fall back to plain vertical stacking via the shared y-cursor.
static void layout_place(purr_win_t h, int16_t width, int16_t height,
                          uint8_t default_gap, int16_t *out_x, int16_t *out_y) {
    int idx = (int)(h - 1);
    layout_state_t *l = &s_layout[idx];
    if (l->active) {
        *out_x = l->x;
        *out_y = l->y;
        l->x = (int16_t)(l->x + width + l->pad);
        if (height > l->row_h) l->row_h = height;
    } else {
        *out_x = 4;
        *out_y = s_cursor_y[idx];
        s_cursor_y[idx] = (int16_t)(s_cursor_y[idx] + height + default_gap);
    }
}

// ── Per-window paint / message stubs ─────────────────────────────────────────

// Set only for the duration of a win_paint_func() call that has a
// registered canvas_paint_cb — canvas_rect()/canvas_text() are only valid
// called synchronously from inside that callback (same task, same call
// stack, so a plain static is safe — no concurrent writers).
static const mw_gl_draw_info_t *s_canvas_draw_info = NULL;

static void win_paint_func(mw_handle_t window_handle, const mw_gl_draw_info_t *draw_info)
{
    // MiniWin does not clear a window's client area on its own before its
    // controls draw themselves — without an explicit fill here, whatever was
    // on screen before this window (the desktop, a previous window) shows
    // through around/behind labels and buttons that don't fully cover their
    // own row, which reads as a stale "layered" image behind the real UI.
    mw_util_rect_t client = mw_get_window_client_rect(window_handle);
    mw_gl_set_fill(MW_GL_FILL);
    mw_gl_set_border(MW_GL_BORDER_OFF);
    mw_gl_set_solid_fill_colour(MW_CONTROL_UP_COLOUR);
    mw_gl_clear_pattern();
    mw_gl_rectangle(draw_info, 0, 0, client.width, client.height);

    purr_win_t win = win_index_for_handle(window_handle);
    if (win == 0) return;

    if (s_wins[win-1].canvas_paint_cb) {
        s_canvas_draw_info = draw_info;
        s_wins[win-1].canvas_paint_cb(win, s_wins[win-1].canvas_paint_user);
        s_canvas_draw_info = NULL;
    }

    // Every textarea in this window gets ONE outline, always — grey when
    // idle, blue when it's the focused field. The underlying control
    // (ui_text_box.c, upstream) draws no border of its own at all (it was
    // built as a read-only display widget, not an editable field — see
    // mw_ta_create()'s own comment), so a field was previously a flat,
    // borderless white rectangle floating on whatever background colour
    // surrounded it. Drawn here (window paint), not at control-creation
    // time, so it survives every window repaint instead of getting erased
    // by this same function's own background fill above.
    //
    // Deliberately ONE ring per field, not a separate always-there grey
    // ring plus a second, differently-inset blue ring stacked around it —
    // that first version drew both, 1px apart, for whichever field was
    // focused, and read as "the lines don't line up" rather than a single
    // clean border. Same inset for every field regardless of focus state
    // fixes that: only the colour changes.
    purr_wid_t focused = s_ta_focused[win-1];
    mw_gl_set_fill(MW_GL_NO_FILL);
    mw_gl_set_border(MW_GL_BORDER_ON);
    mw_gl_set_line(MW_GL_SOLID_LINE);
    for (int i = 0; i < MAX_WIDS; i++) {
        if (!s_wids[i].used || s_wids[i].mw_win != window_handle || !s_ta_buf[i]) continue;
        mw_util_rect_t tr = mw_get_control_rect(s_wids[i].mw_ctrl);
        mw_gl_set_fg_colour(((purr_wid_t)(i + 1) == focused) ? MW_HAL_LCD_BLUE : MW_HAL_LCD_GREY10);
        mw_gl_rectangle(draw_info,
            (int16_t)(tr.x - 1), (int16_t)(tr.y - 1),
            (int16_t)(tr.width + 2), (int16_t)(tr.height + 2));
    }
}

static void mw_win_destroy(purr_win_t h);   // defined below; forward-declared for the close-icon handler

static void mw_ta_focus(purr_wid_t wid);   // defined below — this function's own MW_TEXT_BOX_TOUCHED_MESSAGE case needs it

static void win_message_func(const mw_message_t *msg)
{
    if (!msg) return;
    if (msg->message_id == MW_BUTTON_PRESSED_MESSAGE) {
        mw_handle_t ctrl = msg->sender_handle;
        mw_handle_t win  = msg->recipient_handle;
        for (int i = 0; i < MAX_WIDS; i++) {
            if (s_wids[i].used && s_wids[i].mw_win == win && s_wids[i].mw_ctrl == ctrl) {
                if (s_wids[i].cb) {
                    // Button click callbacks run synchronously on this task —
                    // for .meow apps this is a direct lua_pcall() (see
                    // lua_runtime.c's threading-model comment). If a script's
                    // click handler ever blocks or loops, this is where it
                    // shows up as stuck.
                    purr_kernel_ui_breadcrumb("process_message/button_click_cb");
                    s_wids[i].cb((purr_wid_t)(i+1), PURR_EVENT_CLICKED, s_wids[i].user);
                    purr_kernel_ui_breadcrumb("process_message/button_click_cb_done");
                }
                break;
            }
        }
    } else if (msg->message_id == MW_TOUCH_DOWN_MESSAGE || msg->message_id == MW_TOUCH_UP_MESSAGE ||
               msg->message_id == MW_TOUCH_DRAG_MESSAGE) {
        // MiniWin already posts these to a window's own message_func,
        // client-relative, whenever a touch lands in the client area and
        // doesn't hit any native control (see process_touch_message()'s
        // window-client branch in MiniWin/miniwin.c) — no new touch
        // plumbing needed, just consuming what's already sent. Only
        // dispatched to apps that registered via purr_win_canvas_on_touch();
        // apps that only use native buttons never see these (MiniWin routes
        // touches inside a control's rect to the control instead).
        purr_win_t win = win_index_for_handle(msg->recipient_handle);
        if (win != 0 && s_wins[win-1].canvas_touch_cb) {
            int16_t x = (int16_t)(msg->message_data >> 16);
            int16_t y = (int16_t)(msg->message_data & 0xFFFFU);
            bool pressed = (msg->message_id != MW_TOUCH_UP_MESSAGE);
            s_wins[win-1].canvas_touch_cb(win, x, y, pressed, s_wins[win-1].canvas_touch_user);
        }
    } else if (msg->message_id == MW_LIST_BOX_ITEM_PRESSED_MESSAGE) {
        mw_handle_t ctrl = msg->sender_handle;
        mw_handle_t win  = msg->recipient_handle;
        for (int i = 0; i < MAX_WIDS; i++) {
            if (s_wids[i].used && s_wids[i].mw_win == win && s_wids[i].mw_ctrl == ctrl) {
                // Capture the real pressed index from message_data BEFORE
                // firing callbacks — mw_list_get_selected() reads this, not
                // MiniWin's own box.line_is_selected (which the list box
                // widget clears itself before ever posting this message;
                // see list_slot_t's comment). i indexes s_list_data the same
                // way it indexes s_wids — both are allocated together by
                // mw_list_create()'s single first-free-slot scan.
                s_list_data[i].last_pressed = (int)(uint8_t)msg->message_data;
                // MiniWin's list box has no intermediate highlight-without-confirm
                // state — a tap both selects and confirms in one message, so both
                // events fire together rather than SELECTED preceding ACTIVATED
                // by any meaningful interval.
                if (s_wids[i].cb) {
                    s_wids[i].cb((purr_wid_t)(i+1), PURR_EVENT_SELECTED, s_wids[i].user);
                    s_wids[i].cb((purr_wid_t)(i+1), PURR_EVENT_ACTIVATED, s_wids[i].user);
                }
                break;
            }
        }
    } else if (msg->message_id == MW_TEXT_BOX_TOUCHED_MESSAGE) {
        // PURR OS's own MiniWin/miniwin.h addition — see that message's own
        // doc comment and ui_text_box.c's MW_TOUCH_DOWN_MESSAGE case for
        // why: real, live bug, "only the first field ever takes text" no
        // matter which one was actually tapped, since nothing previously
        // updated s_ta_focused[] except an app's own one-time initial
        // purr_win_textarea_focus() call.
        mw_handle_t touched_ctrl = (mw_handle_t)msg->message_data;
        mw_handle_t win_h        = msg->recipient_handle;
        for (int i = 0; i < MAX_WIDS; i++) {
            if (s_wids[i].used && s_wids[i].mw_win == win_h && s_wids[i].mw_ctrl == touched_ctrl) {
                mw_ta_focus((purr_wid_t)(i + 1));
                break;
            }
        }
    } else if (msg->message_id == MW_WINDOW_REMOVED_MESSAGE) {
        // Fires synchronously from inside mw_remove_window() — either ours
        // (mw_win_destroy(), top-down: torn_down was already set true just
        // before that call, so we skip out immediately below) or MiniWin's
        // own title-bar close-icon handling calling it directly (bottom-up:
        // torn_down is still false here, since nothing on our side initiated
        // this). Only the bottom-up case needs handling — the app never
        // asked to close, so nobody else is going to run its close callback
        // or free our per-window bookkeeping unless we do it here.
        //
        // Deliberately NOT running the close callback (and therefore
        // app_manager_stop()'s whole teardown chain) synchronously right
        // here: mw_remove_window() is holding its own file-static
        // `reentered` re-entrancy guard for the duration of this dispatch
        // (MiniWin/miniwin.c), and a multi-window app's deinit() (e.g.
        // meshchat.c closing more than one window) would make a NESTED
        // mw_remove_window() call for its other window(s) while that guard
        // is still held — MiniWin silently no-ops the nested call, leaking
        // that window's pool slot forever. Confirmed live: enough leaked
        // slots eventually exhaust MiniWin's fixed-size window table and
        // trip MW_ASSERT("Bad window handle"), which ends in a bare,
        // unescapable while(true){} on the task holding
        // purr_kernel_ui_lock() — freezing the entire UI. Deferring the
        // actual teardown to run from miniwin_module.c's pump loop, AFTER
        // this dispatch (and its reentered guard) has fully unwound, means
        // any further purr_win_destroy() calls a close callback makes are
        // fresh, top-level mw_remove_window() calls instead of nested ones.
        mw_handle_t wh  = msg->recipient_handle;
        purr_win_t  win = win_index_for_handle(wh);
        if (win == 0 || s_wins[win-1].torn_down) return;

        s_wins[win-1].torn_down = true;
#ifdef CONFIG_PURR_MINIWIN_DESKTOP_WINCE
        // mw_win_destroy()'s top-down path unregisters the taskbar entry
        // itself (miniwin_win.c's own wce_taskbar_unregister() call) before
        // it ever calls mw_remove_window() — but that path is skipped
        // entirely here, since torn_down is already true by the time
        // mw_win_destroy() runs for a bottom-up close (X icon). Without
        // this, the taskbar keeps a stale handle for this window forever;
        // the very next repaint calls mw_get_window_flags() on it and hits
        // MW_ASSERT("Bad window handle") — confirmed live right after the
        // mw_remove_window() infinite-loop fix started letting bottom-up
        // closes actually complete instead of hanging before ever reaching
        // this far.
        wce_taskbar_unregister(wh);
#endif
        if (s_pending_close_count < MAX_PENDING_CLOSES) {
            s_pending_closes[s_pending_close_count++] = win;
        }
    } else if (msg->message_id == MW_KEY_PRESSED_MESSAGE) {
        // miniwin_keyboard.c posts this straight to the focused window for
        // every physical key-down (masked to the low byte — plain ASCII);
        // nothing here ever consumed it, so typed characters never reached
        // any textarea's buffer at all — confirmed live once the
        // mw_ta_create() layout fix made textareas actually visible enough
        // to notice typing did nothing.
        purr_win_t win = win_index_for_handle(msg->recipient_handle);
        if (win == 0) return;
        purr_wid_t focused_ta = s_ta_focused[win-1];
        if (focused_ta == 0) return;
        int idx = (int)(focused_ta - 1);
        if (idx < 0 || idx >= MAX_WIDS || !s_ta_buf[idx]) return;

        uint8_t key = (uint8_t)msg->message_data;
        if (key == 0x08 || key == 0x7F) {
            // Backspace/Delete — trim one char off the end.
            size_t len = strlen(s_ta_buf[idx]);
            if (len > 0) {
                s_ta_buf[idx][len - 1] = '\0';
                mw_ta_recalc_height(idx);
                mw_paint_control(s_wids[idx].mw_ctrl);
            }
        } else if (key == '\r' || key == '\n') {
            // Enter = submit when the app registered a textarea callback
            // (PURR_EVENT_ACTIVATED — same meaning as a list's confirm):
            // Terminal runs the command exactly as its Send button would.
            // Only textareas with no callback get a literal newline — and
            // note mw_ta_append("\n") into a one-line-high input box locked
            // the whole UI solid on tab5 (first device with a physical
            // keyboard driving MiniWin), so the submit path isn't just
            // nicer UX, it's the one that's actually known-safe.
            if (s_wids[idx].cb) {
                s_wids[idx].cb(focused_ta, PURR_EVENT_ACTIVATED, s_wids[idx].user);
            } else {
                mw_ta_append(focused_ta, "\n");
            }
        } else if (key >= 0x20 && key <= 0x7E) {
            char ch[2] = { (char)key, '\0' };
            mw_ta_append(focused_ta, ch);
        }
    }
}

// Drains windows queued by the MW_WINDOW_REMOVED_MESSAGE handler above —
// must be called from a top-level context (miniwin_module.c's pump loop,
// right after mw_process_message()), never from inside any MiniWin-internal
// callback dispatch. Snapshot-and-clear before iterating: a close callback
// can itself synchronously queue further closes (a multi-window app's
// deinit()), which must not be processed by / interleaved with this same
// drain pass.
void miniwin_win_process_pending_closes(void) {
    if (s_pending_close_count == 0) return;
    purr_win_t local[MAX_PENDING_CLOSES];
    int n = s_pending_close_count;
    memcpy(local, s_pending_closes, sizeof(purr_win_t) * (size_t)n);
    s_pending_close_count = 0;

    for (int i = 0; i < n; i++) {
        purr_win_t win = local[i];
        if (win < 1 || win > MAX_WINS || !s_wins[win-1].used) continue;
        purr_win_cb_t cb   = s_wins[win-1].close_cb;
        void         *user = s_wins[win-1].close_user;
        if (cb) cb((purr_wid_t)win, PURR_EVENT_CLICKED, user);
        // Whether or not that callback itself already tore this window down
        // (e.g. app_manager's does, via the app's own deinit()), this is a
        // no-op by then — mw_win_destroy() guards on the pool slot's `used`
        // flag. If no callback was registered at all (a dialog with no
        // purr_win_on_close call), this is what actually frees the heap
        // storage and pool slot instead of leaking them forever. torn_down
        // is already true (set synchronously above), so this correctly
        // skips the redundant mw_remove_window() call — MiniWin's own
        // close-icon handling already did that part, bottom-up.
        mw_win_destroy(win);
    }
}

// ── Window ────────────────────────────────────────────────────────────────────

static purr_win_t mw_win_create(const char *title) {
    int16_t h_disp = (int16_t)mw_hal_lcd_get_display_height();
#ifdef CONFIG_PURR_MINIWIN_DESKTOP_WINCE
    // Leave the taskbar strip out of every app window's own rect — see
    // wce_taskbar_height()'s comment for why this is what actually keeps
    // the taskbar/Start button/wallpaper desktop visible (and multiple
    // apps switchable via taskbar buttons) instead of a maximized app
    // window's full-screen rect blotting out everything beneath it,
    // desktop included, at the highest z-order.
    h_disp -= wce_taskbar_height();
#endif
    mw_util_rect_t r = { 0, 0, mw_hal_lcd_get_display_width(), h_disp };
    mw_handle_t h = mw_add_window(&r, title,
        win_paint_func, win_message_func,
        NULL, 0,
        MW_WINDOW_FLAG_HAS_BORDER | MW_WINDOW_FLAG_HAS_TITLE_BAR | MW_WINDOW_FLAG_CAN_BE_CLOSED,
        NULL);
    if (h == MW_INVALID_HANDLE) return 0;
    mw_set_window_visible(h, false);
#ifdef CONFIG_PURR_MINIWIN_DESKTOP_WINCE
    // Every window that goes through purr_win_create() gets a taskbar
    // button automatically — apps don't call wce_taskbar_register themselves.
    wce_taskbar_register(h, title);
#endif
    return alloc_win(h);
}

// Frees each of this window's widgets' heap storage (textarea buffer, list
// entries) and marks their wid slots unused. also_remove_control must be
// false when MiniWin has already torn the controls down itself (bottom-up
// close-icon path) — calling mw_remove_control() on an already-removed
// control handle hits MW_ASSERT("Bad control handle") and halts.
static void free_widget_storage_for_window(mw_handle_t wh, bool also_remove_control);   // defined below

static void mw_win_on_close(purr_win_t h, purr_win_cb_t cb, void *user) {
    if (h < 1 || h > MAX_WINS) return;
    s_wins[h-1].close_cb = cb;
    s_wins[h-1].close_user = user;
}

static void mw_win_destroy(purr_win_t h) {
    if (h < 1 || h > MAX_WINS || !s_wins[h-1].used) return;
    mw_handle_t wh = s_wins[h-1].mw_win;
    bool already_torn_down = s_wins[h-1].torn_down;

    // also_remove_control=false in BOTH cases here — a real, live bug found
    // via a genuine top-down destroy actually happening for the first time
    // (app_manager's memory watchdog force-stopping OOBE): the !already_
    // torn_down branch below calls MiniWin's own mw_remove_window(wh)
    // right after this, which ALREADY iterates and removes every control
    // whose parent_handle == wh (MiniWin/miniwin.c's own mw_remove_window(),
    // "remove all controls that have this window as a parent"). Passing
    // true here as well made free_widget_storage_for_window() remove them
    // FIRST, so mw_remove_window()'s own loop then tried to remove the same
    // already-freed control handles a second time — MW_ASSERT("Bad control
    // handle"), confirmed live (crash_guard: "MW_ASSERT mw_remove_control:
    // 5352 Bad control handle"). The already_torn_down==true branch was
    // already correctly false (MiniWin's own close-icon handling already
    // removed them before mw_win_destroy() ever runs) — this function's
    // own remaining job in both cases is just freeing THIS file's own
    // malloc'd storage (textarea buffers, list entries) and marking s_wids
    // slots unused, never touching MiniWin's control array itself.
    free_widget_storage_for_window(wh, /*also_remove_control=*/false);
    s_ta_focused[h-1] = 0;

    if (!already_torn_down) {
        // Normal top-down destroy: MiniWin hasn't touched this window yet.
        // Mark torn_down before calling mw_remove_window() — it dispatches
        // MW_WINDOW_REMOVED_MESSAGE synchronously, and by the time that
        // reaches win_message_func() above this flag must already read true
        // so it knows this removal is already being handled right here and
        // doesn't fire the close callback a second time.
        s_wins[h-1].torn_down = true;
#ifdef CONFIG_PURR_MINIWIN_DESKTOP_WINCE
        wce_taskbar_unregister(wh);
#endif
        mw_remove_window(wh);
    }
    // else: MiniWin's title-bar close icon (or a nested call from within the
    // close-callback dispatch above) already removed the window and its
    // controls — nothing left to do here but release our own bookkeeping.

    s_cursor_y[h-1] = 0;
    s_layout[h-1].active = false;
    free_win(h);

#ifdef CONFIG_PURR_MINIWIN_DESKTOP_WINCE
    // Closing this window exposes whatever's underneath — see
    // wce_redraw_layers()'s doc comment for why mw_paint_all() alone can't
    // be trusted to catch that on its own. Unconditional (not just the
    // !already_torn_down branch above) since both teardown paths end up
    // here and both can leave the same stale pixels behind.
    wce_redraw_layers();
#endif
}

static void mw_win_show(purr_win_t h) {
    mw_handle_t wh = get_win(h);
    if (wh == MW_INVALID_HANDLE) return;
    mw_set_window_visible(wh, true);
    mw_bring_window_to_front(wh);
    // Becoming visible does not by itself trigger a repaint (same class of
    // gotcha as mw_init() not painting the root window on its own — see
    // miniwin_module.c) — without this, a newly-launched app can sit fully
    // initialized but invisible on screen until some unrelated event happens
    // to force a repaint.
    mw_paint_all();
}

static void mw_win_hide(purr_win_t h) {
    mw_handle_t wh = get_win(h);
    if (wh != MW_INVALID_HANDLE) mw_set_window_visible(wh, false);
}

// ── Textarea storage — declared earlier, above win_message_func (see that
// forward comment) — and List storage, same (see its own forward comment).

// Avoids relying on strdup, which newlib-nano configs can omit.
static char *dup_str(const char *s) {
    size_t len = strlen(s) + 1;
    char *out = (char *)malloc(len);
    if (out) memcpy(out, s, len);
    return out;
}

static void free_list_entries(int idx) {
    if (s_list_data[idx].box.list_box_entries) {
        for (int j = 0; j < s_list_data[idx].box.number_of_items; j++)
            free((void *)s_list_data[idx].box.list_box_entries[j].label);
        free((void *)s_list_data[idx].box.list_box_entries);
        s_list_data[idx].box.list_box_entries = NULL;
        s_list_data[idx].box.number_of_items = 0;
    }
}

static void free_widget_storage_for_window(mw_handle_t wh, bool also_remove_control) {
    for (int i = 0; i < MAX_WIDS; i++) {
        if (s_wids[i].used && s_wids[i].mw_win == wh) {
            if (also_remove_control) mw_remove_control(s_wids[i].mw_ctrl);
            if (s_ta_buf[i]) { free(s_ta_buf[i]); s_ta_buf[i] = NULL; }
            free_list_entries(i);
            s_wids[i].used = false;
        }
    }
}

static void mw_win_clear(purr_win_t h) {
    mw_handle_t wh = get_win(h);
    if (wh == MW_INVALID_HANDLE) return;
    if (h >= 1 && h <= MAX_WINS) s_ta_focused[h-1] = 0;
    free_widget_storage_for_window(wh, /*also_remove_control=*/true);
    if (h >= 1 && h <= MAX_WINS) {
        s_cursor_y[h-1] = 0;
        s_layout[h-1].active = false;
    }
}

// ── Labels ────────────────────────────────────────────────────────────────────

// EXT_RAM_BSS_ATTR — see s_list_data's own comment above for the full
// reasoning (real measured internal-DRAM pressure on tdeck_plus, safe
// here since these are only touched on redraw, no-op on PSRAM-less
// devices).
static EXT_RAM_BSS_ATTR mw_ui_label_data_t s_label_data[MAX_WIDS];
static EXT_RAM_BSS_ATTR char               s_label_text[MAX_WIDS][MW_UI_LABEL_MAX_CHARS + 1];
static uint8_t            s_label_align[MAX_WIDS];
static int16_t            s_label_width[MAX_WIDS];

// MiniWin's label control always draws left-justified at a fixed offset and
// offers no API to reposition a control after creation, so centre/right
// alignment is approximated by left-padding the rendered copy with spaces —
// the logical (unpadded) text is kept separately in s_label_text so repeated
// label_set()/label_align() calls don't compound the padding.
static void apply_label_render(int idx) {
    const char *text = s_label_text[idx];
    purr_align_t align = (purr_align_t)s_label_align[idx];
    if (align == PURR_ALIGN_LEFT) {
        strncpy(s_label_data[idx].label, text, MW_UI_LABEL_MAX_CHARS);
        s_label_data[idx].label[MW_UI_LABEL_MAX_CHARS] = '\0';
        return;
    }

    mw_gl_set_font(MW_GL_FONT_9);
    int16_t text_w  = mw_gl_get_string_width_pixels(text);
    int16_t space_w = mw_gl_get_string_width_pixels(" ");
    int16_t avail   = (int16_t)(s_label_width[idx] - MW_UI_LABEL_X_OFFSET - text_w);
    int16_t pad_px  = (align == PURR_ALIGN_CENTER) ? (int16_t)(avail / 2) : avail;
    int n_spaces = (pad_px > 0 && space_w > 0) ? (pad_px / space_w) : 0;
    if (n_spaces > MW_UI_LABEL_MAX_CHARS) n_spaces = MW_UI_LABEL_MAX_CHARS;

    char buf[MW_UI_LABEL_MAX_CHARS + 1];
    int i = 0;
    for (; i < n_spaces; i++) buf[i] = ' ';
    buf[i] = '\0';
    strncat(buf, text, MW_UI_LABEL_MAX_CHARS - (size_t)i);
    strncpy(s_label_data[idx].label, buf, MW_UI_LABEL_MAX_CHARS);
    s_label_data[idx].label[MW_UI_LABEL_MAX_CHARS] = '\0';
}

static purr_wid_t mw_label_create(purr_win_t h, const char *text) {
    mw_handle_t wh = get_win(h);
    if (wh == MW_INVALID_HANDLE) return 0;

    int slot = -1;
    for (int i = 0; i < MAX_WIDS; i++) {
        if (!s_wids[i].used) { slot = i; break; }
    }
    if (slot < 0) return 0;

    int16_t width = s_layout[h-1].active
        ? MW_ROW_LABEL_WIDTH
        : (int16_t)(mw_hal_lcd_get_display_width() - 8);

    memset(&s_label_data[slot], 0, sizeof(s_label_data[slot]));
    strncpy(s_label_text[slot], text, MW_UI_LABEL_MAX_CHARS);
    s_label_text[slot][MW_UI_LABEL_MAX_CHARS] = '\0';
    s_label_align[slot] = PURR_ALIGN_LEFT;
    s_label_width[slot] = width;
    apply_label_render(slot);

    int16_t x, y;
    layout_place(h, width, MW_UI_LABEL_HEIGHT, 2, &x, &y);
    mw_handle_t ctrl = mw_ui_label_add_new(x, y, width,
        wh,
        MW_CONTROL_FLAG_IS_VISIBLE | MW_CONTROL_FLAG_IS_ENABLED,
        &s_label_data[slot]);

    return alloc_wid(wh, ctrl, NULL, NULL);
}

static void mw_label_set(purr_wid_t wid, const char *text) {
    wid_slot_t *s = get_wid(wid);
    if (!s) return;
    int idx = (int)(wid - 1);
    strncpy(s_label_text[idx], text, MW_UI_LABEL_MAX_CHARS);
    s_label_text[idx][MW_UI_LABEL_MAX_CHARS] = '\0';
    apply_label_render(idx);
    mw_paint_control(s->mw_ctrl);
}

static void mw_label_align(purr_wid_t wid, purr_align_t align) {
    wid_slot_t *s = get_wid(wid);
    if (!s) return;
    int idx = (int)(wid - 1);
    s_label_align[idx] = (uint8_t)align;
    apply_label_render(idx);
    mw_paint_control(s->mw_ctrl);
}

// ── Buttons ───────────────────────────────────────────────────────────────────

// EXT_RAM_BSS_ATTR — see s_list_data's own comment above.
static EXT_RAM_BSS_ATTR mw_ui_button_data_t s_btn_data[MAX_WIDS];

static purr_wid_t mw_btn_create(purr_win_t h, const char *label,
                                  purr_win_cb_t cb, void *user) {
    mw_handle_t wh = get_win(h);
    if (wh == MW_INVALID_HANDLE) return 0;

    int slot = -1;
    for (int i = 0; i < MAX_WIDS; i++) {
        if (!s_wids[i].used) { slot = i; break; }
    }
    if (slot < 0) return 0;

    memset(&s_btn_data[slot], 0, sizeof(s_btn_data[slot]));
    strncpy(s_btn_data[slot].button_label, label, MW_UI_BUTTON_LABEL_MAX_CHARS);

    int16_t x, y;
    layout_place(h, MW_UI_BUTTON_WIDTH, MW_UI_BUTTON_HEIGHT, 4, &x, &y);
    mw_handle_t ctrl = mw_ui_button_add_new(x, y,
        wh,
        MW_CONTROL_FLAG_IS_VISIBLE | MW_CONTROL_FLAG_IS_ENABLED,
        &s_btn_data[slot]);

    return alloc_wid(wh, ctrl, cb, user);
}

static void mw_btn_enable(purr_wid_t wid, bool enabled) {
    wid_slot_t *s = get_wid(wid);
    if (s) mw_set_control_enabled(s->mw_ctrl, enabled);
}

// ── Textarea ──────────────────────────────────────────────────────────────────

static purr_wid_t mw_ta_create(purr_win_t h, uint16_t w_pct, uint16_t h_pct) {
    mw_handle_t wh = get_win(h);
    if (wh == MW_INVALID_HANDLE) return 0;

    int slot = -1;
    for (int i = 0; i < MAX_WIDS; i++) {
        if (!s_wids[i].used) { slot = i; break; }
    }
    if (slot < 0) return 0;

    s_ta_buf[slot] = (char *)malloc(MW_TA_BUF_SIZE);
    if (!s_ta_buf[slot]) return 0;
    s_ta_buf[slot][0] = '\0';

    memset(&s_ta_data[slot], 0, sizeof(s_ta_data[slot]));
    s_ta_data[slot].text = s_ta_buf[slot];
    s_ta_data[slot].tt_font = &mf_rlefont_DejaVuSans12;
    // fg_colour/bg_colour default to 0 (black) from the memset above, which
    // renders as an all-black box (black text on a black fill) — not a
    // crash, just invisible. Give it a readable black-on-white default.
    s_ta_data[slot].fg_colour = MW_HAL_LCD_BLACK;
    s_ta_data[slot].bg_colour = MW_HAL_LCD_WHITE;

    // Percent-of-CLIENT-AREA, not percent-of-display: sizing against the
    // full display over-allocates by the title bar + (WinCE desktop) taskbar
    // strip, so a stack that sums near 100% (terminal's 75% output + 10%
    // input + Send button) pushed its last controls past the window's client
    // rect — invisible below the bottom edge, "hidden under the taskbar" on
    // tab5 where the tall-screen proportions made it obvious.
    mw_util_rect_t client = mw_get_window_client_rect(wh);
    int16_t width  = (int16_t)((client.width  * w_pct) / 100 - 8);
    int16_t height = (int16_t)((client.height * h_pct) / 100);
    // Was hardcoded to (4, 28), ignoring layout_place() entirely — every
    // other widget type (list/button/label) goes through layout_place() to
    // get a position that accounts for an active row/col layout or the
    // vertical-stacking cursor. A textarea placed beside a list in a row
    // (fileman's file-list + preview-pane layout, exactly this app's
    // structure) or as the second of two stacked textareas (terminal's
    // output + input, meshchat's chat log + input) landed right on top of
    // whatever came before it instead of beside/below it — confirmed live
    // as a white rectangle (this control's default bg_colour) covering a
    // file list's contents.
    int16_t x, y;
    layout_place(h, width, height, 4, &x, &y);
    mw_util_rect_t r = { x, y, width, height };
    mw_handle_t ctrl = mw_ui_text_box_add_new(&r, wh,
        MW_CONTROL_FLAG_IS_VISIBLE | MW_CONTROL_FLAG_IS_ENABLED,
        &s_ta_data[slot]);
    return alloc_wid(wh, ctrl, NULL, NULL);
}

// MiniWin's real MW_TEXT_BOX_SET_TEXT_MESSAGE handler (ui_text_box.c)
// recalculates text_height_pixels whenever the content changes — required
// because text_box_paint_function background-fills everything below
// text_height_pixels every repaint. Our append/set skip that message path
// (they mutate the buffer directly), so without this the control keeps
// whatever text_height_pixels was computed at creation time — for a
// textarea that starts empty and gets its real content filled in right
// after (about.c, etc.), that's ~0, so the very next repaint immediately
// paints over almost all of the text it just drew.
static void mw_ta_recalc_height(int idx) {
    mw_handle_t ctrl = s_wids[idx].mw_ctrl;
    s_ta_data[idx].text_height_pixels = mw_gl_tt_get_render_text_lines(
        mw_get_control_rect(ctrl).width,
        s_ta_data[idx].justification,
        s_ta_data[idx].tt_font,
        s_ta_data[idx].text);
}

static void mw_ta_append(purr_wid_t wid, const char *text) {
    wid_slot_t *s = get_wid(wid);
    if (!s) return;
    int idx = (int)(wid - 1);
    if (!s_ta_buf[idx]) return;
    strncat(s_ta_buf[idx], text, MW_TA_BUF_SIZE - strlen(s_ta_buf[idx]) - 1);
    mw_ta_recalc_height(idx);
    mw_paint_control(s->mw_ctrl);
}

static void mw_ta_set(purr_wid_t wid, const char *text) {
    wid_slot_t *s = get_wid(wid);
    if (!s) return;
    int idx = (int)(wid - 1);
    if (!s_ta_buf[idx]) return;
    strncpy(s_ta_buf[idx], text, MW_TA_BUF_SIZE - 1);
    s_ta_buf[idx][MW_TA_BUF_SIZE - 1] = '\0';
    s_ta_data[idx].lines_to_scroll = 0;   // full replace — scroll back to top
    mw_ta_recalc_height(idx);
    mw_paint_control(s->mw_ctrl);
}

static void mw_ta_clear(purr_wid_t wid) {
    mw_ta_set(wid, "");
}

static const char *mw_ta_get(purr_wid_t wid) {
    int idx = (int)(wid - 1);
    if (idx < 0 || idx >= MAX_WIDS || !s_wids[idx].used) return NULL;
    return s_ta_buf[idx];
}

static void mw_ta_focus(purr_wid_t wid) {
    wid_slot_t *s = get_wid(wid);
    if (!s) return;
    purr_win_t win = win_index_for_handle(s->mw_win);
    if (win == 0) return;
    s_ta_focused[win-1] = wid;
    mw_paint_window_client(s->mw_win);
}

static void mw_ta_cb(purr_wid_t wid, purr_win_cb_t cb, void *user) {
    wid_slot_t *s = get_wid(wid);
    if (s) { s->cb = cb; s->user = user; }
}

// ── List (flat, non-nested selectable list) ──────────────────────────────────

static purr_wid_t mw_list_create(purr_win_t h, uint16_t w_pct, uint16_t h_pct) {
    mw_handle_t wh = get_win(h);
    if (wh == MW_INVALID_HANDLE) return 0;

    int slot = -1;
    for (int i = 0; i < MAX_WIDS; i++) {
        if (!s_wids[i].used) { slot = i; break; }
    }
    if (slot < 0) return 0;

    // Percent of the window's client area — see mw_ta_create()'s comment for
    // why display-based percentages overflow the window.
    mw_util_rect_t client = mw_get_window_client_rect(wh);
    int16_t width  = (int16_t)((client.width * w_pct) / 100 - 8);
    if (width < MW_UI_LIST_BOX_MIN_WIDTH) width = MW_UI_LIST_BOX_MIN_WIDTH;
    int16_t avail_h    = (int16_t)((client.height * h_pct) / 100);
    int16_t num_lines  = (int16_t)(avail_h / MW_UI_LIST_BOX_ROW_HEIGHT);
    if (num_lines < 1) num_lines = 1;

    memset(&s_list_data[slot], 0, sizeof(s_list_data[slot]));
    s_list_data[slot].box.number_of_lines = (uint8_t)num_lines;
    s_list_data[slot].box.number_of_items = 0;
    s_list_data[slot].box.list_box_entries = NULL;
    s_list_data[slot].box.line_enables = MW_ALL_ITEMS_ENABLED;
    s_list_data[slot].last_pressed = -1;

    int16_t x, y;
    layout_place(h, width, (int16_t)(num_lines * MW_UI_LIST_BOX_ROW_HEIGHT), 4, &x, &y);
    mw_handle_t ctrl = mw_ui_list_box_add_new(x, y, width,
        wh,
        MW_CONTROL_FLAG_IS_VISIBLE | MW_CONTROL_FLAG_IS_ENABLED,
        &s_list_data[slot].box);
    if (ctrl == MW_INVALID_HANDLE) return 0;

    return alloc_wid(wh, ctrl, NULL, NULL);
}

static void mw_list_set_items(purr_wid_t wid, const char **items, int count) {
    wid_slot_t *s = get_wid(wid);
    if (!s) return;
    int idx = (int)(wid - 1);
    free_list_entries(idx);

    if (count > 0 && items) {
        mw_ui_list_box_entry *entries =
            (mw_ui_list_box_entry *)malloc(sizeof(mw_ui_list_box_entry) * (size_t)count);
        if (entries) {
            for (int i = 0; i < count; i++) {
                entries[i].label = dup_str(items[i] ? items[i] : "");
                entries[i].icon  = NULL;
            }
            s_list_data[idx].box.list_box_entries = entries;
            s_list_data[idx].box.number_of_items = (uint8_t)count;
        }
    }
    s_list_data[idx].box.lines_to_scroll = 0;
    s_list_data[idx].box.line_is_selected = false;
    s_list_data[idx].last_pressed = -1;  // content changed — old index may not even exist anymore
    mw_paint_control(s->mw_ctrl);
}

static void mw_list_clear(purr_wid_t wid) {
    mw_list_set_items(wid, NULL, 0);
}

static int mw_list_get_selected(purr_wid_t wid) {
    int idx = (int)(wid - 1);
    if (idx < 0 || idx >= MAX_WIDS || !s_wids[idx].used) return -1;
    return s_list_data[idx].last_pressed;
}

static void mw_list_set_selected(purr_wid_t wid, int index) {
    wid_slot_t *s = get_wid(wid);
    if (!s) return;
    int idx = (int)(wid - 1);
    if (index < 0) {
        s_list_data[idx].box.line_is_selected = false;
        s_list_data[idx].last_pressed = -1;
    } else {
        s_list_data[idx].box.selection = (uint8_t)index;
        s_list_data[idx].box.line_is_selected = true;
        s_list_data[idx].last_pressed = index;
    }
    mw_paint_control(s->mw_ctrl);
}

static void mw_list_cb(purr_wid_t wid, purr_win_cb_t cb, void *user) {
    wid_slot_t *s = get_wid(wid);
    if (s) { s->cb = cb; s->user = user; }
}

// ── Layout ────────────────────────────────────────────────────────────────────

static purr_wid_t mw_layout_begin(purr_win_t h, purr_layout_t dir, uint8_t pad, bool grow) {
    (void)grow;  // MiniWin's manual cursor-based layout has no flex-grow concept
    if (h < 1 || h > MAX_WINS) return 0;
    layout_state_t *l = &s_layout[h-1];
    l->active = (dir == PURR_LAYOUT_ROW);
    l->x = 4;
    l->y = s_cursor_y[h-1];
    l->row_h = 0;
    l->pad = pad;
    return (purr_wid_t)h;
}

static void mw_layout_end(purr_wid_t wid) {
    if (wid < 1 || wid > MAX_WINS) return;
    layout_state_t *l = &s_layout[wid-1];
    if (l->active) {
        s_cursor_y[wid-1] = (int16_t)(l->y + l->row_h + l->pad);
    }
    l->active = false;
}

// ── Keyboard ──────────────────────────────────────────────────────────────────

static void mw_kb_show(purr_win_t h, purr_wid_t target) { (void)h; (void)target; }
static void mw_kb_hide(purr_win_t h) { (void)h; }

// ── Canvas (raw draw + touch) ────────────────────────────────────────────────

static void mw_canvas_on_paint(purr_win_t h, purr_win_paint_cb_t cb, void *user) {
    if (h < 1 || h > MAX_WINS) return;
    s_wins[h-1].canvas_paint_cb = cb;
    s_wins[h-1].canvas_paint_user = user;
}

static void mw_canvas_on_touch(purr_win_t h, purr_win_touch_cb_t cb, void *user) {
    if (h < 1 || h > MAX_WINS) return;
    s_wins[h-1].canvas_touch_cb = cb;
    s_wins[h-1].canvas_touch_user = user;
}

static void mw_canvas_rect(purr_win_t h, int16_t x, int16_t y, int16_t w, int16_t h_px, uint32_t color) {
    if (h < 1 || h > MAX_WINS || !s_canvas_draw_info) return;
    mw_gl_set_fill(MW_GL_FILL);
    mw_gl_set_border(MW_GL_BORDER_OFF);
    mw_gl_set_solid_fill_colour((mw_hal_lcd_colour_t)color);
    mw_gl_clear_pattern();
    mw_gl_rectangle(s_canvas_draw_info, x, y, w, h_px);
}

static void mw_canvas_text(purr_win_t h, int16_t x, int16_t y, const char *text, uint32_t color) {
    if (h < 1 || h > MAX_WINS || !s_canvas_draw_info || !text) return;
    mw_gl_set_fg_colour((mw_hal_lcd_colour_t)color);
    mw_gl_set_bg_transparency(MW_GL_BG_TRANSPARENT);
    mw_gl_set_font(MW_GL_FONT_16);
    mw_gl_string(s_canvas_draw_info, x, y, text);
}

static void mw_canvas_repaint(purr_win_t h) {
    mw_handle_t wh = get_win(h);
    if (wh == MW_INVALID_HANDLE) return;
    mw_paint_window_client(wh);
}

static void mw_canvas_size(purr_win_t h, int16_t *w, int16_t *h_out) {
    mw_handle_t wh = get_win(h);
    if (wh == MW_INVALID_HANDLE) { if (w) *w = 0; if (h_out) *h_out = 0; return; }
    mw_util_rect_t client = mw_get_window_client_rect(wh);
    if (w) *w = client.width;
    if (h_out) *h_out = client.height;
}

// ── Registration ──────────────────────────────────────────────────────────────

static const catcall_ui_t s_miniwin_ui = {
    .name             = "miniwin",
    .catcall_version  = CATCALL_UI_VERSION,
    .win_create       = mw_win_create,
    .win_destroy      = mw_win_destroy,
    .win_show         = mw_win_show,
    .win_hide         = mw_win_hide,
    .win_clear        = mw_win_clear,
    .win_on_close     = mw_win_on_close,
    .label_create     = mw_label_create,
    .label_set        = mw_label_set,
    .label_align      = mw_label_align,
    .btn_create       = mw_btn_create,
    .btn_enable       = mw_btn_enable,
    .textarea_create  = mw_ta_create,
    .textarea_append  = mw_ta_append,
    .textarea_set     = mw_ta_set,
    .textarea_clear   = mw_ta_clear,
    .textarea_get     = mw_ta_get,
    .textarea_focus   = mw_ta_focus,
    .textarea_cb      = mw_ta_cb,
    .list_create      = mw_list_create,
    .list_set_items   = mw_list_set_items,
    .list_clear       = mw_list_clear,
    .list_get_selected = mw_list_get_selected,
    .list_set_selected = mw_list_set_selected,
    .list_cb          = mw_list_cb,
    .layout_begin     = mw_layout_begin,
    .layout_end       = mw_layout_end,
    .kb_show          = mw_kb_show,
    .kb_hide          = mw_kb_hide,
    .canvas_on_paint  = mw_canvas_on_paint,
    .canvas_on_touch  = mw_canvas_on_touch,
    .canvas_rect      = mw_canvas_rect,
    .canvas_text      = mw_canvas_text,
    .canvas_repaint   = mw_canvas_repaint,
    .canvas_size      = mw_canvas_size,
};

void miniwin_win_register(void) {
    purr_kernel_register_ui(&s_miniwin_ui);
}
