// launcher_lvgl.c — the home screen: a 3x2 grid of BARE icons + labels
// (no visible box/tile around either), tap to launch, swipe left/right
// to change pages (with a small page-dot indicator), swipe down/up to
// open/close a simple scrollable notification panel. One extra page
// (2026-09-17, by direct request) sits past the last grid page: Recents,
// a filtered view of just the currently-RUNNING apps (app_manager_entry_
// running()), each shown as a bordered square with a thin white bottom
// strip carrying its name + a close button — see s_page's own comment
// for how it's threaded into the same LEFT/RIGHT paging as the grid.
// Deliberately nothing beyond that — no dock/drawer, no swipe-to-dismiss,
// no Home/Back gestures here at all (systemui_lvgl.c owns Home/Back/Lock
// on its own bar, entirely separately).
//
// Talks to app_manager (local and remote — see below), loaded once
// loginUI's claw_personal_init() has already succeeded (app_manager_
// notify_unlocked() already fired inside login_core.c). LVGL-only
// (app.pcat's `variants = "lvgl"`).
//
// Icons — source/modules/common/purr_icons.c/.h, BlackBerry OS 10 art.
// Not every app has a mapped icon (icon_name_for_app() below); a tile
// with none just shows its label alone.
//
// Background — the wallpaper image, set once by kernel_tdp_boot.c as a
// screen STYLE (not a widget here), so it's already there before this
// package ever runs and survives every lv_obj_clean() this file does
// (briefly swapped for plain black, then reverted by direct request —
// see kernel_tdp_boot.c's own comment). Nothing in this file needs to
// touch it.
//
// Grid geometry reserves TOP_BAR_H at the top and BOTTOM_BAR_H at the
// bottom for systemui_lvgl.c's own status row and Home bar — must match
// that file's own TOP_BAR_H/HOME_BAR_HIT_H (no shared header between the
// two for this).
//
// Notification panel — a plain lv_obj_create() left SCROLLABLE (the one
// object in this file that doesn't clear that flag), restricted to
// VERTICAL scrolling only (lv_obj_set_scroll_dir(), by direct request —
// "scrollable list up and down, not side to side"). Full screen width,
// the top 75% of the screen's own upper half tall (see NOTIF_H's own
// comment), real black background (distinct from the wallpaper behind
// it, so it reads as a real panel rather than a hole in the icons) with
// a grab handle at the bottom, holding one real CARD rectangle per
// notification (see CARD_H's own comment on where that look came from).
// LVGL's own native scrolling handles overflow if there are more cards
// than fit, which is the whole point of leaving SCROLLABLE on. One real
// caveat, honestly noted rather than solved: a vertical swipe that
// starts ON the panel's own scrolled content may be consumed as a scroll
// instead of bubbling up as the close gesture below — the grab handle
// (a sibling, not part of the scrollable content) or swiping from just
// outside the cards is the reliable way to close it.
//
// Window management for a launched app to actually draw into — purr_lv_
// win.h (source/modules/common/), registered from kernel_tdp_boot.c's
// lvgl_hw_init(). Not this file's concern at all.
//
// Display/touch-indev setup is NOT here — kernel_tdp_boot.c's shared
// lvgl_hw_init() already did that. This file only ever creates WIDGETS
// and attaches click/gesture callbacks on the already-set-up default
// screen — every declaration below is an OPAQUE pointer type, no LVGL
// struct mirroring at all, except launcher_notification_t (see its own
// comment) and launcher_point_t is not needed here (no per-widget touch
// tracking this pass — see this file's own top comment on why the
// notification panel has no swipe-to-dismiss any more).
#if defined(SYSCLAW_BACKEND_LVGL)

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

typedef struct lv_obj_t   lv_obj_t;
typedef struct lv_disp_t  lv_disp_t;
typedef struct lv_event_t lv_event_t;
typedef struct lv_indev_t lv_indev_t;
typedef int16_t lv_coord_t;   // LV_USE_LARGE_COORD is off in this project's lv_conf.h — confirmed, not assumed
typedef int     lv_event_code_t;
typedef uint8_t lv_dir_t;     // lv_area.h: typedef uint8_t lv_dir_t; confirmed before use

extern lv_disp_t *lv_disp_get_default(void);
extern lv_obj_t  *lv_disp_get_scr_act(lv_disp_t *disp);
extern lv_obj_t  *lv_obj_create(lv_obj_t *parent);
extern void       lv_obj_set_pos(lv_obj_t *obj, lv_coord_t x, lv_coord_t y);
extern void       lv_obj_set_size(lv_obj_t *obj, lv_coord_t w, lv_coord_t h);
extern lv_obj_t  *lv_label_create(lv_obj_t *parent);
extern void       lv_label_set_text(lv_obj_t *obj, const char *text);
extern void      *lv_obj_add_event_cb(lv_obj_t *obj, void (*event_cb)(lv_event_t *e),
                                       lv_event_code_t filter, void *user_data);
extern void      *lv_event_get_user_data(lv_event_t *e);
extern void       lv_obj_clean(lv_obj_t *obj);
extern void       lv_obj_clear_flag(lv_obj_t *obj, uint32_t f);
extern void       lv_obj_add_flag(lv_obj_t *obj, uint32_t f);
extern void       lv_obj_set_style_bg_opa(lv_obj_t *obj, uint8_t value, uint32_t selector);
extern void       lv_obj_set_style_border_width(lv_obj_t *obj, lv_coord_t value, uint32_t selector);
extern lv_obj_t  *lv_img_create(lv_obj_t *parent);
extern void       lv_img_set_src(lv_obj_t *obj, const void *src);
extern lv_indev_t *lv_indev_get_act(void);
extern lv_dir_t    lv_indev_get_gesture_dir(const lv_indev_t *indev);

// LV_EVENT_CLICKED's real numeric value — confirmed against CoreOS/
// managed_components/lvgl__lvgl/src/core/lv_event.h's own lv_event_code_t
// enum (LV_EVENT_ALL=0, PRESSED, PRESSING, PRESS_LOST, SHORT_CLICKED,
// LONG_PRESSED, LONG_PRESSED_REPEAT, then CLICKED=7, GESTURE=12) before
// writing this, not assumed.
#define LV_EVENT_CLICKED_VALUE 7
#define LV_EVENT_GESTURE_VALUE 12

// Confirmed against lv_obj.h's own anonymous flag enum (lv_obj_flag_t is
// a plain uint32_t bitmask there: HIDDEN=1<<0, SCROLLABLE=1<<4,
// GESTURE_BUBBLE=1<<15).
#define LV_OBJ_FLAG_HIDDEN_VALUE         (1UL << 0)
#define LV_OBJ_FLAG_SCROLLABLE_VALUE     (1UL << 4)
#define LV_OBJ_FLAG_GESTURE_BUBBLE_VALUE (1UL << 15)
#define LV_OPA_TRANSP_VALUE  0
#define LV_OPA_DIM_VALUE     80    // page dot, inactive — plain opacity, no color needed
#define LV_OPA_COVER_VALUE   255   // page dot, active

// lv_area.h's lv_dir_t bitmask: LEFT=1<<0, RIGHT=1<<1, TOP=1<<2,
// BOTTOM=1<<3 — confirmed against lv_indev.c's own indev_gesture()
// before relying on which is which: it reports LV_DIR_TOP for a swipe
// toward the top of the screen ("swipe up") and LV_DIR_BOTTOM for one
// toward the bottom ("swipe down"). Not the inverted convention it
// easily could have been.
#define LV_DIR_LEFT_VALUE  (1U << 0)
#define LV_DIR_RIGHT_VALUE (1U << 1)
#define LV_DIR_UP_VALUE    (1U << 2)   // LV_DIR_TOP
#define LV_DIR_DOWN_VALUE  (1U << 3)   // LV_DIR_BOTTOM
// LV_DIR_VER = LV_DIR_TOP | LV_DIR_BOTTOM (lv_area.h) — passed to
// lv_obj_set_scroll_dir() so the notification panel only ever scrolls
// vertically, never horizontally.
#define LV_DIR_VER_VALUE   (LV_DIR_UP_VALUE | LV_DIR_DOWN_VALUE)

// app_manager's own dual-mode registry — app_manager_count()/_entry_name()/
// _launch_idx() already dispatch on app_manager's own s_remote_mode
// internally. app_manager_entry_name() is a plain accessor, never
// exposing app_entry_t itself.
extern int  app_manager_count(void);
extern bool app_manager_entry_name(int idx, char *out, size_t out_sz);
extern int  app_manager_launch_idx(int idx);
// app_manager_entry_running()/_stop() — the Recents page below's own
// filter (only show what's actually running, out of the full catalog
// app_manager_count()/_entry_name() report) and its own close button.
extern bool app_manager_entry_running(int idx);
extern void app_manager_stop(int idx);

// purr_icon_get() — source/modules/common/purr_icons.h, exposed through
// the claw import table. Returns an opaque `const void *` this file
// hands straight to lv_img_set_src() without ever looking inside it.
extern const void *purr_icon_get(const char *name);

// purr_kernel_notify_count()/_at() — real content for the notification
// panel. launcher_notification_t mirrors purr_kernel.h's real
// purr_notification_t struct — a deliberate exception to this file's
// "opaque pointers only" rule for LVGL types, since it's this codebase's
// OWN struct: three fixed-size char arrays, no pointers, no compile-
// flag-dependent layout the way an LVGL struct might have.
#define NOTIFY_TITLE_LEN  32
#define NOTIFY_BODY_LEN   64
#define NOTIFY_SOURCE_LEN 16
typedef struct {
    char     title[NOTIFY_TITLE_LEN];
    char     body[NOTIFY_BODY_LEN];
    char     source[NOTIFY_SOURCE_LEN];
    uint64_t timestamp_ms;
} launcher_notification_t;
extern int  purr_kernel_notify_count(void);
extern bool purr_kernel_notify_at(int idx, launcher_notification_t *out);

// purr_lv_style_shade_bg()/_row_text() — source/modules/common/purr_lv_
// style.h, apply real lv_color_t values this file never touches itself
// (see that header's own top comment on why that boundary matters).
// Both take/return only the same opaque lv_obj_t* already established as
// safe everywhere else in this file.
extern void purr_lv_style_shade_bg(lv_obj_t *obj);   // pure black — notification panel bg
extern void purr_lv_style_row_text(lv_obj_t *obj);   // white text — readable on that black bg
extern void purr_lv_style_card(lv_obj_t *obj);       // lighter grey, rounded — one rectangle per notification
extern void purr_lv_style_border(lv_obj_t *obj);     // 1px grey border — makes each card's edge visible
extern void purr_lv_style_handle(lv_obj_t *obj);     // grey pill — the notification panel's own grab handle
extern void purr_lv_style_white_bg(lv_obj_t *obj);    // pure white — Recents card's own bottom strip
extern void purr_lv_style_strip_text(lv_obj_t *obj);  // black text — readable on that white strip

// lv_obj_set_style_pad_left/_right — zero the default theme's own
// container padding (CoreOS/managed_components/lvgl__lvgl/src/extra/
// themes/default/lv_theme_default.c's own PAD_DEF, 16-24px depending on
// display size — a plain lv_obj_create() is themed as a "card" there,
// which carries this non-zero default) on s_notif_panel and each
// notification card. Found live: a card's own right edge rendered well
// past its explicit lv_obj_set_size() width — left unfixed, LVGL
// positions/sizes a plain lv_obj_create()'s CHILDREN relative to that
// parent's CONTENT box, which default padding insets from the parent's
// true outer edge; a card at "local x=8" was actually landing at
// roughly pad_left(~20) + 8 = 28 globally, and 28 + CARD_W ran past the
// screen's own 320px edge. Zeroing padding here makes lv_obj_set_pos()'s
// numbers in this file mean what they look like they mean.
extern void lv_obj_set_style_pad_left(lv_obj_t *obj, lv_coord_t value, uint32_t selector);
extern void lv_obj_set_style_pad_right(lv_obj_t *obj, lv_coord_t value, uint32_t selector);

// lv_obj_set_scroll_dir() — restricts s_notif_panel's own native
// scrolling to vertical only (see NOTIF_H's own comment).
extern void lv_obj_set_scroll_dir(lv_obj_t *obj, lv_dir_t dir);

// lv_label_set_long_mode() — truncates a card's title/body text instead
// of letting it overflow past the card's own edge (see CARD_TEXT_W's own
// comment on the real bug this fixes). lv_label_long_mode_t is a plain
// scalar enum (typedef'd int here, same as lv_event_code_t elsewhere in
// this file) — confirmed against lv_label.h's own enum before relying on
// LV_LABEL_LONG_DOT_VALUE's value: WRAP=0, DOT=1, SCROLL=2,
// SCROLL_CIRCULAR=3, CLIP=4.
typedef int lv_label_long_mode_t;
extern void lv_label_set_long_mode(lv_obj_t *obj, lv_label_long_mode_t long_mode);
#define LV_LABEL_LONG_DOT_VALUE 1

// Grid geometry — fixed, not computed from the real screen size. Sized
// for T-Deck Plus's known 320x240: 3 columns x 2 rows = 6 tiles/page.
// TOP_BAR_H/BOTTOM_BAR_H reserve systemui_lvgl.c's own status row and
// Home bar (must match that file's own TOP_BAR_H/HOME_BAR_HIT_H).
#define SCREEN_W        320
#define SCREEN_H        240
#define TOP_BAR_H       20
#define BOTTOM_BAR_H    20
#define TILE_SIZE       80
#define TILE_MARGIN     8
#define GRID_COLS       3
#define GRID_ROWS       2
#define TILES_PER_PAGE  (GRID_COLS * GRID_ROWS)
#define TILE_NAME_MAX   32
#define ICON_SIZE       32
#define GRID_START_X    ((SCREEN_W - (GRID_COLS * TILE_SIZE + (GRID_COLS - 1) * TILE_MARGIN)) / 2)
#define GRID_START_Y    (TOP_BAR_H + TILE_MARGIN)

// Page-dot indicator — small squares just below the grid, opacity-only
// active/inactive distinction (LV_OPA_COVER vs LV_OPA_DIM) so this needs
// no color at all, same "plain scalar, no lv_color_t" safety as
// everything else here. Capped at MAX_PAGE_DOTS persistent objects,
// reused the same way the tiles themselves are — good for up to 30 apps
// (5 pages x 6 tiles), plenty for this device today.
#define MAX_PAGE_DOTS  5
#define DOT_SIZE       6
#define DOT_GAP        6
#define DOT_Y          (GRID_START_Y + GRID_ROWS * (TILE_SIZE + TILE_MARGIN) + 4)

// Notification panel — full screen width (2026-09-17: narrowed to 80%
// then widened back by direct request, "why is it not full width of the
// display"); height is the top 75% of the screen's own upper half
// (SCREEN_H/2 * 75% = 90), sitting right below the status bar rather
// than at y=0 so it doesn't run under systemui's own top row. Each
// notification is its own card (see CARD_H's own comment on where that
// look came from) inside this panel, which is left SCROLLABLE —
// restricted to VERTICAL only (lv_obj_set_scroll_dir() in claw_personal_
// init() below), by direct request ("scrollable list up and down, not
// side to side"). A grab handle at the bottom (see HANDLE_W's own
// comment) is the one visible affordance for closing it.
#define NOTIF_W  SCREEN_W
#define NOTIF_H  ((SCREEN_H / 2) * 75 / 100)
#define NOTIF_X  0
#define NOTIF_Y  TOP_BAR_H
// One real rectangle per notification, adapted from systemui_ios.c's own
// build_notif_card() look (Mochi's own backend) — lighter grey + a
// visible border so each reads as its own card against the panel's
// black background. Title/body labels are WIDTH-CONSTRAINED and
// truncated with LV_LABEL_LONG_DOT_VALUE rather than left to overflow —
// found live: a long notification's text ran straight past the card's
// own right edge with nothing stopping it, which read as the whole
// panel being "cut off on one side" even though the panel itself was
// correctly sized. No swipe-to-dismiss — not asked for, and the whole
// panel is a native scroll view instead.
#define CARD_W       (NOTIF_W - 16)
#define CARD_H       36
#define CARD_GAP     6
#define CARD_AREA_Y  26   // below the "Notifications" title
#define CARD_TEXT_W  (CARD_W - 12)   // label width inside a card, leaving 6px padding each side

// lv_label_set_long_mode(LV_LABEL_LONG_DOT) + a fixed lv_obj_set_size()
// on the label was the first fix attempted here — found live it still
// wasn't reliably stopping long body text (NOTIFY_BODY_LEN allows up to
// 63 characters, and at this font size that's comfortably wider than
// CARD_TEXT_W) from drawing past the card's own right edge. Truncating
// the STRING ITSELF before it ever reaches lv_label_set_text() removes
// any dependency on exactly how/when LVGL's own long-mode recalculates
// truncation — a plain snprintf can't overflow the width it's given,
// full stop. MAX_CARD_CHARS=30 is a conservative average-character-width
// estimate for CARD_TEXT_W at this font size, not a measured exact fit —
// same "close enough, not pixel-measured" precedent this file's own
// fixed x/y positions already use throughout.
#define MAX_CARD_CHARS 30

// Copies at most MAX_CARD_CHARS of `in` into `out` (size out_sz),
// appending "..." if it had to cut anything — same visual convention
// LV_LABEL_LONG_DOT itself uses, just computed here instead of trusted
// to compute itself.
static void truncate_for_card(char *out, size_t out_sz, const char *in)
{
    size_t len = strlen(in);
    if (len <= MAX_CARD_CHARS) {
        snprintf(out, out_sz, "%s", in);
        return;
    }
    snprintf(out, out_sz, "%.*s...", MAX_CARD_CHARS - 3, in);
}

// Grab handle — purely decorative (see purr_lv_style_handle()'s own
// comment on why this doesn't actually drag), centered at the panel's
// own bottom edge, by direct request ("add a handle to the shade for me
// to be able to grab and close it").
#define HANDLE_W  40
#define HANDLE_H  5

// Recents ("running apps") page — one extra view tacked onto the END of
// the ordinary grid-page cycle (see s_page's own comment below on how
// that's modeled), reached by swiping left past the last grid page, by
// direct request. Its own 2x2 grid (NOT the app grid's 3x2 — by direct
// request), own geometry, own real card background (see purr_lv_style_
// card() below) so a Recents square reads as its own solid tile rather
// than the app grid's bare icon+label look. Filtered to app_manager_
// entry_running() instead of the whole catalog. Capped at RECENTS_MAX (one
// page's worth) — no second recents page; a 5th running app just doesn't
// show, same "good enough for this device today" cap MAX_PAGE_DOTS's own
// comment already accepts elsewhere in this file.
#define RECENTS_COLS   2
#define RECENTS_ROWS   2
#define RECENTS_MAX    (RECENTS_COLS * RECENTS_ROWS)
// Cards fill almost the whole screen edge-to-edge (BB10 Active Frames
// reference, by direct request: "make the square bigger and more evenly
// laid out, more like this") — thin, EVEN margins on all four sides and
// between cards, rather than the earlier fixed-square tiles sitting in a
// sea of unused margin. Not forced to a perfect square: the available
// width (320px, minus margins) and height (200px between the two bars,
// minus margins) aren't equal, so a card here is sized independently on
// each axis to actually fill that space, same as the reference's own
// cards aren't perfect squares either.
#define RECENTS_OUTER_MARGIN 6
#define RECENTS_GAP          6
#define RECENTS_TILE_W  ((SCREEN_W - 2 * RECENTS_OUTER_MARGIN - (RECENTS_COLS - 1) * RECENTS_GAP) / RECENTS_COLS)
#define RECENTS_TILE_H  (((SCREEN_H - TOP_BAR_H - BOTTOM_BAR_H) - 2 * RECENTS_OUTER_MARGIN - (RECENTS_ROWS - 1) * RECENTS_GAP) / RECENTS_ROWS)
#define RECENTS_START_X RECENTS_OUTER_MARGIN
#define RECENTS_START_Y (TOP_BAR_H + RECENTS_OUTER_MARGIN)
// Thin bottom strip — 10px exactly, by direct request. Holds the app
// name (left) and the close button (right), both bottom-aligned in the
// card the same way the strip itself is.
#define STRIP_H      10
#define CLOSE_W      14
#define CLOSE_H      STRIP_H

static lv_obj_t *s_tiles[TILES_PER_PAGE];
static lv_obj_t *s_icons[TILES_PER_PAGE];
static lv_obj_t *s_labels[TILES_PER_PAGE];
static lv_obj_t *s_dots[MAX_PAGE_DOTS];
static lv_obj_t *s_notif_panel  = NULL;
static lv_obj_t *s_notif_handle = NULL;

// Recents widgets — persistent, reused across refreshes exactly like
// s_tiles[]/s_icons[]/s_labels[] above (never destroyed/recreated on a
// normal refresh, only hidden/shown/relabeled).
static lv_obj_t *s_recent_cards[RECENTS_MAX];
static lv_obj_t *s_recent_icons[RECENTS_MAX];
static lv_obj_t *s_recent_names[RECENTS_MAX];
static lv_obj_t *s_recent_close[RECENTS_MAX];
static lv_obj_t *s_recent_empty = NULL;
// The REAL app_manager index behind each visible recents slot — recents
// is a FILTERED view (only running apps), so slot N is almost never
// app_manager index N the way a grid tile's index arithmetic works; this
// is what recent_card_click_cb()/recent_close_click_cb() actually launch/
// stop.
static int s_recent_idx[RECENTS_MAX];

// One extra logical "page" past the real grid pages (index == page_
// count()) is the Recents view — see recents_active()/switch_page()
// below for the two places that actually interpret this. Keeping it as
// one int rather than a separate bool+page pair means the existing
// LEFT/RIGHT wraparound math in gesture_cb() below needs no special-
// casing at all: recents just naturally sits at the end of the cycle.
static int  s_page       = 0;
static bool s_notif_open = false;

// The ONE place that decides which library icon (purr_icons.h) each app
// gets — not every app maps to something in this deliberately small icon
// set; anything unmapped just falls back to label-only.
static const char *icon_name_for_app(const char *app_name)
{
    if (strcmp(app_name, "calculator")  == 0) return "calculator";
    if (strcmp(app_name, "clock")       == 0) return "clock";
    if (strcmp(app_name, "diagnostics") == 0) return "settings";
    if (strcmp(app_name, "settings")    == 0) return "settings";   // same gear as diagnostics — this icon set has only one; a real second icon is future work, not a bug
    if (strcmp(app_name, "notepad")     == 0) return "file_manager";
    return NULL;
}

static int page_count(void)
{
    int count = app_manager_count();
    int pages = (count + TILES_PER_PAGE - 1) / TILES_PER_PAGE;
    if (pages < 1) pages = 1;
    if (pages > MAX_PAGE_DOTS) pages = MAX_PAGE_DOTS;   // real cap — see MAX_PAGE_DOTS's own comment
    return pages;
}

// Updates all six tile slots' icon/label/visibility for the CURRENT
// s_page — never creates or destroys a widget, only changes what the
// already-built ones show.
static void refresh_page(void)
{
    int count = app_manager_count();
    for (int slot = 0; slot < TILES_PER_PAGE; slot++) {
        int idx = s_page * TILES_PER_PAGE + slot;
        if (idx >= count) {
            lv_obj_add_flag(s_tiles[slot], LV_OBJ_FLAG_HIDDEN_VALUE);
            continue;
        }
        lv_obj_clear_flag(s_tiles[slot], LV_OBJ_FLAG_HIDDEN_VALUE);

        char name[TILE_NAME_MAX];
        app_manager_entry_name(idx, name, sizeof(name));
        lv_label_set_text(s_labels[slot], name);
        // lv_img_set_src(obj, NULL) is a real, safe no-op in this LVGL
        // version (clears the image rather than crashing) — confirmed
        // before relying on it here.
        lv_img_set_src(s_icons[slot], purr_icon_get(icon_name_for_app(name)));
    }
}

// Updates the dot row for the CURRENT s_page/page_count() — same "reuse
// built widgets, never destroy" shape as refresh_page().
static void refresh_dots(void)
{
    int pages = page_count();
    for (int i = 0; i < MAX_PAGE_DOTS; i++) {
        if (i >= pages) {
            lv_obj_add_flag(s_dots[i], LV_OBJ_FLAG_HIDDEN_VALUE);
            continue;
        }
        lv_obj_clear_flag(s_dots[i], LV_OBJ_FLAG_HIDDEN_VALUE);
        lv_obj_set_style_bg_opa(s_dots[i], (i == s_page) ? LV_OPA_COVER_VALUE : LV_OPA_DIM_VALUE, 0);
    }
}

// True while s_page is sitting on the one extra "page" past the real
// grid pages — see s_page's own comment above.
static bool recents_active(void)
{
    return s_page == page_count();
}

// Hides every grid tile + the page-dot row — the state Recents needs the
// screen in while it's showing (same shape as hide_notif() hiding the
// grid's own siblings, just for a different pair of views).
static void hide_grid(void)
{
    for (int slot = 0; slot < TILES_PER_PAGE; slot++) {
        lv_obj_add_flag(s_tiles[slot], LV_OBJ_FLAG_HIDDEN_VALUE);
    }
    for (int i = 0; i < MAX_PAGE_DOTS; i++) {
        lv_obj_add_flag(s_dots[i], LV_OBJ_FLAG_HIDDEN_VALUE);
    }
}

// Rebuilds every recents slot's icon/name/visibility — same "reuse
// already-built widgets, never destroy" shape as refresh_page(), just
// filtered to app_manager_entry_running() instead of the whole catalog.
// Run fresh every time Recents is entered (not per frame — see
// show_recents()) so a just-closed app disappears immediately and a
// just-launched one shows up the next time this page is visited.
static void refresh_recents(void)
{
    int shown = 0;
    int cat_count = app_manager_count();
    for (int cat_idx = 0; cat_idx < cat_count && shown < RECENTS_MAX; cat_idx++) {
        if (!app_manager_entry_running(cat_idx)) continue;

        char name[TILE_NAME_MAX];
        app_manager_entry_name(cat_idx, name, sizeof(name));

        s_recent_idx[shown] = cat_idx;
        lv_obj_clear_flag(s_recent_cards[shown], LV_OBJ_FLAG_HIDDEN_VALUE);
        lv_label_set_text(s_recent_names[shown], name);
        lv_img_set_src(s_recent_icons[shown], purr_icon_get(icon_name_for_app(name)));
        shown++;
    }
    for (int slot = shown; slot < RECENTS_MAX; slot++) {
        lv_obj_add_flag(s_recent_cards[slot], LV_OBJ_FLAG_HIDDEN_VALUE);
    }

    if (shown == 0) {
        lv_obj_clear_flag(s_recent_empty, LV_OBJ_FLAG_HIDDEN_VALUE);
    } else {
        lv_obj_add_flag(s_recent_empty, LV_OBJ_FLAG_HIDDEN_VALUE);
    }
}

static void show_recents(void)
{
    hide_grid();
    refresh_recents();
}

// Leaving Recents for a real grid page — the mirror of show_recents():
// hide every recents slot, then let the caller's own refresh_page()/
// refresh_dots() (already called right after switch_page() everywhere
// below) repopulate the grid.
static void hide_recents(void)
{
    for (int slot = 0; slot < RECENTS_MAX; slot++) {
        lv_obj_add_flag(s_recent_cards[slot], LV_OBJ_FLAG_HIDDEN_VALUE);
    }
    lv_obj_add_flag(s_recent_empty, LV_OBJ_FLAG_HIDDEN_VALUE);
}

// The one place s_page actually changes and the screen gets told about
// it — used by gesture_cb()'s LEFT/RIGHT handling below instead of each
// call site re-deriving "am I entering/leaving Recents" by hand.
static void switch_page(int new_page)
{
    s_page = new_page;
    if (recents_active()) {
        show_recents();
    } else {
        hide_recents();
        refresh_page();
        refresh_dots();
    }
}

// Bare tap target — no background fill, no border, not scrollable. A
// tile is nothing but its own icon+label children.
static lv_obj_t *make_bare_target(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                                   lv_coord_t w, lv_coord_t h,
                                   void (*cb)(lv_event_t *e), void *user_data)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE_VALUE);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP_VALUE, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_add_event_cb(obj, cb, LV_EVENT_CLICKED_VALUE, user_data);
    return obj;
}

static void tile_click_cb(lv_event_t *e)
{
    void *user_data = lv_event_get_user_data(e);
    int slot = (int)(intptr_t)user_data;
    int idx = s_page * TILES_PER_PAGE + slot;
    if (idx < app_manager_count()) app_manager_launch_idx(idx);
}

// Tapping a Recents square's own body (NOT its close button — a child
// object with its own CLICKED handler below, which LVGL delivers events
// to directly rather than also bubbling up to this parent's handler,
// same non-bubbling default s_notif_handle's own comment already relies
// on) re-shows that already-running app — app_manager_launch_idx() on an
// already-RUNNING entry re-shows its tracked window rather than
// relaunching it (app_manager_launch_path()'s own fix for exactly this).
static void recent_card_click_cb(lv_event_t *e)
{
    void *user_data = lv_event_get_user_data(e);
    int slot = (int)(intptr_t)user_data;
    app_manager_launch_idx(s_recent_idx[slot]);
}

// The close button — stops the app, then immediately rebuilds this page
// so the closed square disappears without needing a full page re-visit.
static void recent_close_click_cb(lv_event_t *e)
{
    void *user_data = lv_event_get_user_data(e);
    int slot = (int)(intptr_t)user_data;
    app_manager_stop(s_recent_idx[slot]);
    refresh_recents();
}

// Rebuilds the notification panel's content — one real card rectangle
// per notification, stacked with manually-incremented Y positions (no
// flex-layout import in this file — see this file's own top comment on
// why the import list stays this short). LVGL's own native scrolling
// (the panel's SCROLLABLE flag is never cleared, restricted to vertical
// only — see claw_personal_init() below) takes over automatically once
// the stacked cards are taller than NOTIF_H; nothing else needed for
// that. Full clean+rebuild on every open rather than persistent/reused
// widgets — simpler, and this only ever runs on a real open, not per
// frame.
static void refresh_notif_panel(void)
{
    if (!s_notif_panel) return;
    lv_obj_clean(s_notif_panel);

    lv_obj_t *title = lv_label_create(s_notif_panel);
    lv_label_set_text(title, "Notifications");
    lv_obj_set_pos(title, 8, 6);
    purr_lv_style_row_text(title);

    int n = purr_kernel_notify_count();
    if (n == 0) {
        lv_obj_t *empty = lv_label_create(s_notif_panel);
        lv_label_set_text(empty, "No new notifications");
        lv_obj_set_pos(empty, 8, CARD_AREA_Y);
        purr_lv_style_row_text(empty);
    } else {
        for (int i = 0; i < n; i++) {
            launcher_notification_t note;
            if (!purr_kernel_notify_at(i, &note)) break;

            lv_obj_t *card = lv_obj_create(s_notif_panel);
            lv_obj_set_pos(card, 8, (lv_coord_t)(CARD_AREA_Y + i * (CARD_H + CARD_GAP)));
            lv_obj_set_size(card, CARD_W, CARD_H);
            lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE_VALUE);
            lv_obj_set_style_pad_left(card, 0, 0);
            lv_obj_set_style_pad_right(card, 0, 0);
            purr_lv_style_card(card);
            purr_lv_style_border(card);

            char title_buf[MAX_CARD_CHARS + 4];
            truncate_for_card(title_buf, sizeof(title_buf), note.title);
            lv_obj_t *title_lbl = lv_label_create(card);
            lv_label_set_text(title_lbl, title_buf);
            lv_obj_set_pos(title_lbl, 6, 3);
            lv_obj_set_size(title_lbl, CARD_TEXT_W, 14);
            lv_label_set_long_mode(title_lbl, LV_LABEL_LONG_DOT_VALUE);
            purr_lv_style_row_text(title_lbl);

            char body_buf[MAX_CARD_CHARS + 4];
            truncate_for_card(body_buf, sizeof(body_buf), note.body);
            lv_obj_t *body_lbl = lv_label_create(card);
            lv_label_set_text(body_lbl, body_buf);
            lv_obj_set_pos(body_lbl, 6, 19);
            lv_obj_set_size(body_lbl, CARD_TEXT_W, 14);
            lv_label_set_long_mode(body_lbl, LV_LABEL_LONG_DOT_VALUE);
            purr_lv_style_row_text(body_lbl);
        }
    }
}

static void show_notif(void)
{
    s_notif_open = true;
    refresh_notif_panel();
    lv_obj_clear_flag(s_notif_panel, LV_OBJ_FLAG_HIDDEN_VALUE);
    lv_obj_clear_flag(s_notif_handle, LV_OBJ_FLAG_HIDDEN_VALUE);
}

static void hide_notif(void)
{
    s_notif_open = false;
    lv_obj_add_flag(s_notif_panel, LV_OBJ_FLAG_HIDDEN_VALUE);
    lv_obj_add_flag(s_notif_handle, LV_OBJ_FLAG_HIDDEN_VALUE);
}

// Fires on the SCREEN (see LV_OBJ_FLAG_GESTURE_BUBBLE_VALUE's own
// comment on why it lands here and not on a tile) for any swipe that
// started somewhere in the grid. Left/right change pages; down opens
// the notification panel, up closes it — nothing else, on purpose (no
// Home/Back gesture here at all; see this file's own top comment).
static void gesture_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);

    if (dir == LV_DIR_DOWN_VALUE) {
        if (!s_notif_open) show_notif();
        return;
    }
    if (dir == LV_DIR_UP_VALUE) {
        if (s_notif_open) hide_notif();
        return;
    }
    if (s_notif_open) return;   // left/right paging makes no sense while the panel covers the grid

    // +1: one extra logical page past the real grid pages is Recents
    // (see s_page's own comment) — LEFT past the last grid page lands
    // there; LEFT again wraps back to page 0, same as any other page.
    int views = page_count() + 1;
    if (dir == LV_DIR_LEFT_VALUE) {
        switch_page((s_page + 1) % views);
    } else if (dir == LV_DIR_RIGHT_VALUE) {
        switch_page((s_page - 1 + views) % views);
    }
}

int claw_personal_init(void)
{
    lv_disp_t *disp = lv_disp_get_default();
    if (!disp) return -1;   // display driver not registered — see kernel_tdp_boot.c's lvgl_hw_init()
    lv_obj_t *scr = lv_disp_get_scr_act(disp);
    if (!scr) return -1;

    // Wipe whatever the previous screen (loginUI) left on the default
    // screen.
    lv_obj_clean(scr);

    s_page = 0;
    s_notif_open = false;

    for (int slot = 0; slot < TILES_PER_PAGE; slot++) {
        int col = slot % GRID_COLS;
        int row = slot / GRID_COLS;

        lv_obj_t *tile = make_bare_target(scr,
            GRID_START_X + col * (TILE_SIZE + TILE_MARGIN),
            GRID_START_Y + row * (TILE_SIZE + TILE_MARGIN),
            TILE_SIZE, TILE_SIZE,
            tile_click_cb, (void *)(intptr_t)slot);
        s_tiles[slot] = tile;
        lv_obj_add_flag(tile, LV_OBJ_FLAG_GESTURE_BUBBLE_VALUE);

        lv_obj_t *icon = lv_img_create(tile);
        lv_obj_set_pos(icon, (TILE_SIZE - ICON_SIZE) / 2, 6);
        s_icons[slot] = icon;
        lv_obj_add_flag(icon, LV_OBJ_FLAG_GESTURE_BUBBLE_VALUE);

        lv_obj_t *label = lv_label_create(tile);
        lv_obj_set_pos(label, 2, 6 + ICON_SIZE + 2);
        s_labels[slot] = label;
        lv_obj_add_flag(label, LV_OBJ_FLAG_GESTURE_BUBBLE_VALUE);
    }

    // Page dots — centered under the grid.
    {
        int pages = page_count();
        lv_coord_t total_w = (lv_coord_t)(pages * DOT_SIZE + (pages - 1) * DOT_GAP);
        lv_coord_t start_x = (lv_coord_t)((SCREEN_W - total_w) / 2);
        for (int i = 0; i < MAX_PAGE_DOTS; i++) {
            lv_obj_t *dot = lv_obj_create(scr);
            lv_obj_set_pos(dot, (lv_coord_t)(start_x + i * (DOT_SIZE + DOT_GAP)), DOT_Y);
            lv_obj_set_size(dot, DOT_SIZE, DOT_SIZE);
            lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE_VALUE);
            lv_obj_set_style_border_width(dot, 0, 0);
            s_dots[i] = dot;
        }
    }

    // Recents ("running apps") page — its own 2x2 grid, own RECENTS_*
    // geometry (see that block's own comment on why this isn't the app
    // grid's 3x2 layout). Every card starts HIDDEN — switch_page()/
    // refresh_recents() is what ever reveals one, exactly like the grid
    // tiles' own initial state before refresh_page() runs.
    for (int slot = 0; slot < RECENTS_MAX; slot++) {
        int col = slot % RECENTS_COLS;
        int row = slot / RECENTS_COLS;
        lv_coord_t x = (lv_coord_t)(RECENTS_START_X + col * (RECENTS_TILE_W + RECENTS_GAP));
        lv_coord_t y = (lv_coord_t)(RECENTS_START_Y + row * (RECENTS_TILE_H + RECENTS_GAP));

        // The card itself — a real card background (purr_lv_style_card(),
        // by direct request: "add a background to the apps", was
        // transparent before) plus a border so it reads as a solid tile
        // against the wallpaper rather than just an icon floating on it.
        lv_obj_t *card = lv_obj_create(scr);
        lv_obj_set_pos(card, x, y);
        lv_obj_set_size(card, RECENTS_TILE_W, RECENTS_TILE_H);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE_VALUE);
        lv_obj_set_style_pad_left(card, 0, 0);
        lv_obj_set_style_pad_right(card, 0, 0);
        purr_lv_style_card(card);
        purr_lv_style_border(card);
        lv_obj_add_flag(card, LV_OBJ_FLAG_HIDDEN_VALUE);
        // Gesture-bubble on the card AND its icon (not the strip/close
        // button — those are real tap targets already, same reasoning
        // as the grid tiles above) so a swipe starting on a card's own
        // icon area still reaches gesture_cb() on the screen to page
        // back out of Recents, same as the grid tiles' own comment.
        lv_obj_add_flag(card, LV_OBJ_FLAG_GESTURE_BUBBLE_VALUE);
        lv_obj_add_event_cb(card, recent_card_click_cb, LV_EVENT_CLICKED_VALUE, (void *)(intptr_t)slot);
        s_recent_cards[slot] = card;

        lv_obj_t *icon = lv_img_create(card);
        lv_obj_set_pos(icon, (RECENTS_TILE_W - ICON_SIZE) / 2, (RECENTS_TILE_H - STRIP_H - ICON_SIZE) / 2);
        lv_obj_add_flag(icon, LV_OBJ_FLAG_GESTURE_BUBBLE_VALUE);
        s_recent_icons[slot] = icon;

        // The bottom strip — "whole strip in white" by direct request,
        // full tile width, thin (10-13px — enough room for the name and
        // the close button, nothing more). A plain lv_obj_create() child
        // rather than styling the card's own bottom edge directly, so it
        // reads as visually separate from the card background above it.
        lv_obj_t *strip = lv_obj_create(card);
        lv_obj_set_pos(strip, 0, RECENTS_TILE_H - STRIP_H);
        lv_obj_set_size(strip, RECENTS_TILE_W, STRIP_H);
        lv_obj_clear_flag(strip, LV_OBJ_FLAG_SCROLLABLE_VALUE);
        lv_obj_set_style_pad_left(strip, 0, 0);
        lv_obj_set_style_pad_right(strip, 0, 0);
        lv_obj_set_style_border_width(strip, 0, 0);
        purr_lv_style_white_bg(strip);

        // App name — left side of the strip, truncated the same manual
        // way notification cards already are (see truncate_for_card()'s
        // own comment) rather than trusted to LVGL's own long-mode.
        lv_obj_t *name_lbl = lv_label_create(strip);
        lv_obj_set_pos(name_lbl, 2, 0);
        lv_obj_set_size(name_lbl, RECENTS_TILE_W - CLOSE_W - 4, STRIP_H);
        lv_label_set_long_mode(name_lbl, LV_LABEL_LONG_DOT_VALUE);
        purr_lv_style_strip_text(name_lbl);
        s_recent_names[slot] = name_lbl;

        // Close button — right side of the strip, its own CLICKED
        // handler (see recent_card_click_cb()'s own comment on why this
        // doesn't also fire the card's handler).
        lv_obj_t *close_btn = lv_obj_create(strip);
        lv_obj_set_pos(close_btn, RECENTS_TILE_W - CLOSE_W, 0);
        lv_obj_set_size(close_btn, CLOSE_W, CLOSE_H);
        lv_obj_clear_flag(close_btn, LV_OBJ_FLAG_SCROLLABLE_VALUE);
        lv_obj_set_style_bg_opa(close_btn, LV_OPA_TRANSP_VALUE, 0);
        lv_obj_set_style_border_width(close_btn, 0, 0);
        lv_obj_add_event_cb(close_btn, recent_close_click_cb, LV_EVENT_CLICKED_VALUE, (void *)(intptr_t)slot);
        s_recent_close[slot] = close_btn;

        lv_obj_t *close_lbl = lv_label_create(close_btn);
        lv_label_set_text(close_lbl, "X");
        lv_obj_set_pos(close_lbl, 4, 0);
        purr_lv_style_strip_text(close_lbl);
    }

    s_recent_empty = lv_label_create(scr);
    lv_label_set_text(s_recent_empty, "No running apps");
    lv_obj_set_pos(s_recent_empty, RECENTS_START_X, RECENTS_START_Y);
    lv_obj_add_flag(s_recent_empty, LV_OBJ_FLAG_HIDDEN_VALUE);

    // Notification panel — the one object in this file left SCROLLABLE
    // (see this file's own top comment on why). Real black background so
    // it reads as a solid panel over the wallpaper, not a hole showing
    // the photo through it — white content text is what actually shows
    // against that black.
    s_notif_panel = lv_obj_create(scr);
    lv_obj_set_pos(s_notif_panel, NOTIF_X, NOTIF_Y);
    lv_obj_set_size(s_notif_panel, NOTIF_W, NOTIF_H);
    purr_lv_style_shade_bg(s_notif_panel);
    lv_obj_set_style_pad_left(s_notif_panel, 0, 0);
    lv_obj_set_style_pad_right(s_notif_panel, 0, 0);
    lv_obj_set_scroll_dir(s_notif_panel, LV_DIR_VER_VALUE);
    lv_obj_add_flag(s_notif_panel, LV_OBJ_FLAG_HIDDEN_VALUE);
    lv_obj_add_flag(s_notif_panel, LV_OBJ_FLAG_GESTURE_BUBBLE_VALUE);

    // Grab handle — a SIBLING of s_notif_panel (not a child), created
    // right after it so it draws on top of the panel's own bottom edge —
    // deliberately NOT a child: a child would scroll away with the
    // notification cards the moment the list scrolls, and the whole
    // point of a handle is that it's always there to grab. Shown/hidden
    // together with the panel in show_notif()/hide_notif().
    // Deliberately left CLICKABLE (lv_obj_create()'s own default — not
    // cleared here, unlike every bare tile/dot elsewhere in this file):
    // a touch starting exactly on the handle needs to be captured BY the
    // handle itself, not fall through to the scrollable panel behind it.
    // Not scrollable itself, so LVGL never tries to interpret a drag
    // here as an internal scroll the way it might on the panel's own
    // content — a swipe starting on the handle bubbles cleanly to the
    // screen every time, which is what makes it the RELIABLE close
    // point this file's own top comment promises, rather than just a
    // visual hint sitting on top of the same ambiguity as the rest of
    // the panel.
    s_notif_handle = lv_obj_create(scr);
    lv_obj_set_pos(s_notif_handle, (SCREEN_W - HANDLE_W) / 2, NOTIF_Y + NOTIF_H - HANDLE_H - 4);
    lv_obj_set_size(s_notif_handle, HANDLE_W, HANDLE_H);
    lv_obj_clear_flag(s_notif_handle, LV_OBJ_FLAG_SCROLLABLE_VALUE);
    purr_lv_style_handle(s_notif_handle);
    lv_obj_add_flag(s_notif_handle, LV_OBJ_FLAG_HIDDEN_VALUE);
    lv_obj_add_flag(s_notif_handle, LV_OBJ_FLAG_GESTURE_BUBBLE_VALUE);

    // Swipe left/right/up/down — gesture_cb() fires here, on the screen
    // itself, because the screen is the first ancestor in the bubble
    // chain WITHOUT LV_OBJ_FLAG_GESTURE_BUBBLE_VALUE set.
    lv_obj_add_event_cb(scr, gesture_cb, LV_EVENT_GESTURE_VALUE, NULL);

    refresh_page();
    refresh_dots();

    return 0;
}

void claw_personal_deinit(void)
{
    // Called on every relock: kernel_tdp_boot.c's session loop unloads
    // the launcher before reloading loginUI. No heap allocation of our
    // own to free, no registered catcall_ui_t — nothing to actually tear
    // down. The tiles themselves are left in place deliberately: the
    // NEXT thing to touch the default screen (loginUI, on relock)
    // already clears it via its own lv_obj_clean() at init.
}

#endif // SYSCLAW_BACKEND_LVGL
