// systemui_ios.c — iOS-style system UI.
//
// Implements the same purr_systemui_* contract as systemui_android.c (see
// systemui.h); exactly one of the two compiles into a build, chosen by the
// CONFIG_PURR_SYSTEMUI_STYLE_* Kconfig choice. Hosts are unaffected either
// way — Mochi, Cupcake and Tabby all call the identical API.
//
// ── What differs from the Android style ─────────────────────────────────────
//   * No Back/Home/Recents nav bar at all. iOS uses a home indicator, which
//     belongs to the host's own shell (Mochi draws one), so
//     purr_systemui_navbar_height() is always 0 here and suppress_navbar is
//     moot rather than honoured.
//   * Notifications are CARDS, not text rows — rounded, with the posting app's
//     icon and accent colour, a relative timestamp, title and body.
//   * That card is built by ONE function, build_notif_card(), used by both the
//     Notification Center and the lock screen. That is the whole point: on iOS
//     a notification looks identical wherever it appears, so making the lock
//     screen and the shade share a builder is what makes them match, rather
//     than two styles that drift apart.
//   * Both notification surfaces scroll the same way the Recents carousel
//     does — a plain vertical scroll container with the scrollbar hidden and
//     elastic scrolling off, so a flick behaves consistently everywhere.
//
// ── What is kept from the Android style ─────────────────────────────────────
// The two drag-down hotzones (left half = Notification Center, right half =
// Control Center) and their drag/settle mechanics. That gesture model is
// already proven on this hardware and maps cleanly onto iOS's own
// notification-vs-control split, so re-deriving it would be churn.
//
// ── No wall clock ───────────────────────────────────────────────────────────
// There is no RTC or NTP anywhere in this codebase (checked: no sntp usage,
// and catcall_gps_t carries no time field), so every "time" shown here is
// elapsed uptime formatted as a clock. The lock screen says so in its subtitle
// rather than silently implying it is the real time.

#include "systemui.h"
#include "sdkconfig.h"

#if defined(CONFIG_PURR_SYSTEMUI) && defined(CONFIG_PURR_SYSTEMUI_STYLE_IOS)

#include "../../kernel/core/purr_kernel.h"
#include "../../kernel/catcalls/purr_win.h"
#include "../app_manager/app_manager.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <stdio.h>
#include <string.h>

#ifdef CONFIG_PURR_FEATURE_MESHTASTIC
#include "../meshtastic/meshtastic.h"
#endif

static const char *TAG = "systemui.ios";

static const purr_systemui_host_t *s_host = NULL;

// ── Palette ─────────────────────────────────────────────────────────────────
// iOS chrome sits on a dark scrim with light frosted cards on top. Values are
// Apple's system colours where there is a direct equivalent.
#define COL_SCRIM        lv_color_black()
#define COL_CARD         lv_color_hex(0xFFFFFF)
#define COL_CARD_TEXT    lv_color_hex(0x1C1C1E)   // label
#define COL_CARD_SUB     lv_color_hex(0x8E8E93)   // secondaryLabel
#define COL_STATUS_TEXT  lv_color_hex(0x1C1C1E)
#define COL_LOCK_TEXT    lv_color_hex(0xFFFFFF)
#define COL_LOCK_SUB     lv_color_hex(0xB0B0B8)
#define COL_ACCENT       lv_color_hex(0x007AFF)   // systemBlue
#define COL_GREEN        lv_color_hex(0x34C759)
#define COL_RED          lv_color_hex(0xFF3B30)
#define COL_GREY         lv_color_hex(0x8E8E93)

#define PANEL_EXPANDED_H 220
#define CARD_RADIUS      14
#define CARD_H           56   // body text sits close to the lower edge at 54
#define CARD_GAP         6
#define CARD_ICON        18

// Which app_manager index is foregrounded, or -1 for "home screen showing".
static int s_lp_foreground_idx = -1;

// ── Panels ──────────────────────────────────────────────────────────────────

typedef enum { PANEL_PEEK, PANEL_EXPANDED } panel_state_t;

typedef struct {
    lv_obj_t     *panel;
    lv_obj_t     *handle;
    lv_obj_t     *scroll;    // the card container
    panel_state_t state;
    lv_coord_t    press_y0;
    lv_coord_t    base_y;
} panel_t;

static panel_t s_notif_panel;
static panel_t s_ctrl_panel;

static lv_obj_t *s_hotzone_left;
static lv_obj_t *s_hotzone_right;

static lv_obj_t *s_status_clock;
static lv_obj_t *s_status_batt;
static lv_obj_t *s_status_wifi;
static lv_obj_t *s_status_lora;
static lv_obj_t *s_status_mail;

static lv_obj_t *s_lock_screen;
static lv_obj_t *s_lock_clock;
static lv_obj_t *s_lock_sub;
static lv_obj_t *s_lock_cards;
static bool      s_locked = false;

static void panel_set_state(panel_t *p, panel_state_t s);
static void set_status_visible(bool show);

// ── Relative timestamps ─────────────────────────────────────────────────────

// "now" / "3m" / "2h" — iOS's own shorthand. Uptime-relative, like everything
// else here; a notification posted before the last reboot cannot exist anyway,
// since the kernel's notification ring is cleared on boot.
static void rel_time(uint64_t then_ms, char *out, size_t out_sz)
{
    uint64_t now = purr_kernel_uptime_ms();
    uint64_t d   = (now > then_ms) ? (now - then_ms) : 0;
    uint64_t s   = d / 1000ULL;
    if (s < 60)          snprintf(out, out_sz, "now");
    else if (s < 3600)   snprintf(out, out_sz, "%um", (unsigned)(s / 60));
    else                 snprintf(out, out_sz, "%uh", (unsigned)(s / 3600));
}

// ── The shared notification card ────────────────────────────────────────────
// Used by BOTH the Notification Center and the lock screen — see this file's
// header for why that sharing is the point rather than an optimisation.
//
//   ┌──────────────────────────────────┐
//   │ [■] SOURCE                  2m   │
//   │ Title                            │
//   │ Body, truncated with an ellipsis │
//   └──────────────────────────────────┘
//
// The icon square uses the host's per-app icon and accent colour, so a
// notification carries the same visual identity the app has on the
// springboard. A host that supplies neither still gets a plain grey square
// rather than a hole in the layout.
// Swipe-to-dismiss state. The clear button is built with every card but kept
// hidden; a horizontal swipe on that card reveals it, and tapping it removes
// that one notification. Revealing a button rather than deleting on the swipe
// itself is deliberate — an accidental brush should never silently destroy
// something, and there is no undo here.
//
// Only one card may have its button showing at a time, so revealing on a new
// card hides the previous one; that also means a stray swipe is cancelled by
// swiping anything else.
static int s_reveal_idx = -1;   // notification index whose X is showing, or -1

static void notif_rebuild(void);

static void notif_clear_one_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    purr_kernel_notify_remove(idx);
    s_reveal_idx = -1;
    // Deferred: this runs inside an LVGL click dispatch, and the rebuild
    // lv_obj_clean()s the very container holding the button being clicked.
    // Tearing that down mid-dispatch is the documented way to hang the render
    // task (same reason window destroy is deferred in the window backends).
    lv_async_call((lv_async_cb_t)notif_rebuild, NULL);
}

static void notif_card_swipe_cb(lv_event_t *e)
{
    static lv_coord_t x0 = 0;
    lv_event_code_t code = lv_event_get_code(e);
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;

    lv_point_t pt;
    lv_indev_get_point(indev, &pt);

    if (code == LV_EVENT_PRESSED) {
        x0 = pt.x;
    } else if (code == LV_EVENT_RELEASED) {
        lv_coord_t dx = (lv_coord_t)(pt.x - x0);
        if (dx < 0) dx = (lv_coord_t)-dx;
        if (dx < 30) return;                  // a tap, not a swipe
        s_reveal_idx = (s_reveal_idx == idx) ? -1 : idx;
        lv_async_call((lv_async_cb_t)notif_rebuild, NULL);
    }
}

static lv_obj_t *build_notif_card(lv_obj_t *parent, const purr_notification_t *n,
                                   lv_coord_t w, bool on_dark)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, w, CARD_H);
    lv_obj_set_style_radius(card, CARD_RADIUS, 0);
    lv_obj_set_style_bg_color(card, COL_CARD, 0);
    // Frosted rather than solid: iOS notification cards are translucent over
    // whatever is behind them. Real blur is not affordable per-frame here, but
    // partial opacity over a dark scrim reads the same way at this size.
    purr_systemui_fx_bg_opa_keep(card, on_dark ? LV_OPA_80 : LV_OPA_90);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_CLICKABLE);

    // App mark
    lv_obj_t *mark = lv_obj_create(card);
    lv_obj_remove_style_all(mark);
    lv_obj_set_size(mark, CARD_ICON, CARD_ICON);
    lv_obj_set_pos(mark, 8, 7);
    lv_obj_set_style_radius(mark, 5, 0);
    lv_obj_set_style_bg_color(mark,
        (s_host && s_host->tint_color) ? s_host->tint_color(n->source, 0x30) : COL_GREY, 0);
    lv_obj_set_style_bg_opa(mark, LV_OPA_COVER, 0);
    lv_obj_clear_flag(mark, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(mark, LV_OBJ_FLAG_CLICKABLE);

    if (s_host && s_host->icon_for_app) {
        lv_obj_t *img = lv_img_create(mark);
        lv_img_set_src(img, s_host->icon_for_app(n->source));
        // Source assets are 48px; 256 = 100% in lv_img_set_zoom().
        lv_img_set_zoom(img, (uint16_t)(((CARD_ICON - 5) * 256) / 48));
        lv_obj_center(img);
        lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
    }

    lv_obj_t *src = lv_label_create(card);
    lv_label_set_text(src, n->source[0] ? n->source : "system");
    lv_label_set_long_mode(src, LV_LABEL_LONG_DOT);
    lv_obj_set_width(src, (lv_coord_t)(w - 34 - 40));
    lv_obj_set_style_text_color(src, COL_CARD_SUB, 0);
    lv_obj_set_style_text_font(src, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(src, 32, 7);

    char ts[12];
    rel_time(n->timestamp_ms, ts, sizeof(ts));
    lv_obj_t *tl = lv_label_create(card);
    lv_label_set_text(tl, ts);
    lv_obj_set_style_text_color(tl, COL_CARD_SUB, 0);
    lv_obj_set_style_text_font(tl, &lv_font_montserrat_14, 0);
    lv_obj_align(tl, LV_ALIGN_TOP_RIGHT, -8, 7);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, n->title);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(title, (lv_coord_t)(w - 16));
    lv_obj_set_style_text_color(title, COL_CARD_TEXT, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(title, 8, 24);

    lv_obj_t *body = lv_label_create(card);
    lv_label_set_text(body, n->body);
    lv_label_set_long_mode(body, LV_LABEL_LONG_DOT);
    lv_obj_set_width(body, (lv_coord_t)(w - 16));
    lv_obj_set_style_text_color(body, COL_CARD_SUB, 0);
    lv_obj_set_style_text_font(body, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(body, 8, 38);

    return card;
}

// Fills a scroll container with one card per notification, newest first.
// Shared by the shade and the lock screen so both always agree.
//
// A card whose index matches s_reveal_idx is narrowed to leave room for the
// clear button beside it, which is what makes the reveal read as the card
// sliding aside rather than a button appearing on top of it.
static void fill_notif_cards(lv_obj_t *scroll, lv_coord_t card_w, bool on_dark)
{
    if (!scroll) return;
    lv_obj_clean(scroll);

    int n = purr_kernel_notify_count();
    if (n == 0) {
        lv_obj_t *empty = lv_label_create(scroll);
        lv_label_set_text(empty, "No Notifications");
        lv_obj_set_style_text_color(empty, on_dark ? COL_LOCK_SUB : COL_CARD_SUB, 0);
        lv_obj_set_style_text_font(empty, &lv_font_montserrat_14, 0);
        return;
    }
    for (int i = 0; i < n; i++) {
        purr_notification_t note;
        if (!purr_kernel_notify_at(i, &note)) break;

        bool revealed = (i == s_reveal_idx);

        // A row wrapper so the card and its clear button sit side by side and
        // the flex column still lays them out as one item.
        lv_obj_t *row = lv_obj_create(scroll);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, card_w, CARD_H);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_coord_t cw = revealed ? (lv_coord_t)(card_w - CARD_H - 6) : card_w;
        lv_obj_t *card = build_notif_card(row, &note, cw, on_dark);
        lv_obj_set_pos(card, 0, 0);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(card, notif_card_swipe_cb, LV_EVENT_PRESSED,  (void *)(intptr_t)i);
        lv_obj_add_event_cb(card, notif_card_swipe_cb, LV_EVENT_RELEASED, (void *)(intptr_t)i);

        if (revealed) {
            lv_obj_t *del = lv_obj_create(row);
            lv_obj_remove_style_all(del);
            lv_obj_set_size(del, CARD_H, CARD_H);
            lv_obj_set_pos(del, (lv_coord_t)(card_w - CARD_H), 0);
            lv_obj_set_style_radius(del, CARD_RADIUS, 0);
            lv_obj_set_style_bg_color(del, COL_RED, 0);
            lv_obj_set_style_bg_opa(del, LV_OPA_COVER, 0);
            lv_obj_clear_flag(del, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(del, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(del, notif_clear_one_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

            lv_obj_t *x = lv_label_create(del);
            lv_label_set_text(x, LV_SYMBOL_CLOSE);
            lv_obj_set_style_text_color(x, lv_color_white(), 0);
            lv_obj_set_style_text_font(x, &lv_font_montserrat_14, 0);
            lv_obj_center(x);
            lv_obj_clear_flag(x, LV_OBJ_FLAG_CLICKABLE);
        }
    }
}

// Repaints whichever notification surface is currently on screen. Both the
// shade and the lock screen show the same cards, so a dismissal has to be
// reflected wherever the user actually is.
static void refresh_lock_notifs(void);

static void notif_rebuild(void)
{
    if (!s_host) return;
    lv_coord_t w = (lv_coord_t)(s_host->width() - 24);
    // Via refresh_lock_notifs(), not fill_notif_cards() directly — the lock
    // screen may be showing a privacy summary rather than the cards, and
    // dismissing one must not silently expand the list.
    if (s_locked)                                refresh_lock_notifs();
    if (s_notif_panel.state == PANEL_EXPANDED)   fill_notif_cards(s_notif_panel.scroll, w, true);
}

// Same scroll behaviour the Recents carousel uses — vertical only, no
// scrollbar, no elastic overshoot.
static lv_obj_t *make_scroll(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                              lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *s = lv_obj_create(parent);
    lv_obj_remove_style_all(s);
    lv_obj_set_size(s, w, h);
    lv_obj_set_pos(s, x, y);
    lv_obj_set_style_bg_opa(s, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_row(s, CARD_GAP, 0);
    lv_obj_set_scroll_dir(s, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_layout(s, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    return s;
}

// ── Panel drag ──────────────────────────────────────────────────────────────

// At rest a panel is entirely off the top of the screen, not peeking.
//
// The Android style leaves its panels peeking by PURR_SYSTEMUI_STATUS_H so the
// visible sliver doubles as the status bar's backdrop. Doing that here put an
// opaque dark strip across the top 22px permanently, which the iOS status
// bar's dark-on-light text then rendered invisibly against — confirmed on
// hardware as "the statusbar is all messed up and i cant see anything".
//
// iOS's shade is fully hidden until pulled anyway, and nothing is lost by
// hiding it: the drag is caught by the always-present hotzones (see
// purr_systemui_init), not by the panel itself, so there is no need for any
// part of the panel to be on-screen to start the gesture.
static lv_coord_t panel_y_for(panel_state_t s)
{
    return (s == PANEL_EXPANDED) ? 0 : (lv_coord_t)(-PANEL_EXPANDED_H);
}

static void panel_set_state(panel_t *p, panel_state_t s)
{
    p->state = s;
    lv_obj_set_y(p->panel, panel_y_for(s));
    if (s == PANEL_EXPANDED) {
        lv_obj_clear_flag(p->handle, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(p->panel);
        // An open panel is its own opaque backdrop, so the indicators are
        // legible on it and useful there — show them even if an app had
        // hidden them.
        set_status_visible(true);
        if (p == &s_notif_panel) {
            fill_notif_cards(p->scroll, (lv_coord_t)(s_host->width() - 24), true);
        }
    } else {
        lv_obj_add_flag(p->handle, LV_OBJ_FLAG_HIDDEN);
        // Collapsing returns to whatever the underlying state wants: hidden
        // over an app, visible on the home screen.
        set_status_visible(s_lp_foreground_idx < 0);
    }
}

static void panel_drag_cb(lv_event_t *e)
{
    panel_t *p = (panel_t *)lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;

    if (code == LV_EVENT_PRESSED) {
        lv_point_t pt; lv_indev_get_point(indev, &pt);
        p->press_y0 = pt.y;
        p->base_y   = lv_obj_get_y(p->panel);
        lv_obj_move_foreground(p->panel);
    } else if (code == LV_EVENT_PRESSING) {
        lv_point_t pt; lv_indev_get_point(indev, &pt);
        lv_coord_t ny = (lv_coord_t)(p->base_y + (pt.y - p->press_y0));
        if (ny < panel_y_for(PANEL_PEEK)) ny = panel_y_for(PANEL_PEEK);
        if (ny > 0) ny = 0;
        lv_obj_set_y(p->panel, ny);
    } else if (code == LV_EVENT_RELEASED) {
        lv_coord_t y = lv_obj_get_y(p->panel);
        panel_set_state(p, (y > -(PANEL_EXPANDED_H / 2)) ? PANEL_EXPANDED : PANEL_PEEK);
    }
}

// Only one panel open at a time — starting a drag on one collapses the other.
static void hotzone_left_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_PRESSED) return;
    if (s_ctrl_panel.state == PANEL_EXPANDED) panel_set_state(&s_ctrl_panel, PANEL_PEEK);
}
static void hotzone_right_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_PRESSED) return;
    if (s_notif_panel.state == PANEL_EXPANDED) panel_set_state(&s_notif_panel, PANEL_PEEK);
}

static void build_panel(panel_t *p, uint16_t w, const char *title)
{
    p->panel = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(p->panel);
    lv_obj_set_size(p->panel, w, PANEL_EXPANDED_H);
    lv_obj_set_pos(p->panel, 0, panel_y_for(PANEL_PEEK));
    lv_obj_set_style_bg_color(p->panel, COL_SCRIM, 0);
    purr_systemui_fx_bg_opa(p->panel, LV_OPA_80);
    lv_obj_set_style_radius(p->panel, 0, 0);
    lv_obj_clear_flag(p->panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(p->panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(p->panel, panel_drag_cb, LV_EVENT_PRESSED, p);
    lv_obj_add_event_cb(p->panel, panel_drag_cb, LV_EVENT_PRESSING, p);
    lv_obj_add_event_cb(p->panel, panel_drag_cb, LV_EVENT_RELEASED, p);

    lv_obj_t *hdr = lv_label_create(p->panel);
    lv_label_set_text(hdr, title);
    lv_obj_set_style_text_color(hdr, COL_LOCK_TEXT, 0);
    lv_obj_set_style_text_font(hdr, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(hdr, 12, PURR_SYSTEMUI_STATUS_H + 2);

    p->scroll = make_scroll(p->panel, 12, (lv_coord_t)(PURR_SYSTEMUI_STATUS_H + 20),
                             (lv_coord_t)(w - 24),
                             (lv_coord_t)(PANEL_EXPANDED_H - PURR_SYSTEMUI_STATUS_H - 30));

    // Grab handle, iOS's pill, shown only while open.
    p->handle = lv_obj_create(p->panel);
    lv_obj_remove_style_all(p->handle);
    lv_obj_set_size(p->handle, 40, 5);
    lv_obj_set_style_radius(p->handle, 3, 0);
    lv_obj_set_style_bg_color(p->handle, COL_GREY, 0);
    lv_obj_set_style_bg_opa(p->handle, LV_OPA_COVER, 0);
    lv_obj_align(p->handle, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_add_flag(p->handle, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(p->handle, LV_OBJ_FLAG_CLICKABLE);

    p->state = PANEL_PEEK;
}

// ── Control Center (running apps) ───────────────────────────────────────────

static void ctrl_open_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    const app_entry_t *app = app_manager_get(idx);
    if (app && app->window) purr_win_show(app->window);
    purr_systemui_enter_app(idx);
    panel_set_state(&s_ctrl_panel, PANEL_PEEK);
}

static void ctrl_kill_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    app_manager_stop(idx);
    if (idx == s_lp_foreground_idx) purr_systemui_return_home();
}

static void refresh_ctrl(void)
{
    if (!s_ctrl_panel.scroll) return;
    lv_obj_clean(s_ctrl_panel.scroll);
    lv_coord_t w = (lv_coord_t)(s_host->width() - 24);

    int n = app_manager_count();
    bool any = false;
    for (int i = 0; i < n; i++) {
        const app_entry_t *app = app_manager_get(i);
        if (!app || app->state != APP_STATE_RUNNING) continue;
        any = true;

        lv_obj_t *row = lv_obj_create(s_ctrl_panel.scroll);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, w, 34);
        lv_obj_set_style_radius(row, 10, 0);
        lv_obj_set_style_bg_color(row, COL_CARD, 0);
        purr_systemui_fx_bg_opa_keep(row, LV_OPA_20);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, app->name);
        lv_obj_set_style_text_color(lbl, COL_LOCK_TEXT, 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 10, 0);

        lv_obj_t *open = lv_btn_create(row);
        lv_obj_set_size(open, 46, 24);
        lv_obj_set_style_radius(open, 12, 0);
        lv_obj_set_style_bg_color(open, COL_ACCENT, 0);
        lv_obj_align(open, LV_ALIGN_RIGHT_MID, -56, 0);
        lv_obj_t *ol = lv_label_create(open);
        lv_label_set_text(ol, "Open");
        lv_obj_center(ol);
        lv_obj_add_event_cb(open, ctrl_open_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *kill = lv_btn_create(row);
        lv_obj_set_size(kill, 46, 24);
        lv_obj_set_style_radius(kill, 12, 0);
        lv_obj_set_style_bg_color(kill, COL_RED, 0);
        lv_obj_align(kill, LV_ALIGN_RIGHT_MID, -6, 0);
        lv_obj_t *kl = lv_label_create(kill);
        lv_label_set_text(kl, "End");
        lv_obj_center(kl);
        lv_obj_add_event_cb(kill, ctrl_kill_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    if (!any) {
        lv_obj_t *lbl = lv_label_create(s_ctrl_panel.scroll);
        lv_label_set_text(lbl, "Nothing Running");
        lv_obj_set_style_text_color(lbl, COL_LOCK_SUB, 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    }
}

// ── Status bar ──────────────────────────────────────────────────────────────
// Flat, not translucent: legibility in daylight on this panel beats an effect
// that is barely visible at 22px tall. Clock left, indicators right — iOS's
// arrangement, and the opposite of the Android style's battery-left layout.

// Right-aligned, right-to-left, with each item allotted a fixed slot.
//
// The first attempt positioned these at absolute x offsets from the left,
// which collided the moment the battery label grew from a bare glyph to
// "85% <glyph>" — it overran the wifi glyph sitting at a fixed x. Anchoring
// every indicator to the RIGHT edge with cumulative offsets means a wider
// battery string pushes nothing, because each slot's right edge is fixed.
#define ST_SLOT_BATT  8     // battery is the rightmost item
#define ST_SLOT_WIFI  64
#define ST_SLOT_LORA  86
#define ST_SLOT_MAIL  108

static void build_status(uint16_t w)
{
    (void)w;

    s_status_clock = lv_label_create(lv_layer_top());
    lv_obj_set_style_text_font(s_status_clock, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_status_clock, COL_STATUS_TEXT, 0);
    lv_label_set_text(s_status_clock, "0:00");
    lv_obj_align(s_status_clock, LV_ALIGN_TOP_LEFT, 10, 4);

    s_status_mail = lv_label_create(lv_layer_top());
    lv_obj_set_style_text_font(s_status_mail, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_status_mail, COL_ACCENT, 0);
    lv_label_set_text(s_status_mail, "");
    lv_obj_align(s_status_mail, LV_ALIGN_TOP_RIGHT, -ST_SLOT_MAIL, 4);

    s_status_lora = lv_label_create(lv_layer_top());
    lv_obj_set_style_text_font(s_status_lora, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_status_lora, LV_SYMBOL_GPS);
    lv_obj_align(s_status_lora, LV_ALIGN_TOP_RIGHT, -ST_SLOT_LORA, 4);

    s_status_wifi = lv_label_create(lv_layer_top());
    lv_obj_set_style_text_font(s_status_wifi, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_status_wifi, LV_SYMBOL_WIFI);
    lv_obj_align(s_status_wifi, LV_ALIGN_TOP_RIGHT, -ST_SLOT_WIFI, 4);

    s_status_batt = lv_label_create(lv_layer_top());
    lv_obj_set_style_text_font(s_status_batt, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_status_batt, COL_STATUS_TEXT, 0);
    lv_label_set_text(s_status_batt, LV_SYMBOL_BATTERY_FULL);
    lv_obj_align(s_status_batt, LV_ALIGN_TOP_RIGHT, -ST_SLOT_BATT, 4);
}

static void uptime_clock(char *out, size_t out_sz)
{
    uint64_t s = purr_kernel_uptime_ms() / 1000ULL;
    snprintf(out, out_sz, "%u:%02u",
             (unsigned)((s / 3600ULL) % 24ULL), (unsigned)((s / 60ULL) % 60ULL));
}

static void refresh_status(void)
{
    char buf[16];
    uptime_clock(buf, sizeof(buf));
    lv_label_set_text(s_status_clock, buf);

    lv_obj_set_style_text_color(s_status_wifi,
        purr_kernel_wifi_connected() ? COL_STATUS_TEXT : COL_GREY, 0);
    lv_obj_set_style_text_color(s_status_lora,
        purr_kernel_lora_available() ? COL_STATUS_TEXT : COL_GREY, 0);

    int unread = purr_kernel_notify_count();
    if (unread > 0) {
        snprintf(buf, sizeof(buf), LV_SYMBOL_BELL " %d", unread);
        lv_label_set_text(s_status_mail, buf);
    } else {
        lv_label_set_text(s_status_mail, "");
    }

    int pct = purr_kernel_battery_percent();
    const char *sym = pct < 0   ? LV_SYMBOL_BATTERY_FULL :
                       pct > 80 ? LV_SYMBOL_BATTERY_FULL :
                       pct > 55 ? LV_SYMBOL_BATTERY_3    :
                       pct > 30 ? LV_SYMBOL_BATTERY_2    :
                       pct > 10 ? LV_SYMBOL_BATTERY_1    : LV_SYMBOL_BATTERY_EMPTY;
    if (pct < 0) snprintf(buf, sizeof(buf), "%s", sym);
    else         snprintf(buf, sizeof(buf), "%d%% %s", pct, sym);
    lv_label_set_text(s_status_batt, buf);
    lv_obj_set_style_text_color(s_status_batt,
        (pct >= 0 && pct <= 10) ? COL_RED : COL_STATUS_TEXT, 0);
    // Re-align after the text changed width — LVGL sizes a label to content,
    // so a right-anchored label must be re-aligned or it drifts as the
    // percentage gains and loses digits.
    lv_obj_align(s_status_batt, LV_ALIGN_TOP_RIGHT, -ST_SLOT_BATT, 4);
}

// ── Lock screen ─────────────────────────────────────────────────────────────
// iOS's arrangement: oversized clock at the top, then the notification stack
// below it — built from the SAME card builder the shade uses, which is the
// whole reason the two match.

static void restore_brightness(void)
{
    uint8_t level = 255;
    nvs_handle_t h;
    if (nvs_open("purr_settings", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "brightness", &level);
        nvs_close(h);
    }
    const catcall_display_t *disp = purr_kernel_display();
    if (disp && disp->set_brightness) disp->set_brightness(level);
}

// Notification privacy on the lock screen.
//
// When purr_kernel_lock_hide_notifications() is set (the default), the cards
// are replaced by a single count — "3 Notifications" — and revealed only by an
// upward swipe. The reveal is per-lock, not persistent: locking again re-hides
// them, because the whole point is that someone who picks the device up
// afterwards should not find them already open.
static bool s_lock_notifs_revealed = false;

static void refresh_lock_notifs(void);

// Upward swipe reveals the hidden list. Deliberately not any tap: a tap
// dismisses the lock, so the two gestures must stay distinguishable or
// reaching for the notifications would unlock the device instead.
static void lock_swipe_cb(lv_event_t *e)
{
    static lv_coord_t y0 = 0;
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;

    lv_point_t pt;
    lv_indev_get_point(indev, &pt);

    if (code == LV_EVENT_PRESSED) {
        y0 = pt.y;
    } else if (code == LV_EVENT_RELEASED) {
        lv_coord_t dy = (lv_coord_t)(y0 - pt.y);   // positive = swiped up
        if (dy >= 30 && !s_lock_notifs_revealed) {
            s_lock_notifs_revealed = true;
            refresh_lock_notifs();
        }
    }
}

static void lock_dismiss_cb(lv_event_t *e)
{
    (void)e;
    if (!s_locked) return;
    // A swipe also produces a click on release; ignore the one that just
    // revealed the list, or the same gesture would unlock straight past it.
    lv_indev_t *indev = lv_indev_get_act();
    if (indev) {
        lv_point_t vect;
        lv_indev_get_vect(indev, &vect);
        if (vect.y < -20) return;
    }
    s_locked = false;
    lv_obj_add_flag(s_lock_screen, LV_OBJ_FLAG_HIDDEN);
}

static void build_lock(uint16_t w, uint16_t h)
{
    s_lock_screen = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_lock_screen);
    lv_obj_set_size(s_lock_screen, w, h);
    lv_obj_set_pos(s_lock_screen, 0, 0);
    lv_obj_set_style_bg_color(s_lock_screen, COL_SCRIM, 0);
    lv_obj_set_style_bg_opa(s_lock_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_lock_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_lock_screen, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_lock_screen, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_lock_screen, lock_dismiss_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_lock_screen, lock_swipe_cb, LV_EVENT_PRESSED,  NULL);
    lv_obj_add_event_cb(s_lock_screen, lock_swipe_cb, LV_EVENT_RELEASED, NULL);

    // Wallpaper behind everything, so the lock screen is recognisably the same
    // device as the home screen rather than a flat slab. Dimmed, because the
    // clock and notification cards have to stay legible over an arbitrary
    // image the user chose — iOS does the same.
    const lv_img_dsc_t *wp = (s_host && s_host->wallpaper) ? s_host->wallpaper() : NULL;
    if (wp) {
        lv_obj_t *bg = lv_img_create(s_lock_screen);
        lv_img_set_src(bg, wp);
        lv_obj_set_pos(bg, 0, 0);
        lv_obj_clear_flag(bg, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *dim = lv_obj_create(s_lock_screen);
        lv_obj_remove_style_all(dim);
        lv_obj_set_size(dim, w, h);
        lv_obj_set_pos(dim, 0, 0);
        lv_obj_set_style_bg_color(dim, COL_SCRIM, 0);
        purr_systemui_fx_bg_opa(dim, LV_OPA_50);
        lv_obj_clear_flag(dim, LV_OBJ_FLAG_CLICKABLE);
    }

    s_lock_clock = lv_label_create(s_lock_screen);
    lv_obj_set_style_text_color(s_lock_clock, COL_LOCK_TEXT, 0);
    lv_obj_set_style_text_font(s_lock_clock, &lv_font_montserrat_32, 0);
    lv_label_set_text(s_lock_clock, "0:00");
    lv_obj_set_style_text_align(s_lock_clock, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_lock_clock, LV_ALIGN_TOP_MID, 0, 26);

    s_lock_sub = lv_label_create(s_lock_screen);
    lv_obj_set_style_text_color(s_lock_sub, COL_LOCK_SUB, 0);
    lv_obj_set_style_text_font(s_lock_sub, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_lock_sub, "");
    lv_obj_set_style_text_align(s_lock_sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_lock_sub, LV_ALIGN_TOP_MID, 0, 62);

    // Notification stack, same cards and same scrolling as the shade — or, when
    // privacy is on, a single count line in its place (see refresh_lock_notifs).
    s_lock_cards = make_scroll(s_lock_screen, 12, 84,
                                (lv_coord_t)(w - 24), (lv_coord_t)(h - 84 - 24));

    lv_obj_t *hint = lv_label_create(s_lock_screen);
    lv_obj_set_style_text_color(hint, COL_LOCK_SUB, 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_label_set_text(hint, "tap to unlock");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -6);
}

static void refresh_lock(void)
{
    char buf[48];
    uptime_clock(buf, sizeof(buf));
    lv_label_set_text(s_lock_clock, buf);

    uint64_t s = purr_kernel_uptime_ms() / 1000ULL;
    unsigned hh = (unsigned)(s / 3600ULL), mm = (unsigned)((s / 60ULL) % 60ULL);
    int nodes = 0;
#ifdef CONFIG_PURR_FEATURE_MESHTASTIC
    nodes = mesh_manager_node_count();
#endif
    // States plainly that this is uptime, not wall-clock — there is no RTC to
    // read a real time from, and implying otherwise would be a lie on screen.
    if (hh > 0) snprintf(buf, sizeof(buf), "up %uh %um  ·  %d node%s", hh, mm, nodes, nodes == 1 ? "" : "s");
    else        snprintf(buf, sizeof(buf), "up %um  ·  %d node%s", mm, nodes, nodes == 1 ? "" : "s");
    lv_label_set_text(s_lock_sub, buf);
}

// Fills the lock screen's notification area: either the real cards, or a
// privacy summary with a hint that they can be revealed.
static void refresh_lock_notifs(void)
{
    if (!s_lock_cards || !s_host) return;
    lv_coord_t w = (lv_coord_t)(s_host->width() - 24);

    bool hide = purr_kernel_lock_hide_notifications() && !s_lock_notifs_revealed;
    if (!hide) {
        fill_notif_cards(s_lock_cards, w, true);
        return;
    }

    lv_obj_clean(s_lock_cards);
    int n = purr_kernel_notify_count();

    lv_obj_t *summary = lv_label_create(s_lock_cards);
    char buf[48];
    if (n == 0) snprintf(buf, sizeof(buf), "No Notifications");
    else        snprintf(buf, sizeof(buf), "%d Notification%s", n, n == 1 ? "" : "s");
    lv_label_set_text(summary, buf);
    lv_obj_set_style_text_color(summary, COL_LOCK_TEXT, 0);
    lv_obj_set_style_text_font(summary, &lv_font_montserrat_14, 0);

    if (n > 0) {
        lv_obj_t *hint = lv_label_create(s_lock_cards);
        lv_label_set_text(hint, "swipe up to show");
        lv_obj_set_style_text_color(hint, COL_LOCK_SUB, 0);
        lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    }
}

static void lock_check_idle(void)
{
    if (s_locked) return;
    uint8_t timeout_min = purr_kernel_screen_timeout_min();
    uint64_t elapsed = purr_kernel_uptime_ms() - s_host->last_activity_ms();
    if (elapsed < (uint64_t)timeout_min * 60000ULL) return;

    s_locked = true;
    const catcall_display_t *disp = purr_kernel_display();
    if (disp && disp->set_brightness) disp->set_brightness(0);
    refresh_lock();
    // Re-hide on every fresh lock: a reveal is for this lock only, or someone
    // picking the device up later would find the list already open.
    s_lock_notifs_revealed = false;
    refresh_lock_notifs();
    lv_obj_clear_flag(s_lock_screen, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_lock_screen);
}

// ── Recents (app switcher) ──────────────────────────────────────────────────
// iOS's switcher: running apps as large cards side by side, scrolled
// horizontally. Each card carries the app's icon on its accent colour, its
// name, and an X to end it; tapping the card body brings that app forward.
//
// Horizontal rather than the Android style's vertical stagger, because that is
// what the shape of iOS's switcher is — and on a 320x240 landscape panel a
// horizontal row of tall cards uses the space far better than a vertical
// stack, which would only fit one and a half cards.
//
// Built lazily on first open and refilled on every subsequent one: the running
// set changes between opens, and keeping a stale card for a dead app would
// offer to switch to something that no longer exists.

static lv_obj_t *s_recents_backdrop = NULL;
static lv_obj_t *s_recents_scroll   = NULL;

bool purr_systemui_recents_open(void)
{
    return s_recents_backdrop && !lv_obj_has_flag(s_recents_backdrop, LV_OBJ_FLAG_HIDDEN);
}

void purr_systemui_close_recents(void)
{
    if (s_recents_backdrop) lv_obj_add_flag(s_recents_backdrop, LV_OBJ_FLAG_HIDDEN);
}

static void recents_open_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    const app_entry_t *app = app_manager_get(idx);
    if (app && app->window) purr_win_show(app->window);
    purr_systemui_close_recents();
    purr_systemui_enter_app(idx);
}

static void recents_kill_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    app_manager_stop(idx);
    if (idx == s_lp_foreground_idx) purr_systemui_return_home();
    // Deferred — this fires inside a click dispatch on a button that the
    // refill is about to lv_obj_clean() out from under itself.
    lv_async_call((lv_async_cb_t)purr_systemui_open_recents, NULL);
}

static void recents_backdrop_cb(lv_event_t *e)
{
    (void)e;
    purr_systemui_close_recents();
}

void purr_systemui_open_recents(void)
{
    if (!s_host) return;
    uint16_t w = s_host->width();
    uint16_t h = s_host->height();

    if (!s_recents_backdrop) {
        s_recents_backdrop = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(s_recents_backdrop);
        lv_obj_set_size(s_recents_backdrop, w, h);
        lv_obj_set_pos(s_recents_backdrop, 0, 0);
        lv_obj_set_style_bg_color(s_recents_backdrop, COL_SCRIM, 0);
        purr_systemui_fx_bg_opa(s_recents_backdrop, LV_OPA_80);
        lv_obj_clear_flag(s_recents_backdrop, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(s_recents_backdrop, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_recents_backdrop, recents_backdrop_cb, LV_EVENT_CLICKED, NULL);

        s_recents_scroll = lv_obj_create(s_recents_backdrop);
        lv_obj_remove_style_all(s_recents_scroll);
        lv_obj_set_size(s_recents_scroll, w, (lv_coord_t)(h - 40));
        lv_obj_set_pos(s_recents_scroll, 0, 20);
        lv_obj_set_style_bg_opa(s_recents_scroll, LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_column(s_recents_scroll, 10, 0);
        lv_obj_set_style_pad_all(s_recents_scroll, 12, 0);
        lv_obj_set_scroll_dir(s_recents_scroll, LV_DIR_HOR);
        lv_obj_set_scrollbar_mode(s_recents_scroll, LV_SCROLLBAR_MODE_OFF);
        lv_obj_clear_flag(s_recents_scroll, LV_OBJ_FLAG_SCROLL_ELASTIC);
        lv_obj_set_layout(s_recents_scroll, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(s_recents_scroll, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(s_recents_scroll, LV_FLEX_ALIGN_START,
                               LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    }

    lv_obj_clean(s_recents_scroll);

    lv_coord_t card_w = (lv_coord_t)((w * 55) / 100);
    lv_coord_t card_h = (lv_coord_t)(h - 64);

    int n = app_manager_count();
    bool any = false;
    for (int i = 0; i < n; i++) {
        const app_entry_t *app = app_manager_get(i);
        if (!app || app->state != APP_STATE_RUNNING) continue;
        any = true;

        lv_obj_t *card = lv_obj_create(s_recents_scroll);
        lv_obj_remove_style_all(card);
        lv_obj_set_size(card, card_w, card_h);
        lv_obj_set_style_radius(card, 16, 0);
        lv_obj_set_style_bg_color(card, COL_CARD, 0);
        purr_systemui_fx_bg_opa_keep(card, LV_OPA_90);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(card, recents_open_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *mark = lv_obj_create(card);
        lv_obj_remove_style_all(mark);
        lv_obj_set_size(mark, 44, 44);
        lv_obj_set_style_radius(mark, 11, 0);
        lv_obj_set_style_bg_color(mark,
            s_host->tint_color ? s_host->tint_color(app->name, 0x30) : COL_GREY, 0);
        lv_obj_set_style_bg_opa(mark, LV_OPA_COVER, 0);
        lv_obj_align(mark, LV_ALIGN_CENTER, 0, -14);
        lv_obj_clear_flag(mark, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(mark, LV_OBJ_FLAG_SCROLLABLE);

        if (s_host->icon_for_app) {
            lv_obj_t *img = lv_img_create(mark);
            lv_img_set_src(img, s_host->icon_for_app(app->name));
            lv_img_set_zoom(img, (uint16_t)((26 * 256) / 48));
            lv_obj_center(img);
            lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
        }

        lv_obj_t *name = lv_label_create(card);
        lv_label_set_text(name, app->name);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_width(name, (lv_coord_t)(card_w - 16));
        lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(name, COL_CARD_TEXT, 0);
        lv_obj_set_style_text_font(name, &lv_font_montserrat_14, 0);
        lv_obj_align(name, LV_ALIGN_CENTER, 0, 20);
        lv_obj_clear_flag(name, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *kill = lv_obj_create(card);
        lv_obj_remove_style_all(kill);
        lv_obj_set_size(kill, 26, 26);
        lv_obj_set_style_radius(kill, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(kill, COL_RED, 0);
        lv_obj_set_style_bg_opa(kill, LV_OPA_COVER, 0);
        lv_obj_align(kill, LV_ALIGN_TOP_RIGHT, -6, 6);
        lv_obj_clear_flag(kill, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(kill, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(kill, recents_kill_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *x = lv_label_create(kill);
        lv_label_set_text(x, LV_SYMBOL_CLOSE);
        lv_obj_set_style_text_color(x, lv_color_white(), 0);
        lv_obj_set_style_text_font(x, &lv_font_montserrat_14, 0);
        lv_obj_center(x);
        lv_obj_clear_flag(x, LV_OBJ_FLAG_CLICKABLE);
    }

    if (!any) {
        lv_obj_t *lbl = lv_label_create(s_recents_scroll);
        lv_label_set_text(lbl, "No Recent Apps");
        lv_obj_set_style_text_color(lbl, COL_LOCK_SUB, 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    }

    lv_obj_clear_flag(s_recents_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_recents_backdrop);
    set_status_visible(true);   // the switcher is system chrome, not an app
}

// ── Public API ──────────────────────────────────────────────────────────────

int  purr_systemui_foreground_idx(void) { return s_lp_foreground_idx; }
bool purr_systemui_is_locked(void)      { return s_locked; }

void purr_systemui_wake(void)
{
    if (!s_locked) return;
    restore_brightness();
}

// iOS has no nav bar; the host draws its own home indicator, so there is never
// a bar for host content to keep clear of.
int16_t purr_systemui_navbar_height(void) { return 0; }

// Status indicators hide while an app is foregrounded and come back on the
// home screen — the same behaviour the Android style has, and what iOS does
// for apps that request a hidden status bar. App windows here are genuinely
// full-screen, so anything left on lv_layer_top() floats over app content
// with no guarantee of contrast behind it; hiding is both the iOS convention
// and the legible choice.
//
// Reachable while hidden: dragging either top hotzone opens a panel, and
// panel_set_state() shows the indicators again for as long as it is open.
static void set_status_visible(bool show)
{
    lv_obj_t *items[] = { s_status_clock, s_status_batt, s_status_wifi,
                          s_status_lora,  s_status_mail };
    for (size_t i = 0; i < sizeof(items) / sizeof(items[0]); i++) {
        if (!items[i]) continue;
        if (show) lv_obj_clear_flag(items[i], LV_OBJ_FLAG_HIDDEN);
        else      lv_obj_add_flag(items[i], LV_OBJ_FLAG_HIDDEN);
    }
}

void purr_systemui_enter_app(int idx)
{
    s_lp_foreground_idx = idx;
    if (s_notif_panel.panel) panel_set_state(&s_notif_panel, PANEL_PEEK);
    if (s_ctrl_panel.panel)  panel_set_state(&s_ctrl_panel,  PANEL_PEEK);
    set_status_visible(false);
}

void purr_systemui_return_home(void)
{
    s_lp_foreground_idx = -1;
    set_status_visible(true);
}

// See systemui.h. The panels here are built once by purr_systemui_init() and
// only ever slid up and down afterwards, so nothing re-reads the effects flag
// on its own — this is what makes the Settings toggle take effect immediately
// rather than at the next boot.
//
// Only the persistent surfaces need naming. The notification and recents cards
// are rebuilt each time they are shown, so notif_rebuild() below is enough to
// refresh every card currently on screen; the recents cards refresh when the
// carousel is next opened.
void purr_systemui_fx_refresh(void)
{
    if (s_notif_panel.panel) purr_systemui_fx_bg_opa(s_notif_panel.panel, LV_OPA_80);
    if (s_ctrl_panel.panel)  purr_systemui_fx_bg_opa(s_ctrl_panel.panel,  LV_OPA_80);
    if (s_recents_backdrop)  purr_systemui_fx_bg_opa(s_recents_backdrop,  LV_OPA_80);
    notif_rebuild();
}

void purr_systemui_init(const purr_systemui_host_t *host)
{
    if (!host) { ESP_LOGE(TAG, "init called with NULL host — system UI disabled"); return; }
    s_host = host;

    uint16_t w = s_host->width();
    uint16_t h = s_host->height();

    build_panel(&s_notif_panel, w, "Notifications");
    build_panel(&s_ctrl_panel,  w, "Control Center");

    s_hotzone_left = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_hotzone_left);
    lv_obj_set_size(s_hotzone_left, (lv_coord_t)(w / 2), PURR_SYSTEMUI_STATUS_H);
    lv_obj_set_pos(s_hotzone_left, 0, 0);
    lv_obj_set_style_bg_opa(s_hotzone_left, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_hotzone_left, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_hotzone_left, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_hotzone_left, hotzone_left_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_hotzone_left, panel_drag_cb, LV_EVENT_PRESSED,  &s_notif_panel);
    lv_obj_add_event_cb(s_hotzone_left, panel_drag_cb, LV_EVENT_PRESSING, &s_notif_panel);
    lv_obj_add_event_cb(s_hotzone_left, panel_drag_cb, LV_EVENT_RELEASED, &s_notif_panel);

    s_hotzone_right = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_hotzone_right);
    lv_obj_set_size(s_hotzone_right, (lv_coord_t)(w - w / 2), PURR_SYSTEMUI_STATUS_H);
    lv_obj_set_pos(s_hotzone_right, (lv_coord_t)(w / 2), 0);
    lv_obj_set_style_bg_opa(s_hotzone_right, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_hotzone_right, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_hotzone_right, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_hotzone_right, hotzone_right_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_hotzone_right, panel_drag_cb, LV_EVENT_PRESSED,  &s_ctrl_panel);
    lv_obj_add_event_cb(s_hotzone_right, panel_drag_cb, LV_EVENT_PRESSING, &s_ctrl_panel);
    lv_obj_add_event_cb(s_hotzone_right, panel_drag_cb, LV_EVENT_RELEASED, &s_ctrl_panel);

    build_status(w);
    build_lock(w, h);

    ESP_LOGI(TAG, "iOS system UI built (%ux%u)", w, h);
}

void purr_systemui_tick(void)
{
    if (!s_host) return;

    refresh_status();

    // Rebuild the open panel only when its content actually changed, not every
    // tick. A periodic rebuild both fights an in-progress scroll on the very
    // list being rebuilt (the hazard the Android style hit) and would stomp the
    // swipe-reveal state mid-gesture.
    static int s_last_notif_count = -1;
    int n = purr_kernel_notify_count();
    if (s_notif_panel.state == PANEL_EXPANDED && n != s_last_notif_count) {
        s_last_notif_count = n;
        fill_notif_cards(s_notif_panel.scroll, (lv_coord_t)(s_host->width() - 24), true);
    } else if (s_notif_panel.state != PANEL_EXPANDED) {
        s_last_notif_count = n;   // resync while closed so reopening is current
    }

    if (s_ctrl_panel.state == PANEL_EXPANDED) {
        refresh_ctrl();
    }

    lock_check_idle();
    if (s_locked) refresh_lock();
}

#endif // CONFIG_PURR_SYSTEMUI && CONFIG_PURR_SYSTEMUI_STYLE_IOS
