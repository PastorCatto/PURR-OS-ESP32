// systemui_login_ios.c — macOS-style login window for iOS-style PURR OS
// hosts (Mochi et al — CONFIG_PURR_SYSTEMUI_STYLE_IOS).
//
// Sibling of systemui_login.c (the original Windows-XP-style welcome
// screen), not a replacement for it: that file now explicitly excludes this
// style (`!defined(CONFIG_PURR_SYSTEMUI_STYLE_IOS)`), this file explicitly
// requires it, so exactly one of the two ever defines
// purr_systemui_show_login()/_show_relock()/_relock_active() per build —
// same "exactly one TU" invariant purr_systemui_init() already relies on
// across systemui_xp.c/systemui_ios.c/systemui_android.c.
//
// ── Why a new file instead of restyling the old one in place ────────────
// The two screens' visual language genuinely differs (dimmed wallpaper +
// circular avatar + real inline account row, vs. a flat gradient + single
// boxed avatar + tap-to-reveal picker overlay) enough that branching one
// function on style would mean every layout constant doubling as an
// if/else. Splitting by TU is the pattern this module already uses for the
// exact same reason at the chrome-style layer.
//
// ── What's identical to systemui_login.c, on purpose ─────────────────────
// Every piece of SESSION LOGIC is reused unchanged, not reinvented: the
// user_mgr_verify() -> user_mgr_set_logged_in() -> app_manager_notify_
// unlocked() success sequence, remote-account reconnect
// (begin_remote_reconnect()/reconnect_task()), continue-offline, and the
// full "Log in to a server" pairing flow (server_login_task(), the same
// pairing_verify_user() -> _request_user_access() -> _poll_user_access() ->
// _register_user_key() sequence, the same lv_async_call() cross-task-UI-
// update pattern). Only the LVGL construction/positioning code differs.
// See each function's own comment for what, if anything, actually changed.
//
// ── What's actually different from systemui_login.c ─────────────────────
// - Background: host->wallpaper() + a dark scrim, not a flat gradient — the
//   exact same pattern systemui_ios.c's own lock screen already uses
//   (build_lock(), this file's sibling), for the same reason: recognisably
//   the same device as the home screen, dimmed so text stays legible over
//   an arbitrary user-chosen image.
// - Multi-account: an always-visible inline row of avatar circles (real
//   macOS login-window behaviour) instead of a "Switch user" text link that
//   opens a separate tap-to-reveal overlay. Shown only when user_mgr_
//   count() > 1; skipped entirely for the common single-account case, same
//   as systemui_login.c already does. show_user_picker()'s old overlay is
//   gone — folded into this row plus a small always-visible "Log in to a
//   server" link, since with the row inline there's no separate "switch
//   user" step left to gate behind.
// - Avatars are colour-hashed per username (Apple's own 9-colour system
//   palette, same technique Mochi's springboard already uses for per-app
//   icon colour — see mochi_springboard.c's hash_str()/palette — reimple-
//   mented locally here rather than importing a Mochi-specific function
//   into a module several other, non-Mochi hosts also use).
// - Relock never shows the avatar row even when built (matches real macOS:
//   the lock screen shows only the current session's user, the login
//   window shows everyone) — moot today anyway, see below.
//
// ── Relock is unreachable for this style today ────────────────────────────
// systemui_ios.c's own idle-timeout lock (build_lock()/lock_check_idle() in
// that file) is a SEPARATE, already-iOS-styled, password-less tap-to-
// dismiss overlay that has never called purr_systemui_show_relock() at
// all. This file still implements show_relock()/relock_active() for real
// (not stubbed) — same shared-tree-plus-a-flag shape enter_login_ui()
// already uses — so it's correct and ready if that ever changes, but
// nothing exercises it today. Deliberately out of scope for this pass: the
// ask was a boot-only login window with the existing lock screen left
// alone, and systemui_ios.c's lock screen already *is* separate, for free.

#include "systemui.h"
#include "../user_mgr/user_mgr.h"
#include "../app_manager/app_manager.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

// Same PURR_SYSTEMUI_HAS_SERVER_LOGIN reasoning as systemui_login.c's own
// identical block — tab5/esp32p4 has no pairing transport (proximity's
// ESP-NOW can't exist on that radio-less target), so the whole "Log in to
// a server" feature compiles out there, not just disables at runtime.
#if defined(CONFIG_IDF_TARGET_ESP32P4)
#define PURR_SYSTEMUI_HAS_SERVER_LOGIN 0
#else
#define PURR_SYSTEMUI_HAS_SERVER_LOGIN 1
#include "../pairing/pairing.h"
#include "../app_manager_remote/app_manager_remote.h"
#endif

#if defined(CONFIG_PURR_SYSTEMUI) && defined(CONFIG_PURR_SYSTEMUI_STYLE_IOS)

static const char *TAG = "systemui_login_ios";

// ── Palette ──────────────────────────────────────────────────────────────
// Intentionally the SAME constants as systemui_ios.c's own palette (that
// file sits right next to this one and this screen needs to read light-on-
// dark same as its lock screen) — redefined locally rather than shared via
// a header, matching how each style file in this module already owns its
// own palette block.
#define COL_SCRIM        lv_color_black()
#define COL_CARD         lv_color_hex(0xFFFFFF)
#define COL_CARD_TEXT    lv_color_hex(0x1C1C1E)
#define COL_CARD_SUB     lv_color_hex(0x8E8E93)
#define COL_LOCK_TEXT    lv_color_hex(0xFFFFFF)
#define COL_LOCK_SUB     lv_color_hex(0xB0B0B8)
#define COL_ACCENT       lv_color_hex(0x007AFF)
#define COL_GREEN        lv_color_hex(0x34C759)
#define COL_RED          lv_color_hex(0xFF3B30)
#define COL_GREY         lv_color_hex(0x8E8E93)

#define AVATAR_SZ        52   // main avatar
#define ROW_AVATAR_SZ    34   // account-row avatars
#define ROW_H            48   // account row band, including padding
#define ICON_BTN_SZ      28
#define NOTIF_ROW_H      26

static const purr_systemui_host_t *s_host;
static bool s_built         = false;
static bool s_relock_mode   = false;
static bool s_relock_active = false;
static char s_current_username[USER_MGR_USERNAME_MAX];

static lv_obj_t *s_screen;
static lv_obj_t *s_avatar;              // circle, bg colour = hash(username)
static lv_obj_t *s_avatar_initial_lbl;
static lv_obj_t *s_name_lbl;
static lv_obj_t *s_logged_on_lbl;

static lv_obj_t *s_ta;
static lv_obj_t *s_status_lbl;
static lv_obj_t *s_keyboard;
static lv_obj_t *s_offline_lbl;

static lv_obj_t *s_osk_btn;
static lv_obj_t *s_power_btn;

static lv_obj_t *s_notif_list;

// Y position s_avatar/s_name_lbl/etc. get aligned to, once at build time —
// depends on whether the account row exists for THIS boot's account count
// (see build_screen()'s own comment on why deciding this once is safe).
static lv_coord_t s_content_top;

// ── Account row (multi-account only) ────────────────────────────────────
// Real macOS login-window behaviour: every known account visible directly,
// no separate "switch user" tap-to-reveal step. Built only when user_mgr_
// count() > 1 — see build_screen(). Persistent per-row username storage,
// same reasoning systemui_login.c's own s_picker_names[] comment gives:
// user_mgr_at() only ever copies into a caller-owned buffer, and a click
// callback's user_data needs something that outlives the loop that created
// it.
static lv_obj_t *s_row_avatars[USER_MGR_MAX_USERS];
static char      s_row_names[USER_MGR_MAX_USERS][USER_MGR_USERNAME_MAX];
static int       s_row_count = 0;

#if PURR_SYSTEMUI_HAS_SERVER_LOGIN
// ── Server login screen ──────────────────────────────────────────────────
// Same design as systemui_login.c's own — see that file's much longer
// header comment on this section for the full reasoning (home-base target,
// why the background task, why lv_async_call() everywhere). Restyled
// construction only; open_server_login_screen()'s BODY (the state machine)
// is unchanged from that file.
static bool       s_srv_built = false;
static lv_obj_t  *s_srv_screen;
static lv_obj_t  *s_srv_user_ta;
static lv_obj_t  *s_srv_pass_ta;
static lv_obj_t  *s_srv_server_lbl;
static lv_obj_t  *s_srv_status_lbl;
static lv_obj_t  *s_srv_go_btn;
static lv_obj_t  *s_srv_osk_btn;
static lv_obj_t  *s_srv_keyboard;

static uint8_t        s_srv_mac[6];
static bool            s_srv_have_server    = false;
static volatile bool   s_srv_login_in_flight = false;

// "Log in to a server" — always visible on the primary screen (see
// build_screen()), lives here rather than the unconditional static block
// above since it (like every other s_srv_* symbol) has no reason to exist
// on a target with no server-login support at all.
static lv_obj_t *s_srv_link_lbl;
#endif // PURR_SYSTEMUI_HAS_SERVER_LOGIN

static void begin_remote_reconnect(const char *username);
static void try_unlock(void);
static void rebuild_notifications(void);
static void enter_login_ui(const purr_systemui_host_t *host, bool relock);
static void continue_offline_cb(lv_event_t *e);
static void apply_current_identity(void);
static void refresh_row_selection(void);

// ── Avatar colour hash ───────────────────────────────────────────────────
// Same technique as mochi_springboard.c's hash_str()/mochi_color_for_app()
// (FNV-1a into a fixed saturated palette rather than a free hash-to-RGB,
// which tends to produce muddy colours) — reimplemented locally rather
// than calling into Mochi, since this module hosts other, non-Mochi
// launchers too.
static uint32_t hash_str(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return h;
}

static lv_color_t avatar_color_for(const char *username)
{
    static const uint32_t palette[] = {
        0x007AFF, 0x34C759, 0xFF9500, 0xFF3B30, 0xAF52DE,
        0x5AC8FA, 0xFFCC00, 0xFF2D55, 0x5856D6,
    };
    return lv_color_hex(palette[hash_str(username) % (sizeof(palette) / sizeof(palette[0]))]);
}

// Builds one avatar circle (used for both the single main avatar and each
// account-row avatar) — a coloured circle plus a centred white letter
// initial, no image asset (none exists — see this file's own header
// comment). Returns the circle itself so the caller can style/track it
// further (main avatar's initial label is tracked separately since it
// needs updating on identity switch; row avatars don't need their initial
// tracked past creation).
static lv_obj_t *build_avatar(lv_obj_t *parent, const char *username, lv_coord_t size, const lv_font_t *font)
{
    lv_obj_t *circle = lv_obj_create(parent);
    lv_obj_remove_style_all(circle);
    lv_obj_set_size(circle, size, size);
    purr_fx_radius(circle, LV_RADIUS_CIRCLE);
    lv_obj_set_style_bg_color(circle, avatar_color_for(username), 0);
    lv_obj_set_style_bg_opa(circle, LV_OPA_COVER, 0);
    lv_obj_clear_flag(circle, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *initial = lv_label_create(circle);
    char buf[2] = { (char)toupper((unsigned char)username[0]), '\0' };
    lv_label_set_text(initial, buf);
    lv_obj_set_style_text_color(initial, lv_color_white(), 0);
    lv_obj_set_style_text_font(initial, font, 0);
    lv_obj_center(initial);
    lv_obj_clear_flag(initial, LV_OBJ_FLAG_CLICKABLE);

    return circle;
}

static void ta_ready_cb(lv_event_t *e)  { (void)e; try_unlock(); }
static void go_btn_cb(lv_event_t *e)    { (void)e; try_unlock(); }

static void ta_changed_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_set_style_outline_opa(s_ta, LV_OPA_TRANSP, 0);
    lv_label_set_text(s_status_lbl, "");
}

// Same manual-toggle reasoning as systemui_login.c's own osk_toggle_cb() —
// the host contract has no "has a physical keyboard" hook.
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

// ── Session logic — identical to systemui_login.c's own, see that file ──

static void finish_login_success(void)
{
    if (s_relock_mode) {
        s_relock_active = false;
        const catcall_display_t *disp = purr_kernel_display();
        if (disp && disp->set_brightness) disp->set_brightness(255);
    }
    if (s_keyboard) lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_screen, LV_OBJ_FLAG_HIDDEN);   // hide, not delete
}

static void try_unlock(void)
{
    if (!s_ta || s_current_username[0] == '\0') return;

    if (user_mgr_account_type(s_current_username) == USER_ACCOUNT_REMOTE) {
        begin_remote_reconnect(s_current_username);
        return;
    }

    const char *pw = lv_textarea_get_text(s_ta);

    if (user_mgr_verify(s_current_username, pw)) {
        ESP_LOGI(TAG, "%s OK for '%s'", s_relock_mode ? "unlock" : "login", s_current_username);
        user_mgr_set_logged_in(s_current_username);
        // Local unlock — see app_manager.h's own doc comment on
        // app_manager_notify_unlocked(). Load-bearing: without this,
        // app_manager_count()/get() keep reporting the local registry as
        // empty forever (the exact "0 apps" bug this session already
        // root-caused once, for Mochi's own springboard). Idempotent, so
        // calling it again on every relock/unlock cycle is harmless.
        app_manager_notify_unlocked();
        finish_login_success();
        return;
    }

    ESP_LOGW(TAG, "%s failed for '%s'", s_relock_mode ? "unlock" : "login", s_current_username);
    lv_textarea_set_text(s_ta, "");
    lv_label_set_text(s_status_lbl, "The password is incorrect. Try again.");
    lv_obj_set_style_outline_color(s_ta, COL_RED, 0);
    lv_obj_set_style_outline_width(s_ta, 2, 0);
    lv_obj_set_style_outline_opa(s_ta, LV_OPA_COVER, 0);
}

static void continue_offline_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "continuing offline as '%s'", s_current_username);
    user_mgr_set_logged_in(s_current_username);
    app_manager_notify_unlocked();
    if (s_offline_lbl) lv_obj_add_flag(s_offline_lbl, LV_OBJ_FLAG_HIDDEN);
    finish_login_success();
}

// ── Notifications (relock only) — identical construction to
// systemui_login.c's own rebuild_notifications(), reusing this module's
// established COL_CARD/_TEXT/_SUB pair (white card, dark text) rather than
// that file's separate COL_NOTIF_* constants — same colours, same "frosted
// card over whatever's behind it" look systemui_ios.c's own notification
// cards already use elsewhere in this module.

static void rebuild_notifications(void)
{
    if (!s_notif_list) return;
    lv_obj_clean(s_notif_list);

    int n = purr_kernel_notify_count();
    if (n == 0) {
        lv_obj_t *empty = lv_label_create(s_notif_list);
        lv_label_set_text(empty, "No notifications");
        lv_obj_set_style_text_color(empty, COL_LOCK_SUB, 0);
        return;
    }

    for (int i = 0; i < n; i++) {
        purr_notification_t note;
        if (!purr_kernel_notify_at(i, &note)) continue;

        lv_obj_t *row = lv_obj_create(s_notif_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, NOTIF_ROW_H);
        lv_obj_set_style_bg_color(row, COL_CARD, 0);
        purr_systemui_fx_bg_opa(row, LV_OPA_90);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_pad_left(row, 6, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *title = lv_label_create(row);
        lv_label_set_text(title, note.title[0] ? note.title : note.source);
        lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
        lv_obj_set_width(title, LV_PCT(96));
        lv_obj_set_style_text_color(title, COL_CARD_TEXT, 0);
        lv_obj_set_pos(title, 0, 1);
        lv_obj_clear_flag(title, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *body = lv_label_create(row);
        lv_label_set_text(body, note.body);
        lv_label_set_long_mode(body, LV_LABEL_LONG_DOT);
        lv_obj_set_width(body, LV_PCT(96));
        lv_obj_set_style_text_color(body, COL_CARD_SUB, 0);
        lv_obj_set_pos(body, 0, 13);
        lv_obj_clear_flag(body, LV_OBJ_FLAG_CLICKABLE);
    }
}

#if PURR_SYSTEMUI_HAS_SERVER_LOGIN
// ── Server login — state machine identical to systemui_login.c's own,
// restyled construction only. See that file's much longer header comment
// on this whole section (right before its own enter_login_ui()) for the
// full design reasoning; not repeated here to avoid two copies drifting.

#define SERVER_LOGIN_APPROVAL_TIMEOUT_MS (5UL * 60UL * 1000UL)

typedef struct {
    uint8_t mac[6];
    char    username[USER_MGR_USERNAME_MAX];
    char    password[64];
} server_login_ctx_t;

static void open_server_login_screen(void);

static void async_set_status(void *user_data)
{
    char *msg = (char *)user_data;
    if (s_srv_status_lbl) lv_label_set_text(s_srv_status_lbl, msg);
    free(msg);
}

static void post_status(const char *msg)
{
    size_t n = strlen(msg) + 1;
    char *copy = malloc(n);
    if (!copy) return;
    memcpy(copy, msg, n);
    lv_async_call(async_set_status, copy);
}

static void async_login_success(void *user_data)
{
    uint8_t *mac = (uint8_t *)user_data;
    if (s_srv_keyboard) lv_obj_add_flag(s_srv_keyboard, LV_OBJ_FLAG_HIDDEN);
    if (s_srv_screen)   lv_obj_add_flag(s_srv_screen, LV_OBJ_FLAG_HIDDEN);
    if (s_keyboard) lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    if (s_screen)   lv_obj_add_flag(s_screen, LV_OBJ_FLAG_HIDDEN);
    app_manager_remote_connect(mac);
    free(mac);
}

static void post_login_success(const uint8_t mac[6])
{
    uint8_t *copy = malloc(6);
    if (!copy) return;
    memcpy(copy, mac, 6);
    lv_async_call(async_login_success, copy);
}

static void async_set_local_status(void *user_data)
{
    char *msg = (char *)user_data;
    if (s_status_lbl) lv_label_set_text(s_status_lbl, msg);
    free(msg);
}

static void post_local_status(const char *msg)
{
    size_t n = strlen(msg) + 1;
    char *copy = malloc(n);
    if (!copy) return;
    memcpy(copy, msg, n);
    lv_async_call(async_set_local_status, copy);
}

static void async_show_offline_option(void *user_data)
{
    (void)user_data;
    if (s_offline_lbl) lv_obj_clear_flag(s_offline_lbl, LV_OBJ_FLAG_HIDDEN);
}

typedef struct {
    uint8_t mac[6];
    char    username[USER_MGR_USERNAME_MAX];
} reconnect_ctx_t;

static void reconnect_task(void *arg)
{
    reconnect_ctx_t *ctx = (reconnect_ctx_t *)arg;
    if (pairing_verify_user(ctx->mac, ctx->username)) {
        post_login_success(ctx->mac);
    } else {
        bool offline_ok = user_mgr_get_offline_access(ctx->username);
        post_local_status(offline_ok
            ? "Couldn't reach the server \xE2\x80\x94 tap another account, or Continue offline"
            : "Couldn't reach the server \xE2\x80\x94 tap another account to log in locally");
        if (offline_ok) lv_async_call(async_show_offline_option, NULL);
    }
    free(ctx);
    vTaskDelete(NULL);
}

static void begin_remote_reconnect(const char *username)
{
    uint8_t mac[6];
    if (!user_mgr_get_remote_mac(username, mac)) {
        post_local_status("No server on record for this account");
        return;
    }
    reconnect_ctx_t *ctx = malloc(sizeof(*ctx));
    if (!ctx) return;
    memcpy(ctx->mac, mac, 6);
    snprintf(ctx->username, sizeof(ctx->username), "%s", username);

    if (s_status_lbl) lv_label_set_text(s_status_lbl, "Reconnecting...");
    TaskHandle_t task = NULL;
    BaseType_t ok = xTaskCreate(reconnect_task, "reconnect", 4096, ctx, 3, &task);
    if (ok != pdPASS) {
        free(ctx);
        if (s_status_lbl) lv_label_set_text(s_status_lbl, "Could not start reconnect");
    }
}

static void server_login_task(void *arg)
{
    server_login_ctx_t *ctx = (server_login_ctx_t *)arg;

    if (pairing_verify_user(ctx->mac, ctx->username)) {
        post_status("Connected");
        post_login_success(ctx->mac);
        goto done;
    }

    post_status("Checking password...");
    if (!pairing_request_user_access(ctx->mac, ctx->username, ctx->password)) {
        post_status("Wrong username or password");
        goto done;
    }

    post_status("Waiting for approval on the server...");
    pairing_user_access_status_t status = PAIRING_USERAUTH_PENDING;
    for (uint32_t waited_ms = 0; waited_ms < SERVER_LOGIN_APPROVAL_TIMEOUT_MS; waited_ms += 2000) {
        status = pairing_poll_user_access(ctx->mac, ctx->username);
        if (status != PAIRING_USERAUTH_PENDING) break;
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    if (status == PAIRING_USERAUTH_NONE) {
        post_status("Request denied or expired");
        goto done;
    }
    if (status != PAIRING_USERAUTH_APPROVED) {
        post_status("Timed out waiting for approval");
        goto done;
    }

    post_status("Approved \xE2\x80\x94 connecting...");
    if (!pairing_register_user_key(ctx->mac, ctx->username) || !pairing_verify_user(ctx->mac, ctx->username)) {
        post_status("Connection failed \xE2\x80\x94 try again");
        goto done;
    }
    post_status("Connected");
    post_login_success(ctx->mac);

done:
    memset(ctx->password, 0, sizeof(ctx->password));
    free(ctx);
    s_srv_login_in_flight = false;
    vTaskDelete(NULL);
}

static void submit_server_login(void)
{
    if (!s_srv_have_server) return;
    if (s_srv_login_in_flight) return;

    const char *u = lv_textarea_get_text(s_srv_user_ta);
    if (!u || !u[0]) {
        lv_label_set_text(s_srv_status_lbl, "Enter a username");
        return;
    }
    const char *p = lv_textarea_get_text(s_srv_pass_ta);

    server_login_ctx_t *ctx = malloc(sizeof(*ctx));
    if (!ctx) return;
    memcpy(ctx->mac, s_srv_mac, 6);
    snprintf(ctx->username, sizeof(ctx->username), "%s", u);
    snprintf(ctx->password, sizeof(ctx->password), "%s", p ? p : "");

    lv_label_set_text(s_srv_status_lbl, "Connecting...");
    s_srv_login_in_flight = true;
    TaskHandle_t task = NULL;
    // Plain xTaskCreate(), internal-RAM stack — NOT PSRAM. Same reasoning
    // systemui_login.c's own submit_server_login() comment documents in
    // full: server_login_task() writes to NVS (pairing_verify_user() ->
    // user_mgr_create_remote() -> save_all()) on success, and a task doing
    // that needs a stack ESP-IDF's own esp_task_stack_is_sane_cache_
    // disabled() assert will accept — confirmed as a real crash on real
    // hardware there, not a theoretical concern.
    BaseType_t ok = xTaskCreate(server_login_task, "srv_login", 4096, ctx, 3, &task);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "server login: task create failed");
        s_srv_login_in_flight = false;
        free(ctx);
        lv_label_set_text(s_srv_status_lbl, "Could not start \xE2\x80\x94 try again");
    }
}

static void srv_go_btn_cb(lv_event_t *e)   { (void)e; submit_server_login(); }
static void srv_ta_ready_cb(lv_event_t *e) { (void)e; submit_server_login(); }
static void srv_user_focus_cb(lv_event_t *e) { (void)e; if (s_srv_keyboard) lv_keyboard_set_textarea(s_srv_keyboard, s_srv_user_ta); }
static void srv_pass_focus_cb(lv_event_t *e) { (void)e; if (s_srv_keyboard) lv_keyboard_set_textarea(s_srv_keyboard, s_srv_pass_ta); }

static void srv_osk_toggle_cb(lv_event_t *e)
{
    (void)e;
    if (!s_srv_keyboard) return;
    if (lv_obj_has_flag(s_srv_keyboard, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_clear_flag(s_srv_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_srv_keyboard);
    } else {
        lv_obj_add_flag(s_srv_keyboard, LV_OBJ_FLAG_HIDDEN);
    }
}

static void refresh_server_label(void)
{
    s_srv_have_server = pairing_get_home_base(s_srv_mac);
    if (!s_srv_have_server) {
        lv_label_set_text(s_srv_server_lbl, "No server configured");
        if (s_srv_go_btn) lv_obj_add_state(s_srv_go_btn, LV_STATE_DISABLED);
        return;
    }

    char name_buf[sizeof(((paired_device_t *)0)->name)] = "?";
    int n = pairing_device_count();
    for (int i = 0; i < n; i++) {
        paired_device_t pd;
        if (pairing_device_at(i, &pd) && memcmp(pd.mac, s_srv_mac, 6) == 0) {
            snprintf(name_buf, sizeof(name_buf), "%s", pd.name);
            break;
        }
    }
    char buf[80];
    snprintf(buf, sizeof(buf), "Server: %s", name_buf);
    lv_label_set_text(s_srv_server_lbl, buf);
    if (s_srv_go_btn) lv_obj_clear_state(s_srv_go_btn, LV_STATE_DISABLED);
}

static void back_from_server_login_cb(lv_event_t *e)
{
    (void)e;
    if (s_srv_keyboard) lv_obj_add_flag(s_srv_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_srv_screen, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_screen);
    lv_group_t *g = s_host->group ? s_host->group() : NULL;
    if (g) lv_group_focus_obj(s_ta);
}

static void srv_login_btn_cb(lv_event_t *e) { (void)e; open_server_login_screen(); }

// Same dimmed-wallpaper background as the primary screen — see build_
// screen()'s own comment on the pattern (this file always builds the
// primary screen first, so host->wallpaper() has already been resolved
// once; re-resolving here keeps this screen self-contained rather than
// caching a pointer across two functions for no real benefit).
static void open_server_login_screen(void)
{
    uint16_t w = s_host->width();
    uint16_t h = s_host->height();

    if (!s_srv_built) {
        s_srv_screen = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(s_srv_screen);
        lv_obj_set_size(s_srv_screen, w, h);
        lv_obj_set_pos(s_srv_screen, 0, 0);
        lv_obj_set_style_bg_color(s_srv_screen, COL_SCRIM, 0);
        lv_obj_set_style_bg_opa(s_srv_screen, LV_OPA_COVER, 0);
        lv_obj_clear_flag(s_srv_screen, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(s_srv_screen, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(s_srv_screen, LV_OBJ_FLAG_HIDDEN);

        const lv_img_dsc_t *wp = (s_host && s_host->wallpaper) ? s_host->wallpaper() : NULL;
        if (wp) {
            lv_obj_t *bg = lv_img_create(s_srv_screen);
            lv_img_set_src(bg, wp);
            lv_obj_set_pos(bg, 0, 0);
            lv_obj_clear_flag(bg, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_move_background(bg);

            lv_obj_t *dim = lv_obj_create(s_srv_screen);
            lv_obj_remove_style_all(dim);
            lv_obj_set_size(dim, w, h);
            lv_obj_set_pos(dim, 0, 0);
            lv_obj_set_style_bg_color(dim, COL_SCRIM, 0);
            purr_systemui_fx_bg_opa(dim, LV_OPA_60);
            lv_obj_clear_flag(dim, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_move_background(dim);
            lv_obj_move_foreground(bg);
            lv_obj_move_foreground(dim);
        }

        lv_obj_t *title = lv_label_create(s_srv_screen);
        lv_label_set_text(title, "Log in to a server");
        lv_obj_set_style_text_color(title, COL_LOCK_TEXT, 0);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_32, 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, (lv_coord_t)(PURR_SYSTEMUI_STATUS_H + 8));

        lv_coord_t urow_y = (lv_coord_t)(PURR_SYSTEMUI_STATUS_H + 60);
        s_srv_user_ta = lv_textarea_create(s_srv_screen);
        lv_textarea_set_one_line(s_srv_user_ta, true);
        lv_textarea_set_placeholder_text(s_srv_user_ta, "Username");
        lv_obj_set_size(s_srv_user_ta, (lv_coord_t)(w * 4 / 5), 36);
        lv_obj_align(s_srv_user_ta, LV_ALIGN_TOP_MID, 0, urow_y);
        purr_fx_radius(s_srv_user_ta, 10);
        lv_obj_set_style_bg_color(s_srv_user_ta, lv_color_white(), 0);
        purr_systemui_fx_bg_opa(s_srv_user_ta, LV_OPA_20);
        lv_obj_set_style_border_width(s_srv_user_ta, 0, 0);
        lv_obj_set_style_text_color(s_srv_user_ta, COL_LOCK_TEXT, 0);
        lv_obj_add_event_cb(s_srv_user_ta, srv_user_focus_cb, LV_EVENT_FOCUSED, NULL);

        lv_coord_t prow_y = (lv_coord_t)(urow_y + 44);
        lv_obj_t *prow = lv_obj_create(s_srv_screen);
        lv_obj_remove_style_all(prow);
        lv_obj_set_width(prow, (lv_coord_t)(w * 4 / 5));
        lv_obj_set_height(prow, 36);
        lv_obj_align(prow, LV_ALIGN_TOP_MID, 0, prow_y);
        lv_obj_set_layout(prow, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(prow, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(prow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(prow, 8, 0);
        lv_obj_clear_flag(prow, LV_OBJ_FLAG_SCROLLABLE);

        s_srv_pass_ta = lv_textarea_create(prow);
        lv_textarea_set_password_mode(s_srv_pass_ta, true);
        lv_textarea_set_one_line(s_srv_pass_ta, true);
        lv_textarea_set_placeholder_text(s_srv_pass_ta, "Password");
        lv_obj_set_flex_grow(s_srv_pass_ta, 1);
        lv_obj_set_height(s_srv_pass_ta, 36);
        purr_fx_radius(s_srv_pass_ta, 10);
        lv_obj_set_style_bg_color(s_srv_pass_ta, lv_color_white(), 0);
        purr_systemui_fx_bg_opa(s_srv_pass_ta, LV_OPA_20);
        lv_obj_set_style_border_width(s_srv_pass_ta, 0, 0);
        lv_obj_set_style_text_color(s_srv_pass_ta, COL_LOCK_TEXT, 0);
        lv_obj_add_event_cb(s_srv_pass_ta, srv_ta_ready_cb, LV_EVENT_READY, NULL);
        lv_obj_add_event_cb(s_srv_pass_ta, srv_pass_focus_cb, LV_EVENT_FOCUSED, NULL);

        s_srv_go_btn = lv_btn_create(prow);
        lv_obj_set_size(s_srv_go_btn, 36, 36);
        purr_fx_radius(s_srv_go_btn, LV_RADIUS_CIRCLE);
        lv_obj_set_style_bg_color(s_srv_go_btn, COL_ACCENT, 0);
        lv_obj_add_event_cb(s_srv_go_btn, srv_go_btn_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *arrow = lv_label_create(s_srv_go_btn);
        lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
        lv_obj_center(arrow);

        s_srv_server_lbl = lv_label_create(s_srv_screen);
        lv_obj_set_style_text_color(s_srv_server_lbl, COL_LOCK_SUB, 0);
        lv_obj_set_style_text_font(s_srv_server_lbl, &lv_font_montserrat_14, 0);
        lv_obj_align(s_srv_server_lbl, LV_ALIGN_TOP_MID, 0, (lv_coord_t)(prow_y + 44));

        s_srv_status_lbl = lv_label_create(s_srv_screen);
        lv_label_set_text(s_srv_status_lbl, "");
        lv_obj_set_style_text_color(s_srv_status_lbl, COL_RED, 0);
        lv_obj_set_style_text_font(s_srv_status_lbl, &lv_font_montserrat_14, 0);
        lv_obj_align(s_srv_status_lbl, LV_ALIGN_TOP_MID, 0, (lv_coord_t)(prow_y + 66));

        lv_coord_t btn_y = (lv_coord_t)(h - ICON_BTN_SZ - 6);

        lv_obj_t *back_btn = lv_obj_create(s_srv_screen);
        lv_obj_remove_style_all(back_btn);
        lv_obj_set_size(back_btn, 72, ICON_BTN_SZ);
        lv_obj_set_pos(back_btn, 8, btn_y);
        lv_obj_set_style_radius(back_btn, 8, 0);
        lv_obj_set_style_bg_color(back_btn, lv_color_white(), 0);
        purr_systemui_fx_bg_opa(back_btn, LV_OPA_20);
        lv_obj_clear_flag(back_btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(back_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(back_btn, back_from_server_login_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *back_lbl = lv_label_create(back_btn);
        lv_label_set_text(back_lbl, "< Back");
        lv_obj_set_style_text_color(back_lbl, COL_LOCK_TEXT, 0);
        lv_obj_center(back_lbl);
        lv_obj_clear_flag(back_lbl, LV_OBJ_FLAG_CLICKABLE);

        s_srv_osk_btn = lv_obj_create(s_srv_screen);
        lv_obj_remove_style_all(s_srv_osk_btn);
        lv_obj_set_size(s_srv_osk_btn, ICON_BTN_SZ, ICON_BTN_SZ);
        lv_obj_set_pos(s_srv_osk_btn, (lv_coord_t)(8 + 72 + 8), btn_y);
        purr_fx_radius(s_srv_osk_btn, LV_RADIUS_CIRCLE);
        lv_obj_set_style_bg_color(s_srv_osk_btn, lv_color_white(), 0);
        purr_systemui_fx_bg_opa(s_srv_osk_btn, LV_OPA_20);
        lv_obj_clear_flag(s_srv_osk_btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(s_srv_osk_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_srv_osk_btn, srv_osk_toggle_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *osk_glyph = lv_label_create(s_srv_osk_btn);
        lv_label_set_text(osk_glyph, LV_SYMBOL_KEYBOARD);
        lv_obj_set_style_text_color(osk_glyph, COL_LOCK_TEXT, 0);
        lv_obj_center(osk_glyph);
        lv_obj_clear_flag(osk_glyph, LV_OBJ_FLAG_CLICKABLE);

        s_srv_keyboard = lv_keyboard_create(lv_layer_top());
        lv_keyboard_set_textarea(s_srv_keyboard, s_srv_user_ta);
        lv_obj_add_flag(s_srv_keyboard, LV_OBJ_FLAG_HIDDEN);

        lv_group_t *g = s_host->group ? s_host->group() : NULL;
        if (g) {
            lv_group_add_obj(g, s_srv_user_ta);
            lv_group_add_obj(g, s_srv_pass_ta);
            lv_group_add_obj(g, s_srv_go_btn);
            lv_group_add_obj(g, back_btn);
            lv_group_add_obj(g, s_srv_osk_btn);
        }

        s_srv_built = true;
    }

    lv_textarea_set_text(s_srv_user_ta, "");
    lv_textarea_set_text(s_srv_pass_ta, "");
    lv_label_set_text(s_srv_status_lbl, "");
    refresh_server_label();
    if (s_srv_keyboard) lv_obj_add_flag(s_srv_keyboard, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_flag(s_screen, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_srv_screen, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_srv_screen);

    lv_group_t *g = s_host->group ? s_host->group() : NULL;
    if (g) lv_group_focus_obj(s_srv_user_ta);

    ESP_LOGI(TAG, "server login screen shown");
}
#else // !PURR_SYSTEMUI_HAS_SERVER_LOGIN
static void begin_remote_reconnect(const char *username)
{
    ESP_LOGE(TAG, "begin_remote_reconnect('%s') called on a target with no "
                   "server-login support — should be unreachable", username);
}
#endif // PURR_SYSTEMUI_HAS_SERVER_LOGIN

// ── Account row (multi-account only) ─────────────────────────────────────

static void row_avatar_cb(lv_event_t *e)
{
    const char *name = (const char *)lv_event_get_user_data(e);
    strncpy(s_current_username, name, sizeof(s_current_username) - 1);
    s_current_username[sizeof(s_current_username) - 1] = '\0';
    apply_current_identity();
}

// Outline ring on whichever avatar is s_current_username right now — same
// technique mochi_springboard.c's apply_selection_style() already uses for
// its own icon grid (outline width/colour/opa, not a border, so the
// avatar's own size never shifts).
static void refresh_row_selection(void)
{
    for (int i = 0; i < s_row_count; i++) {
        bool selected = strcmp(s_row_names[i], s_current_username) == 0;
        lv_obj_set_style_outline_width(s_row_avatars[i], selected ? 3 : 0, 0);
        lv_obj_set_style_outline_color(s_row_avatars[i], COL_ACCENT, 0);
        lv_obj_set_style_outline_pad(s_row_avatars[i], selected ? 2 : 0, 0);
        lv_obj_set_style_outline_opa(s_row_avatars[i], selected ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    }
}

// Builds the row once, at s_content_top's own decision point (build_
// screen()) — only ever called when user_mgr_count() > 1, matching that
// decision. Rebuilding per-show isn't needed the way systemui_login.c's
// old show_user_picker() rebuilt every open: a new account can't appear
// while this screen is up (creating one requires being logged in already,
// see this file's header comment on why user_mgr_count() can't change
// mid-display), so a one-time build at first show is correct, not stale.
static void build_account_row(uint16_t w)
{
    lv_obj_t *row = lv_obj_create(s_screen);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, w, ROW_AVATAR_SZ + 4);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, (lv_coord_t)(PURR_SYSTEMUI_STATUS_H + 6));
    lv_obj_set_layout(row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_set_scroll_dir(row, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);

    s_row_count = user_mgr_count();
    if (s_row_count > USER_MGR_MAX_USERS) s_row_count = USER_MGR_MAX_USERS;

    lv_group_t *g = s_host->group ? s_host->group() : NULL;

    for (int i = 0; i < s_row_count; i++) {
        if (!user_mgr_at(i, s_row_names[i], sizeof(s_row_names[i]))) continue;
        lv_obj_t *circle = build_avatar(row, s_row_names[i], ROW_AVATAR_SZ, &lv_font_montserrat_14);
        lv_obj_add_flag(circle, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(circle, row_avatar_cb, LV_EVENT_CLICKED, s_row_names[i]);
        if (g) lv_group_add_obj(g, circle);
        s_row_avatars[i] = circle;
    }
}

// ── Build / re-show ───────────────────────────────────────────────────────

static void build_screen(const purr_systemui_host_t *host, bool has_row)
{
    uint16_t w = host->width();
    uint16_t h = host->height();

    s_screen = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_screen);
    lv_obj_set_size(s_screen, w, h);
    lv_obj_set_pos(s_screen, 0, 0);
    lv_obj_set_style_bg_color(s_screen, COL_SCRIM, 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_screen, LV_OBJ_FLAG_CLICKABLE);   // eats touches — nothing behind it should receive them

    // Dimmed wallpaper — same pattern as systemui_ios.c's own build_lock(),
    // see this file's header comment for why. NULL wallpaper (e.g. Flow/
    // Cheetah, which sets .wallpaper = NULL) falls back to the plain scrim
    // colour already set above.
    const lv_img_dsc_t *wp = (host->wallpaper) ? host->wallpaper() : NULL;
    if (wp) {
        lv_obj_t *bg = lv_img_create(s_screen);
        lv_img_set_src(bg, wp);
        lv_obj_set_pos(bg, 0, 0);
        lv_obj_clear_flag(bg, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *dim = lv_obj_create(s_screen);
        lv_obj_remove_style_all(dim);
        lv_obj_set_size(dim, w, h);
        lv_obj_set_pos(dim, 0, 0);
        lv_obj_set_style_bg_color(dim, COL_SCRIM, 0);
        purr_systemui_fx_bg_opa(dim, LV_OPA_60);
        lv_obj_clear_flag(dim, LV_OBJ_FLAG_CLICKABLE);
    }

    if (has_row) build_account_row(w);

    // s_content_top decided once here — see its own declaration comment.
    s_content_top = (lv_coord_t)(PURR_SYSTEMUI_STATUS_H + (has_row ? ROW_H : 10));

    lv_obj_t *frame = lv_obj_create(s_screen);
    lv_obj_remove_style_all(frame);
    lv_obj_set_size(frame, AVATAR_SZ, AVATAR_SZ);
    lv_obj_align(frame, LV_ALIGN_TOP_MID, 0, s_content_top);
    s_avatar = frame;   // recoloured per-identity in apply_current_identity()
    purr_fx_radius(s_avatar, LV_RADIUS_CIRCLE);
    lv_obj_set_style_bg_opa(s_avatar, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_avatar, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    s_avatar_initial_lbl = lv_label_create(s_avatar);
    lv_obj_set_style_text_color(s_avatar_initial_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_avatar_initial_lbl, &lv_font_montserrat_32, 0);
    lv_obj_center(s_avatar_initial_lbl);
    lv_obj_clear_flag(s_avatar_initial_lbl, LV_OBJ_FLAG_CLICKABLE);

    s_name_lbl = lv_label_create(s_screen);
    lv_obj_set_style_text_color(s_name_lbl, COL_LOCK_TEXT, 0);
    lv_obj_set_style_text_font(s_name_lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(s_name_lbl, LV_ALIGN_TOP_MID, 0, (lv_coord_t)(s_content_top + AVATAR_SZ + 6));

    s_logged_on_lbl = lv_label_create(s_screen);
    lv_label_set_text(s_logged_on_lbl, "Logged on");
    lv_obj_set_style_text_color(s_logged_on_lbl, COL_GREEN, 0);
    lv_obj_set_style_text_font(s_logged_on_lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(s_logged_on_lbl, LV_ALIGN_TOP_MID, 0, (lv_coord_t)(s_content_top + AVATAR_SZ + 24));
    lv_obj_add_flag(s_logged_on_lbl, LV_OBJ_FLAG_HIDDEN);

    lv_coord_t row_y = (lv_coord_t)(s_content_top + AVATAR_SZ + 40);
    lv_obj_t *prow = lv_obj_create(s_screen);
    lv_obj_remove_style_all(prow);
    lv_obj_set_width(prow, (lv_coord_t)(w * 4 / 5));
    lv_obj_set_height(prow, 36);
    lv_obj_align(prow, LV_ALIGN_TOP_MID, 0, row_y);
    lv_obj_set_layout(prow, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(prow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(prow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(prow, 8, 0);
    lv_obj_clear_flag(prow, LV_OBJ_FLAG_SCROLLABLE);

    // Translucent-white "frosted" field, not stock LVGL textarea chrome —
    // the macOS-login-window look this whole file is for.
    s_ta = lv_textarea_create(prow);
    lv_textarea_set_password_mode(s_ta, true);
    lv_textarea_set_one_line(s_ta, true);
    lv_textarea_set_placeholder_text(s_ta, "Password");
    lv_obj_set_flex_grow(s_ta, 1);
    lv_obj_set_height(s_ta, 36);
    purr_fx_radius(s_ta, 10);
    lv_obj_set_style_bg_color(s_ta, lv_color_white(), 0);
    purr_systemui_fx_bg_opa(s_ta, LV_OPA_20);
    lv_obj_set_style_border_width(s_ta, 0, 0);
    lv_obj_set_style_text_color(s_ta, COL_LOCK_TEXT, 0);
    lv_obj_add_event_cb(s_ta, ta_ready_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_ta, ta_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *go_btn = lv_btn_create(prow);
    lv_obj_set_size(go_btn, 36, 36);
    purr_fx_radius(go_btn, LV_RADIUS_CIRCLE);
    lv_obj_set_style_bg_color(go_btn, COL_ACCENT, 0);
    lv_obj_add_event_cb(go_btn, go_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *arrow = lv_label_create(go_btn);
    lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
    lv_obj_center(arrow);

    s_status_lbl = lv_label_create(s_screen);
    lv_label_set_text(s_status_lbl, "");
    lv_obj_set_style_text_color(s_status_lbl, COL_RED, 0);
    lv_obj_set_style_text_font(s_status_lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(s_status_lbl, LV_ALIGN_TOP_MID, 0, (lv_coord_t)(row_y + 40));

    s_offline_lbl = lv_label_create(s_screen);
    lv_label_set_text(s_offline_lbl, "Continue offline");
    lv_obj_set_style_text_color(s_offline_lbl, COL_LOCK_SUB, 0);
    lv_obj_set_style_text_font(s_offline_lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(s_offline_lbl, LV_ALIGN_TOP_MID, 0, (lv_coord_t)(row_y + 60));
    lv_obj_add_flag(s_offline_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_offline_lbl, continue_offline_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_offline_lbl, LV_OBJ_FLAG_HIDDEN);

    lv_coord_t btn_y = (lv_coord_t)(h - ICON_BTN_SZ - 6);
    s_osk_btn = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_osk_btn);
    lv_obj_set_size(s_osk_btn, ICON_BTN_SZ, ICON_BTN_SZ);
    lv_obj_set_pos(s_osk_btn, 8, btn_y);
    purr_fx_radius(s_osk_btn, LV_RADIUS_CIRCLE);
    lv_obj_set_style_bg_color(s_osk_btn, lv_color_white(), 0);
    purr_systemui_fx_bg_opa(s_osk_btn, LV_OPA_20);
    lv_obj_clear_flag(s_osk_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_osk_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_osk_btn, osk_toggle_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *osk_glyph = lv_label_create(s_osk_btn);
    lv_label_set_text(osk_glyph, LV_SYMBOL_KEYBOARD);
    lv_obj_set_style_text_color(osk_glyph, COL_LOCK_TEXT, 0);
    lv_obj_center(osk_glyph);
    lv_obj_clear_flag(osk_glyph, LV_OBJ_FLAG_CLICKABLE);

    s_power_btn = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_power_btn);
    lv_obj_set_size(s_power_btn, ICON_BTN_SZ, ICON_BTN_SZ);
    lv_obj_set_pos(s_power_btn, (lv_coord_t)(8 + ICON_BTN_SZ + 8), btn_y);
    purr_fx_radius(s_power_btn, LV_RADIUS_CIRCLE);
    lv_obj_set_style_bg_color(s_power_btn, COL_RED, 0);
    purr_systemui_fx_bg_opa(s_power_btn, LV_OPA_80);
    lv_obj_clear_flag(s_power_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_power_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_power_btn, power_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *power_glyph = lv_label_create(s_power_btn);
    lv_label_set_text(power_glyph, LV_SYMBOL_POWER);
    lv_obj_set_style_text_color(power_glyph, lv_color_white(), 0);
    lv_obj_center(power_glyph);
    lv_obj_clear_flag(power_glyph, LV_OBJ_FLAG_CLICKABLE);

#if PURR_SYSTEMUI_HAS_SERVER_LOGIN
    // Always-visible link — no picker step to gate it behind any more now
    // that the account row (when present) is inline. Bottom-right, mirrors
    // OSK/power's bottom-left placement.
    s_srv_link_lbl = lv_label_create(s_screen);
    lv_label_set_text(s_srv_link_lbl, "Log in to a server");
    lv_obj_set_style_text_color(s_srv_link_lbl, COL_LOCK_SUB, 0);
    lv_obj_set_style_text_font(s_srv_link_lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(s_srv_link_lbl, LV_ALIGN_BOTTOM_RIGHT, -10, -10);
    lv_obj_add_flag(s_srv_link_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_srv_link_lbl, srv_login_btn_cb, LV_EVENT_CLICKED, NULL);
#endif

    // Notification list — relock only, same lower band the OSK/power row
    // occupies at login (the two are never shown together).
    s_notif_list = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_notif_list);
    lv_obj_set_size(s_notif_list, (lv_coord_t)(w * 4 / 5), (lv_coord_t)(h - btn_y - 4));
    lv_obj_align(s_notif_list, LV_ALIGN_TOP_MID, 0, btn_y);
    lv_obj_set_layout(s_notif_list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_notif_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_notif_list, LV_DIR_VER);
    lv_obj_set_style_pad_row(s_notif_list, 3, 0);

    s_keyboard = lv_keyboard_create(lv_layer_top());
    lv_keyboard_set_textarea(s_keyboard, s_ta);
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);

    lv_group_t *g = host->group ? host->group() : NULL;
    if (g) {
        lv_group_add_obj(g, s_ta);
        lv_group_add_obj(g, go_btn);
        lv_group_add_obj(g, s_osk_btn);
        lv_group_add_obj(g, s_power_btn);
        lv_group_add_obj(g, s_offline_lbl);
#if PURR_SYSTEMUI_HAS_SERVER_LOGIN
        lv_group_add_obj(g, s_srv_link_lbl);
#endif
    }
}

// Re-applies the current identity's avatar/name/field-reset without
// rebuilding anything — used both by enter_login_ui()'s own "reset for
// this show" step and by row_avatar_cb() when the row switches identity
// mid-display.
static void apply_current_identity(void)
{
    lv_obj_set_style_bg_color(s_avatar, avatar_color_for(s_current_username), 0);
    char initial[2] = { (char)toupper((unsigned char)s_current_username[0]), '\0' };
    lv_label_set_text(s_avatar_initial_lbl, initial);
    lv_label_set_text(s_name_lbl, s_current_username);
    lv_textarea_set_text(s_ta, "");
    lv_label_set_text(s_status_lbl, "");
    lv_obj_set_style_outline_opa(s_ta, LV_OPA_TRANSP, 0);
    if (s_offline_lbl) lv_obj_add_flag(s_offline_lbl, LV_OBJ_FLAG_HIDDEN);
    refresh_row_selection();

    if (user_mgr_account_type(s_current_username) == USER_ACCOUNT_REMOTE) {
        begin_remote_reconnect(s_current_username);
    }
}

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
        // Row only for a fresh login with more than one known account —
        // never for relock (real macOS shows only the current session's
        // user on the lock screen, not a switcher) and never when this
        // boot only has one account (matches systemui_login.c's own
        // single-account behaviour). Decided once — see s_content_top's
        // own declaration comment on why that's safe.
        bool has_row = (!relock) && (user_mgr_count() > 1);
        build_screen(host, has_row);
        s_built = true;
    }

    // ── Reset for this show ──────────────────────────────────────────────
    apply_current_identity();
    if (s_keyboard) lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);

    if (relock) {
        lv_obj_clear_flag(s_logged_on_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_osk_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_power_btn, LV_OBJ_FLAG_HIDDEN);
#if PURR_SYSTEMUI_HAS_SERVER_LOGIN
        if (s_srv_link_lbl) lv_obj_add_flag(s_srv_link_lbl, LV_OBJ_FLAG_HIDDEN);
#endif
        lv_obj_clear_flag(s_notif_list, LV_OBJ_FLAG_HIDDEN);
        rebuild_notifications();
    } else {
        lv_obj_add_flag(s_logged_on_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_osk_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_power_btn, LV_OBJ_FLAG_HIDDEN);
#if PURR_SYSTEMUI_HAS_SERVER_LOGIN
        if (s_srv_link_lbl) lv_obj_clear_flag(s_srv_link_lbl, LV_OBJ_FLAG_HIDDEN);
#endif
        lv_obj_add_flag(s_notif_list, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_screen);

    lv_group_t *g = host->group ? host->group() : NULL;
    if (g) lv_group_focus_obj(s_ta);

    ESP_LOGI(TAG, "%s screen shown for '%s'", relock ? "relock" : "login", username);

    // Same deliberate NON-auto-fire of begin_remote_reconnect() here as
    // systemui_login.c's own enter_login_ui() — see that file's comment on
    // why an earlier version doing so was a real security regression
    // (walk up to an already-key-registered REMOTE session, in with zero
    // input). apply_current_identity() above DOES still fire it when the
    // row switches to a different REMOTE account mid-display — that's a
    // deliberate user action (a tap), not an automatic show, same
    // distinction that file's comment draws.
}

void purr_systemui_show_login(const purr_systemui_host_t *host)
{
    if (!host) return;
    if (user_mgr_is_logged_in()) return;
    enter_login_ui(host, false);
}

void purr_systemui_show_relock(const purr_systemui_host_t *host)
{
    if (!host) return;
    if (!user_mgr_is_logged_in()) return;
    if (s_relock_active) return;

    enter_login_ui(host, true);
    if (!s_screen) return;

    s_relock_active = true;
    const catcall_display_t *disp = purr_kernel_display();
    if (disp && disp->set_brightness) disp->set_brightness(0);
}

bool purr_systemui_relock_active(void) { return s_relock_active; }

#endif // defined(CONFIG_PURR_SYSTEMUI) && defined(CONFIG_PURR_SYSTEMUI_STYLE_IOS)
