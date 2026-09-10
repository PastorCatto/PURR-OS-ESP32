// cardstack_ui.c — the card stack itself: home card, app cards, end-of-stack
// Task Manager card, plus the three gestures (vertical snap-scroll, two-stage
// status bar drag, left-edge swipe-home).
//
// v1 scope: app cards show name + icon (no artwork yet — you'll supply
// images later, at which point they become the full-bleed card background
// per the design discussion; the fallback path below is exactly that case).
//
// catcall_ui_t backend (cardstack_win.c) already exists and is registered —
// tapping a card's tile calls app_manager_launch_idx() below, and the
// launched app's purr_win_t already renders full-screen on top of the stack
// via that backend. What's still missing is purely the visual/UX seam
// between "tapped card" and "window appeared": no dim/transition on open,
// and no signal back to the stack when the window's close button hides it
// (see cardstack_win.c's close_btn_event_cb) so the stack can restore itself.

#include "cardstack.h"
#include "../../kernel/core/purr_kernel.h"
#include "../app_manager/app_manager.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "cardstack_ui";

// STATUS_PEEK_H derives from cardstack.h's CARDSTACK_STATUS_PEEK_H — shared
// with cardstack_win.c, which needs the same value to keep app windows (and
// their close buttons) clear of the status hotzone below.
#define STATUS_PEEK_H     CARDSTACK_STATUS_PEEK_H
#define STATUS_EXPANDED_H 160
#define EDGE_PX           30
#define HOME_SWIPE_THRESH 60
#define MAX_CARDS         16
#define MAX_HOME_NOTIFS   5
#define MAX_HOME_DOCK     5

// Defined after app_card_open_cb (needs it as the dock icons' click handler);
// forward-declared here so build_home_card can call it.
static void build_home_dock(lv_obj_t *tile, uint16_t tw);

typedef struct {
    lv_obj_t *obj;
    bool      is_home;
    bool      is_taskmgr;
    int       app_idx;          // index into app_manager registry, -1 if N/A
    lv_obj_t *clock_lbl;        // home only
    lv_obj_t *date_lbl;         // home only
    lv_obj_t *notif_lbls[MAX_HOME_NOTIFS]; // home only
    lv_obj_t *taskmgr_list;     // taskmgr only
    lv_obj_t *status_dot;       // app cards only — live-tile running indicator
    lv_obj_t *status_lbl;       // app cards only
} card_t;

static card_t   s_cards[MAX_CARDS];
static int      s_card_count = 0;
static lv_obj_t *s_stack;

// ── Status bar (persistent strip + drag-down notification center) ────────────
// The bar itself is always on screen now — no more fully-hidden state. Drag
// down to expand it into the full notification center; drag back up to
// return to the normal thin strip (never further than that).

typedef enum { STATUS_PEEK, STATUS_EXPANDED } status_state_t;

static lv_obj_t      *s_status_panel;
static lv_obj_t      *s_status_hotzone;
static lv_obj_t      *s_icon_wifi;
static lv_obj_t      *s_icon_lora;
static lv_obj_t      *s_icon_mail;
static lv_obj_t      *s_icon_mail_badge;
static lv_obj_t      *s_icon_battery;
static lv_obj_t      *s_status_title_lbl;
static lv_obj_t      *s_status_notif_box;
static lv_obj_t      *s_status_handle;
static status_state_t s_status_state = STATUS_PEEK;
static lv_coord_t     s_status_press_y0;
static lv_coord_t     s_status_base_y;

static lv_coord_t status_y_for_state(status_state_t s)
{
    switch (s) {
        case STATUS_PEEK:     return (lv_coord_t)(-(STATUS_EXPANDED_H - STATUS_PEEK_H));
        case STATUS_EXPANDED: return 0;
    }
    return (lv_coord_t)(-(STATUS_EXPANDED_H - STATUS_PEEK_H));
}

static void status_set_state(status_state_t s)
{
    s_status_state = s;
    lv_obj_set_y(s_status_panel, status_y_for_state(s));
    // The grab handle is only meaningful (and only shown) once the
    // notification center is actually open — in the resting peek state
    // it has nothing to grab onto yet, so showing it there was a bug.
    if (s == STATUS_EXPANDED) lv_obj_clear_flag(s_status_handle, LV_OBJ_FLAG_HIDDEN);
    else                       lv_obj_add_flag(s_status_handle, LV_OBJ_FLAG_HIDDEN);
}

static void status_hotzone_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;

    if (code == LV_EVENT_PRESSED) {
        lv_point_t p;
        lv_indev_get_point(indev, &p);
        s_status_press_y0 = p.y;
        s_status_base_y   = lv_obj_get_y(s_status_panel);
    } else if (code == LV_EVENT_PRESSING) {
        lv_point_t p;
        lv_indev_get_point(indev, &p);
        lv_coord_t dy = (lv_coord_t)(p.y - s_status_press_y0);
        lv_coord_t ny = (lv_coord_t)(s_status_base_y + dy);
        // Never goes further up than the resting peek position — the bar
        // is permanent now, there's no "hidden" state to drag into.
        if (ny < status_y_for_state(STATUS_PEEK)) ny = status_y_for_state(STATUS_PEEK);
        if (ny > 0) ny = 0;
        lv_obj_set_y(s_status_panel, ny);
    } else if (code == LV_EVENT_RELEASED) {
        lv_coord_t y = lv_obj_get_y(s_status_panel);
        status_state_t target = (y > -(STATUS_EXPANDED_H / 2)) ? STATUS_EXPANDED : STATUS_PEEK;
        status_set_state(target);
    }
}

static void build_status_panel(uint16_t w)
{
    s_status_panel = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_status_panel);
    lv_obj_set_size(s_status_panel, w, STATUS_EXPANDED_H);
    lv_obj_set_pos(s_status_panel, 0, status_y_for_state(STATUS_PEEK));
    lv_obj_set_style_bg_color(s_status_panel, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_status_panel, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_status_panel, LV_OBJ_FLAG_SCROLLABLE);
    // The panel itself is also draggable (not just the top hotzone strip) —
    // this gives the swipe-to-dismiss gesture priority over the whole
    // visible notification area, not just a thin strip at the very top.
    lv_obj_add_flag(s_status_panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_status_panel, status_hotzone_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_status_panel, status_hotzone_event_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_status_panel, status_hotzone_event_cb, LV_EVENT_RELEASED, NULL);

    s_status_title_lbl = lv_label_create(s_status_panel);
    lv_obj_set_style_text_color(s_status_title_lbl, lv_color_make(0xA0, 0xA0, 0xA0), 0);
    lv_label_set_text(s_status_title_lbl, "Notifications");
    lv_obj_set_pos(s_status_title_lbl, 4, STATUS_PEEK_H + 2);

    // Visible grab handle — a small pill the user can see and pull, at the
    // very bottom of the panel. Purely a visual affordance: the drag itself
    // is bound to the whole panel above, but this is what tells the user
    // where (and that) they can grab to dismiss.
    s_status_handle = lv_obj_create(s_status_panel);
    lv_obj_remove_style_all(s_status_handle);
    lv_obj_set_size(s_status_handle, 40, 5);
    lv_obj_set_style_bg_color(s_status_handle, lv_color_make(0x80, 0x80, 0x80), 0);
    lv_obj_set_style_bg_opa(s_status_handle, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_status_handle, 3, 0);
    lv_obj_clear_flag(s_status_handle, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_status_handle, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(s_status_handle, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_add_flag(s_status_handle, LV_OBJ_FLAG_HIDDEN); // shown only once expanded

    s_status_notif_box = lv_obj_create(s_status_panel);
    lv_obj_remove_style_all(s_status_notif_box);
    lv_obj_set_style_bg_opa(s_status_notif_box, LV_OPA_TRANSP, 0);
    lv_obj_set_size(s_status_notif_box, w - 8, STATUS_EXPANDED_H - STATUS_PEEK_H - 22);
    lv_obj_set_pos(s_status_notif_box, 4, STATUS_PEEK_H + 18);
    lv_obj_set_flex_flow(s_status_notif_box, LV_FLEX_FLOW_COLUMN);
    // Must NOT be clickable/scrollable — lv_obj_create()'s defaults are both
    // on, which would otherwise swallow the drag-to-dismiss gesture handled
    // by the panel above the moment the touch starts over the notif list
    // instead of the top hotzone strip (this was the original "can't swipe
    // it away" bug — the gesture was only ever bound to a 22px-tall target).
    lv_obj_clear_flag(s_status_notif_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_status_notif_box, LV_OBJ_FLAG_CLICKABLE);

    // Hotzone — always present even when the panel is fully hidden (the
    // panel itself sits off-screen above y=0 in that state, so it can't
    // catch the initial reveal-drag; this strip can, since it never moves).
    s_status_hotzone = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_status_hotzone);
    lv_obj_set_size(s_status_hotzone, w, STATUS_PEEK_H);
    lv_obj_set_pos(s_status_hotzone, 0, 0);
    lv_obj_set_style_bg_opa(s_status_hotzone, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_status_hotzone, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_status_hotzone, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_status_hotzone, status_hotzone_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_status_hotzone, status_hotzone_event_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_status_hotzone, status_hotzone_event_cb, LV_EVENT_RELEASED, NULL);
}

static void mail_icon_click_cb(lv_event_t *e)
{
    (void)e;
    status_set_state(s_status_state == STATUS_PEEK ? STATUS_EXPANDED : STATUS_PEEK);
}

#define ICON_ON  lv_color_make(0x4D, 0xD0, 0x6B)  // clean green, reads well on black
#define ICON_OFF lv_color_make(0x55, 0x55, 0x55)  // dim grey, still legible, clearly "off"

// WiFi/LoRa/mail status cluster — the only things on the persistent top
// strip (no clock, no "PURR OS" text — both stay on the home card per the
// design discussion). WiFi/LoRa are plain indicators (non-clickable, so
// drags starting over them still reach the hotzone underneath); the mail
// icon is the one clickable shortcut straight to the notification center,
// with a small red dot when anything's pending.
static void build_status_icons(uint16_t w)
{
    // Battery sits on the left, away from the WiFi/LoRa/mail cluster on
    // the right — standard status-bar layout, and keeps the right side
    // from getting crowded.
    s_icon_battery = lv_label_create(lv_layer_top());
    lv_obj_set_style_text_font(s_icon_battery, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_icon_battery, lv_color_white(), 0);
    lv_label_set_text(s_icon_battery, LV_SYMBOL_BATTERY_FULL);
    lv_obj_set_pos(s_icon_battery, 6, 4);

    s_icon_wifi = lv_label_create(lv_layer_top());
    lv_obj_set_style_text_font(s_icon_wifi, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_icon_wifi, LV_SYMBOL_WIFI);
    lv_obj_set_pos(s_icon_wifi, (lv_coord_t)(w - 58), 4);

    s_icon_lora = lv_label_create(lv_layer_top());
    lv_obj_set_style_text_font(s_icon_lora, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_icon_lora, LV_SYMBOL_GPS);
    lv_obj_set_pos(s_icon_lora, (lv_coord_t)(w - 36), 4);

    s_icon_mail = lv_label_create(lv_layer_top());
    lv_obj_set_style_text_font(s_icon_mail, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_icon_mail, lv_color_white(), 0);
    lv_label_set_text(s_icon_mail, LV_SYMBOL_ENVELOPE);
    lv_obj_set_pos(s_icon_mail, (lv_coord_t)(w - 16), 4);
    lv_obj_add_flag(s_icon_mail, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_icon_mail, mail_icon_click_cb, LV_EVENT_CLICKED, NULL);

    s_icon_mail_badge = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_icon_mail_badge);
    lv_obj_set_size(s_icon_mail_badge, 6, 6);
    lv_obj_set_style_bg_color(s_icon_mail_badge, lv_color_make(0xE0, 0x30, 0x30), 0);
    lv_obj_set_style_bg_opa(s_icon_mail_badge, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_icon_mail_badge, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_pos(s_icon_mail_badge, (lv_coord_t)(w - 8), 2);
    lv_obj_clear_flag(s_icon_mail_badge, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_icon_mail_badge, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_icon_mail_badge, LV_OBJ_FLAG_HIDDEN);
}

static void refresh_status_icons(void)
{
    lv_obj_set_style_text_color(s_icon_wifi, purr_kernel_wifi_connected() ? ICON_ON : ICON_OFF, 0);
    lv_obj_set_style_text_color(s_icon_lora, purr_kernel_lora_available() ? ICON_ON : ICON_OFF, 0);

    if (purr_kernel_notify_count() > 0) lv_obj_clear_flag(s_icon_mail_badge, LV_OBJ_FLAG_HIDDEN);
    else                                 lv_obj_add_flag(s_icon_mail_badge, LV_OBJ_FLAG_HIDDEN);

    int pct = purr_kernel_battery_percent();
    const char *sym;
    lv_color_t  color = lv_color_white();
    if (pct < 0)        { sym = LV_SYMBOL_BATTERY_FULL;  color = ICON_OFF; } // unknown — dim, not red
    else if (pct > 80)  sym = LV_SYMBOL_BATTERY_FULL;
    else if (pct > 55)  sym = LV_SYMBOL_BATTERY_3;
    else if (pct > 30)  sym = LV_SYMBOL_BATTERY_2;
    else if (pct > 10)  sym = LV_SYMBOL_BATTERY_1;
    else                { sym = LV_SYMBOL_BATTERY_EMPTY; color = lv_color_make(0xE0, 0x40, 0x40); }
    lv_label_set_text(s_icon_battery, sym);
    lv_obj_set_style_text_color(s_icon_battery, color, 0);
}

// ── Left-edge swipe-to-home ─────────────────────────────────────────────────

static lv_coord_t s_edge_press_x, s_edge_press_y;
static bool       s_edge_press_valid;

static int current_card_idx(void)
{
    int32_t sy = lv_obj_get_scroll_y(s_stack);
    int best = 0;
    int32_t best_d = 0x7FFFFFFF;
    for (int i = 0; i < s_card_count; i++) {
        int32_t cy = lv_obj_get_y(s_cards[i].obj);
        int32_t d = cy - sy;
        if (d < 0) d = -d;
        if (d < best_d) { best_d = d; best = i; }
    }
    return best;
}

static void stack_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;

    if (code == LV_EVENT_PRESSED) {
        lv_point_t p;
        lv_indev_get_point(indev, &p);
        s_edge_press_x = p.x;
        s_edge_press_y = p.y;
        s_edge_press_valid = (p.x < EDGE_PX);
    } else if (code == LV_EVENT_RELEASED) {
        if (!s_edge_press_valid) return;
        s_edge_press_valid = false;
        lv_point_t p;
        lv_indev_get_point(indev, &p);
        lv_coord_t dx = (lv_coord_t)(p.x - s_edge_press_x);
        lv_coord_t dy = (lv_coord_t)(p.y - s_edge_press_y);
        if (dx > HOME_SWIPE_THRESH && dx > (dy < 0 ? -dy : dy) && current_card_idx() != 0) {
            lv_obj_scroll_to_y(s_stack, lv_obj_get_y(s_cards[0].obj), LV_ANIM_ON);
        }
    }
}

// ── Card construction ────────────────────────────────────────────────────────

#define CARD_MARGIN 6
#define CARD_RADIUS 16  // rounded-off look, no more cut corner

// A "card" is two objects: a full-size, invisible/transparent slot (this is
// what's positioned at exact multiples of the screen height for scroll-snap
// math, and what current_card_idx()/scroll_to() reason about), and a
// visually inset "tile" child the user actually sees — bordered, with a
// gap on all sides so the stack's black background shows through behind
// and between cards. All card content goes on the tile, never the slot.
static lv_obj_t *new_card(uint16_t w, uint16_t h, lv_obj_t **out_tile)
{
    lv_obj_t *slot = lv_obj_create(s_stack);
    lv_obj_remove_style_all(slot);
    lv_obj_set_size(slot, w, h);
    lv_obj_set_style_bg_opa(slot, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(slot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(slot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(slot, LV_OBJ_FLAG_SNAPPABLE);

    lv_obj_t *tile = lv_obj_create(slot);
    lv_obj_remove_style_all(tile);
    lv_obj_set_size(tile, (lv_coord_t)(w - 2 * CARD_MARGIN), (lv_coord_t)(h - 2 * CARD_MARGIN));
    lv_obj_set_pos(tile, CARD_MARGIN, CARD_MARGIN);
    lv_obj_set_style_bg_color(tile, lv_color_make(0x20, 0x20, 0x20), 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(tile, 2, 0);
    lv_obj_set_style_border_color(tile, lv_color_make(0x60, 0x60, 0x60), 0);
    lv_obj_set_style_radius(tile, CARD_RADIUS, 0);
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_CLICKABLE);

    if (out_tile) *out_tile = tile;
    return slot;
}

static void build_home_card(uint16_t w, uint16_t h)
{
    card_t *c = &s_cards[s_card_count++];
    memset(c, 0, sizeof(*c));
    c->is_home = true;
    c->app_idx = -1;
    lv_obj_t *tile;
    c->obj = new_card(w, h, &tile);
    // Dark blue-to-navy vertical gradient — a "wallpaper" feel without
    // carrying over the mobile-UI kernel's baked photographic background
    // (that asset was wired to a specific kernel's direct push_pixels path
    // and isn't a generally reusable Cardstack asset; a real image backdrop
    // is a follow-up once someone can tune it visually on hardware).
    lv_obj_set_style_bg_color(tile, lv_color_make(0x10, 0x18, 0x30), 0);
    lv_obj_set_style_bg_grad_color(tile, lv_color_make(0x05, 0x08, 0x14), 0);
    lv_obj_set_style_bg_grad_dir(tile, LV_GRAD_DIR_VER, 0);

    lv_coord_t tw = (lv_coord_t)(w - 2 * CARD_MARGIN);

    // Baked-in top status strip — always visible on the home card itself,
    // independent of the draggable peek/expand overlay (which still works
    // here too, same as every other card).
    lv_obj_t *strip = lv_obj_create(tile);
    lv_obj_remove_style_all(strip);
    lv_obj_set_size(strip, tw, STATUS_PEEK_H);
    lv_obj_set_pos(strip, 0, 0);
    lv_obj_set_style_bg_opa(strip, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(strip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *strip_lbl = lv_label_create(strip);
    lv_obj_set_style_text_color(strip_lbl, lv_color_white(), 0);
    lv_label_set_text(strip_lbl, "PURR OS DP1");
    lv_obj_set_pos(strip_lbl, 4, 3);

    c->clock_lbl = lv_label_create(tile);
    lv_obj_set_style_text_color(c->clock_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(c->clock_lbl, &lv_font_montserrat_14, 0);
    lv_label_set_text(c->clock_lbl, "00:00");
    lv_obj_align(c->clock_lbl, LV_ALIGN_TOP_MID, 0, 36);

    c->date_lbl = lv_label_create(tile);
    lv_obj_set_style_text_color(c->date_lbl, lv_color_make(0xC0, 0xC0, 0xC0), 0);
    lv_label_set_text(c->date_lbl, "uptime --");
    lv_obj_align(c->date_lbl, LV_ALIGN_TOP_MID, 0, 70);

    for (int i = 0; i < MAX_HOME_NOTIFS; i++) {
        c->notif_lbls[i] = lv_label_create(tile);
        lv_obj_set_style_text_color(c->notif_lbls[i], lv_color_make(0xE0, 0xE0, 0xE0), 0);
        lv_label_set_long_mode(c->notif_lbls[i], LV_LABEL_LONG_DOT);
        lv_obj_set_width(c->notif_lbls[i], (lv_coord_t)(tw - 12));
        lv_obj_set_pos(c->notif_lbls[i], 6, (lv_coord_t)(96 + i * 16));
        lv_label_set_text(c->notif_lbls[i], "");
    }

    // Dock row — the mobile-UI kernel's floating-dock aesthetic, ported as
    // an addition to the home card rather than a whole separate UI stack:
    // quick-launch icons for the first few registered apps, backed by the
    // same app_manager registry every other card uses (no baked-in apps).
    build_home_dock(tile, (uint16_t)tw);
}

// Dims the stack behind a card the moment it's tapped, so the jump to a
// full-screen app window reads as one motion ("the card opened into this")
// rather than a jump-cut. cardstack_win.c's close button calls
// cardstack_ui_on_window_closed() to clear it again — the window itself is
// shown via cw_win_show()'s lv_obj_move_foreground(), so it naturally lands
// above this overlay regardless of which one was raised first.
static lv_obj_t *s_dim_overlay;

static void build_dim_overlay(void)
{
    s_dim_overlay = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(s_dim_overlay);
    lv_obj_set_size(s_dim_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_dim_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_dim_overlay, LV_OPA_60, 0);
    lv_obj_clear_flag(s_dim_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_dim_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_dim_overlay, LV_OBJ_FLAG_HIDDEN);
}

void cardstack_ui_on_window_closed(void)
{
    if (s_dim_overlay) lv_obj_add_flag(s_dim_overlay, LV_OBJ_FLAG_HIDDEN);
}

static void app_card_open_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    ESP_LOGI(TAG, "launching app idx=%d", idx);
    if (s_dim_overlay) {
        lv_obj_clear_flag(s_dim_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_dim_overlay);
    }
    app_manager_launch_idx(idx);
}

static void build_home_dock(lv_obj_t *tile, uint16_t tw)
{
    int n = app_manager_count();
    if (n <= 0) return;
    if (n > MAX_HOME_DOCK) n = MAX_HOME_DOCK;

    lv_obj_t *dock = lv_obj_create(tile);
    lv_obj_remove_style_all(dock);
    lv_obj_set_size(dock, (lv_coord_t)(tw - 16), 48);
    lv_obj_align(dock, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_bg_color(dock, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(dock, LV_OPA_40, 0);
    lv_obj_set_style_radius(dock, 12, 0);
    lv_obj_clear_flag(dock, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(dock, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(dock, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dock, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    for (int i = 0; i < n; i++) {
        const app_entry_t *app = app_manager_get(i);
        if (!app) continue;
        lv_obj_t *icon = lv_label_create(dock);
        lv_obj_set_style_text_font(icon, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(icon, lv_color_white(), 0);
        // No per-app artwork ported from the mobile-UI kernel's hand-drawn
        // icon set yet — generic file glyph until real icons exist.
        lv_label_set_text(icon, LV_SYMBOL_FILE);
        lv_obj_clear_flag(icon, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(icon, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(icon, app_card_open_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
}

// Live-tile status strip height — the "small chunk" on top of the card.
// The remaining ~2/3 of the tile below it is the image+title area.
#define APP_STRIP_RATIO 0.32f

// Deterministic per-app tint — hashes the app's name into a muted dark
// color, so each card reads as visually distinct without looking chaotic
// (every component stays in a narrow dark range, same as the old flat
// 0x20/0x20/0x20 default, just hue-shifted per app). Deterministic on the
// name rather than actually random so a given app's card color doesn't
// jump around between reboots/rebuilds.
static uint32_t hash_str(const char *s)
{
    uint32_t h = 2166136261u; // FNV-1a
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return h;
}

static lv_color_t app_tint_color(const char *name, uint8_t base)
{
    uint32_t h = hash_str(name);
    uint8_t r = (uint8_t)(base + ((h >> 0)  & 0x1F));
    uint8_t g = (uint8_t)(base + ((h >> 8)  & 0x1F));
    uint8_t b = (uint8_t)(base + ((h >> 16) & 0x1F));
    return lv_color_make(r, g, b);
}

static void build_app_card(uint16_t w, uint16_t h, int app_idx, const char *name)
{
    card_t *c = &s_cards[s_card_count++];
    memset(c, 0, sizeof(*c));
    c->app_idx = app_idx;
    lv_obj_t *tile;
    c->obj = new_card(w, h, &tile);
    lv_obj_set_style_bg_color(tile, app_tint_color(name, 0x18), 0);

    // Whole tile is the tap target now — no separate "Open" button.
    lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(tile, app_card_open_cb, LV_EVENT_CLICKED, (void *)(intptr_t)app_idx);

    lv_coord_t tw = (lv_coord_t)(w - 2 * CARD_MARGIN);
    lv_coord_t th = (lv_coord_t)(h - 2 * CARD_MARGIN);
    lv_coord_t strip_h = (lv_coord_t)(th * APP_STRIP_RATIO);

    // Live-tile status strip — same hue family as the tile, just a touch
    // lighter, so it reads as "part of the card" rather than a separate
    // bar. Rounded only at the top to match the tile's own corners.
    lv_obj_t *strip = lv_obj_create(tile);
    lv_obj_remove_style_all(strip);
    lv_obj_set_size(strip, tw, strip_h);
    lv_obj_set_pos(strip, 0, 0);
    lv_obj_set_style_bg_color(strip, app_tint_color(name, 0x28), 0);
    lv_obj_set_style_bg_opa(strip, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(strip, CARD_RADIUS, 0);
    lv_obj_clear_flag(strip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(strip, LV_OBJ_FLAG_CLICKABLE);

    c->status_dot = lv_obj_create(strip);
    lv_obj_remove_style_all(c->status_dot);
    lv_obj_set_size(c->status_dot, 8, 8);
    lv_obj_set_style_radius(c->status_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(c->status_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(c->status_dot, lv_color_make(0x60, 0x60, 0x60), 0);
    lv_obj_clear_flag(c->status_dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(c->status_dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(c->status_dot, LV_ALIGN_LEFT_MID, 8, 0);

    c->status_lbl = lv_label_create(strip);
    lv_obj_set_style_text_color(c->status_lbl, lv_color_make(0xC0, 0xC0, 0xC0), 0);
    lv_label_set_text(c->status_lbl, "Idle");
    lv_obj_align(c->status_lbl, LV_ALIGN_LEFT_MID, 22, 0);

    // No image provided yet — fallback: name + generic icon, centered in
    // the remaining ~2/3 area below the strip. Once artwork exists for this
    // app, this becomes the full-bleed background for that area instead.
    // Both offsets are relative to the *tile's* center, shifted down by
    // half the strip height so they land in the middle of the area below it.
    lv_coord_t mid_offset = (lv_coord_t)(strip_h / 2);

    lv_obj_t *icon = lv_label_create(tile);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(icon, lv_color_make(0x90, 0x90, 0x90), 0);
    lv_label_set_text(icon, LV_SYMBOL_FILE);
    lv_obj_align(icon, LV_ALIGN_CENTER, 0, (lv_coord_t)(mid_offset - 12));

    lv_obj_t *name_lbl = lv_label_create(tile);
    lv_obj_set_style_text_color(name_lbl, lv_color_white(), 0);
    lv_label_set_text(name_lbl, name);
    lv_obj_align(name_lbl, LV_ALIGN_CENTER, 0, (lv_coord_t)(mid_offset + 12));
}

static void taskmgr_kill_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    app_manager_stop(idx);
    cardstack_ui_tick(); // refresh the list immediately
}

static void build_taskmgr_card(uint16_t w, uint16_t h)
{
    card_t *c = &s_cards[s_card_count++];
    memset(c, 0, sizeof(*c));
    c->is_taskmgr = true;
    c->app_idx = -1;
    lv_obj_t *tile;
    c->obj = new_card(w, h, &tile);

    lv_obj_t *title = lv_label_create(tile);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_label_set_text(title, "Task Manager");
    lv_obj_set_pos(title, 8, 8);

    c->taskmgr_list = lv_obj_create(tile);
    lv_obj_remove_style_all(c->taskmgr_list);
    lv_obj_set_style_bg_opa(c->taskmgr_list, LV_OPA_TRANSP, 0);
    lv_obj_set_size(c->taskmgr_list, (lv_coord_t)(w - 2 * CARD_MARGIN - 16), (lv_coord_t)(h - 2 * CARD_MARGIN - 40));
    lv_obj_set_pos(c->taskmgr_list, 8, 32);
    lv_obj_set_flex_flow(c->taskmgr_list, LV_FLEX_FLOW_COLUMN);
}

static void build_end_marker(uint16_t w, uint16_t h)
{
    (void)build_taskmgr_card; // silence unused warning if ever reordered
    build_taskmgr_card(w, h);
}

// ── Public API ────────────────────────────────────────────────────────────────

void cardstack_ui_init(void)
{
    uint16_t w = cardstack_hal_width();
    uint16_t h = cardstack_hal_height();

    // Reserve the top STATUS_PEEK_H px for the persistent status bar — the
    // card stack lives entirely below it now instead of being covered by
    // the bar's bottom edge.
    uint16_t card_h = (uint16_t)(h - STATUS_PEEK_H);

    s_stack = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(s_stack);
    lv_obj_set_size(s_stack, w, card_h);
    lv_obj_set_pos(s_stack, 0, STATUS_PEEK_H);
    lv_obj_set_style_bg_color(s_stack, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_stack, LV_OPA_COVER, 0);
    lv_obj_set_scroll_dir(s_stack, LV_DIR_VER);
    lv_obj_set_scroll_snap_y(s_stack, LV_SCROLL_SNAP_START);
    // Momentum/elastic kept scrolling well past the card the user actually
    // released on ("autoscroll") — snap should be the only thing moving it
    // once the finger lifts, never inertia.
    lv_obj_clear_flag(s_stack, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_clear_flag(s_stack, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_add_event_cb(s_stack, stack_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_stack, stack_event_cb, LV_EVENT_RELEASED, NULL);

    s_card_count = 0;
    build_home_card(w, card_h);

    int n = app_manager_count();
    for (int i = 0; i < n; i++) {
        const app_entry_t *app = app_manager_get(i);
        if (!app) continue;
        if (s_card_count >= MAX_CARDS - 1) break; // leave room for end marker
        build_app_card(w, card_h, i, app->name);
    }

    build_end_marker(w, card_h);

    // Lay cards out top-to-bottom in order (LVGL flex/column would also
    // work, but explicit positions keep current_card_idx()'s y-lookup exact).
    lv_coord_t y = 0;
    for (int i = 0; i < s_card_count; i++) {
        lv_obj_set_pos(s_cards[i].obj, 0, y);
        y += (lv_coord_t)card_h;
    }

    build_status_panel(w);
    build_status_icons(w);
    build_dim_overlay();

    ESP_LOGI(TAG, "card stack built: %d card(s)", s_card_count);
}

static void format_uptime(char *buf, size_t buf_sz)
{
    uint64_t s = purr_kernel_uptime_ms() / 1000;
    unsigned hh = (unsigned)(s / 3600);
    unsigned mm = (unsigned)((s % 3600) / 60);
    unsigned ss = (unsigned)(s % 60);
    snprintf(buf, buf_sz, "uptime %02u:%02u:%02u", hh, mm, ss);
}

static void refresh_home_card(card_t *c)
{
    char buf[40];
    format_uptime(buf, sizeof(buf));
    lv_label_set_text(c->date_lbl, buf);

    char clk[8];
    uint64_t s = purr_kernel_uptime_ms() / 1000;
    snprintf(clk, sizeof(clk), "%02u:%02u", (unsigned)((s / 3600) % 24), (unsigned)((s % 3600) / 60));
    lv_label_set_text(c->clock_lbl, clk);

    int n = purr_kernel_notify_count();
    if (n > MAX_HOME_NOTIFS) n = MAX_HOME_NOTIFS;
    for (int i = 0; i < MAX_HOME_NOTIFS; i++) {
        if (i < n) {
            purr_notification_t note;
            if (purr_kernel_notify_at(i, &note)) {
                char line[PURR_NOTIFY_TITLE_LEN + PURR_NOTIFY_BODY_LEN + 4];
                snprintf(line, sizeof(line), "%s: %s", note.title, note.body);
                lv_label_set_text(c->notif_lbls[i], line);
            }
        } else {
            lv_label_set_text(c->notif_lbls[i], "");
        }
    }
}

static void refresh_status_notif_box(void)
{
    lv_obj_clean(s_status_notif_box);
    int n = purr_kernel_notify_count();
    for (int i = 0; i < n; i++) {
        purr_notification_t note;
        if (!purr_kernel_notify_at(i, &note)) break;
        lv_obj_t *row = lv_label_create(s_status_notif_box);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_text_color(row, lv_color_white(), 0);
        char line[PURR_NOTIFY_TITLE_LEN + PURR_NOTIFY_BODY_LEN + 4];
        snprintf(line, sizeof(line), "%s: %s", note.title, note.body);
        lv_label_set_text(row, line);
    }
}

static void refresh_taskmgr_card(card_t *c)
{
    lv_obj_clean(c->taskmgr_list);
    int n = app_manager_count();
    for (int i = 0; i < n; i++) {
        const app_entry_t *app = app_manager_get(i);
        if (!app) continue;

        lv_obj_t *row = lv_obj_create(c->taskmgr_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), 26);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *lbl = lv_label_create(row);
        lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
        const char *state_str =
            app->state == APP_STATE_RUNNING ? "running" :
            app->state == APP_STATE_ERROR   ? "error"   : "idle";
        char line[48 + 12];
        snprintf(line, sizeof(line), "%s (%s)", app->name, state_str);
        lv_label_set_text(lbl, line);
        lv_obj_set_pos(lbl, 0, 4);

        if (app->state == APP_STATE_RUNNING) {
            lv_obj_t *btn = lv_btn_create(row);
            lv_obj_set_size(btn, 56, 22);
            lv_obj_align(btn, LV_ALIGN_RIGHT_MID, 0, 0);
            lv_obj_t *btn_lbl = lv_label_create(btn);
            lv_label_set_text(btn_lbl, "Kill");
            lv_obj_center(btn_lbl);
            lv_obj_add_event_cb(btn, taskmgr_kill_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        }
    }
}

static void refresh_app_card_status(card_t *c)
{
    if (c->app_idx < 0) return;
    const app_entry_t *app = app_manager_get(c->app_idx);
    if (!app) return;

    const char *text;
    lv_color_t  color;
    switch (app->state) {
        case APP_STATE_RUNNING: text = "Running"; color = lv_color_make(0x4D, 0xD0, 0x6B); break;
        case APP_STATE_ERROR:   text = "Error";   color = lv_color_make(0xE0, 0x40, 0x40); break;
        default:                text = "Idle";    color = lv_color_make(0x60, 0x60, 0x60); break;
    }
    lv_label_set_text(c->status_lbl, text);
    lv_obj_set_style_bg_color(c->status_dot, color, 0);
}

void cardstack_ui_tick(void)
{
    refresh_status_notif_box();
    refresh_status_icons();

    for (int i = 0; i < s_card_count; i++) {
        card_t *c = &s_cards[i];
        if (c->is_home)         refresh_home_card(c);
        if (c->is_taskmgr)      refresh_taskmgr_card(c);
        if (c->app_idx >= 0)    refresh_app_card_status(c);
    }
}

// trackball.c only samples its GPIOs when poll_event() is actually called —
// there's no background task of its own. Its debounce/deadzone timing (40ms)
// and move-interval throttle (120ms) are wall-clock based, so it needs to be
// drained on (close to) every main-loop tick, not on cardstack_ui_tick()'s
// ~200ms cadence — that was too sparse for the driver's own state machine to
// ever see a clean edge, which is why the trackball did nothing at all.
void cardstack_ui_poll_trackball(void)
{
    static int16_t accum = 0;
    int16_t dy;
    if (cardstack_hal_poll_trackball(&dy)) {
        accum += dy;
        if (accum > 40 || accum < -40) {
            int idx = current_card_idx();
            idx += (accum > 0) ? 1 : -1;
            if (idx < 0) idx = 0;
            if (idx >= s_card_count) idx = s_card_count - 1;
            lv_obj_scroll_to_y(s_stack, lv_obj_get_y(s_cards[idx].obj), LV_ANIM_ON);
            accum = 0;
        }
    }
}
