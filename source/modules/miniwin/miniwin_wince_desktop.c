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
#include "miniwin_wince_icons.h"
#include "miniwin_lock.h"
#include "MiniWin/miniwin_utilities.h"
#include "MiniWin/gl/gl.h"
#include "MiniWin/hal/hal_lcd.h"
#include "MiniWin/bitmaps/mw_bitmaps.h"
#include "../../kernel/core/purr_kernel.h"
#include "../app_manager/app_manager.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdio.h>
#ifdef CONFIG_PURR_FEATURE_MESHTASTIC
#include "../meshtastic/meshtastic.h"
#endif

// ── Palette + bevels (ported from wince_common.cpp) ─────────────────────────

#define WCE_DESKTOP 0x008080
#define WCE_BAR     0xC0C0C0
#define WCE_HI      0xFFFFFF
#define WCE_SHD     0x808080
#define WCE_DARK    0x404040
#define WCE_TXT     0x000000
#define WCE_MBKG    0xD4D0C8

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

// ── Desktop icons ────────────────────────────────────────────────────────────
//
// A curated subset of apps gets a desktop icon (not every registered app —
// that's the *other*, mutually exclusive MiniWin desktop style's job, see
// miniwin_module.c's icon-grid desktop). Keyed by the exact app_manager name
// string each app registers with.

#define DTICON_LEFT     8
#define DTICON_TOP      8
#define DTICON_CELL_W   64
#define DTICON_CELL_H   48

typedef struct {
    const char    *app_name;   // must match the app's purr_module_header_t.name
    const uint8_t *icon;
    uint8_t        icon_w, icon_h;
} wce_desktop_icon_t;

static const wce_desktop_icon_t s_desktop_icons[] = {
    { "settings",   wce_icon_settings,            WCE_ICON_SIZE, WCE_ICON_SIZE },
    { "terminal",   wce_icon_terminal,             WCE_ICON_SIZE, WCE_ICON_SIZE },
    { "fileman",    mw_bitmaps_folder_icon_large,  16,            16            },
    { "calculator", wce_icon_calculator,           WCE_ICON_SIZE, WCE_ICON_SIZE },
    { "msn",        wce_icon_msn,                  WCE_ICON_SIZE, WCE_ICON_SIZE },
};
#define DESKTOP_ICON_COUNT (int)(sizeof(s_desktop_icons) / sizeof(s_desktop_icons[0]))

// -1 if this particular device's build doesn't register that app — its grid
// slot is then skipped but stays reserved (position comes from the table
// index, not a filtered count), so icons don't shift around across devices
// with different app sets.
static int dt_icon_app_index(const char *name) {
    int n = app_manager_count();
    for (int i = 0; i < n; i++) {
        const app_entry_t *app = app_manager_get(i);
        if (app && strcmp(app->name, name) == 0) return i;
    }
    return -1;
}

static void dt_icon_cell_pos(int idx, int16_t *out_x, int16_t *out_y) {
    int cols = (int)((SCR_W - DTICON_LEFT) / DTICON_CELL_W);
    if (cols < 1) cols = 1;
    int col = idx % cols;
    int row = idx / cols;
    *out_x = (int16_t)(DTICON_LEFT + col * DTICON_CELL_W);
    *out_y = (int16_t)(DTICON_TOP  + row * DTICON_CELL_H);
}

static void draw_desktop_icons(const mw_gl_draw_info_t *d) {
    for (int i = 0; i < DESKTOP_ICON_COUNT; i++) {
        if (dt_icon_app_index(s_desktop_icons[i].app_name) < 0) continue;

        int16_t x, y;
        dt_icon_cell_pos(i, &x, &y);

        // White, not the taskbar's black WCE_TXT — the wallpaper is a dark
        // teal (WCE_DESKTOP), so white is what actually reads against it.
        mw_gl_set_fg_colour(WCE_HI);
        mw_gl_set_bg_transparency(MW_GL_BG_TRANSPARENT);
        mw_gl_monochrome_bitmap(d, x, y,
                                 s_desktop_icons[i].icon_w, s_desktop_icons[i].icon_h,
                                 s_desktop_icons[i].icon);

        mw_gl_set_font(MW_GL_FONT_9);
        mw_gl_string(d, x, (int16_t)(y + WCE_ICON_SIZE + 1), s_desktop_icons[i].app_name);
    }
}

// Hit-tests the whole cell, not just the (sometimes smaller, e.g. fileman's
// 16x16) icon bitmap footprint — a bigger, more forgiving tap target.
static bool dt_icon_hit_test(int16_t tx, int16_t ty, int *out_app_idx) {
    for (int i = 0; i < DESKTOP_ICON_COUNT; i++) {
        int app_idx = dt_icon_app_index(s_desktop_icons[i].app_name);
        if (app_idx < 0) continue;

        int16_t x, y;
        dt_icon_cell_pos(i, &x, &y);
        if (tx >= x && tx < x + DTICON_CELL_W && ty >= y && ty < y + DTICON_CELL_H) {
            *out_app_idx = app_idx;
            return true;
        }
    }
    return false;
}

// ── Window 1: Wallpaper (z=1, lowest) ────────────────────────────────────────

static mw_handle_t s_wallpaper_handle = MW_INVALID_HANDLE;

static void wallpaper_paint(mw_handle_t handle, const mw_gl_draw_info_t *d) {
    (void)handle;
    mw_gl_set_fill(MW_GL_FILL); mw_gl_set_border(MW_GL_BORDER_OFF);
    mw_gl_set_solid_fill_colour(WCE_DESKTOP);
    mw_gl_rectangle(d, 0, 0, (int16_t)SCR_W, (int16_t)SCR_H);
}

// Nothing to do — this window only ever receives a touch that missed every
// window above it (icons, taskbar+menu, and while locked, the lock window).
static void wallpaper_message(const mw_message_t *msg) { (void)msg; }

// ── Window 2: Desktop icons (z=2) ────────────────────────────────────────────
//
// Rect is full screen minus the taskbar strip (0,0,SCR_W,TASKBAR_Y) — it
// never overlaps the taskbar+menu window's resting rect, and while that
// window grows upward to cover an open Start Menu popup, its higher
// z-order correctly wins touch resolution over whatever icons happen to
// sit in that same screen region (exactly what you want: a tap on a
// visually-open menu shouldn't reach an icon it's covering).

static mw_handle_t s_icons_handle = MW_INVALID_HANDLE;

static void icons_paint(mw_handle_t handle, const mw_gl_draw_info_t *d) {
    (void)handle;
    // Explicit background fill first — this window doesn't rely on the
    // wallpaper window's own repaint correctly showing through beneath it;
    // it's self-sufficient for its own rect, exactly like the original
    // single-window desktop_paint() always was (fill background, then draw
    // icons on top, all in one paint call). Confirmed live: depending on
    // the wallpaper window (a separate, lower z-order window) to already
    // have cleared this region wasn't reliably happening — an app window
    // or the lock overlay closing left stale pixels behind regardless of
    // how many "please redraw" triggers got added elsewhere.
    mw_gl_set_fill(MW_GL_FILL); mw_gl_set_border(MW_GL_BORDER_OFF);
    mw_gl_set_solid_fill_colour(WCE_DESKTOP);
    mw_gl_rectangle(d, 0, 0, (int16_t)SCR_W, TASKBAR_Y);
    draw_desktop_icons(d);
}

static void icons_message(const mw_message_t *msg) {
    if (msg->message_id != MW_TOUCH_DOWN_MESSAGE) return;
    int16_t tx = (int16_t)(msg->message_data >> 16);
    int16_t ty = (int16_t)(msg->message_data & 0xFFFF);
    int app_idx;
    if (dt_icon_hit_test(tx, ty, &app_idx)) {
        app_manager_launch_idx(app_idx);
        mw_paint_all();
    }
}

// ── Window 3: Taskbar + Start Menu (z=3) ─────────────────────────────────────
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

// Taskbar corner rotates between free RAM and battery voltage every
// STATUS_ROTATE_TICKS repaints — flipped in miniwin_module.c's task loop
// (which already repaints this exact rect once a second for RAM
// freshness), read here to decide which string to format.
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
    mw_paint_window_client(s_wallpaper_handle);
    mw_paint_window_client(s_icons_handle);
    mw_paint_window_client(s_taskbar_handle);
    // Belt and suspenders: the three handle-targeted repaints above rely on
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

// Resizes/repositions the taskbar+menu window to exactly match its current
// visual footprint and repaints it. Call after anything that could change
// current_menu_height()'s result (smenu open/close/folder/page).
static void resize_taskbar_window(void) {
    int16_t menu_h = current_menu_height();
    int16_t new_h  = (int16_t)(TASKBAR_H + menu_h);
    int16_t new_y  = (int16_t)(SCR_H - new_h);
    mw_reposition_window(s_taskbar_handle, 0, new_y);
    mw_resize_window(s_taskbar_handle, (int16_t)SCR_W, new_h);
    // The Start Menu can vacate screen area the icons window owns when it
    // shrinks back down (or grows, briefly overlapping) — icons doesn't
    // get an automatic repaint just because the window above it moved, so
    // this needs the full wce_redraw_layers() treatment (not just
    // repainting taskbar+icons individually) for the same reason that
    // function's own doc comment explains — targeted per-window repaints
    // alone aren't reliably clearing everything here.
    wce_redraw_layers();
}

static void taskbar_paint(mw_handle_t handle, const mw_gl_draw_info_t *d) {
    (void)handle;
    int16_t menu_h = current_menu_height();

    // Same self-sufficiency reasoning as icons_paint()'s background fill:
    // when the Start Menu is open this window's rect grows upward past
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

    // Oversized on purpose — GCC's format-truncation check can't prove
    // "%d.%02uV" stays short even though mv is always a small int in
    // practice; the corner box just visually clips whatever overflows.
    char stat[24];
    if (s_status_show_battery) {
        int mv = purr_kernel_battery_voltage_mv();
        if (mv >= 0) snprintf(stat, sizeof(stat), "%d.%02uV", mv / 1000, (unsigned)((mv % 1000) / 10));
        else         snprintf(stat, sizeof(stat), "no batt");
    } else {
        snprintf(stat, sizeof(stat), "%ukB",
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_8BIT) / 1024));
    }
    int16_t bx = SCR_W - 50;
    wce_draw_sunken(d, bx, (int16_t)(menu_h + 2), 48, TASKBAR_H - 4, WCE_BAR);
    mw_gl_set_fg_colour(WCE_TXT);
    mw_gl_set_bg_transparency(MW_GL_BG_TRANSPARENT);
    mw_gl_set_font(MW_GL_FONT_9);
    mw_gl_string(d, bx + 4, (int16_t)(local_start_y + 5), stat);

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

// ── Window 4: Lock overlay (z=4, created invisible) ──────────────────────────
//
// Two windows, both created once at startup and toggled visible/invisible
// together (never recreated) — a full-screen teal backdrop (visual coverage
// only, matches the desktop's own WCE_DESKTOP colour) and a small draggable
// "Enter Password" dialog on top of it. MW_WINDOW_FLAG_HAS_TITLE_BAR is what
// makes the dialog draggable — MiniWin's own touch handling already
// distinguishes a touch-down on a window's title bar from one on its client
// area and drags the window in response, no extra code needed here.
// MW_WINDOW_FLAG_IS_MODAL is what actually blocks input from reaching
// anything underneath.
//
// User/password fields are decorative only — static text, not real text
// entry (no credential store exists anywhere in this codebase to check
// against). OK and Cancel both just dismiss via miniwin_lock_handle_touch(),
// which correctly folds in the "first touch after the backlight went dark
// just wakes it, doesn't dismiss" behaviour miniwin_lock.c already handles
// for every other input path — pressing a button while the screen is dark
// wakes it instead of unlocking, consistent with tapping anywhere else.
//
// A third "Status" button opens a separate, also-draggable, on-demand
// window with uptime/battery/node count/unread message count — counts
// only, never message content or node identities, since this is still a
// lock screen.

// Buttons use MW_CONTROL_FLAG_LARGE_SIZE (100x30, vs the 50x15 default) —
// too small to comfortably tap confirmed live. Three large buttons can't
// fit in one row on a 320px-wide screen with any margin, so OK/Cancel
// share a row and Status gets its own row below.
#define DIALOG_W  220
#define DIALOG_H  190
#define STATUS_W  190
#define STATUS_H  140

static mw_handle_t s_lock_backdrop_handle = MW_INVALID_HANDLE;
static mw_handle_t s_lock_dialog_handle   = MW_INVALID_HANDLE;
static mw_handle_t s_status_handle        = MW_INVALID_HANDLE;
static mw_handle_t s_btn_ok, s_btn_cancel, s_btn_status, s_btn_close;

mw_handle_t wce_lock_handle(void) { return s_lock_dialog_handle; }

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

static void lock_dialog_paint(mw_handle_t handle, const mw_gl_draw_info_t *d) {
    mw_util_rect_t client = mw_get_window_client_rect(handle);
    mw_gl_set_fill(MW_GL_FILL); mw_gl_set_border(MW_GL_BORDER_OFF);
    mw_gl_set_solid_fill_colour(WCE_BAR);
    mw_gl_rectangle(d, 0, 0, client.width, client.height);

    mw_gl_set_font(MW_GL_FONT_9);
    mw_gl_set_bg_transparency(MW_GL_BG_TRANSPARENT);
    mw_gl_set_fg_colour(WCE_TXT);
    mw_gl_string(d, 8, 10, "User:");
    mw_gl_string(d, 8, 32, "Password:");

    // Decorative field boxes — static text, not real entry (see file
    // header comment for why).
    wce_draw_sunken(d, 60, 4, DIALOG_W - 70, 16, WCE_HI);
    wce_draw_sunken(d, 60, 26, DIALOG_W - 70, 16, WCE_HI);
    mw_gl_set_fg_colour(WCE_TXT);
    mw_gl_set_bg_transparency(MW_GL_BG_TRANSPARENT);
    mw_gl_string(d, 64, 8, "catcollector");
    mw_gl_string(d, 64, 30, "********");
}

static void lock_dialog_message(const mw_message_t *msg) {
    if (msg->message_id == MW_BUTTON_PRESSED_MESSAGE) {
        if (msg->sender_handle == s_btn_status) {
            show_status_window();
            return;
        }
        // OK or Cancel — no real auth to check yet, both just dismiss.
        if (miniwin_lock_handle_touch(true)) {
            mw_paint_window_client(s_lock_dialog_handle);
        }
        return;
    }
    if (msg->message_id != MW_TOUCH_DOWN_MESSAGE) return;
    // Any touch on the dialog's own background (not a button) — same as
    // the backdrop, only wakes if dark, never dismisses on its own.
    if (miniwin_lock_handle_touch(false)) {
        mw_paint_window_client(s_lock_dialog_handle);
    }
}

// ── Transition ────────────────────────────────────────────────────────────

static void on_lock_transition(bool locked) {
    if (locked) {
        mw_set_window_visible(s_lock_backdrop_handle, true);
        mw_bring_window_to_front(s_lock_backdrop_handle);
        mw_set_window_visible(s_lock_dialog_handle, true);
        mw_bring_window_to_front(s_lock_dialog_handle);
        mw_paint_window_client(s_lock_backdrop_handle);
        mw_paint_window_frame(s_lock_dialog_handle, MW_WINDOW_FRAME_COMPONENT_ALL);
        mw_paint_window_client(s_lock_dialog_handle);
    } else {
        mw_set_window_visible(s_lock_dialog_handle, false);
        mw_set_window_visible(s_lock_backdrop_handle, false);
        hide_status_window();
        // Not mw_paint_all() directly — see wce_redraw_layers()'s doc
        // comment for why targeted-then-mw_paint_all()-fallback is what
        // that function actually does; App windows were never touched by
        // locking (unlike an old minimize-every-window approach), so they
        // don't need repainting; only the 3 layers the lock overlay was
        // covering do.
        wce_redraw_layers();
    }
}

// ── Root hooks (no-ops — the windows above handle all rendering) ───────

void mw_user_init(void) {
    mw_util_rect_t r;

    mw_util_set_rect(&r, 0, 0, SCR_W, SCR_H);
    s_wallpaper_handle = mw_add_window(&r, "",
        wallpaper_paint, wallpaper_message, NULL, 0,
        MW_WINDOW_FLAG_IS_VISIBLE | MW_WINDOW_FLAG_TOUCH_FOCUS_AND_EVENT,
        NULL);

    mw_util_set_rect(&r, 0, 0, SCR_W, TASKBAR_Y);
    s_icons_handle = mw_add_window(&r, "",
        icons_paint, icons_message, NULL, 0,
        MW_WINDOW_FLAG_IS_VISIBLE | MW_WINDOW_FLAG_TOUCH_FOCUS_AND_EVENT,
        NULL);

    mw_util_set_rect(&r, 0, TASKBAR_Y, SCR_W, TASKBAR_H);
    s_taskbar_handle = mw_add_window(&r, "",
        taskbar_paint, taskbar_message, NULL, 0,
        MW_WINDOW_FLAG_IS_VISIBLE | MW_WINDOW_FLAG_TOUCH_FOCUS_AND_EVENT,
        NULL);

    mw_util_set_rect(&r, 0, 0, SCR_W, SCR_H);
    s_lock_backdrop_handle = mw_add_window(&r, "",
        lock_backdrop_paint, lock_backdrop_message, NULL, 0,
        MW_WINDOW_FLAG_TOUCH_FOCUS_AND_EVENT,   // IS_VISIBLE deliberately omitted — shown on lock
        NULL);

    mw_util_set_rect(&r, (int16_t)((SCR_W - DIALOG_W) / 2), (int16_t)((SCR_H - DIALOG_H) / 2),
                      DIALOG_W, DIALOG_H);
    s_lock_dialog_handle = mw_add_window(&r, "Enter Password", lock_dialog_paint, lock_dialog_message,
        NULL, 0,
        MW_WINDOW_FLAG_HAS_BORDER | MW_WINDOW_FLAG_HAS_TITLE_BAR |
        MW_WINDOW_FLAG_IS_MODAL | MW_WINDOW_FLAG_TOUCH_FOCUS_AND_EVENT,   // IS_VISIBLE deliberately omitted
        NULL);

    static mw_ui_button_data_t ok_data, cancel_data, status_data;
    memset(&ok_data, 0, sizeof(ok_data));
    memset(&cancel_data, 0, sizeof(cancel_data));
    memset(&status_data, 0, sizeof(status_data));
    (void)mw_util_safe_strcpy(ok_data.button_label,     MW_UI_BUTTON_LABEL_MAX_CHARS, "OK");
    (void)mw_util_safe_strcpy(cancel_data.button_label, MW_UI_BUTTON_LABEL_MAX_CHARS, "Cancel");
    (void)mw_util_safe_strcpy(status_data.button_label, MW_UI_BUTTON_LABEL_MAX_CHARS, "Status");
    uint16_t btn_flags = MW_CONTROL_FLAG_IS_VISIBLE | MW_CONTROL_FLAG_IS_ENABLED |
                         MW_CONTROL_FLAG_LARGE_SIZE;
    s_btn_ok     = mw_ui_button_add_new(10,  122, s_lock_dialog_handle, btn_flags, &ok_data);
    s_btn_cancel = mw_ui_button_add_new(110, 122, s_lock_dialog_handle, btn_flags, &cancel_data);
    s_btn_status = mw_ui_button_add_new(60,  156, s_lock_dialog_handle, btn_flags, &status_data);

    miniwin_lock_set_transition_cb(on_lock_transition);
    mw_paint_all();
}

void mw_user_root_paint_function(const mw_gl_draw_info_t *draw_info) { (void)draw_info; }
void mw_user_root_message_function(const mw_message_t *message) { (void)message; }

#endif  // CONFIG_PURR_MINIWIN_DESKTOP_WINCE
