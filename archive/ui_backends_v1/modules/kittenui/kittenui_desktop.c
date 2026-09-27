// kittenui_desktop.c — Windows XP Luna desktop shell
//
// Draws a 320×240 XP-style desktop:
//   • Teal desktop background
//   • 28px taskbar at bottom: Start button (green), window buttons, clock
//   • Start menu (rises above taskbar on Start press): scrollable app list
//   • App windows draggable on the desktop via lv_win
//
// Depends on kittenui (LVGL already initialised) and app_manager.

#include "kittenui_desktop.h"
#include "kittenui.h"
#include "lvgl.h"
#include "../../modules/app_manager/app_manager.h"
#include "../../kernel/core/purr_kernel.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <time.h>
#include <string.h>
#include <stdio.h>

static const char *TAG = "xp_desktop";

// ── Layout constants ──────────────────────────────────────────────────────────

#define TASKBAR_H      28
#define START_BTN_W    56
#define CLOCK_W        52
#define NOTIF_BTN_W    36
#define NOTIF_PANEL_W  200
#define NOTIF_PANEL_MAX_H 160
#define MENU_W         160
#define MENU_ITEM_H    28
#define DESKTOP_COLOR  lv_color_make(0x00, 0x80, 0x80)   // XP teal
#define TASKBAR_COLOR  lv_color_make(0x24, 0x5E, 0xDC)   // XP taskbar blue
#define START_COLOR    lv_color_make(0x3A, 0x9C, 0x23)   // XP green
#define TITLE_COLOR    lv_color_make(0x09, 0x38, 0x9D)   // XP title bar gradient start
#define WIN_BG_COLOR   lv_color_make(0xEC, 0xE9, 0xD8)   // Luna parchment

// ── State ─────────────────────────────────────────────────────────────────────

static lv_obj_t *s_desktop   = NULL;
static lv_obj_t *s_taskbar   = NULL;
static lv_obj_t *s_clock_lbl = NULL;
static lv_obj_t *s_start_menu = NULL;
static bool      s_menu_open  = false;

// Notification bell — see build_notif_panel()/kittenui_desktop_tick()
static lv_obj_t *s_notif_btn   = NULL;
static lv_obj_t *s_notif_lbl   = NULL;
static lv_obj_t *s_notif_panel = NULL;
static bool      s_notif_open  = false;
static int       s_notif_last_count = -1;   // forces first-tick redraw

// Taskbar window strip (middle section)
static lv_obj_t *s_win_strip = NULL;

// Track open windows for taskbar buttons (max 8)
#define MAX_WINDOWS 8
static struct {
    lv_obj_t  *win;
    lv_obj_t  *taskbar_btn;
    char       title[32];
    bool       used;
} s_windows[MAX_WINDOWS];

// ── Styles ────────────────────────────────────────────────────────────────────

static lv_style_t s_sty_desktop;
static lv_style_t s_sty_taskbar;
static lv_style_t s_sty_start_btn;
static lv_style_t s_sty_start_btn_pr;
static lv_style_t s_sty_menu;
static lv_style_t s_sty_menu_item;
static lv_style_t s_sty_menu_item_pr;
static lv_style_t s_sty_taskbar_btn;
static lv_style_t s_sty_taskbar_btn_pr;
static lv_style_t s_sty_clock;

static void init_styles(void)
{
    // Desktop
    lv_style_init(&s_sty_desktop);
    lv_style_set_bg_color(&s_sty_desktop, DESKTOP_COLOR);
    lv_style_set_bg_opa(&s_sty_desktop, LV_OPA_COVER);
    lv_style_set_border_width(&s_sty_desktop, 0);
    lv_style_set_pad_all(&s_sty_desktop, 0);

    // Taskbar
    lv_style_init(&s_sty_taskbar);
    lv_style_set_bg_color(&s_sty_taskbar, TASKBAR_COLOR);
    lv_style_set_bg_opa(&s_sty_taskbar, LV_OPA_COVER);
    lv_style_set_border_width(&s_sty_taskbar, 0);
    lv_style_set_radius(&s_sty_taskbar, 0);
    lv_style_set_pad_all(&s_sty_taskbar, 2);

    // Start button normal
    lv_style_init(&s_sty_start_btn);
    lv_style_set_bg_color(&s_sty_start_btn, START_COLOR);
    lv_style_set_bg_opa(&s_sty_start_btn, LV_OPA_COVER);
    lv_style_set_text_color(&s_sty_start_btn, lv_color_white());
    lv_style_set_text_font(&s_sty_start_btn, &lv_font_montserrat_14);
    lv_style_set_radius(&s_sty_start_btn, 10);
    lv_style_set_border_width(&s_sty_start_btn, 0);
    lv_style_set_shadow_width(&s_sty_start_btn, 4);
    lv_style_set_shadow_color(&s_sty_start_btn, lv_color_make(0x00, 0x40, 0x00));
    lv_style_set_shadow_ofs_y(&s_sty_start_btn, 1);
    lv_style_set_pad_hor(&s_sty_start_btn, 6);
    lv_style_set_pad_ver(&s_sty_start_btn, 3);

    // Start button pressed
    lv_style_init(&s_sty_start_btn_pr);
    lv_style_set_bg_color(&s_sty_start_btn_pr, lv_color_make(0x2A, 0x7C, 0x13));
    lv_style_set_shadow_width(&s_sty_start_btn_pr, 1);

    // Start menu panel
    lv_style_init(&s_sty_menu);
    lv_style_set_bg_color(&s_sty_menu, lv_color_make(0xEB, 0xE8, 0xD7));
    lv_style_set_bg_opa(&s_sty_menu, LV_OPA_COVER);
    lv_style_set_border_color(&s_sty_menu, lv_color_make(0x10, 0x36, 0x9E));
    lv_style_set_border_width(&s_sty_menu, 1);
    lv_style_set_radius(&s_sty_menu, 4);
    lv_style_set_shadow_width(&s_sty_menu, 8);
    lv_style_set_shadow_color(&s_sty_menu, lv_color_make(0x00, 0x00, 0x00));
    lv_style_set_shadow_opa(&s_sty_menu, LV_OPA_30);
    lv_style_set_pad_all(&s_sty_menu, 2);

    // Start menu item
    lv_style_init(&s_sty_menu_item);
    lv_style_set_bg_opa(&s_sty_menu_item, LV_OPA_TRANSP);
    lv_style_set_text_color(&s_sty_menu_item, lv_color_black());
    lv_style_set_text_font(&s_sty_menu_item, &lv_font_montserrat_14);
    lv_style_set_border_width(&s_sty_menu_item, 0);
    lv_style_set_radius(&s_sty_menu_item, 3);
    lv_style_set_pad_hor(&s_sty_menu_item, 8);
    lv_style_set_pad_ver(&s_sty_menu_item, 4);
    lv_style_set_min_height(&s_sty_menu_item, MENU_ITEM_H);

    // Start menu item pressed / hovered
    lv_style_init(&s_sty_menu_item_pr);
    lv_style_set_bg_color(&s_sty_menu_item_pr, lv_color_make(0x31, 0x6A, 0xC5));
    lv_style_set_bg_opa(&s_sty_menu_item_pr, LV_OPA_COVER);
    lv_style_set_text_color(&s_sty_menu_item_pr, lv_color_white());

    // Taskbar app button normal
    lv_style_init(&s_sty_taskbar_btn);
    lv_style_set_bg_color(&s_sty_taskbar_btn, lv_color_make(0x3A, 0x6E, 0xE8));
    lv_style_set_bg_opa(&s_sty_taskbar_btn, LV_OPA_COVER);
    lv_style_set_text_color(&s_sty_taskbar_btn, lv_color_white());
    lv_style_set_text_font(&s_sty_taskbar_btn, &lv_font_montserrat_14);
    lv_style_set_border_color(&s_sty_taskbar_btn, lv_color_make(0x60, 0x90, 0xFF));
    lv_style_set_border_width(&s_sty_taskbar_btn, 1);
    lv_style_set_radius(&s_sty_taskbar_btn, 2);
    lv_style_set_pad_hor(&s_sty_taskbar_btn, 4);
    lv_style_set_pad_ver(&s_sty_taskbar_btn, 2);

    // Taskbar button pressed (window focused)
    lv_style_init(&s_sty_taskbar_btn_pr);
    lv_style_set_bg_color(&s_sty_taskbar_btn_pr, lv_color_make(0x1A, 0x4E, 0xC8));
    lv_style_set_border_color(&s_sty_taskbar_btn_pr, lv_color_white());

    // Clock label
    lv_style_init(&s_sty_clock);
    lv_style_set_bg_opa(&s_sty_clock, LV_OPA_TRANSP);
    lv_style_set_text_color(&s_sty_clock, lv_color_white());
    lv_style_set_text_font(&s_sty_clock, &lv_font_montserrat_14);
    lv_style_set_pad_hor(&s_sty_clock, 4);
    lv_style_set_pad_ver(&s_sty_clock, 0);
}

// ── Start menu ────────────────────────────────────────────────────────────────

static void menu_item_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);

    // Close the menu
    if (s_start_menu) {
        lv_obj_del(s_start_menu);
        s_start_menu = NULL;
    }
    s_menu_open = false;

    const app_entry_t *app = app_manager_get(idx);
    if (app) {
        ESP_LOGI(TAG, "launching: %s", app->name);
        app_manager_launch_idx(idx);
    }
}

static void build_start_menu(void)
{
    lv_coord_t sw = lv_disp_get_hor_res(NULL);
    lv_coord_t sh = lv_disp_get_ver_res(NULL);

    int app_count = app_manager_count();
    int menu_h = 8 + (app_count > 0 ? app_count : 1) * MENU_ITEM_H;
    if (menu_h > sh - TASKBAR_H - 4) menu_h = sh - TASKBAR_H - 4;

    s_start_menu = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_start_menu);
    lv_obj_add_style(s_start_menu, &s_sty_menu, 0);
    lv_obj_set_size(s_start_menu, MENU_W, menu_h);
    lv_obj_set_pos(s_start_menu, 0, sh - TASKBAR_H - menu_h - 2);
    lv_obj_set_flex_flow(s_start_menu, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_snap_y(s_start_menu, LV_SCROLL_SNAP_NONE);
    lv_obj_set_scrollbar_mode(s_start_menu, LV_SCROLLBAR_MODE_AUTO);

    if (app_count == 0) {
        lv_obj_t *lbl = lv_label_create(s_start_menu);
        lv_label_set_text(lbl, "No apps installed");
        lv_obj_set_style_text_color(lbl, lv_color_make(0x80, 0x80, 0x80), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_pad_all(lbl, 8, 0);
        return;
    }

    for (int i = 0; i < app_count; i++) {
        const app_entry_t *app = app_manager_get(i);
        if (!app) continue;

        lv_obj_t *btn = lv_btn_create(s_start_menu);
        lv_obj_remove_style_all(btn);
        lv_obj_add_style(btn, &s_sty_menu_item, 0);
        lv_obj_add_style(btn, &s_sty_menu_item_pr, LV_STATE_PRESSED);
        lv_obj_set_width(btn, LV_PCT(100));
        lv_obj_set_height(btn, MENU_ITEM_H);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, app->name);
        lv_obj_center(lbl);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);

        lv_obj_add_event_cb(btn, menu_item_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
    }
}

static void close_notif_panel(void);   // defined below, in the notif-bell section

static void start_btn_cb(lv_event_t *e)
{
    (void)e;
    if (s_notif_open) close_notif_panel();
    if (s_menu_open) {
        if (s_start_menu) { lv_obj_del(s_start_menu); s_start_menu = NULL; }
        s_menu_open = false;
    } else {
        build_start_menu();
        s_menu_open = true;
    }
}

// Close menu on desktop click
static void desktop_click_cb(lv_event_t *e)
{
    (void)e;
    if (s_menu_open && s_start_menu) {
        lv_obj_del(s_start_menu);
        s_start_menu = NULL;
        s_menu_open  = false;
    }
    if (s_notif_open && s_notif_panel) {
        lv_obj_del(s_notif_panel);
        s_notif_panel = NULL;
        s_notif_open  = false;
    }
}

// ── Notification bell ─────────────────────────────────────────────────────────
// Renders whatever's in the kernel's notification ring buffer
// (purr_kernel_notify*() — see purr_kernel.h) — every producer just calls
// purr_kernel_notify(title, body, source) and it shows up here automatically,
// same as it already does on Cardstack.

static void close_notif_panel(void)
{
    if (s_notif_panel) { lv_obj_del(s_notif_panel); s_notif_panel = NULL; }
    s_notif_open = false;
}

static void notif_clear_cb(lv_event_t *e)
{
    (void)e;
    purr_kernel_notify_clear();
    close_notif_panel();
}

static void build_notif_panel(void)
{
    lv_coord_t sh = lv_disp_get_ver_res(NULL);
    lv_coord_t sw = lv_disp_get_hor_res(NULL);

    int n = purr_kernel_notify_count();
    int row_h = MENU_ITEM_H + 8;
    int panel_h = 8 + (n > 0 ? n * row_h : MENU_ITEM_H) + MENU_ITEM_H;
    if (panel_h > NOTIF_PANEL_MAX_H) panel_h = NOTIF_PANEL_MAX_H;

    s_notif_panel = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_notif_panel);
    lv_obj_add_style(s_notif_panel, &s_sty_menu, 0);
    lv_obj_set_size(s_notif_panel, NOTIF_PANEL_W, panel_h);
    lv_obj_set_pos(s_notif_panel, sw - NOTIF_PANEL_W, sh - TASKBAR_H - panel_h - 2);
    lv_obj_set_flex_flow(s_notif_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(s_notif_panel, LV_SCROLLBAR_MODE_AUTO);

    if (n == 0) {
        lv_obj_t *lbl = lv_label_create(s_notif_panel);
        lv_label_set_text(lbl, "No notifications");
        lv_obj_set_style_text_color(lbl, lv_color_make(0x80, 0x80, 0x80), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_pad_all(lbl, 8, 0);
        return;
    }

    for (int i = 0; i < n; i++) {
        purr_notification_t note;
        if (!purr_kernel_notify_at(i, &note)) continue;

        lv_obj_t *row = lv_obj_create(s_notif_panel);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, row_h);
        lv_obj_set_style_pad_all(row, 4, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *title = lv_label_create(row);
        lv_label_set_text(title, note.title);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(title, lv_color_black(), 0);
        lv_obj_set_width(title, LV_PCT(100));
        lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);

        lv_obj_t *body = lv_label_create(row);
        lv_label_set_text(body, note.body);
        lv_obj_set_style_text_font(body, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(body, lv_color_make(0x50, 0x50, 0x50), 0);
        lv_obj_set_width(body, LV_PCT(100));
        lv_obj_set_pos(body, 0, 16);
        lv_label_set_long_mode(body, LV_LABEL_LONG_DOT);
    }

    lv_obj_t *clear_btn = lv_btn_create(s_notif_panel);
    lv_obj_remove_style_all(clear_btn);
    lv_obj_add_style(clear_btn, &s_sty_menu_item, 0);
    lv_obj_add_style(clear_btn, &s_sty_menu_item_pr, LV_STATE_PRESSED);
    lv_obj_set_width(clear_btn, LV_PCT(100));
    lv_obj_t *clear_lbl = lv_label_create(clear_btn);
    lv_label_set_text(clear_lbl, "Clear all");
    lv_obj_center(clear_lbl);
    lv_obj_add_event_cb(clear_btn, notif_clear_cb, LV_EVENT_CLICKED, NULL);
}

static void notif_btn_cb(lv_event_t *e)
{
    (void)e;
    if (s_menu_open) { lv_obj_del(s_start_menu); s_start_menu = NULL; s_menu_open = false; }
    if (s_notif_open) {
        close_notif_panel();
    } else {
        build_notif_panel();
        s_notif_open = true;
    }
}

// ── Taskbar window button ─────────────────────────────────────────────────────

static void taskbar_win_btn_cb(lv_event_t *e)
{
    lv_obj_t *win = (lv_obj_t *)lv_event_get_user_data(e);
    if (!win) return;
    bool hidden = !lv_obj_is_visible(win);
    if (hidden) {
        lv_obj_clear_flag(win, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(win);
    } else {
        lv_obj_add_flag(win, LV_OBJ_FLAG_HIDDEN);
    }
}

static void win_close_cb(lv_event_t *e)
{
    lv_obj_t *win = (lv_obj_t *)lv_event_get_user_data(e);
    if (!win) return;

    // Remove taskbar button and slot
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (s_windows[i].used && s_windows[i].win == win) {
            if (s_windows[i].taskbar_btn)
                lv_obj_del(s_windows[i].taskbar_btn);
            s_windows[i].used = false;
            break;
        }
    }
    lv_obj_del(win);
}

// ── Window creation ───────────────────────────────────────────────────────────

lv_obj_t *kittenui_desktop_open_window(const char *title)
{
    // Find free slot
    int slot = -1;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!s_windows[i].used) { slot = i; break; }
    }
    if (slot < 0) {
        ESP_LOGW(TAG, "max windows reached");
        return NULL;
    }

    lv_coord_t sw = lv_disp_get_hor_res(NULL);
    lv_coord_t sh = lv_disp_get_ver_res(NULL);
    lv_coord_t win_h = sh - TASKBAR_H - 4;

    // lv_win gives us a titled panel with content area
    lv_obj_t *win = lv_win_create(s_desktop, 24);
    lv_win_add_title(win, title);

    // Style the title bar
    lv_obj_t *hdr = lv_win_get_header(win);
    lv_obj_set_style_bg_color(hdr, TITLE_COLOR, 0);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(hdr, lv_color_white(), 0);
    lv_obj_set_style_text_font(hdr, &lv_font_montserrat_14, 0);

    // Close button in title bar
    lv_obj_t *close_btn = lv_win_add_btn(win, LV_SYMBOL_CLOSE, 22);
    lv_obj_set_style_bg_color(close_btn, lv_color_make(0xC0, 0x20, 0x20), 0);
    lv_obj_set_style_bg_opa(close_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(close_btn, lv_color_white(), 0);
    lv_obj_set_style_radius(close_btn, 2, 0);
    lv_obj_add_event_cb(close_btn, win_close_cb, LV_EVENT_CLICKED, win);

    // Content area style (Luna parchment)
    lv_obj_t *cont = lv_win_get_content(win);
    lv_obj_set_style_bg_color(cont, WIN_BG_COLOR, 0);
    lv_obj_set_style_bg_opa(cont, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(cont, 6, 0);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    // Window border and position
    lv_obj_set_style_border_color(win, lv_color_make(0x10, 0x36, 0x9E), 0);
    lv_obj_set_style_border_width(win, 1, 0);
    lv_obj_set_style_radius(win, 4, 0);
    lv_obj_set_style_shadow_width(win, 8, 0);
    lv_obj_set_style_shadow_color(win, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(win, LV_OPA_30, 0);
    lv_obj_set_size(win, sw - 2, win_h);
    lv_obj_set_pos(win, 1, 1);

    // Taskbar button for this window
    lv_obj_t *tbtn = lv_btn_create(s_win_strip);
    lv_obj_remove_style_all(tbtn);
    lv_obj_add_style(tbtn, &s_sty_taskbar_btn, 0);
    lv_obj_add_style(tbtn, &s_sty_taskbar_btn_pr, LV_STATE_PRESSED);
    lv_obj_set_height(tbtn, TASKBAR_H - 6);
    lv_obj_set_width(tbtn, 80);

    lv_obj_t *tbtn_lbl = lv_label_create(tbtn);
    lv_label_set_text(tbtn_lbl, title);
    lv_label_set_long_mode(tbtn_lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(tbtn_lbl, 72);
    lv_obj_center(tbtn_lbl);
    lv_obj_set_style_text_font(tbtn_lbl, &lv_font_montserrat_14, 0);

    lv_obj_add_event_cb(tbtn, taskbar_win_btn_cb, LV_EVENT_CLICKED, win);

    // Register slot
    s_windows[slot].win         = win;
    s_windows[slot].taskbar_btn = tbtn;
    s_windows[slot].used        = true;
    strncpy(s_windows[slot].title, title, sizeof(s_windows[slot].title) - 1);

    ESP_LOGI(TAG, "opened window: %s (slot %d)", title, slot);
    return win;
}

// ── Desktop init ──────────────────────────────────────────────────────────────

void kittenui_desktop_init(void)
{
    lv_coord_t sw = lv_disp_get_hor_res(NULL);
    lv_coord_t sh = lv_disp_get_ver_res(NULL);

    memset(s_windows, 0, sizeof(s_windows));

    init_styles();

    // ── Desktop background ────────────────────────────────────────────────────
    s_desktop = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(s_desktop);
    lv_obj_add_style(s_desktop, &s_sty_desktop, 0);
    lv_obj_set_size(s_desktop, sw, sh - TASKBAR_H);
    lv_obj_set_pos(s_desktop, 0, 0);
    lv_obj_clear_flag(s_desktop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_desktop, desktop_click_cb, LV_EVENT_CLICKED, NULL);

    // ── Taskbar ───────────────────────────────────────────────────────────────
    s_taskbar = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(s_taskbar);
    lv_obj_add_style(s_taskbar, &s_sty_taskbar, 0);
    lv_obj_set_size(s_taskbar, sw, TASKBAR_H);
    lv_obj_set_pos(s_taskbar, 0, sh - TASKBAR_H);
    lv_obj_clear_flag(s_taskbar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s_taskbar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_taskbar, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_taskbar, 3, 0);

    // Start button
    lv_obj_t *start = lv_btn_create(s_taskbar);
    lv_obj_remove_style_all(start);
    lv_obj_add_style(start, &s_sty_start_btn, 0);
    lv_obj_add_style(start, &s_sty_start_btn_pr, LV_STATE_PRESSED);
    lv_obj_set_size(start, START_BTN_W, TASKBAR_H - 4);

    lv_obj_t *start_lbl = lv_label_create(start);
    lv_label_set_text(start_lbl, LV_SYMBOL_HOME "  start");
    lv_obj_center(start_lbl);
    lv_obj_set_style_text_font(start_lbl, &lv_font_montserrat_14, 0);

    lv_obj_add_event_cb(start, start_btn_cb, LV_EVENT_CLICKED, NULL);

    // Separator line
    lv_obj_t *sep = lv_obj_create(s_taskbar);
    lv_obj_remove_style_all(sep);
    lv_obj_set_size(sep, 1, TASKBAR_H - 6);
    lv_obj_set_style_bg_color(sep, lv_color_make(0x60, 0x90, 0xFF), 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);

    // Window button strip (flex-grow to fill available space)
    s_win_strip = lv_obj_create(s_taskbar);
    lv_obj_remove_style_all(s_win_strip);
    lv_obj_set_style_bg_opa(s_win_strip, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_grow(s_win_strip, 1);
    lv_obj_set_height(s_win_strip, TASKBAR_H);
    lv_obj_set_flex_flow(s_win_strip, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_win_strip, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_win_strip, 2, 0);
    lv_obj_clear_flag(s_win_strip, LV_OBJ_FLAG_SCROLLABLE);

    // Notification bell (between window strip and clock)
    s_notif_btn = lv_btn_create(s_taskbar);
    lv_obj_remove_style_all(s_notif_btn);
    lv_obj_set_size(s_notif_btn, NOTIF_BTN_W, TASKBAR_H - 6);
    lv_obj_set_style_bg_opa(s_notif_btn, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(s_notif_btn, notif_btn_cb, LV_EVENT_CLICKED, NULL);

    s_notif_lbl = lv_label_create(s_notif_btn);
    lv_obj_set_style_text_color(s_notif_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_notif_lbl, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_notif_lbl, LV_SYMBOL_BELL);
    lv_obj_center(s_notif_lbl);

    // Clock (right-aligned)
    s_clock_lbl = lv_label_create(s_taskbar);
    lv_obj_remove_style_all(s_clock_lbl);
    lv_obj_add_style(s_clock_lbl, &s_sty_clock, 0);
    lv_obj_set_width(s_clock_lbl, CLOCK_W);
    lv_label_set_text(s_clock_lbl, "00:00");
    lv_obj_set_style_text_align(s_clock_lbl, LV_TEXT_ALIGN_RIGHT, 0);

    // Initial clock draw and app scan
    kittenui_desktop_tick();

    // Scan for apps now that desktop is up
    app_manager_scan();

    ESP_LOGI(TAG, "XP desktop ready (%dx%d), %d apps",
             (int)sw, (int)sh, app_manager_count());
}

// ── Clock tick ────────────────────────────────────────────────────────────────

void kittenui_desktop_tick(void)
{
    if (!s_clock_lbl) return;

    // Use FreeRTOS tick count as a simple clock source (ms since boot)
    uint32_t ticks_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    uint32_t secs  = ticks_ms / 1000;
    uint32_t mins  = (secs / 60) % 60;
    uint32_t hours = (secs / 3600) % 24;

    char buf[8];
    snprintf(buf, sizeof(buf), "%02lu:%02lu", (unsigned long)hours, (unsigned long)mins);
    lv_label_set_text(s_clock_lbl, buf);

    // Notification bell badge — only touch the label (and re-render an
    // already-open panel) when the count actually changed, same idea as the
    // clock only needing to move once a minute; avoids rebuilding LVGL
    // objects every tick for no reason.
    int count = purr_kernel_notify_count();
    if (count != s_notif_last_count) {
        s_notif_last_count = count;
        char nbuf[16];
        if (count > 0) snprintf(nbuf, sizeof(nbuf), LV_SYMBOL_BELL " %d", count);
        else           snprintf(nbuf, sizeof(nbuf), LV_SYMBOL_BELL);
        if (s_notif_lbl) lv_label_set_text(s_notif_lbl, nbuf);
        if (s_notif_open) { close_notif_panel(); build_notif_panel(); s_notif_open = true; }
    }
}
