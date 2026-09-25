// miniwin_wince_desktop.c — WinCE-style taskbar+start-menu desktop for the
// generic MiniWin .purr module. Ported from the WinCE shell baked directly
// into kernel_tdeck_plus_arduino (wince_shell/common/taskbar.cpp), generalized
// so any native-kernel MiniWin device can use it: the Start Menu's Programs
// list is built from the real app_manager registry instead of a fixed
// catalog, and taskbar entries are registered automatically by every window
// that goes through purr_win_create() (see the wce_taskbar_* calls in
// miniwin_win.c) rather than by hand in each app.
//
// Wallpaper, desktop icons, the taskbar+Start Menu, and the lock overlay
// are four SEPARATE, real z-ordered MiniWin windows (not one big window
// manually dispatching touches by coordinate range) — see each window's
// own comment below for why. MiniWin resolves touch input by picking the
// highest z-order window whose window_rect contains the touch point
// (find_window_point_is_in(), MiniWin/miniwin.c) — rect + z-order, not
// what's visually drawn there — so each window's rect has to track its
// real visual footprint, which is why the taskbar+menu window resizes
// itself live when the Start Menu opens/closes.
//
// This 4-window architecture was tried once before, then reverted (see
// commit fb39a3a1's message: "regressions") back to a single-window
// design — the regressions were stale-pixel/redraw bugs, not anything
// about the feature set itself. Restored here with wce_redraw_layers()
// (this file's own belt-and-suspenders repaint) plus miniwin_module.c's
// bounded per-tick message draining (drains a burst of paint-related
// messages in one go instead of one every 5ms tick, which was causing a
// separate, unrelated "takes a few redraws to see the taskbar" symptom
// confirmed live on tonight's single-window build) — between the two,
// this should be considerably more robust than the original attempt.
//
// IMPORTANT: every repaint triggered by user interaction in this file goes
// through wce_redraw_layers(), which always ends in mw_paint_all() — never
// a bare mw_paint_window_client()/_rect() targeted at the desktop-layer
// windows. That's deliberate, not just belt-and-suspenders: MiniWin's
// do_paint_window_client()/do_paint_window_client_rect() (MiniWin/
// miniwin.c) both skip occlusion entirely whenever the target window
// currently holds input focus, and these desktop windows hold focus
// throughout normal interaction (touch grants it, and MiniWin's own touch
// handling calls mw_bring_window_to_front() unconditionally on whatever
// window a touch lands on) — a targeted repaint fired right after
// launching an app can blast straight over the just-opened app window
// instead of respecting its z-order, confirmed live as "app windows don't
// appear" on a build that used mw_paint_window_client() here instead.
// mw_paint_all() has no such shortcut, so it's the only repaint call any
// of this file's app-launch/taskbar-focus-switch paths ever use.
//
// Only compiled in when CONFIG_PURR_MINIWIN_DESKTOP_WINCE is set.
#include "sdkconfig.h"

#ifdef CONFIG_PURR_MINIWIN_DESKTOP_WINCE

#include "miniwin_wince_desktop.h"
#include "miniwin_lock.h"
#include "MiniWin/miniwin_utilities.h"
#include "MiniWin/gl/gl.h"
#include "MiniWin/hal/hal_lcd.h"
#include "../../kernel/core/purr_kernel.h"
#include "../app_manager/app_manager.h"
#include "../user_mgr/user_mgr.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include <time.h>
#ifdef CONFIG_PURR_FEATURE_MESHTASTIC
#include "../meshtastic/meshtastic.h"
#endif

// ── Palette + bevels (ported from wince_common.cpp) ─────────────────────────

static const char *TAG = "wce_desktop";

#define WCE_DESKTOP 0x008080
#define WCE_BAR     0xC0C0C0
#define WCE_HI      0xFFFFFF
#define WCE_SHD     0x808080
#define WCE_DARK    0x404040
#define WCE_TXT     0x000000
#define WCE_MBKG    0xD4D0C8
#define WCE_BATT_OK  0x008000   // battery glyph fill, >20%
#define WCE_BATT_LOW 0xC00000   // battery glyph fill, <=20%

#define TASKBAR_H   22

static void wce_draw_raised(const mw_gl_draw_info_t *d,
                             int16_t x, int16_t y, int16_t w, int16_t h, uint32_t fill) {
    mw_gl_set_fill(MW_GL_FILL); mw_gl_set_border(MW_GL_BORDER_OFF);
    mw_gl_set_solid_fill_colour(fill); mw_gl_rectangle(d, x, y, w, h);
    mw_gl_set_fg_colour(WCE_HI);
    mw_gl_hline(d, x, x+w-1, y); mw_gl_vline(d, x, y, y+h-1);
    mw_gl_set_fg_colour(WCE_SHD);
    mw_gl_hline(d, x+1, x+w-2, y+h-2); mw_gl_vline(d, x+w-2, y+1, y+h-2);
    mw_gl_set_fg_colour(WCE_DARK);
    mw_gl_hline(d, x, x+w-1, y+h-1); mw_gl_vline(d, x+w-1, y, y+h-1);
}

static void wce_draw_sunken(const mw_gl_draw_info_t *d,
                             int16_t x, int16_t y, int16_t w, int16_t h, uint32_t fill) {
    mw_gl_set_fill(MW_GL_FILL); mw_gl_set_border(MW_GL_BORDER_OFF);
    mw_gl_set_solid_fill_colour(fill); mw_gl_rectangle(d, x, y, w, h);
    mw_gl_set_fg_colour(WCE_DARK);
    mw_gl_hline(d, x, x+w-1, y); mw_gl_vline(d, x, y, y+h-1);
    mw_gl_set_fg_colour(WCE_SHD);
    mw_gl_hline(d, x+1, x+w-2, y+1); mw_gl_vline(d, x+1, y+1, y+h-2);
    mw_gl_set_fg_colour(WCE_HI);
    mw_gl_hline(d, x, x+w-1, y+h-1); mw_gl_vline(d, x+w-1, y, y+h-1);
}

// ── Taskbar registry (ported from wince_taskbar.cpp) ────────────────────────

#define TASKBAR_MAX_ENTRIES 8

typedef struct { mw_handle_t handle; char name[12]; } wce_taskbar_entry_t;

static wce_taskbar_entry_t s_taskbar_entries[TASKBAR_MAX_ENTRIES];
static int         s_taskbar_count = 0;
static mw_handle_t s_taskbar_focused = MW_INVALID_HANDLE;

void wce_taskbar_register(mw_handle_t handle, const char *name) {
    if (s_taskbar_count >= TASKBAR_MAX_ENTRIES) return;
    s_taskbar_entries[s_taskbar_count].handle = handle;
    strncpy(s_taskbar_entries[s_taskbar_count].name, name ? name : "",
            sizeof(s_taskbar_entries[0].name) - 1);
    s_taskbar_entries[s_taskbar_count].name[sizeof(s_taskbar_entries[0].name) - 1] = '\0';
    s_taskbar_count++;
    s_taskbar_focused = handle;
}

void wce_taskbar_unregister(mw_handle_t handle) {
    if (s_taskbar_focused == handle) s_taskbar_focused = MW_INVALID_HANDLE;
    for (int i = 0; i < s_taskbar_count; i++) {
        if (s_taskbar_entries[i].handle == handle) {
            for (int j = i; j < s_taskbar_count - 1; j++)
                s_taskbar_entries[j] = s_taskbar_entries[j + 1];
            s_taskbar_count--;
            return;
        }
    }
}

// ── Geometry ──────────────────────────────────────────────────────────────

#define SCR_W       mw_hal_lcd_get_display_width()
#define SCR_H       mw_hal_lcd_get_display_height()
#define TASKBAR_Y   (SCR_H - TASKBAR_H)
#define START_X     2
#define START_Y     (TASKBAR_Y + 2)
#define START_W     52
#define START_H     (TASKBAR_H - 4)
#define SMENU_W     130
#define SMENU_IH    18
#define SMENU_SEP_H 8
#define SMENU_X     0
#define SMENU_TL_H  (3 * SMENU_IH + SMENU_SEP_H + 4)
#define SMENU_MAX_PROGRAMS 10
#define SMENU_MAX_NOTIFS   8

// Programs/Notifications folders cap each page to SMENU_PAGE_SIZE items
// with a trailing "More (x/y)" row that cycles pages (wraps back to page 0
// past the last) — keeps the submenu box from growing past the taskbar/off
// the top of the screen as either list grows, instead of an unbounded-
// height list (SMENU_MAX_PROGRAMS/SMENU_MAX_NOTIFS above already cap the
// underlying counts, but at 10/8 items that's still taller than fits well
// on most of this codebase's smaller displays).
#define SMENU_PAGE_SIZE 7

typedef struct {
    int  page_start;
    int  page_items;   // items shown on this page (<= SMENU_PAGE_SIZE)
    int  page_count;   // total pages (>= 1)
    bool has_more;      // page_count > 1 — draws/handles the "More" row
    int  max_items;     // 1 (Back) + page_items + (has_more ? 1 : 0)
} smenu_page_t;

// Clamps *page into range as a side effect (list can shrink under a stale
// page index — same defensive clamp s_node_idx uses elsewhere in this
// codebase for a shrinking table).
static smenu_page_t smenu_paginate(int total, int *page) {
    smenu_page_t p;
    p.page_count = (total + SMENU_PAGE_SIZE - 1) / SMENU_PAGE_SIZE;
    if (p.page_count < 1) p.page_count = 1;
    if (*page >= p.page_count) *page = 0;
    p.page_start = *page * SMENU_PAGE_SIZE;
    p.page_items = total - p.page_start;
    if (p.page_items > SMENU_PAGE_SIZE) p.page_items = SMENU_PAGE_SIZE;
    if (p.page_items < 0) p.page_items = 0;
    p.has_more  = p.page_count > 1;
    p.max_items = 1 + p.page_items + (p.has_more ? 1 : 0);
    return p;
}

static int programs_count(void) {
    int n = app_manager_count();
    if (n > SMENU_MAX_PROGRAMS) n = SMENU_MAX_PROGRAMS;
    return n;
}

static int notifs_count(void) {
    int n = purr_kernel_notify_count();
    if (n > SMENU_MAX_NOTIFS) n = SMENU_MAX_NOTIFS;
    return n;
}

static void draw_sel(const mw_gl_draw_info_t *d, int16_t x, int16_t y, int16_t w, int16_t h,
                      bool selected, bool pressed) {
    if (selected && pressed) {
        mw_gl_set_solid_fill_colour(0xFFFFFF);
        mw_gl_rectangle(d, x, y, w, h);
        mw_gl_set_fg_colour(0x000080);
    } else if (selected) {
        mw_gl_set_solid_fill_colour(0x000080);
        mw_gl_rectangle(d, x, y, w, h);
        mw_gl_set_fg_colour(0xFFFFFF);
    } else {
        mw_gl_set_fg_colour(WCE_TXT);
    }
}

static void draw_smenu_box(const mw_gl_draw_info_t *d, int16_t smy, int16_t smh) {
    mw_gl_set_fill(MW_GL_FILL); mw_gl_set_border(MW_GL_BORDER_OFF);
    mw_gl_set_solid_fill_colour(WCE_MBKG);
    mw_gl_rectangle(d, SMENU_X, smy, SMENU_W, smh);
    mw_gl_set_fg_colour(WCE_DARK);
    mw_gl_set_fill(MW_GL_NO_FILL); mw_gl_set_border(MW_GL_BORDER_ON);
    mw_gl_rectangle(d, SMENU_X, smy, SMENU_W, smh);
    mw_gl_set_fill(MW_GL_FILL); mw_gl_set_border(MW_GL_BORDER_OFF);
    mw_gl_set_fg_colour(WCE_TXT);
    mw_gl_set_bg_transparency(MW_GL_BG_TRANSPARENT);
    mw_gl_set_font(MW_GL_FONT_9);
}

// ── Wallpaper — MiniWin's own root window, NOT a regular mw_add_window() ────
//
// Was a separate z=1 window here, sized full-screen like everything else.
// Real bug, confirmed live: MiniWin/miniwin.c's own process_touch_message()
// gives ANY non-root window focus + brings it to front the instant a touch
// lands on it that it doesn't already have focus (check_and_process_touch_
// on_window_without_focus()) — and since a newly-focused window that "was
// overlapped" gets its own client area repainted right there, a background
// tap brought this full-screen window to front and repainted it straight
// over the taskbar and any open app window. Confirmed live as "touching
// the background hides the taskbar and windows."
//
// MiniWin's engine already has a purpose-built exemption from exactly this:
// the ROOT window (MW_ROOT_WINDOW_ID/MW_ROOT_WINDOW_HANDLE, created inside
// mw_init() itself, painted/messaged via the mw_user_root_paint_function()/
// mw_user_root_message_function() hooks below) is explicitly excluded from
// the focus-steal path (process_touch_message()'s own "unless it's the root
// window" check) — touches on it just post straight to mw_user_root_
// message_function(), no front-bringing, no side-effect repaint. Using it
// instead of a second, redundant full-screen window is the actual fix, not
// a workaround — see those two functions at the bottom of this file, which
// now carry the real desktop-background paint/no-op instead of being dead
// stubs (the icon-grid MiniWin desktop style always used the real root
// window this way; this file just never had before).

// ── Window 1: Taskbar + Start Menu (z=1, lowest real window) ────────────────
//
// Resizes itself live: rect is just the bottom strip (0,TASKBAR_Y,SCR_W,
// TASKBAR_H) when the Start Menu is closed, and grows upward to
// (0,TASKBAR_Y-menu_h,SCR_W,TASKBAR_H+menu_h) the instant it opens — see
// resize_taskbar_window(). Because MiniWin delivers both paint coordinates
// (via draw_info's origin) and touch coordinates (client_x/client_y,
// computed in process_touch_message()) already translated to be relative
// to a window's OWN current top-left, every Y coordinate in this window's
// drawing/hit-testing has to be expressed relative to its current origin
// rather than the screen — see current_menu_height()'s comment for exactly
// how that translation works out.

static mw_handle_t s_taskbar_handle  = MW_INVALID_HANDLE;
static bool s_smenu_open    = false;
static int  s_smenu_folder  = -1;  // -1 = top level, 0 = programs, 1 = notifications
static int  s_smenu_sel     = 0;
static int  s_smenu_page    = 0;   // current page within whichever folder is open
static bool s_smenu_pressed = false;

// Taskbar corner rotates between a real wall clock and a battery-level
// glyph every STATUS_ROTATE_TICKS repaints (5s) — flipped in miniwin_
// module.c's task loop (which already repaints this exact rect once a
// second), read here to decide what to draw.
static bool s_status_show_battery = false;

mw_handle_t wce_taskbar_handle(void) { return s_taskbar_handle; }

void wce_desktop_toggle_status(void) { s_status_show_battery = !s_status_show_battery; }

int16_t wce_taskbar_height(void) { return TASKBAR_H; }

// Height (px) the Start Menu currently needs above the taskbar strip — 0
// when closed. This is both (a) how much the taskbar+menu window grows by
// on open, and (b) — since the window's origin is defined to always sit
// exactly at this many pixels above TASKBAR_Y — the fixed offset that
// turns every old screen-absolute Y coordinate in this file's drawing/
// touch code into the window-local one MiniWin now expects:
//   - taskbar-strip elements (always screen-Y TASKBAR_Y..SCR_H-1) become
//     local Y = menu_h + (that fixed small offset from TASKBAR_Y), i.e.
//     just add menu_h to what used to be a TASKBAR_Y-relative offset.
//   - Start Menu box elements (screen-Y smy..smy+smh, where smy = TASKBAR_Y
//     - menu_h by construction) become local Y = (offset from smy) — i.e.
//     the smy term simply drops out, since local smy is always 0.
static int16_t current_menu_height(void) {
    if (!s_smenu_open) return 0;
    if (s_smenu_folder < 0) return SMENU_TL_H;
    int total = s_smenu_folder == 0 ? programs_count() : notifs_count();
    smenu_page_t pg = smenu_paginate(total, &s_smenu_page);
    return (int16_t)(pg.max_items * SMENU_IH + 4);
}

void wce_status_rect(mw_util_rect_t *out) {
    int16_t menu_h = current_menu_height();
    mw_util_set_rect(out, (int16_t)(SCR_W - 50), (int16_t)(menu_h + 2), 48, TASKBAR_H - 4);
}

void wce_redraw_layers(void) {
    mw_paint_window_client(MW_ROOT_WINDOW_HANDLE);
    mw_paint_window_client(s_taskbar_handle);
    // Belt and suspenders: the handle-targeted repaints above rely on
    // MiniWin's per-window occlusion computation (do_paint_window_client())
    // correctly figuring out "what's really on top of me right now" for a
    // window that ISN'T currently focused/frontmost (icons and wallpaper
    // essentially never are) — confirmed live that alone isn't reliably
    // clearing everything a just-closed/just-unlocked window left behind.
    // mw_paint_all() takes a completely different path (walks z-order low
    // to high, each window's paint straightforwardly overwriting the last
    // rather than pre-computing "don't touch this region"), so it catches
    // whatever the targeted calls above miss — and, separately, is the
    // ONLY safe call here whenever an app might be open behind these
    // layers (see this file's top comment on the focused-window occlusion-
    // bypass shortcut). Unconditional, not gated on anything.
    mw_paint_all();
}

// Last current_menu_height() this actually ran with — -1 forces the first
// call to always treat it as a real change. See resize_taskbar_window()'s
// own use of this: most calls are a highlight move within the SAME menu
// level (arrow keys/trackball), where the popup's own height — and so the
// root window's visible area — never actually changes.
static int16_t s_last_menu_h = -1;

// Resizes/repositions the taskbar+menu window to exactly match its current
// visual footprint and repaints it. Call after anything that could change
// current_menu_height()'s result (smenu open/close/folder/page/selection).
static void resize_taskbar_window(void) {
    int16_t menu_h = current_menu_height();
    int16_t new_h  = (int16_t)(TASKBAR_H + menu_h);
    int16_t new_y  = (int16_t)(SCR_H - new_h);
    mw_reposition_window(s_taskbar_handle, 0, new_y);
    mw_resize_window(s_taskbar_handle, (int16_t)SCR_W, new_h);

    // Root window repaint ONLY when the popup's own height actually
    // changed (open/close/folder/page — the taskbar window's rect grew,
    // shrank, or moved, vacating or newly covering part of the root
    // background) — NOT on every call. Real bug, confirmed live: this
    // used to fire unconditionally, so a bare highlight move (trackball/
    // arrow keys scrolling within the SAME menu level, no resize at all)
    // still repainted the entire full-screen root window every single
    // step — a visible whole-screen flash on top of the taskbar's own
    // correct redraw, reported as "the menu re-draws 2 times per
    // animation." A same-height call only ever needs the taskbar's own
    // client repainted (the highlight lives entirely inside its rect).
    if (menu_h != s_last_menu_h) {
        mw_paint_window_client(MW_ROOT_WINDOW_HANDLE);
    }
    s_last_menu_h = menu_h;
    mw_paint_window_client(s_taskbar_handle);
}

// Battery glyph: outline + nub + a fill proportional to charge, drawn with
// plain mw_gl_* primitives — same "hand-drawn chrome" idiom every bevel in
// this file already uses (wce_draw_raised/_sunken above), rather than a
// new bitmap asset. The existing wce_icon_*/mw_bitmaps_* bitmaps are all
// fixed 1bpp glyphs, not a good fit for a level indicator that needs to
// actually vary. (x,y) is the glyph's own top-left, sized to fit inside
// the taskbar corner's existing 48x(TASKBAR_H-4) sunken box.
static void draw_battery_glyph(const mw_gl_draw_info_t *d, int16_t x, int16_t y) {
    const int16_t body_w = 22, body_h = 12, nub_w = 2, nub_h = 6;

    mw_gl_set_fill(MW_GL_NO_FILL); mw_gl_set_border(MW_GL_BORDER_ON);
    mw_gl_set_fg_colour(WCE_TXT);
    mw_gl_rectangle(d, x, y, body_w, body_h);
    mw_gl_set_fill(MW_GL_FILL); mw_gl_set_border(MW_GL_BORDER_OFF);
    mw_gl_set_solid_fill_colour(WCE_TXT);
    mw_gl_rectangle(d, (int16_t)(x + body_w), (int16_t)(y + (body_h - nub_h) / 2), nub_w, nub_h);

    int pct = purr_kernel_battery_percent();
    if (pct < 0) return;   // unknown/no battery — outline only, honest rather than a guessed fill
    if (pct > 100) pct = 100;
    int16_t inner_w = (int16_t)(body_w - 4);
    int16_t fill_w  = (int16_t)((inner_w * pct) / 100);
    if (fill_w <= 0) return;
    mw_gl_set_solid_fill_colour(pct <= 20 ? WCE_BATT_LOW : WCE_BATT_OK);
    mw_gl_rectangle(d, (int16_t)(x + 2), (int16_t)(y + 2), fill_w, (int16_t)(body_h - 4));
}

static void taskbar_paint(mw_handle_t handle, const mw_gl_draw_info_t *d) {
    (void)handle;
    int16_t menu_h = current_menu_height();

    // Self-sufficiency: this window doesn't rely on the wallpaper window's
    // own repaint showing through beneath it — when the Start Menu is open
    // this window's rect grows upward past
    // the taskbar bar itself, and the area beside the (narrower, SMENU_W-
    // wide) menu popup needs to show plain desktop — draw_smenu_box()
    // only fills its own 130px-wide box, never the rest of this now-taller
    // window's width, so without this fill that leftover space just shows
    // whatever was underneath before this window grew to cover it.
    if (menu_h > 0) {
        mw_gl_set_fill(MW_GL_FILL); mw_gl_set_border(MW_GL_BORDER_OFF);
        mw_gl_set_solid_fill_colour(WCE_DESKTOP);
        mw_gl_rectangle(d, 0, 0, (int16_t)SCR_W, menu_h);
    }

    mw_gl_set_fill(MW_GL_FILL); mw_gl_set_border(MW_GL_BORDER_OFF);
    mw_gl_set_solid_fill_colour(WCE_BAR);
    mw_gl_rectangle(d, 0, menu_h, (int16_t)SCR_W, TASKBAR_H);
    mw_gl_set_fg_colour(WCE_HI);
    mw_gl_hline(d, 0, (int16_t)(SCR_W - 1), menu_h);
    mw_gl_set_fg_colour(WCE_DARK);
    mw_gl_hline(d, 0, (int16_t)(SCR_W - 1), (int16_t)(menu_h + TASKBAR_H - 1));

    int16_t local_start_y = (int16_t)(menu_h + 2);
    if (s_smenu_open) wce_draw_sunken(d, START_X, local_start_y, START_W, START_H, WCE_BAR);
    else              wce_draw_raised(d, START_X, local_start_y, START_W, START_H, WCE_BAR);
    mw_gl_set_fg_colour(WCE_TXT);
    mw_gl_set_bg_transparency(MW_GL_BG_TRANSPARENT);
    mw_gl_set_font(MW_GL_FONT_9);
    mw_gl_string(d, START_X + 6, (int16_t)(local_start_y + 5), "Meow!");

    mw_gl_set_fg_colour(WCE_SHD);
    mw_gl_vline(d, START_X + START_W + 2, local_start_y, (int16_t)(menu_h + TASKBAR_H - 3));
    mw_gl_set_fg_colour(WCE_HI);
    mw_gl_vline(d, START_X + START_W + 3, local_start_y, (int16_t)(menu_h + TASKBAR_H - 3));

    {
        int16_t area_x = (int16_t)(START_X + START_W + 6);
        int16_t area_w = (int16_t)((SCR_W - 52) - area_x);
        int n = s_taskbar_count;
        if (n > 0 && area_w >= 22) {
            int16_t pitch = (int16_t)(area_w / n);
            int16_t bw = (int16_t)(pitch - 2);
            mw_gl_set_bg_transparency(MW_GL_BG_TRANSPARENT);
            mw_gl_set_font(MW_GL_FONT_9);
            for (int i = 0; i < n; i++) {
                int16_t bx = (int16_t)(area_x + i * pitch);
                mw_handle_t eh = s_taskbar_entries[i].handle;
                bool focused = (eh == s_taskbar_focused) &&
                               !(mw_get_window_flags(eh) & MW_WINDOW_FLAG_IS_MINIMISED);
                if (focused) wce_draw_sunken(d, bx, local_start_y, bw, START_H, WCE_BAR);
                else         wce_draw_raised(d, bx, local_start_y, bw, START_H, WCE_BAR);
                mw_gl_set_fg_colour(WCE_TXT);
                mw_gl_string(d, (int16_t)(bx + 3), (int16_t)(local_start_y + 5), s_taskbar_entries[i].name);
            }
        }
    }

    int16_t bx = SCR_W - 50;
    wce_draw_sunken(d, bx, (int16_t)(menu_h + 2), 48, TASKBAR_H - 4, WCE_BAR);
    if (s_status_show_battery) {
        draw_battery_glyph(d, (int16_t)(bx + 13), (int16_t)(local_start_y + 2));
    } else {
        // UTC, not local time: this codebase has no timezone handling
        // (purr_kernel_time_from_utc_calendar()'s own doc comment: "no
        // libc TZ/mktime dependence") — same reasoning systemui_xp.c's
        // own refresh_clock() already documents for its tray clock.
        char clk[16];
        if (purr_kernel_time_is_synced()) {
            time_t now = purr_kernel_time_now();
            struct tm tm_buf;
            gmtime_r(&now, &tm_buf);
            snprintf(clk, sizeof(clk), "%02d:%02d", tm_buf.tm_hour, tm_buf.tm_min);
        } else {
            // Shouldn't happen in practice (purr_kernel's own NVS fallback
            // seeds before any module runs) — fails toward an honest
            // uptime readout rather than "00:00".
            uint64_t s = purr_kernel_uptime_ms() / 1000ULL;
            snprintf(clk, sizeof(clk), "^%u:%02u",
                     (unsigned)(s / 3600ULL), (unsigned)((s / 60ULL) % 60ULL));
        }
        mw_gl_set_fg_colour(WCE_TXT);
        mw_gl_set_bg_transparency(MW_GL_BG_TRANSPARENT);
        mw_gl_set_font(MW_GL_FONT_9);
        mw_gl_string(d, bx + 4, (int16_t)(local_start_y + 5), clk);
    }

    if (!s_smenu_open) return;

    // Start Menu box — local smy is always 0 (the window's own origin is
    // defined to sit exactly at the menu's screen-absolute top edge), so
    // every "smy" term from the pre-split version simply drops out below.
    if (s_smenu_folder < 0) {
        draw_smenu_box(d, 0, SMENU_TL_H);
        draw_sel(d, SMENU_X + 1, 2, SMENU_W - 2, SMENU_IH, s_smenu_sel == 0, s_smenu_pressed);
        mw_gl_string(d, SMENU_X + 8, 2 + 4, "Programs >");
        draw_sel(d, SMENU_X + 1, 2 + SMENU_IH, SMENU_W - 2, SMENU_IH,
                  s_smenu_sel == 1, s_smenu_pressed);
        {
            int nn = purr_kernel_notify_count();
            char nlbl[32];
            if (nn > 0) snprintf(nlbl, sizeof(nlbl), "Notifications (%d) >", nn);
            else        snprintf(nlbl, sizeof(nlbl), "Notifications >");
            mw_gl_string(d, SMENU_X + 8, (int16_t)(2 + SMENU_IH + 4), nlbl);
        }
        mw_gl_set_fg_colour(WCE_SHD);
        mw_gl_hline(d, SMENU_X + 4, SMENU_X + SMENU_W - 4,
                    (int16_t)(2 + 2 * SMENU_IH + SMENU_SEP_H / 2));
        draw_sel(d, SMENU_X + 1, 2 + 2 * SMENU_IH + SMENU_SEP_H, SMENU_W - 2, SMENU_IH,
                  s_smenu_sel == 2, s_smenu_pressed);
        mw_gl_string(d, SMENU_X + 8,
                     (int16_t)(2 + 2 * SMENU_IH + SMENU_SEP_H + 4), "Restart");
    } else if (s_smenu_folder == 0) {
        smenu_page_t pg = smenu_paginate(programs_count(), &s_smenu_page);
        int16_t smh = (int16_t)(pg.max_items * SMENU_IH + 4);
        draw_smenu_box(d, 0, smh);
        draw_sel(d, SMENU_X + 1, 2, SMENU_W - 2, SMENU_IH, s_smenu_sel == 0, s_smenu_pressed);
        mw_gl_string(d, SMENU_X + 8, 2 + 4, "< Back");
        for (int i = 0; i < pg.page_items; i++) {
            const app_entry_t *app = app_manager_get(pg.page_start + i);
            int16_t iy = (int16_t)(2 + (i + 1) * SMENU_IH);
            draw_sel(d, SMENU_X + 1, iy, SMENU_W - 2, SMENU_IH, s_smenu_sel == i + 1, s_smenu_pressed);
            mw_gl_string(d, SMENU_X + 8, iy + 4, app ? app->name : "?");
        }
        if (pg.has_more) {
            int16_t iy = (int16_t)(2 + (pg.page_items + 1) * SMENU_IH);
            draw_sel(d, SMENU_X + 1, iy, SMENU_W - 2, SMENU_IH, s_smenu_sel == pg.page_items + 1, s_smenu_pressed);
            char more_lbl[32];
            snprintf(more_lbl, sizeof(more_lbl), "More (%d/%d)", s_smenu_page + 1, pg.page_count);
            mw_gl_string(d, SMENU_X + 8, iy + 4, more_lbl);
        }
    } else {
        // Notifications — see purr_kernel_notify() in purr_kernel.h. Title
        // only (SMENU_W is narrow); "< Back" doubles as "Clear all" via the
        // handler when there's at least one notification, same one-item
        // economy the rest of this menu already uses.
        int total = notifs_count();
        smenu_page_t pg = smenu_paginate(total, &s_smenu_page);
        int16_t smh = (int16_t)(pg.max_items * SMENU_IH + 4);
        draw_smenu_box(d, 0, smh);
        draw_sel(d, SMENU_X + 1, 2, SMENU_W - 2, SMENU_IH, s_smenu_sel == 0, s_smenu_pressed);
        mw_gl_string(d, SMENU_X + 8, 2 + 4, total > 0 ? "< Back (clear all)" : "< Back");
        for (int i = 0; i < pg.page_items; i++) {
            purr_notification_t note;
            int16_t iy = (int16_t)(2 + (i + 1) * SMENU_IH);
            draw_sel(d, SMENU_X + 1, iy, SMENU_W - 2, SMENU_IH, s_smenu_sel == i + 1, s_smenu_pressed);
            mw_gl_string(d, SMENU_X + 8, iy + 4, purr_kernel_notify_at(pg.page_start + i, &note) ? note.title : "?");
        }
        if (pg.has_more) {
            int16_t iy = (int16_t)(2 + (pg.page_items + 1) * SMENU_IH);
            draw_sel(d, SMENU_X + 1, iy, SMENU_W - 2, SMENU_IH, s_smenu_sel == pg.page_items + 1, s_smenu_pressed);
            char more_lbl[32];
            snprintf(more_lbl, sizeof(more_lbl), "More (%d/%d)", s_smenu_page + 1, pg.page_count);
            mw_gl_string(d, SMENU_X + 8, iy + 4, more_lbl);
        }
    }
}

static void taskbar_message(const mw_message_t *msg) {
    if (msg->message_id == MW_KEY_PRESSED_MESSAGE) {
        uint8_t code = (uint8_t)msg->message_data;
        if (!s_smenu_open && s_taskbar_focused != MW_INVALID_HANDLE) {
            mw_post_message(MW_KEY_PRESSED_MESSAGE,
                            MW_INVALID_HANDLE, s_taskbar_focused,
                            (uint32_t)code, NULL, MW_WINDOW_MESSAGE);
            return;
        }

        // Every smenu-affecting keypress just resizes (a no-op resize if
        // current_menu_height() didn't actually change) and repaints —
        // now that this window is small instead of the whole screen, a
        // full repaint of it is cheap, so there's no need for a separate
        // scoped-vs-full repaint split.
        bool changed = false;

        if (!s_smenu_open) {
            if (code == 0x0D) { s_smenu_open = true; s_smenu_sel = 0; changed = true; }
        } else if (s_smenu_folder < 0) {
            int max_items = 3;
            int prev_sel = s_smenu_sel;
            if (code == 0x01 || code == 0x03) s_smenu_sel = (s_smenu_sel - 1 + max_items) % max_items;
            if (code == 0x02 || code == 0x04) s_smenu_sel = (s_smenu_sel + 1) % max_items;
            if (s_smenu_sel != prev_sel) changed = true;
            if (code == 0x0D) {
                s_smenu_pressed = true;
                mw_paint_window_client(s_taskbar_handle);
                s_smenu_pressed = false;
                if (s_smenu_sel == 0)      { s_smenu_folder = 0; s_smenu_sel = 0; s_smenu_page = 0; }
                else if (s_smenu_sel == 1) { s_smenu_folder = 1; s_smenu_sel = 0; s_smenu_page = 0; }
                else { s_smenu_open = false; s_smenu_folder = -1; purr_kernel_reboot(); }
                changed = true;
            }
            if (code == 0x1B || code == 0x03) { s_smenu_open = false; s_smenu_folder = -1; changed = true; }
        } else if (s_smenu_folder == 0) {
            int count = programs_count();
            smenu_page_t pg = smenu_paginate(count, &s_smenu_page);
            int prev_sel = s_smenu_sel;
            if (code == 0x01) s_smenu_sel = (s_smenu_sel - 1 + pg.max_items) % pg.max_items;
            if (code == 0x02) s_smenu_sel = (s_smenu_sel + 1) % pg.max_items;
            if (s_smenu_sel != prev_sel) changed = true;
            if (code == 0x0D) {
                s_smenu_pressed = true;
                mw_paint_window_client(s_taskbar_handle);
                s_smenu_pressed = false;
                if (s_smenu_sel == 0) {
                    s_smenu_folder = -1; s_smenu_sel = 0; s_smenu_page = 0;
                } else if (pg.has_more && s_smenu_sel == pg.max_items - 1) {
                    s_smenu_page = (s_smenu_page + 1) % pg.page_count;
                    s_smenu_sel = 0;
                } else {
                    int idx = pg.page_start + (s_smenu_sel - 1);
                    s_smenu_open = false; s_smenu_folder = -1; s_smenu_page = 0;
                    if (idx >= 0 && idx < count) app_manager_launch_idx(idx);
                }
                changed = true;
            }
            if (code == 0x1B || code == 0x03) { s_smenu_folder = -1; s_smenu_sel = 0; s_smenu_page = 0; changed = true; }
        } else {
            // Notifications folder
            int count = notifs_count();
            smenu_page_t pg = smenu_paginate(count, &s_smenu_page);
            int prev_sel = s_smenu_sel;
            if (code == 0x01) s_smenu_sel = (s_smenu_sel - 1 + pg.max_items) % pg.max_items;
            if (code == 0x02) s_smenu_sel = (s_smenu_sel + 1) % pg.max_items;
            if (s_smenu_sel != prev_sel) changed = true;
            if (code == 0x0D) {
                s_smenu_pressed = true;
                mw_paint_window_client(s_taskbar_handle);
                s_smenu_pressed = false;
                if (s_smenu_sel == 0) {
                    if (count > 0) purr_kernel_notify_clear();
                    s_smenu_folder = -1; s_smenu_sel = 0; s_smenu_page = 0;
                } else if (pg.has_more && s_smenu_sel == pg.max_items - 1) {
                    s_smenu_page = (s_smenu_page + 1) % pg.page_count;
                    s_smenu_sel = 0;
                }
                // Tapping an individual notification does nothing further —
                // there's no per-notification detail view yet.
                changed = true;   // still need to repaint away the pressed flash
            }
            if (code == 0x1B || code == 0x03) { s_smenu_folder = -1; s_smenu_sel = 0; s_smenu_page = 0; changed = true; }
        }

        if (changed) resize_taskbar_window();
        return;
    }

    if (msg->message_id != MW_TOUCH_DOWN_MESSAGE) return;

    int16_t tx = (int16_t)(msg->message_data >> 16);
    int16_t ty = (int16_t)(msg->message_data & 0xFFFF);
    int16_t menu_h = current_menu_height();

    if (s_smenu_open) {
        // Touch never has a pure "move the highlight without acting" case
        // like keyboard arrow keys do — every tap here either navigates,
        // launches, or closes, so it always just resizes (see this window's
        // top comment) + repaints via resize_taskbar_window().
        if (s_smenu_folder < 0) {
            if (tx >= SMENU_X && tx < SMENU_X + SMENU_W && ty >= 0 && ty < SMENU_TL_H) {
                int rel_y = ty - 2;
                if (rel_y >= 0 && rel_y < SMENU_IH) {
                    s_smenu_folder = 0;
                    s_smenu_page = 0;
                } else if (rel_y >= SMENU_IH && rel_y < 2 * SMENU_IH) {
                    s_smenu_folder = 1;
                    s_smenu_page = 0;
                } else if (rel_y >= 2 * SMENU_IH + SMENU_SEP_H) {
                    s_smenu_open = false;
                    s_smenu_folder = -1;
                    purr_kernel_reboot();
                }
            } else {
                s_smenu_open = false;
                s_smenu_folder = -1;
            }
        } else if (s_smenu_folder == 0) {
            int count = programs_count();
            smenu_page_t pg = smenu_paginate(count, &s_smenu_page);
            int16_t smh = (int16_t)(pg.max_items * SMENU_IH + 4);
            if (tx >= SMENU_X && tx < SMENU_X + SMENU_W && ty >= 0 && ty < smh) {
                int item = (ty - 2) / SMENU_IH;
                if (item == 0) {
                    s_smenu_folder = -1;
                    s_smenu_page = 0;
                } else if (pg.has_more && item == pg.max_items - 1) {
                    s_smenu_page = (s_smenu_page + 1) % pg.page_count;
                } else {
                    int idx = pg.page_start + (item - 1);
                    s_smenu_open = false;
                    s_smenu_folder = -1;
                    s_smenu_page = 0;
                    if (idx >= 0 && idx < count) app_manager_launch_idx(idx);
                }
            } else {
                s_smenu_open = false;
                s_smenu_folder = -1;
            }
        } else {
            // Notifications folder — tapping "< Back" clears (if any exist);
            // tapping an individual entry just navigates back for now.
            int count = notifs_count();
            smenu_page_t pg = smenu_paginate(count, &s_smenu_page);
            int16_t smh = (int16_t)(pg.max_items * SMENU_IH + 4);
            if (tx >= SMENU_X && tx < SMENU_X + SMENU_W && ty >= 0 && ty < smh) {
                int item = (ty - 2) / SMENU_IH;
                if (item == 0) {
                    if (count > 0) purr_kernel_notify_clear();
                    s_smenu_folder = -1;
                    s_smenu_page = 0;
                } else if (pg.has_more && item == pg.max_items - 1) {
                    s_smenu_page = (s_smenu_page + 1) % pg.page_count;
                } else {
                    s_smenu_folder = -1;
                    s_smenu_page = 0;
                }
            } else {
                s_smenu_open = false;
                s_smenu_folder = -1;
            }
        }

        resize_taskbar_window();
        return;
    }

    if (ty >= menu_h && tx >= START_X && tx < START_X + START_W) {
        s_smenu_open = true;
        resize_taskbar_window();
        return;
    }

    {
        int n = s_taskbar_count;
        int16_t area_x = (int16_t)(START_X + START_W + 6);
        int16_t area_w = (int16_t)((SCR_W - 52) - area_x);
        if (n > 0 && ty >= menu_h && area_w >= 22 &&
            tx >= area_x && tx < (int16_t)(area_x + area_w)) {
            int16_t pitch = (int16_t)(area_w / n);
            int idx = (tx - area_x) / pitch;
            if (idx >= 0 && idx < n) {
                mw_handle_t h = s_taskbar_entries[idx].handle;
                bool is_min     = (mw_get_window_flags(h) & MW_WINDOW_FLAG_IS_MINIMISED) != 0;
                bool is_focused = (h == s_taskbar_focused) && !is_min;
                if (is_focused) {
                    mw_set_window_minimised(h, true);
                    s_taskbar_focused = MW_INVALID_HANDLE;
                } else {
                    if (is_min) mw_set_window_minimised(h, false);
                    s_taskbar_focused = h;
                    mw_bring_window_to_front(h);
                }
                resize_taskbar_window();
            }
        }
    }
}

// ── Window 2: Credential dialog (z=2, created invisible) ─────────────────────
//
// One shared "Log On to Windows"-style dialog for TWO distinct moments —
// boot-time login (WCE_CRED_LOGIN, new) and the idle-timeout lock
// (WCE_CRED_LOCK, s_cred_mode's original role) — rather than two near-
// identical windows. Both are the same shape: a small centered, non-
// resizable, title-barred dialog with User:/Password: fields and OK/
// Cancel over a plain backdrop, real user_mgr_verify() on OK. The only
// differences are which mode sets: whether the username field is editable
// (Login: yes, any local account; Lock: no, fixed to whoever's actually
// logged in — matches real Windows lock-workstation behaviour, which
// never lets you switch users from the lock dialog) and the window title
// (wm_set_window_title(), set on each transition into a mode below).
//
// Two windows, both created once at startup and toggled visible/invisible
// together (never recreated) — a full-screen teal backdrop (visual coverage
// only, matches the desktop's own WCE_DESKTOP colour) and the dialog on top
// of it. MW_WINDOW_FLAG_HAS_TITLE_BAR is what makes the dialog draggable —
// MiniWin's own touch handling already distinguishes a touch-down on a
// window's title bar from one on its client area and drags the window in
// response, no extra code needed here. MW_WINDOW_FLAG_IS_MODAL is what
// actually blocks input from reaching anything underneath — set/cleared
// dynamically via mw_set_window_modal() at each show/hide site below,
// NEVER baked into this window's own creation flags. A real, live bug
// found the hard way: MiniWin/miniwin.c's own mw_is_any_window_modal()
// checks only IS_MODAL + IS_USED, never IS_VISIBLE — a window created
// once at boot with IS_MODAL permanently set (even while hidden the
// whole time) makes mw_is_any_window_modal() return true forever, and
// mw_bring_window_to_front() silently refuses to front ANY non-modal
// window while that's true. Confirmed live as "none of the app windows
// open, only their taskbar button appears" — every app's own purr_win_
// show() -> mw_bring_window_to_front() call was being silently ignored
// from the moment this dialog was first created at boot, regardless of
// whether it was ever actually shown.
//
// Real character entry — ported from miniwin_win.c's own MW_KEY_PRESSED_
// MESSAGE handling for purr_win_textarea (backspace/delete, printable-
// char append, Enter-as-submit against a raw buffer): see
// wce_credential_dialog_handle_key() below, called both from this
// window's own message handler (MW_KEY_PRESSED_MESSAGE, the boot-login
// case — nothing is locked then, so miniwin_keyboard.c's plain find-
// focused-window routing already delivers keys here for free) and
// directly from miniwin_keyboard.c while actually locked (bypassing
// miniwin_lock_handle_key()'s legacy Space→Enter-only dismiss path, which
// never delivered a real keystroke anywhere — see that file's own
// comment). Insert-at-end/backspace-from-end only, no click-to-position
// cursor, matching the ported pattern exactly.
//
// A third "Status" button opens a separate, also-draggable, on-demand
// window with uptime/battery/node count/unread message count — counts
// only, never message content or node identities, since this can still be
// a lock screen.

// Default-size buttons (50x15, not MW_CONTROL_FLAG_LARGE_SIZE's 100x30) —
// the large size was originally picked for tap comfort on a purely
// decorative dialog, but confirmed too visually oversized once real
// fields + tap-to-focus zones shared the same 220px-wide dialog. OK/
// Cancel share a row, Status gets its own row below (see the button
// creation calls in mw_user_init() for exact positions).
#define DIALOG_W  220
#define DIALOG_H  190
#define STATUS_W  190
#define STATUS_H  140

static mw_handle_t s_lock_backdrop_handle = MW_INVALID_HANDLE;
static mw_handle_t s_lock_dialog_handle   = MW_INVALID_HANDLE;
static mw_handle_t s_status_handle        = MW_INVALID_HANDLE;
static mw_handle_t s_btn_ok, s_btn_cancel, s_btn_status, s_btn_close;

mw_handle_t wce_lock_handle(void) { return s_lock_dialog_handle; }

// ── Credential state ────────────────────────────────────────────────────

typedef enum { WCE_CRED_NONE, WCE_CRED_LOGIN, WCE_CRED_LOCK } wce_cred_mode_t;

#define CRED_USER_MAX USER_MGR_USERNAME_MAX
#define CRED_PASS_MAX 64

static wce_cred_mode_t s_cred_mode         = WCE_CRED_NONE;
static char            s_cred_user[CRED_USER_MAX] = "";
static char            s_cred_pass[CRED_PASS_MAX] = "";
static bool            s_cred_user_editable = false;
static int             s_cred_field         = 1;   // 0 = username, 1 = password
static char            s_cred_error[32]     = "";

// Repaints the dialog if it's actually the one currently showing — every
// mutation below (typing, tap-to-focus, a failed verify) needs this same
// guard, since wce_credential_dialog_handle_key() can be called even when
// nothing is showing (see its own doc comment).
static void cred_dialog_repaint(void) {
    if (s_cred_mode == WCE_CRED_NONE) return;
    mw_paint_window_client(s_lock_dialog_handle);
}

static void cred_verify_and_proceed(void) {
    bool ok = user_mgr_verify(s_cred_user, s_cred_pass);
    s_cred_pass[0] = '\0';
    if (!ok) {
        ESP_LOGW(TAG, "credential dialog: verify failed for '%s' (mode=%d)", s_cred_user, (int)s_cred_mode);
        snprintf(s_cred_error, sizeof(s_cred_error), "Incorrect password");
        cred_dialog_repaint();
        return;
    }
    ESP_LOGI(TAG, "credential dialog: verify OK for '%s' (mode=%d)", s_cred_user, (int)s_cred_mode);
    s_cred_error[0] = '\0';
    if (s_cred_mode == WCE_CRED_LOGIN) {
        user_mgr_set_logged_in(s_cred_user);
        // app_manager.h's own "Local unlock gate": app_manager_count()/
        // get() report the LOCAL registry as empty until this is called —
        // real idleness, not a scan that already ran silently underneath
        // the login screen. Without it the Start Menu's Programs list (and
        // the icon-grid desktop's own icons) stays permanently empty even
        // though app_manager's own scan already found every app — this
        // was the "0 apps found" bug, confirmed live. systemui_login.c
        // already calls this on its own local-login success path; MiniWin
        // had never been wired to the new gate at all.
        app_manager_notify_unlocked();
        s_cred_mode = WCE_CRED_NONE;
        mw_set_window_modal(s_lock_dialog_handle, false);
        mw_set_window_visible(s_lock_dialog_handle, false);
        mw_set_window_visible(s_lock_backdrop_handle, false);
        wce_redraw_layers();
    } else if (s_cred_mode == WCE_CRED_LOCK) {
        // Fires on_lock_transition(false) below, which hides both windows
        // and redraws the layers underneath — same as the login branch
        // above, just via the shared lock state machine since a real
        // idle-timeout re-lock still needs to work the normal way too.
        miniwin_lock_force_unlock();
    }
}

// ── Status popup ──────────────────────────────────────────────────────────

static void hide_status_window(void) {
    if (s_status_handle == MW_INVALID_HANDLE) return;
    mw_set_window_visible(s_status_handle, false);
    wce_redraw_layers();
}

static void status_paint(mw_handle_t handle, const mw_gl_draw_info_t *d) {
    mw_util_rect_t client = mw_get_window_client_rect(handle);
    mw_gl_set_fill(MW_GL_FILL); mw_gl_set_border(MW_GL_BORDER_OFF);
    mw_gl_set_solid_fill_colour(WCE_BAR);
    mw_gl_rectangle(d, 0, 0, client.width, client.height);

    mw_gl_set_font(MW_GL_FONT_9);
    mw_gl_set_bg_transparency(MW_GL_BG_TRANSPARENT);
    mw_gl_set_fg_colour(WCE_TXT);

    char buf[48];
    uint64_t up_s  = purr_kernel_uptime_ms() / 1000ULL;
    unsigned up_hh = (unsigned)(up_s / 3600ULL);
    unsigned up_mm = (unsigned)((up_s / 60ULL) % 60ULL);
    if (up_hh > 0) snprintf(buf, sizeof(buf), "Uptime:  %uh %um", up_hh, up_mm);
    else           snprintf(buf, sizeof(buf), "Uptime:  %um", up_mm);
    mw_gl_string(d, 8, 8, buf);

    int mv  = purr_kernel_battery_voltage_mv();
    int pct = purr_kernel_battery_percent();
    if (mv >= 0) snprintf(buf, sizeof(buf), "Battery: %d.%02uV  %d%%", mv / 1000, (unsigned)((mv % 1000) / 10), pct);
    else         snprintf(buf, sizeof(buf), "Battery: --");
    mw_gl_string(d, 8, 24, buf);

    int nodes = 0;
#ifdef CONFIG_PURR_FEATURE_MESHTASTIC
    nodes = mesh_manager_node_count();
#endif
    snprintf(buf, sizeof(buf), "Nodes:   %d", nodes);
    mw_gl_string(d, 8, 40, buf);

    int unread = purr_kernel_notify_count();
    snprintf(buf, sizeof(buf), "Unread:  %d message%s", unread, unread == 1 ? "" : "s");
    mw_gl_string(d, 8, 56, buf);
}

static void status_message(const mw_message_t *msg) {
    if (msg->message_id == MW_BUTTON_PRESSED_MESSAGE && msg->sender_handle == s_btn_close) {
        hide_status_window();
    }
}

static void show_status_window(void) {
    if (s_status_handle == MW_INVALID_HANDLE) {
        mw_util_rect_t rect = {
            (int16_t)((SCR_W - STATUS_W) / 2 + 20), (int16_t)((SCR_H - STATUS_H) / 2 + 20),
            STATUS_W, STATUS_H
        };
        s_status_handle = mw_add_window(&rect, "System Status", status_paint, status_message,
                                         NULL, 0,
                                         MW_WINDOW_FLAG_HAS_BORDER | MW_WINDOW_FLAG_HAS_TITLE_BAR,
                                         NULL);
        static mw_ui_button_data_t close_data;
        memset(&close_data, 0, sizeof(close_data));
        (void)mw_util_safe_strcpy(close_data.button_label, MW_UI_BUTTON_LABEL_MAX_CHARS, "Close");
        s_btn_close = mw_ui_button_add_new(STATUS_W / 2 - 50, STATUS_H - 36, s_status_handle,
                                            MW_CONTROL_FLAG_IS_VISIBLE | MW_CONTROL_FLAG_IS_ENABLED |
                                            MW_CONTROL_FLAG_LARGE_SIZE, &close_data);
    }
    mw_set_window_visible(s_status_handle, true);
    mw_bring_window_to_front(s_status_handle);
    mw_paint_window_frame(s_status_handle, MW_WINDOW_FRAME_COMPONENT_ALL);
    mw_paint_window_client(s_status_handle);
}

// ── Backdrop ──────────────────────────────────────────────────────────────

static void lock_backdrop_paint(mw_handle_t handle, const mw_gl_draw_info_t *d) {
    (void)handle;
    mw_gl_set_fill(MW_GL_FILL); mw_gl_set_border(MW_GL_BORDER_OFF);
    mw_gl_set_solid_fill_colour(WCE_DESKTOP);
    mw_gl_rectangle(d, 0, 0, (int16_t)SCR_W, (int16_t)SCR_H);
}

static void lock_backdrop_message(const mw_message_t *msg) {
    if (msg->message_id != MW_TOUCH_DOWN_MESSAGE) return;
    // A tap that lands on the backdrop (i.e. outside the dialog) just wakes
    // the screen if it's dark, same as everywhere else — it's not "in the
    // hotspot" so it can never dismiss on its own.
    if (miniwin_lock_handle_touch(false)) {
        mw_paint_window_client(s_lock_backdrop_handle);
    }
}

// ── Dialog ────────────────────────────────────────────────────────────────

static void cred_dialog_paint(mw_handle_t handle, const mw_gl_draw_info_t *d) {
    mw_util_rect_t client = mw_get_window_client_rect(handle);
    mw_gl_set_fill(MW_GL_FILL); mw_gl_set_border(MW_GL_BORDER_OFF);
    mw_gl_set_solid_fill_colour(WCE_BAR);
    mw_gl_rectangle(d, 0, 0, client.width, client.height);

    mw_gl_set_font(MW_GL_FONT_9);
    mw_gl_set_bg_transparency(MW_GL_BG_TRANSPARENT);
    mw_gl_set_fg_colour(WCE_TXT);
    mw_gl_string(d, 8, 10, "User:");
    mw_gl_string(d, 8, 32, "Password:");

    wce_draw_sunken(d, 60, 4, DIALOG_W - 70, 16, WCE_HI);
    wce_draw_sunken(d, 60, 26, DIALOG_W - 70, 16, WCE_HI);
    mw_gl_set_fg_colour(WCE_TXT);
    mw_gl_set_bg_transparency(MW_GL_BG_TRANSPARENT);

    // Trailing "_" marks the focused field — insert-at-end only, no real
    // cursor position, see this section's own header comment.
    char user_disp[CRED_USER_MAX + 2];
    snprintf(user_disp, sizeof(user_disp), "%s%s", s_cred_user, s_cred_field == 0 ? "_" : "");
    mw_gl_string(d, 64, 8, user_disp);

    char pass_disp[CRED_PASS_MAX + 2];
    size_t plen = strlen(s_cred_pass), i = 0;
    for (; i < plen && i < sizeof(pass_disp) - 2; i++) pass_disp[i] = '*';
    if (s_cred_field == 1) pass_disp[i++] = '_';
    pass_disp[i] = '\0';
    mw_gl_string(d, 64, 30, pass_disp);

    if (s_cred_error[0]) {
        mw_gl_set_fg_colour(WCE_BATT_LOW);
        mw_gl_string(d, 8, 50, s_cred_error);
    }
}

// The actual character-editing logic — see this section's own header
// comment for the two paths that call this (this window's own
// MW_KEY_PRESSED_MESSAGE, and miniwin_keyboard.c directly while locked).
// A no-op if nothing is currently showing (s_cred_mode == WCE_CRED_NONE) —
// safe to call speculatively.
void wce_credential_dialog_handle_key(uint8_t keycode) {
    if (s_cred_mode == WCE_CRED_NONE) return;

    char  *buf;
    size_t cap;
    if (s_cred_field == 0 && s_cred_user_editable) { buf = s_cred_user; cap = CRED_USER_MAX; }
    else                                            { buf = s_cred_pass; cap = CRED_PASS_MAX; s_cred_field = 1; }

    if (keycode == 0x08 || keycode == 0x7F) {          // Backspace/Delete
        size_t len = strlen(buf);
        if (len > 0) buf[len - 1] = '\0';
    } else if (keycode == '\r' || keycode == '\n') {   // Enter — submit
        cred_verify_and_proceed();
        return;
    } else if (keycode == '\t' && s_cred_user_editable) {   // Tab — switch field (Login only)
        s_cred_field = s_cred_field ? 0 : 1;
    } else if (keycode >= 0x20 && keycode <= 0x7E) {   // printable ASCII
        size_t len = strlen(buf);
        if (len + 1 < cap) { buf[len] = (char)keycode; buf[len + 1] = '\0'; }
    }
    cred_dialog_repaint();
}

static void cred_dialog_message(const mw_message_t *msg) {
    if (msg->message_id == MW_KEY_PRESSED_MESSAGE) {
        wce_credential_dialog_handle_key((uint8_t)msg->message_data);
        return;
    }
    if (msg->message_id == MW_BUTTON_PRESSED_MESSAGE) {
        if (msg->sender_handle == s_btn_status) { show_status_window(); return; }
        if (msg->sender_handle == s_btn_ok)     { cred_verify_and_proceed(); return; }
        if (msg->sender_handle == s_btn_cancel) {
            // No real "cancel out of logging in/unlocking" destination —
            // just clears the attempt so a mis-tap doesn't strand a wrong
            // password sitting in the field.
            s_cred_pass[0] = '\0';
            s_cred_error[0] = '\0';
            cred_dialog_repaint();
        }
        return;
    }
    if (msg->message_id != MW_TOUCH_DOWN_MESSAGE) return;

    // First touch after the backlight went dark just wakes it, same as
    // everywhere else — never true during boot login (miniwin_lock_
    // is_locked() is false then, so miniwin_lock_handle_touch() always
    // returns false immediately).
    if (miniwin_lock_handle_touch(false)) {
        cred_dialog_repaint();
        return;
    }

    // Tap-to-focus-field. Username only switchable when editable (Login) —
    // Lock mode's username is fixed, so a tap there is simply ignored,
    // same as real Windows never letting you switch users from the lock
    // dialog.
    int16_t ty = (int16_t)(msg->message_data & 0xFFFF);
    if (ty >= 4 && ty < 20 && s_cred_user_editable) {
        s_cred_field = 0;
        cred_dialog_repaint();
    } else if (ty >= 26 && ty < 42) {
        s_cred_field = 1;
        cred_dialog_repaint();
    }
}

// ── Transition ────────────────────────────────────────────────────────────

static void on_lock_transition(bool locked) {
    if (locked) {
        s_cred_mode          = WCE_CRED_LOCK;
        s_cred_user_editable = false;
        (void)mw_util_safe_strcpy(s_cred_user, CRED_USER_MAX, user_mgr_current_user());
        s_cred_pass[0]  = '\0';
        s_cred_error[0] = '\0';
        s_cred_field    = 1;   // password — username is fixed, nothing to edit there
        wm_set_window_title(s_lock_dialog_handle, (char *)"Workstation Locked");
        // Symmetric with cred_verify_and_proceed()'s own app_manager_
        // notify_unlocked() call — see that doc comment for the "Local
        // unlock gate" this codebase now has. Best-effort per app_
        // manager.h's own doc comment on _notify_locked(); real privacy
        // value here (a locked device shows no app list) at zero extra
        // cost since this is already the exact lock/unlock boundary.
        app_manager_notify_locked();
        mw_set_window_visible(s_lock_backdrop_handle, true);
        mw_bring_window_to_front(s_lock_backdrop_handle);
        mw_set_window_visible(s_lock_dialog_handle, true);
        // Modal ON only while actually shown — see this file's top comment
        // (Window 3 section) for the real bug this fixes. Also brings the
        // dialog to front itself (mw_set_window_modal()'s own behavior);
        // the explicit call right after is redundant but harmless.
        mw_set_window_modal(s_lock_dialog_handle, true);
        mw_bring_window_to_front(s_lock_dialog_handle);
        mw_paint_window_client(s_lock_backdrop_handle);
        mw_paint_window_frame(s_lock_dialog_handle, MW_WINDOW_FRAME_COMPONENT_ALL);
        mw_paint_window_client(s_lock_dialog_handle);
    } else {
        s_cred_mode = WCE_CRED_NONE;
        mw_set_window_modal(s_lock_dialog_handle, false);
        // Restores the local registry app_manager_notify_locked() above
        // just hid — idempotent per app_manager.h's own doc comment, so
        // this is safe even though cred_verify_and_proceed()'s LOGIN
        // branch also calls the unlocked side directly (that path never
        // goes through on_lock_transition() at all, since s_locked stays
        // false throughout boot login).
        app_manager_notify_unlocked();
        mw_set_window_visible(s_lock_dialog_handle, false);
        mw_set_window_visible(s_lock_backdrop_handle, false);
        hide_status_window();
        // Not mw_paint_all() directly — see wce_redraw_layers()'s doc
        // comment for why targeted-then-mw_paint_all()-fallback is what
        // that function actually does; App windows were never touched by
        // locking (unlike an old minimize-every-window approach), so they
        // don't need repainting; only the layers the lock overlay was
        // covering do.
        wce_redraw_layers();
    }
}

void mw_user_init(void) {
    mw_util_rect_t r;

    mw_util_set_rect(&r, 0, TASKBAR_Y, SCR_W, TASKBAR_H);
    s_taskbar_handle = mw_add_window(&r, "",
        taskbar_paint, taskbar_message, NULL, 0,
        MW_WINDOW_FLAG_IS_VISIBLE | MW_WINDOW_FLAG_TOUCH_FOCUS_AND_EVENT,
        NULL);

    mw_util_set_rect(&r, 0, 0, SCR_W, SCR_H);
    s_lock_backdrop_handle = mw_add_window(&r, "",
        lock_backdrop_paint, lock_backdrop_message, NULL, 0,
        MW_WINDOW_FLAG_TOUCH_FOCUS_AND_EVENT,   // IS_VISIBLE deliberately omitted — shown on lock/login
        NULL);

    mw_util_set_rect(&r, (int16_t)((SCR_W - DIALOG_W) / 2), (int16_t)((SCR_H - DIALOG_H) / 2),
                      DIALOG_W, DIALOG_H);
    s_lock_dialog_handle = mw_add_window(&r, "Log On to Windows", cred_dialog_paint, cred_dialog_message,
        NULL, 0,
        // MW_WINDOW_FLAG_IS_MODAL deliberately NOT set here — see mw_set_
        // window_modal() calls at each show/hide site below for why.
        MW_WINDOW_FLAG_HAS_BORDER | MW_WINDOW_FLAG_HAS_TITLE_BAR |
        MW_WINDOW_FLAG_TOUCH_FOCUS_AND_EVENT,   // IS_VISIBLE deliberately omitted
        NULL);

    static mw_ui_button_data_t ok_data, cancel_data, status_data;
    memset(&ok_data, 0, sizeof(ok_data));
    memset(&cancel_data, 0, sizeof(cancel_data));
    memset(&status_data, 0, sizeof(status_data));
    (void)mw_util_safe_strcpy(ok_data.button_label,     MW_UI_BUTTON_LABEL_MAX_CHARS, "OK");
    (void)mw_util_safe_strcpy(cancel_data.button_label, MW_UI_BUTTON_LABEL_MAX_CHARS, "Cancel");
    (void)mw_util_safe_strcpy(status_data.button_label, MW_UI_BUTTON_LABEL_MAX_CHARS, "Status");
    // Default-size creation (MW_UI_BUTTON_WIDTH/HEIGHT, 50x15), then resized
    // to a real in-between size via mw_resize_control() — MiniWin's own
    // MW_CONTROL_FLAG_LARGE_SIZE only offers exactly two fixed presets
    // (50x15 or 100x30, see MiniWin/ui/ui_button.h), no size in between via
    // the flag alone. 100x30 was confirmed too big alongside two real
    // fields in a 220px-wide dialog; 50x15 turned out too small the other
    // way. 70x20 (BTN_W/BTN_H below) split the difference. Status' own
    // popup window keeps its original LARGE_SIZE Close button (a bare
    // full-screen-ish window with nothing else in it, where 100x30 is
    // fine) — unrelated to this dialog, not resized.
#define BTN_W 70
#define BTN_H 20
    uint16_t btn_flags = MW_CONTROL_FLAG_IS_VISIBLE | MW_CONTROL_FLAG_IS_ENABLED;
    s_btn_ok     = mw_ui_button_add_new(30, 105, s_lock_dialog_handle, btn_flags, &ok_data);
    s_btn_cancel = mw_ui_button_add_new(120, 105, s_lock_dialog_handle, btn_flags, &cancel_data);
    s_btn_status = mw_ui_button_add_new(75, 130, s_lock_dialog_handle, btn_flags, &status_data);
    (void)mw_resize_control(s_btn_ok, BTN_W, BTN_H);
    (void)mw_resize_control(s_btn_cancel, BTN_W, BTN_H);
    (void)mw_resize_control(s_btn_status, BTN_W, BTN_H);

    miniwin_lock_set_transition_cb(on_lock_transition);

    // Boot-time login gate — mirrors systemui.h's own purr_systemui_boot_
    // login_check()/purr_systemui_show_login() split (the LVGL backends'
    // proven convention for this exact moment, not directly reusable code
    // since that's lv_obj_t-based and MiniWin isn't, but the *behaviour*
    // is exactly right): auto-login silently when the account has no
    // password — never force a modal dialog open with no way to dismiss
    // it correctly, the same lockout-risk reasoning systemui.h's own doc
    // comment gives — otherwise show the real dialog before anything else
    // on the desktop is usable.
    const char *default_user = user_mgr_default_username();
    if (default_user && default_user[0] && !user_mgr_is_logged_in()) {
        if (!user_mgr_has_password(default_user)) {
            ESP_LOGI(TAG, "boot login: '%s' has no password — auto-login", default_user);
            user_mgr_set_logged_in(default_user);
            // This zero-friction bootstrap path bypasses cred_verify_and_
            // proceed()'s own app_manager_notify_unlocked() call entirely
            // — without this, a no-password account (the common case)
            // boots straight into a permanently idle/empty Desktop, same
            // trap systemui.h's own purr_systemui_boot_login_check() has
            // an identical fix + comment for. This was very likely the
            // actual live "0 apps found" bug, not the dialog-login path.
            app_manager_notify_unlocked();
        } else {
            ESP_LOGI(TAG, "boot login: '%s' has a password — showing login dialog", default_user);
            s_cred_mode          = WCE_CRED_LOGIN;
            s_cred_user_editable = true;
            (void)mw_util_safe_strcpy(s_cred_user, CRED_USER_MAX, default_user);
            s_cred_pass[0]  = '\0';
            s_cred_error[0] = '\0';
            s_cred_field    = 1;
            wm_set_window_title(s_lock_dialog_handle, (char *)"Log On to Windows");
            mw_set_window_visible(s_lock_backdrop_handle, true);
            mw_bring_window_to_front(s_lock_backdrop_handle);
            mw_set_window_visible(s_lock_dialog_handle, true);
            // Modal ON only while actually shown — see this file's top
            // comment (Window 3 section) for the real bug this fixes.
            mw_set_window_modal(s_lock_dialog_handle, true);
            mw_bring_window_to_front(s_lock_dialog_handle);
        }
    }

    mw_paint_all();
}

// The real desktop background — see this file's own "Wallpaper" comment
// (top of file) for why this replaced a separate mw_add_window() window.
void mw_user_root_paint_function(const mw_gl_draw_info_t *draw_info) {
    mw_gl_set_fill(MW_GL_FILL); mw_gl_set_border(MW_GL_BORDER_OFF);
    mw_gl_set_solid_fill_colour(WCE_DESKTOP);
    mw_gl_rectangle(draw_info, 0, 0, (int16_t)SCR_W, (int16_t)SCR_H);
}

// Nothing to do — a background tap reaching here (nothing else caught it)
// is correctly a no-op, same as real desktop OSes; MiniWin's own root-
// window exemption is what keeps this from stealing focus/front-position
// away from whatever's actually open (see this file's top comment).
void mw_user_root_message_function(const mw_message_t *message) { (void)message; }

#endif  // CONFIG_PURR_MINIWIN_DESKTOP_WINCE
