// systemui_login.c — Windows XP-style login/welcome screen for PURR OS.
//
// Shared across every host that hosts source/modules/systemui/ (Cupcake,
// Mochi, Flow, Tabby), the same way the status bar and lock screen already
// are — see systemui.h's own doc comment on purr_systemui_show_login() for
// the call-site contract. NOT split by CONFIG_PURR_SYSTEMUI_STYLE_* the way
// systemui_android.c/systemui_ios.c are: the login prompt is one
// implementation regardless of which chrome style the rest of systemui is
// drawing, so this file gates only on the plain module-on/off symbol. Its
// stub (for when the module is off entirely) lives with every other stub in
// systemui_android.c's own `#elif !defined(CONFIG_PURR_SYSTEMUI)` block —
// not duplicated here — so exactly one definition of the symbol ever exists,
// same rule the rest of this module already follows.
//
// ── Redesigned to the user's own spec ────────────────────────────────────
// No tile picker any more — a real, deliberate simplification, not the
// original design degrading. Exactly one account is shown directly (avatar,
// name, password field, all always visible, no tap-to-reveal step):
// user_mgr_default_username() at boot, user_mgr_current_user() on relock —
// both already exist precisely to answer "which account does a login flow
// seed itself with". Multi-account switching is a real, known scope cut for
// this pass, not an oversight — this device has exactly one account today,
// and re-adding a switcher later is additive, not a rework.
//
// ── Also the idle-lock screen now ───────────────────────────────────────
// purr_systemui_show_relock() (systemui.h) drives the SAME screen for the
// idle-timeout case, rather than each systemui style building its own
// separate overlay — real Windows locks to the same welcome screen it boots
// to, and a style's own lock_check_idle()-equivalent just calls this
// instead. relock adds a "Logged on" tag under the name and a scrollable
// notification list below the password row (sourced directly from
// purr_kernel_notify_count()/_at(), the same global ring systemui_ios.c's
// own lock screen already reads via its shared build_notif_card()) — and
// DROPS the OSK-toggle/power/codename row a fresh login shows, since
// mid-session "power off" isn't the same category of choice as it is at a
// cold boot. Verifying the password re-arms it either way — locking is a
// real security boundary, not a dismiss gesture — and success restores
// display brightness and clears purr_systemui_relock_active() itself, so a
// caller's own idle-check and this file's dim/undim pairing can't drift out
// of sync across two files.
//
// Built on lv_layer_top(), the same layer the taskbar/status bar use — see
// purr_systemui_show_login()'s doc comment for why it must be called LAST in
// the host's init() to land on top of everything else by construction order.
// Hidden, not destroyed, on success: this is called again on every relock,
// so the same lv_obj tree is reused for the life of the boot rather than
// rebuilt each time — see enter_login_ui() below.

#include "systemui.h"
#include "../user_mgr/user_mgr.h"
#include "esp_log.h"
#include <string.h>
#include <ctype.h>

#ifdef CONFIG_PURR_SYSTEMUI

static const char *TAG = "systemui_login";

// XP welcome-screen palette — a simplified approximation (a flat vertical
// gradient stands in for the real wallpaper/logo artwork), not a pixel-
// accurate recreation. Polish, if it happens, is a separate pass.
#define COL_BG_TOP     lv_color_hex(0x5A8FD6)
#define COL_BG_BOTTOM  lv_color_hex(0x0A246A)
#define COL_AVATAR_BG  lv_color_hex(0x3A6EA5)
#define COL_AVATAR_FR  lv_color_hex(0xFFFFFF)
#define COL_LABEL      lv_color_hex(0xFFFFFF)
#define COL_LABEL_DIM  lv_color_hex(0xC7D6EF)
#define COL_ERROR      lv_color_hex(0xFFCFCF)
#define COL_GO_BTN     lv_color_hex(0x4CAF50)
#define COL_LOGGED_ON  lv_color_hex(0xBEE8B0)
#define COL_ICON_BTN   lv_color_hex(0x3A6EA5)
#define COL_POWER_BTN  lv_color_hex(0xB03A3A)
#define COL_NOTIF_BG   lv_color_hex(0xFFFFFF)
#define COL_NOTIF_TEXT lv_color_hex(0x1C1C1E)
#define COL_NOTIF_DIM  lv_color_hex(0x6D6D72)

#define AVATAR_SZ   36
#define ICON_BTN_SZ 28
#define NOTIF_ROW_H 26

static const purr_systemui_host_t *s_host;
static bool s_built         = false;
static bool s_relock_mode   = false;
static bool s_relock_active = false;
static char s_current_username[USER_MGR_USERNAME_MAX];

static lv_obj_t *s_screen;
static lv_obj_t *s_title_lbl;
static lv_obj_t *s_avatar_initial_lbl;
static lv_obj_t *s_name_lbl;
static lv_obj_t *s_logged_on_lbl;

static lv_obj_t *s_ta;
static lv_obj_t *s_status_lbl;
static lv_obj_t *s_keyboard;

static lv_obj_t *s_osk_btn;
static lv_obj_t *s_power_btn;
static lv_obj_t *s_codename_lbl;

static lv_obj_t *s_notif_list;

static void try_unlock(void);
static void rebuild_notifications(void);
static void enter_login_ui(const purr_systemui_host_t *host, bool relock);

static void go_btn_cb(lv_event_t *e)   { (void)e; try_unlock(); }
static void ta_ready_cb(lv_event_t *e) { (void)e; try_unlock(); }

static void ta_changed_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_set_style_outline_opa(s_ta, LV_OPA_TRANSP, 0);
    lv_label_set_text(s_status_lbl, "");
}

// Shows/hides the on-screen keyboard on demand — this device has a physical
// one, so it never needed to be automatic, but the contract has no "has a
// physical keyboard" host hook (see the old header comment this replaced),
// and a manual toggle is simple, always-correct groundwork either way.
static void osk_toggle_cb(lv_event_t *e)
{
    (void)e;
    if (!s_keyboard) return;
    if (lv_obj_has_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_clear_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_keyboard);
    } else {
        lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    }
}

static void power_btn_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "power button — shutting down");
    purr_kernel_shutdown();
}

static void try_unlock(void)
{
    if (!s_ta || s_current_username[0] == '\0') return;
    const char *pw = lv_textarea_get_text(s_ta);

    if (user_mgr_verify(s_current_username, pw)) {
        ESP_LOGI(TAG, "%s OK for '%s'", s_relock_mode ? "unlock" : "login", s_current_username);
        user_mgr_set_logged_in(s_current_username);

        if (s_relock_mode) {
            // Owning both halves of the dim/undim pairing here (see this
            // file's header comment) is the whole point — restoring
            // brightness lives next to the dim call in
            // purr_systemui_show_relock() below, not split into whichever
            // style called it.
            s_relock_active = false;
            const catcall_display_t *disp = purr_kernel_display();
            if (disp && disp->set_brightness) disp->set_brightness(255);
        }

        if (s_keyboard) lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_screen, LV_OBJ_FLAG_HIDDEN);   // hide, not delete — see this file's header comment
        return;
    }

    ESP_LOGW(TAG, "%s failed for '%s'", s_relock_mode ? "unlock" : "login", s_current_username);
    lv_textarea_set_text(s_ta, "");
    lv_label_set_text(s_status_lbl, "The password is incorrect. Try again.");
    lv_obj_set_style_outline_color(s_ta, COL_ERROR, 0);
    lv_obj_set_style_outline_width(s_ta, 2, 0);
    lv_obj_set_style_outline_opa(s_ta, LV_OPA_COVER, 0);
}

// ── Notifications (relock only) ─────────────────────────────────────────

static void rebuild_notifications(void)
{
    if (!s_notif_list) return;
    lv_obj_clean(s_notif_list);

    int n = purr_kernel_notify_count();
    if (n == 0) {
        lv_obj_t *empty = lv_label_create(s_notif_list);
        lv_label_set_text(empty, "No notifications");
        lv_obj_set_style_text_color(empty, COL_LABEL_DIM, 0);
        return;
    }

    for (int i = 0; i < n; i++) {
        purr_notification_t note;
        if (!purr_kernel_notify_at(i, &note)) continue;

        lv_obj_t *row = lv_obj_create(s_notif_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, NOTIF_ROW_H);
        lv_obj_set_style_bg_color(row, COL_NOTIF_BG, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_90, 0);
        lv_obj_set_style_radius(row, 4, 0);
        lv_obj_set_style_pad_left(row, 6, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *title = lv_label_create(row);
        lv_label_set_text(title, note.title[0] ? note.title : note.source);
        lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
        lv_obj_set_width(title, LV_PCT(96));
        lv_obj_set_style_text_color(title, COL_NOTIF_TEXT, 0);
        lv_obj_set_pos(title, 0, 1);
        lv_obj_clear_flag(title, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *body = lv_label_create(row);
        lv_label_set_text(body, note.body);
        lv_label_set_long_mode(body, LV_LABEL_LONG_DOT);
        lv_obj_set_width(body, LV_PCT(96));
        lv_obj_set_style_text_color(body, COL_NOTIF_DIM, 0);
        lv_obj_set_pos(body, 0, 13);
        lv_obj_clear_flag(body, LV_OBJ_FLAG_CLICKABLE);
    }
}

// ── Build / re-show ──────────────────────────────────────────────────────
// One shared entry point for both the boot-time login call and every
// relock: the first call builds the whole screen; every call after that
// just resets it and re-shows it. See this file's header comment for why
// relock reuses this instead of a second implementation.
static void enter_login_ui(const purr_systemui_host_t *host, bool relock)
{
    const char *username = relock ? user_mgr_current_user() : user_mgr_default_username();
    if (!username || !username[0]) {
        ESP_LOGW(TAG, "no username to show — nothing to display");
        return;
    }

    s_host = host;
    s_relock_mode = relock;
    strncpy(s_current_username, username, sizeof(s_current_username) - 1);
    s_current_username[sizeof(s_current_username) - 1] = '\0';

    if (!s_built) {
        uint16_t w = host->width();
        uint16_t h = host->height();

        s_screen = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(s_screen);
        lv_obj_set_size(s_screen, w, h);
        lv_obj_set_pos(s_screen, 0, 0);
        lv_obj_set_style_bg_color(s_screen, COL_BG_TOP, 0);
        lv_obj_set_style_bg_grad_color(s_screen, COL_BG_BOTTOM, 0);
        lv_obj_set_style_bg_grad_dir(s_screen, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
        lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(s_screen, LV_OBJ_FLAG_CLICKABLE);   // eats touches — nothing behind it should receive them

        s_title_lbl = lv_label_create(s_screen);
        lv_obj_set_style_text_color(s_title_lbl, COL_LABEL, 0);
        lv_obj_set_style_text_font(s_title_lbl, &lv_font_montserrat_32, 0);
        lv_obj_align(s_title_lbl, LV_ALIGN_TOP_MID, 0, (lv_coord_t)(PURR_SYSTEMUI_STATUS_H + 2));

        s_codename_lbl = lv_label_create(s_screen);
        lv_label_set_text(s_codename_lbl, "PURR OS \xE2\x80\x94 Loooong Cat");   // em dash
        lv_obj_set_style_text_color(s_codename_lbl, COL_LABEL_DIM, 0);
        lv_obj_set_style_text_font(s_codename_lbl, &lv_font_montserrat_14, 0);
        lv_obj_align(s_codename_lbl, LV_ALIGN_TOP_MID, 0, (lv_coord_t)(PURR_SYSTEMUI_STATUS_H + 34));

        lv_coord_t avatar_y = (lv_coord_t)(PURR_SYSTEMUI_STATUS_H + 52);
        lv_obj_t *frame = lv_obj_create(s_screen);
        lv_obj_remove_style_all(frame);
        lv_obj_set_size(frame, AVATAR_SZ + 6, AVATAR_SZ + 6);
        lv_obj_align(frame, LV_ALIGN_TOP_MID, 0, avatar_y);
        lv_obj_set_style_bg_color(frame, COL_AVATAR_FR, 0);
        lv_obj_set_style_bg_opa(frame, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(frame, 6, 0);
        lv_obj_clear_flag(frame, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *avatar = lv_obj_create(frame);
        lv_obj_remove_style_all(avatar);
        lv_obj_set_size(avatar, AVATAR_SZ, AVATAR_SZ);
        lv_obj_center(avatar);
        lv_obj_set_style_bg_color(avatar, COL_AVATAR_BG, 0);
        lv_obj_set_style_bg_opa(avatar, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(avatar, 4, 0);
        lv_obj_clear_flag(avatar, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

        s_avatar_initial_lbl = lv_label_create(avatar);
        lv_obj_set_style_text_color(s_avatar_initial_lbl, lv_color_white(), 0);
        lv_obj_set_style_text_font(s_avatar_initial_lbl, &lv_font_montserrat_32, 0);
        lv_obj_center(s_avatar_initial_lbl);
        lv_obj_clear_flag(s_avatar_initial_lbl, LV_OBJ_FLAG_CLICKABLE);

        s_name_lbl = lv_label_create(s_screen);
        lv_obj_set_style_text_color(s_name_lbl, COL_LABEL, 0);
        lv_obj_set_style_text_font(s_name_lbl, &lv_font_montserrat_14, 0);
        lv_obj_align(s_name_lbl, LV_ALIGN_TOP_MID, 0, (lv_coord_t)(avatar_y + AVATAR_SZ + 8));

        s_logged_on_lbl = lv_label_create(s_screen);
        lv_label_set_text(s_logged_on_lbl, "Logged on");
        lv_obj_set_style_text_color(s_logged_on_lbl, COL_LOGGED_ON, 0);
        lv_obj_align(s_logged_on_lbl, LV_ALIGN_TOP_MID, 0, (lv_coord_t)(avatar_y + AVATAR_SZ + 28));
        lv_obj_add_flag(s_logged_on_lbl, LV_OBJ_FLAG_HIDDEN);

        lv_coord_t row_y = (lv_coord_t)(avatar_y + AVATAR_SZ + 46);
        lv_obj_t *row = lv_obj_create(s_screen);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, (lv_coord_t)(w * 4 / 5));
        lv_obj_set_height(row, 36);
        lv_obj_align(row, LV_ALIGN_TOP_MID, 0, row_y);
        lv_obj_set_layout(row, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 8, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        s_ta = lv_textarea_create(row);
        lv_textarea_set_password_mode(s_ta, true);
        lv_textarea_set_one_line(s_ta, true);
        lv_textarea_set_placeholder_text(s_ta, "Password");
        lv_obj_set_flex_grow(s_ta, 1);
        lv_obj_set_height(s_ta, 36);
        lv_obj_add_event_cb(s_ta, ta_ready_cb, LV_EVENT_READY, NULL);
        lv_obj_add_event_cb(s_ta, ta_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

        lv_obj_t *go_btn = lv_btn_create(row);
        lv_obj_set_size(go_btn, 36, 36);
        purr_fx_radius(go_btn, LV_RADIUS_CIRCLE);
        lv_obj_set_style_bg_color(go_btn, COL_GO_BTN, 0);
        lv_obj_add_event_cb(go_btn, go_btn_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *arrow = lv_label_create(go_btn);
        lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
        lv_obj_center(arrow);

        s_status_lbl = lv_label_create(s_screen);
        lv_label_set_text(s_status_lbl, "");
        lv_obj_set_style_text_color(s_status_lbl, COL_ERROR, 0);
        lv_obj_set_style_text_font(s_status_lbl, &lv_font_montserrat_14, 0);
        lv_obj_align(s_status_lbl, LV_ALIGN_TOP_MID, 0, (lv_coord_t)(row_y + 38));

        // OSK toggle + power — bottom corners, login-screen-only (see this
        // file's header comment for why relock skips both).
        lv_coord_t btn_y = (lv_coord_t)(h - ICON_BTN_SZ - 6);
        s_osk_btn = lv_obj_create(s_screen);
        lv_obj_remove_style_all(s_osk_btn);
        lv_obj_set_size(s_osk_btn, ICON_BTN_SZ, ICON_BTN_SZ);
        lv_obj_set_pos(s_osk_btn, 8, btn_y);
        purr_fx_radius(s_osk_btn, LV_RADIUS_CIRCLE);
        lv_obj_set_style_bg_color(s_osk_btn, COL_ICON_BTN, 0);
        lv_obj_set_style_bg_opa(s_osk_btn, LV_OPA_COVER, 0);
        lv_obj_clear_flag(s_osk_btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(s_osk_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_osk_btn, osk_toggle_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *osk_glyph = lv_label_create(s_osk_btn);
        lv_label_set_text(osk_glyph, LV_SYMBOL_KEYBOARD);
        lv_obj_set_style_text_color(osk_glyph, lv_color_white(), 0);
        lv_obj_center(osk_glyph);
        lv_obj_clear_flag(osk_glyph, LV_OBJ_FLAG_CLICKABLE);

        s_power_btn = lv_obj_create(s_screen);
        lv_obj_remove_style_all(s_power_btn);
        lv_obj_set_size(s_power_btn, ICON_BTN_SZ, ICON_BTN_SZ);
        lv_obj_set_pos(s_power_btn, (lv_coord_t)(8 + ICON_BTN_SZ + 8), btn_y);
        purr_fx_radius(s_power_btn, LV_RADIUS_CIRCLE);
        lv_obj_set_style_bg_color(s_power_btn, COL_POWER_BTN, 0);
        lv_obj_set_style_bg_opa(s_power_btn, LV_OPA_COVER, 0);
        lv_obj_clear_flag(s_power_btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(s_power_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_power_btn, power_btn_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *power_glyph = lv_label_create(s_power_btn);
        lv_label_set_text(power_glyph, LV_SYMBOL_POWER);
        lv_obj_set_style_text_color(power_glyph, lv_color_white(), 0);
        lv_obj_center(power_glyph);
        lv_obj_clear_flag(power_glyph, LV_OBJ_FLAG_CLICKABLE);

        // Notification list — relock only, occupies the same lower band the
        // OSK/power row uses at login (the two are never shown together).
        lv_coord_t notif_y = btn_y;   // same lower band the OSK/power row occupies at login
        s_notif_list = lv_obj_create(s_screen);
        lv_obj_remove_style_all(s_notif_list);
        lv_obj_set_size(s_notif_list, (lv_coord_t)(w * 4 / 5), (lv_coord_t)(h - notif_y - 4));
        lv_obj_align(s_notif_list, LV_ALIGN_TOP_MID, 0, notif_y);
        lv_obj_set_layout(s_notif_list, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(s_notif_list, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_scroll_dir(s_notif_list, LV_DIR_VER);
        lv_obj_set_style_pad_row(s_notif_list, 3, 0);

        // On-screen keyboard, built once, bound to s_ta permanently — see
        // osk_toggle_cb()'s own comment for why this is manual, not
        // capability-detected.
        s_keyboard = lv_keyboard_create(lv_layer_top());
        lv_keyboard_set_textarea(s_keyboard, s_ta);
        lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);

        lv_group_t *g = host->group ? host->group() : NULL;
        if (g) {
            lv_group_add_obj(g, s_ta);
            lv_group_add_obj(g, go_btn);
            lv_group_add_obj(g, s_osk_btn);
            lv_group_add_obj(g, s_power_btn);
        }

        s_built = true;
    }

    // ── Reset for this show, whichever mode ─────────────────────────────
    char initial[2] = { (char)toupper((unsigned char)username[0]), '\0' };
    lv_label_set_text(s_avatar_initial_lbl, initial);
    lv_label_set_text(s_name_lbl, username);
    lv_label_set_text(s_title_lbl, relock ? "Locked" : "Welcome");
    lv_textarea_set_text(s_ta, "");
    lv_label_set_text(s_status_lbl, "");
    lv_obj_set_style_outline_opa(s_ta, LV_OPA_TRANSP, 0);
    if (s_keyboard) lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);

    if (relock) {
        lv_obj_clear_flag(s_logged_on_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_osk_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_power_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_notif_list, LV_OBJ_FLAG_HIDDEN);
        rebuild_notifications();
    } else {
        lv_obj_add_flag(s_logged_on_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_osk_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_power_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_notif_list, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_screen);

    lv_group_t *g = host->group ? host->group() : NULL;
    if (g) lv_group_focus_obj(s_ta);   // straight to the password field — no tile step any more

    ESP_LOGI(TAG, "%s screen shown for '%s'", relock ? "relock" : "login", username);
}

void purr_systemui_show_login(const purr_systemui_host_t *host)
{
    if (!host) return;
    if (user_mgr_is_logged_in()) return;   // boot_login_check already handled it, or logout hasn't happened
    enter_login_ui(host, false);
}

void purr_systemui_show_relock(const purr_systemui_host_t *host)
{
    if (!host) return;
    if (!user_mgr_is_logged_in()) return;   // nothing to relock — not a real session
    if (s_relock_active) return;            // already showing

    enter_login_ui(host, true);
    if (!s_screen) return;   // enter_login_ui() no-ops with no username — see its own log line

    s_relock_active = true;
    const catcall_display_t *disp = purr_kernel_display();
    if (disp && disp->set_brightness) disp->set_brightness(0);
}

bool purr_systemui_relock_active(void) { return s_relock_active; }

#endif // CONFIG_PURR_SYSTEMUI
