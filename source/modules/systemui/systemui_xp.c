// systemui_xp.c — Windows XP-style system UI: a persistent bottom taskbar
// (Start button + Start Menu, one button per running app, a clock-only
// system tray). Idle-lock timing is decided here; the lock screen itself is
// systemui_login.c's shared welcome-screen UI (purr_systemui_show_relock())
// — see this file's own "Idle lock" section below.
//
// Third implementation of the purr_systemui_host_t contract, alongside
// systemui_android.c (nav bar) and systemui_ios.c (home indicator) — see
// systemui.h. Gated CONFIG_PURR_SYSTEMUI_STYLE_XP, same single-condition
// `#if` shape as the other two styles; its stub for "module off entirely"
// is not duplicated here because there isn't one to write — every symbol
// this file defines already has a stub in systemui_android.c's own
// `#elif !defined(CONFIG_PURR_SYSTEMUI)` block (added when each symbol was
// first introduced), and a *style* file contributes no new symbols beyond
// what that block already covers.
//
// A rough draft, deliberately — the user's own framing for this whole XP
// pass. Flat single-column Start Menu (no folders/categories/"All Programs"
// flyout), clock-only tray (no battery/WiFi icons — that logic exists today
// but is private to each of the other two style files, not a shared helper;
// factoring it out is a real, separate follow-up), and no per-window title-
// bar chrome — no backend's catcall_ui_t window contract draws one today
// (see mochi_win.c's own header comment: app windows are chromeless system-
// wide, "the shell owns launching"), so minimize/maximize/close buttons on
// individual windows would mean extending that shared contract, not
// something this style file can add on its own.
//
// The taskbar owns the WHOLE bottom edge itself, unlike the other two
// styles: it never reads or needs host->suppress_navbar, because there is
// no separate nav-bar concept here to suppress — the taskbar IS the bottom
// chrome. purr_systemui_navbar_height() still reports its real height, for
// the same reason the other styles report theirs: so the host's own content
// (the desktop, in flow_home.c) knows to stay clear of it.
//
// "Recents" has no separate surface here — real XP never had one either.
// The taskbar's own running-app buttons already show and switch between
// everything running, all the time, so purr_systemui_open_recents() and
// friends are honest no-ops rather than a repurposed Start Menu.
//
// ── Auto-hide (Windows CE-style), added a pass later ────────────────────
// The taskbar hides itself the moment an app is foregrounded — full screen
// for the app, same "most screen real estate" goal a CE-style kiosk taskbar
// solves for. A thin, invisible, ALWAYS-PRESENT strip at the very bottom
// edge (independent of the taskbar's own visibility, so it still catches a
// touch while the taskbar is hidden) detects a swipe up and reveals it
// again for REVEAL_HOLD_MS before it auto-hides once more — see
// set_taskbar_visible()/reveal_strip_cb(). On the desktop the taskbar is
// simply always shown (purr_systemui_return_home() below), so none of this
// matters there.

#include "systemui.h"
#include "../../kernel/core/purr_kernel.h"
#include "../../kernel/catcalls/purr_win.h"
#include "../app_manager/app_manager.h"
#include "../user_mgr/user_mgr.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

#if defined(CONFIG_PURR_SYSTEMUI) && defined(CONFIG_PURR_SYSTEMUI_STYLE_XP)

static const char *TAG = "systemui.xp";

// ── Palette ──────────────────────────────────────────────────────────────
#define COL_TASKBAR_TOP    lv_color_hex(0x2A5CD6)
#define COL_TASKBAR_BOTTOM lv_color_hex(0x0F3A9E)
#define COL_START_BTN      lv_color_hex(0x3C9A3C)
#define COL_START_BTN_PR   lv_color_hex(0x2E7A2E)
#define COL_TASK_BTN       lv_color_hex(0x3E6FDE)
#define COL_TASK_BTN_ACT   lv_color_hex(0x1A3B8C)   // pressed-in look for the foregrounded app's own button
#define COL_LABEL          lv_color_hex(0xFFFFFF)
#define COL_MENU_BG        lv_color_hex(0xF5F5F5)
#define COL_MENU_TEXT      lv_color_hex(0x000000)
#define COL_MENU_SEP       lv_color_hex(0xD8D8D8)
#define COL_MENU_ACCENT    lv_color_hex(0xE8E8E8)   // top-level menu row hover/back fill

#define TASKBAR_H     30
#define START_BTN_W   64
#define TASK_BTN_W    88
#define TRAY_W        56
#define MENU_W        170
#define MENU_ROW_H    32

static const purr_systemui_host_t *s_host;
static int s_foreground_idx = -1;

// ── Taskbar ──────────────────────────────────────────────────────────────

static lv_obj_t *s_taskbar;
static lv_obj_t *s_start_btn;
static lv_obj_t *s_task_row;      // flex row holding running-app buttons
static lv_obj_t *s_tray_clock;

#define MAX_TASK_BTNS 16
static lv_obj_t *s_task_btns[MAX_TASK_BTNS];
static int       s_task_btn_app[MAX_TASK_BTNS];
static int        s_task_btn_count  = 0;
static int        s_last_running_sig = -1;

// Auto-hide reveal strip — see this file's header comment.
#define REVEAL_STRIP_H       12
#define REVEAL_SWIPE_MIN_PX  20
#define REVEAL_HOLD_MS       3000

static lv_obj_t  *s_reveal_strip;
static lv_coord_t s_reveal_touch_y0;
static bool       s_reveal_touch_down  = false;
static uint64_t   s_reveal_deadline_ms = 0;   // 0 = no pending auto-hide

// ── Start Menu ───────────────────────────────────────────────────────────

static lv_obj_t *s_menu_backdrop;   // full-screen scrim behind the menu, closes it on tap
static lv_obj_t *s_start_menu;
static lv_obj_t *s_menu_list;
static bool       s_menu_open = false;

// Two views sharing s_menu_list: TOP (Log Off / Programs (N) / Control
// Panel) is what opening the Start Menu always shows first; PROGRAMS is the
// full app list, one level in. Groundwork for a later real app rewrite —
// see this file's header comment — not the finished Start Menu shape.
typedef enum { MENU_VIEW_TOP, MENU_VIEW_PROGRAMS } menu_view_t;
static menu_view_t s_menu_view = MENU_VIEW_TOP;

static void set_menu_open(bool open);

// ── Launch/restore/minimize — same dispatch shape used throughout this
//    codebase (see the old flow_home.c's launch_app()) ────────────────────

static void launch_or_restore(int registry_idx)
{
    const app_entry_t *app = app_manager_get(registry_idx);
    if (!app) return;
    ESP_LOGI(TAG, "launching '%s' (idx=%d)", app->name, registry_idx);
    if (app->state == APP_STATE_RUNNING && app->window) {
        purr_win_show(app->window);
    } else {
        app_manager_launch_idx(registry_idx);
    }
    purr_systemui_enter_app(registry_idx);
}

// ── Start Menu ───────────────────────────────────────────────────────────

static void rebuild_start_menu(void);

static void menu_row_click_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    set_menu_open(false);
    launch_or_restore(idx);
}

static void menu_backdrop_click_cb(lv_event_t *e)
{
    (void)e;
    set_menu_open(false);
}

// Generic top-level-style row builder — used for Log Off / Programs (N) /
// Control Panel and the Programs view's own Back row, all of which are
// plain-label actions rather than an app entry. `value` is optional
// right-aligned detail text (Programs' own "(N)" count); NULL for none.
static lv_obj_t *build_menu_action_row(const char *label, const char *value,
                                        lv_event_cb_t cb, void *user)
{
    lv_obj_t *row = lv_obj_create(s_menu_list);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, MENU_ROW_H);
    lv_obj_set_style_bg_color(row, COL_MENU_ACCENT, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_left(row, 10, 0);
    lv_obj_set_style_pad_right(row, 8, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, user);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_color(lbl, COL_MENU_TEXT, 0);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

    if (value) {
        lv_obj_t *v = lv_label_create(row);
        lv_label_set_text(v, value);
        lv_obj_set_style_text_color(v, COL_MENU_SEP, 0);
        lv_obj_align(v, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_clear_flag(v, LV_OBJ_FLAG_CLICKABLE);
    }
    return row;
}

static void menu_logoff_cb(lv_event_t *e)
{
    (void)e;
    set_menu_open(false);
    ESP_LOGI(TAG, "log off");
    // A remote session (Milkbar's "Desktop" button, app_manager.h's remote
    // mode) has no business surviving past its own account logging out —
    // without this, the taskbar/Start Menu kept showing the OLD server's
    // apps underneath the NEXT login's session until something else
    // happened to clear it. Harmless no-op when remote mode isn't on.
    app_manager_clear_remote();
    user_mgr_logout();
    purr_systemui_return_home();
    if (s_host) purr_systemui_show_login(s_host);
}

static void menu_programs_cb(lv_event_t *e)
{
    (void)e;
    s_menu_view = MENU_VIEW_PROGRAMS;
    rebuild_start_menu();
}

static void menu_back_cb(lv_event_t *e)
{
    (void)e;
    s_menu_view = MENU_VIEW_TOP;
    rebuild_start_menu();
}

static void menu_control_panel_cb(lv_event_t *e)
{
    (void)e;
    set_menu_open(false);
    int n = app_manager_count();
    for (int i = 0; i < n; i++) {
        const app_entry_t *app = app_manager_get(i);
        if (app && strcmp(app->name, "settings") == 0) {
            launch_or_restore(i);
            return;
        }
    }
    ESP_LOGW(TAG, "Control Panel: no 'settings' app registered");
}

static void rebuild_start_menu(void)
{
    if (!s_menu_list) return;
    lv_obj_clean(s_menu_list);

    if (s_menu_view == MENU_VIEW_TOP) {
        char count_buf[8];
        snprintf(count_buf, sizeof(count_buf), "(%d)", app_manager_count());
        build_menu_action_row("Log Off",       NULL,      menu_logoff_cb,        NULL);
        build_menu_action_row("Programs",      count_buf, menu_programs_cb,      NULL);
        build_menu_action_row("Control Panel", NULL,      menu_control_panel_cb, NULL);
        return;
    }

    // MENU_VIEW_PROGRAMS — the full app list, same shape this always was,
    // plus a Back row at the top.
    build_menu_action_row("< Back", NULL, menu_back_cb, NULL);

    int n = app_manager_count();
    for (int i = 0; i < n; i++) {
        const app_entry_t *app = app_manager_get(i);
        if (!app) continue;

        lv_obj_t *row = lv_obj_create(s_menu_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, MENU_ROW_H);
        lv_obj_set_style_bg_color(row, COL_MENU_BG, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        if (i + 1 < n) {
            lv_obj_set_style_border_color(row, COL_MENU_SEP, 0);
            lv_obj_set_style_border_width(row, 1, 0);
            lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        }
        lv_obj_set_style_pad_left(row, 10, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, menu_row_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *icon = lv_img_create(row);
        lv_img_set_src(icon, s_host->icon_for_app(app->name));
        lv_img_set_zoom(icon, ICON_ZOOM(20));
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, app->name);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(lbl, (lv_coord_t)(MENU_W - 40));
        lv_obj_set_style_text_color(lbl, COL_MENU_TEXT, 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 26, 0);
        lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    }
}

static void set_menu_open(bool open)
{
    s_menu_open = open;
    if (open) {
        s_menu_view = MENU_VIEW_TOP;   // always opens fresh at the top level
        rebuild_start_menu();
        lv_obj_clear_flag(s_menu_backdrop, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_start_menu, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_menu_backdrop);
        lv_obj_move_foreground(s_start_menu);
    } else {
        lv_obj_add_flag(s_menu_backdrop, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_start_menu, LV_OBJ_FLAG_HIDDEN);
    }
}

static void start_btn_click_cb(lv_event_t *e)
{
    (void)e;
    set_menu_open(!s_menu_open);
}

// ── Taskbar running-app buttons ─────────────────────────────────────────

static void task_btn_click_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    // Real XP behaviour: clicking the ACTIVE window's own taskbar button
    // minimizes it; clicking any other (background) button restores that
    // one instead.
    if (idx == s_foreground_idx) {
        if (s_host->hide_foreground_windows) s_host->hide_foreground_windows();
        purr_systemui_return_home();
    } else {
        launch_or_restore(idx);
    }
}

// Signature of the current running set — cheap way to avoid rebuilding the
// taskbar row every single tick when nothing has actually changed. Order-
// sensitive on purpose: two different running sets should almost never
// collide, and a false negative here just means one extra rebuild, not a
// correctness bug.
static int running_signature(void)
{
    int n = app_manager_count();
    int sig = 0;
    for (int i = 0; i < n; i++) {
        const app_entry_t *a = app_manager_get(i);
        if (a && a->state == APP_STATE_RUNNING) sig = sig * 31 + (i + 1);
    }
    return sig;
}

static void rebuild_task_buttons(void)
{
    if (!s_task_row) return;
    lv_obj_clean(s_task_row);
    s_task_btn_count = 0;

    int n = app_manager_count();
    for (int i = 0; i < n && s_task_btn_count < MAX_TASK_BTNS; i++) {
        const app_entry_t *app = app_manager_get(i);
        if (!app || app->state != APP_STATE_RUNNING) continue;

        lv_obj_t *btn = lv_obj_create(s_task_row);
        lv_obj_remove_style_all(btn);
        lv_obj_set_size(btn, TASK_BTN_W, (lv_coord_t)(TASKBAR_H - 6));
        lv_obj_set_style_radius(btn, 3, 0);
        lv_obj_set_style_bg_color(btn, (i == s_foreground_idx) ? COL_TASK_BTN_ACT : COL_TASK_BTN, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(btn, task_btn_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, app->name);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(lbl, (lv_coord_t)(TASK_BTN_W - 10));
        lv_obj_set_style_text_color(lbl, COL_LABEL, 0);
        lv_obj_center(lbl);
        lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

        s_task_btns[s_task_btn_count]    = btn;
        s_task_btn_app[s_task_btn_count] = i;
        s_task_btn_count++;
    }
}

// ── System tray ──────────────────────────────────────────────────────────

static void refresh_clock(void)
{
    // A real wall clock, not uptime — systemui_ios.c/systemui_android.c's
    // own lock-screen clocks predate purr_kernel_time_now() (their comments
    // still say "no RTC/NTP anywhere in this codebase", which was true when
    // written and isn't any more: wifi_mgr's SNTP callback and a coarse
    // NVS-seeded fallback both feed it now — see purr_kernel.h's own doc
    // comment on purr_kernel_time_is_synced()). Fixing those two files'
    // stale comments/clocks is real but separate from this pass.
    //
    // UTC, not local time: this codebase has no timezone handling
    // (purr_kernel_time_from_utc_calendar()'s own doc comment: "no libc
    // TZ/mktime dependence"), so showing anything else would be a guess
    // dressed up as a fact.
    if (purr_kernel_time_is_synced()) {
        time_t now = purr_kernel_time_now();
        struct tm tm_buf;
        gmtime_r(&now, &tm_buf);
        char buf[16];
        snprintf(buf, sizeof(buf), "%02d:%02d", tm_buf.tm_hour, tm_buf.tm_min);
        lv_label_set_text(s_tray_clock, buf);
        return;
    }

    // Shouldn't happen in practice (the NVS fallback seeds before any module
    // runs), but fails toward an honest uptime readout rather than "00:00".
    uint64_t s = purr_kernel_uptime_ms() / 1000ULL;
    unsigned hh = (unsigned)(s / 3600ULL), mm = (unsigned)((s / 60ULL) % 60ULL);
    char buf[16];
    snprintf(buf, sizeof(buf), "up %u:%02u", hh, mm);
    lv_label_set_text(s_tray_clock, buf);
}

// ── Idle lock ────────────────────────────────────────────────────────────
// Shares systemui_login.c's own welcome-screen UI rather than a separate
// overlay — see systemui.h's purr_systemui_show_relock() doc comment and
// systemui_login.c's own header comment for the full reasoning. This
// function's only remaining job is deciding WHEN the timeout has fired;
// dimming, the credential UI itself, and restoring brightness on a
// successful unlock all now live in that one shared place.

static void lock_check_idle(void)
{
    if (purr_systemui_relock_active()) return;
    uint8_t timeout_min = purr_kernel_screen_timeout_min();
    uint64_t elapsed = purr_kernel_uptime_ms() - s_host->last_activity_ms();
    if (elapsed < (uint64_t)timeout_min * 60000ULL) return;

    purr_systemui_show_relock(s_host);
}

// ── Auto-hide / reveal ───────────────────────────────────────────────────

static void set_taskbar_visible(bool visible)
{
    if (visible) {
        lv_obj_clear_flag(s_taskbar, LV_OBJ_FLAG_HIDDEN);
        // Not needed while the taskbar itself is up — and leaving it
        // clickable here would sit in front of the taskbar's own bottom
        // few pixels for no reason.
        lv_obj_add_flag(s_reveal_strip, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_taskbar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_reveal_strip, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_reveal_strip);
    }
}

static void reveal_strip_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev || lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER) return;

    lv_point_t pt;
    lv_indev_get_point(indev, &pt);

    if (code == LV_EVENT_PRESSED) {
        s_reveal_touch_y0   = pt.y;
        s_reveal_touch_down = true;
    } else if (code == LV_EVENT_RELEASED) {
        if (!s_reveal_touch_down) return;
        s_reveal_touch_down = false;
        // Upward drag: y DECREASES. The strip sits at the very bottom edge,
        // so any real press here already starts near s_reveal_touch_y0 — a
        // small threshold is enough to tell an intentional swipe from a
        // stray tap.
        if ((lv_coord_t)(s_reveal_touch_y0 - pt.y) >= REVEAL_SWIPE_MIN_PX) {
            set_taskbar_visible(true);
            s_reveal_deadline_ms = purr_kernel_uptime_ms() + REVEAL_HOLD_MS;
        }
    }
}

// ── Build ────────────────────────────────────────────────────────────────

static void build_taskbar(uint16_t w, uint16_t h)
{
    s_taskbar = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_taskbar);
    lv_obj_set_size(s_taskbar, w, TASKBAR_H);
    lv_obj_set_pos(s_taskbar, 0, (lv_coord_t)(h - TASKBAR_H));
    lv_obj_set_style_bg_color(s_taskbar, COL_TASKBAR_TOP, 0);
    lv_obj_set_style_bg_grad_color(s_taskbar, COL_TASKBAR_BOTTOM, 0);
    lv_obj_set_style_bg_grad_dir(s_taskbar, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(s_taskbar, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_taskbar, LV_OBJ_FLAG_SCROLLABLE);

    s_start_btn = lv_obj_create(s_taskbar);
    lv_obj_remove_style_all(s_start_btn);
    lv_obj_set_size(s_start_btn, START_BTN_W, (lv_coord_t)(TASKBAR_H - 4));
    lv_obj_set_pos(s_start_btn, 2, 2);
    lv_obj_set_style_radius(s_start_btn, 4, 0);
    lv_obj_set_style_bg_color(s_start_btn, COL_START_BTN, 0);
    lv_obj_set_style_bg_color(s_start_btn, COL_START_BTN_PR, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(s_start_btn, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_start_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_start_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_start_btn, start_btn_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *start_lbl = lv_label_create(s_start_btn);
    lv_label_set_text(start_lbl, "Start");
    lv_obj_set_style_text_color(start_lbl, COL_LABEL, 0);
    lv_obj_center(start_lbl);
    lv_obj_clear_flag(start_lbl, LV_OBJ_FLAG_CLICKABLE);

    s_task_row = lv_obj_create(s_taskbar);
    lv_obj_remove_style_all(s_task_row);
    lv_obj_set_size(s_task_row, (lv_coord_t)(w - START_BTN_W - TRAY_W - 8), (lv_coord_t)(TASKBAR_H - 4));
    lv_obj_set_pos(s_task_row, (lv_coord_t)(START_BTN_W + 6), 2);
    lv_obj_set_style_bg_opa(s_task_row, LV_OPA_TRANSP, 0);
    lv_obj_set_layout(s_task_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_task_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(s_task_row, 4, 0);
    lv_obj_set_scroll_dir(s_task_row, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(s_task_row, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *tray = lv_obj_create(s_taskbar);
    lv_obj_remove_style_all(tray);
    lv_obj_set_size(tray, TRAY_W, (lv_coord_t)(TASKBAR_H - 4));
    lv_obj_set_pos(tray, (lv_coord_t)(w - TRAY_W - 2), 2);
    lv_obj_set_style_bg_opa(tray, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(tray, LV_OBJ_FLAG_SCROLLABLE);
    s_tray_clock = lv_label_create(tray);
    lv_obj_set_style_text_color(s_tray_clock, COL_LABEL, 0);
    lv_obj_center(s_tray_clock);
    refresh_clock();
}

static void build_reveal_strip(uint16_t w, uint16_t h)
{
    s_reveal_strip = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_reveal_strip);
    lv_obj_set_size(s_reveal_strip, w, REVEAL_STRIP_H);
    lv_obj_set_pos(s_reveal_strip, 0, (lv_coord_t)(h - REVEAL_STRIP_H));
    lv_obj_set_style_bg_opa(s_reveal_strip, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_reveal_strip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_reveal_strip, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_reveal_strip, LV_OBJ_FLAG_HIDDEN);   // taskbar starts visible — see set_taskbar_visible()
    lv_obj_add_event_cb(s_reveal_strip, reveal_strip_cb, LV_EVENT_PRESSED,  NULL);
    lv_obj_add_event_cb(s_reveal_strip, reveal_strip_cb, LV_EVENT_RELEASED, NULL);
}

static void build_start_menu(uint16_t w, uint16_t h)
{
    lv_coord_t menu_h = (lv_coord_t)(h * 60 / 100);

    // Backdrop first, so the menu itself paints on top of it (both are
    // raised together in set_menu_open(), preserving that order).
    s_menu_backdrop = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_menu_backdrop);
    lv_obj_set_size(s_menu_backdrop, w, (lv_coord_t)(h - TASKBAR_H));
    lv_obj_set_pos(s_menu_backdrop, 0, 0);
    lv_obj_set_style_bg_opa(s_menu_backdrop, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_menu_backdrop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_menu_backdrop, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_menu_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_menu_backdrop, menu_backdrop_click_cb, LV_EVENT_CLICKED, NULL);

    s_start_menu = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_start_menu);
    lv_obj_set_size(s_start_menu, MENU_W, menu_h);
    lv_obj_set_pos(s_start_menu, 0, (lv_coord_t)(h - TASKBAR_H - menu_h));
    lv_obj_set_style_bg_color(s_start_menu, COL_MENU_BG, 0);
    lv_obj_set_style_bg_opa(s_start_menu, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_start_menu, COL_TASKBAR_TOP, 0);
    lv_obj_set_style_border_width(s_start_menu, 2, 0);
    lv_obj_clear_flag(s_start_menu, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_start_menu, LV_OBJ_FLAG_HIDDEN);

    s_menu_list = lv_obj_create(s_start_menu);
    lv_obj_remove_style_all(s_menu_list);
    lv_obj_set_size(s_menu_list, LV_PCT(100), LV_PCT(100));
    lv_obj_set_layout(s_menu_list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_menu_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_menu_list, LV_DIR_VER);
}

// ── Public API ───────────────────────────────────────────────────────────

void purr_systemui_init(const purr_systemui_host_t *host)
{
    if (!host) { ESP_LOGE(TAG, "init called with NULL host — system UI disabled"); return; }
    s_host = host;

    purr_systemui_boot_login_check();

    uint16_t w = host->width();
    uint16_t h = host->height();

    build_taskbar(w, h);
    build_reveal_strip(w, h);
    build_start_menu(w, h);

    lv_group_t *g = host->group ? host->group() : NULL;
    if (g) {
        lv_group_add_obj(g, s_start_btn);
    }

    // Real credential UI, if boot_login_check above left the session logged
    // out — must be LAST, so it lands on top of everything else built here
    // by construction order alone (same trick the lock screen above already
    // relies on). See purr_systemui_show_login()'s own doc comment.
    purr_systemui_show_login(host);

    ESP_LOGI(TAG, "XP system UI built (%ux%u)", w, h);
}

void purr_systemui_tick(void)
{
    if (!s_host) return;

    refresh_clock();

    int sig = running_signature();
    if (sig != s_last_running_sig) {
        s_last_running_sig = sig;
        rebuild_task_buttons();
    }

    // Auto-hide again once a reveal's hold time is up — only meaningful
    // while an app is foregrounded (on the desktop the taskbar is simply
    // always shown, see purr_systemui_return_home()).
    if (s_reveal_deadline_ms != 0 && s_foreground_idx >= 0 &&
        purr_kernel_uptime_ms() >= s_reveal_deadline_ms) {
        s_reveal_deadline_ms = 0;
        set_taskbar_visible(false);
    }

    lock_check_idle();
}

void purr_systemui_fx_refresh(void)
{
    // Nothing translucent here to re-theme — the taskbar/menu/lock scrim are
    // all deliberately opaque (an XP taskbar never was translucent), so the
    // UI-effects toggle has nothing to flip in this style.
}

int16_t purr_systemui_navbar_height(void) { return TASKBAR_H; }

int  purr_systemui_foreground_idx(void)   { return s_foreground_idx; }

void purr_systemui_enter_app(int idx)
{
    s_foreground_idx = idx;
    s_last_running_sig = -1;   // force a task-button repaint (active highlight moved)
    set_menu_open(false);
    set_taskbar_visible(false);   // CE-style auto-hide — most screen for the app; swipe up to bring it back
    s_reveal_deadline_ms = 0;
}

void purr_systemui_return_home(void)
{
    s_foreground_idx = -1;
    s_last_running_sig = -1;
    s_reveal_deadline_ms = 0;
    set_taskbar_visible(true);   // always shown on the desktop
}

// No separate Recents surface — see this file's header comment.
void purr_systemui_open_recents(void)  { }
bool purr_systemui_recents_open(void)  { return false; }
void purr_systemui_close_recents(void) { }

bool purr_systemui_is_locked(void) { return purr_systemui_relock_active(); }

// "Makes the still-locked lock screen visible again by restoring
// brightness. Does NOT clear the locked state" — systemui.h's own contract
// for this function. That's still true here: it only turns the backlight
// back on so the (still-showing, still-password-gated) relock screen is
// visible — actually clearing the lock is systemui_login.c's try_unlock()
// succeeding, a real credential check, not a touch.
void purr_systemui_wake(void)
{
    if (!purr_systemui_relock_active()) return;
    const catcall_display_t *disp = purr_kernel_display();
    if (disp && disp->set_brightness) disp->set_brightness(255);
}

#endif // CONFIG_PURR_SYSTEMUI && CONFIG_PURR_SYSTEMUI_STYLE_XP
