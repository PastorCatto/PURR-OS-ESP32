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
#include "../app_manager/app_manager.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

// The "Log in to a server" screen needs pairing.h — but pairing.c compiles
// to nothing at all on esp32p4 (tab5): it rides on proximity's ESP-NOW
// transport, which cannot exist on that radio-less target (see pairing's
// own CMakeLists.txt guard, the same `idf_component_register(); return()`
// early-out proximity/proximity_rpc use). systemui itself IS built for
// tab5 (device.pcat lists it), so unconditionally calling a pairing_*
// symbol here would link-fail there specifically. Compiling the whole
// feature out on that one target — not just disabling it at runtime — is
// what keeps this file buildable everywhere systemui already is.
#if defined(CONFIG_IDF_TARGET_ESP32P4)
#define PURR_SYSTEMUI_HAS_SERVER_LOGIN 0
#else
#define PURR_SYSTEMUI_HAS_SERVER_LOGIN 1
#include "../pairing/pairing.h"
#include "../app_manager_remote/app_manager_remote.h"
#endif

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
static lv_obj_t *s_switch_user_lbl;   // opens the user picker — see its own section
static lv_obj_t *s_offline_lbl;       // "Continue offline" — see its own creation-site comment

static lv_obj_t *s_osk_btn;
static lv_obj_t *s_power_btn;
static lv_obj_t *s_codename_lbl;

static lv_obj_t *s_notif_list;

// ── User picker ──────────────────────────────────────────────────────────
// Reachable via "Switch user" on the login/relock screen, and shown
// automatically when an automatic remote reconnect (below) fails — see
// this file's own top-level bug writeup on why a single implicit
// "current" identity with no way to override it is a real dead end for a
// REMOTE account whose server isn't reachable. Lists every known account
// (user_mgr_count()/at(), user_mgr.h) regardless of type; picking a LOCAL
// one just re-shows the normal password field, picking a REMOTE one fires
// the same automatic reconnect enter_login_ui() itself fires.
static lv_obj_t *s_picker_screen;
static lv_obj_t *s_picker_list;
static bool       s_picker_built = false;
// Persistent (not stack/heap) storage for each row's username, handed to
// its own click callback as user_data — user_mgr_at() only ever copies
// into a caller-owned buffer, so these own that buffer across the picker's
// whole lifetime rather than each row capturing a dangling loop-local.
static char s_picker_names[USER_MGR_MAX_USERS][USER_MGR_USERNAME_MAX];

static void apply_current_identity(void);
static void show_user_picker(void);

#if PURR_SYSTEMUI_HAS_SERVER_LOGIN
// ── Server login screen ─────────────────────────────────────────────────
// See this file's own "Server login" section below (right before
// enter_login_ui()) for the full design. No standalone button on the main
// login/relock screen any more — merged into the user picker instead (see
// show_user_picker()'s own "Log in to a server" row): one entry point,
// not two buttons doing adjacent things. That also lifts the old "login-
// mode only" restriction (relock skipped this before, on the reasoning
// "a server session doesn't fit the relock concept") — the picker is
// already reachable from both, so this naturally is too now.

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
static bool            s_srv_have_server  = false;
static volatile bool   s_srv_login_in_flight = false;
#endif // PURR_SYSTEMUI_HAS_SERVER_LOGIN

// Fires an automatic Phase C reconnect (pairing.h) for `username`, a
// USER_ACCOUNT_REMOTE identity — no password, nothing typed. Declared
// unconditionally (real body under PURR_SYSTEMUI_HAS_SERVER_LOGIN, a
// same-signature stub otherwise, see below) so every call site
// (try_unlock(), enter_login_ui(), apply_current_identity()) stays
// unguarded: on a target with no pairing subsystem (esp32p4/tab5) no
// REMOTE account can ever exist in the first place, so the stub is
// structurally never reached, not just silently wrong.
static void begin_remote_reconnect(const char *username);

static void try_unlock(void);
static void rebuild_notifications(void);
static void enter_login_ui(const purr_systemui_host_t *host, bool relock);

static void continue_offline_cb(lv_event_t *e);

static void go_btn_cb(lv_event_t *e)   { (void)e; try_unlock(); }
static void ta_ready_cb(lv_event_t *e) { (void)e; try_unlock(); }
static void switch_user_cb(lv_event_t *e) { (void)e; show_user_picker(); }

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

// Shared tail for "this identity is now considered logged in, on this
// device, right now" — try_unlock()'s own LOCAL-account success path and
// continue_offline_cb() below both end exactly this way.
static void finish_login_success(void)
{
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
}

static void try_unlock(void)
{
    if (!s_ta || s_current_username[0] == '\0') return;

    // user_mgr_verify() is a deliberate dead end for USER_ACCOUNT_REMOTE
    // (see its own doc comment in user_mgr.c) — a remote identity was
    // never authenticated with a locally-checked password, so calling it
    // here would always report "wrong password" no matter what's typed.
    // Route through begin_remote_reconnect() instead — this Go/Enter tap
    // IS what fires it (enter_login_ui() deliberately does NOT auto-fire
    // this on its own any more, see its own comment on why that was a
    // real security regression).
    if (user_mgr_account_type(s_current_username) == USER_ACCOUNT_REMOTE) {
        begin_remote_reconnect(s_current_username);
        return;
    }

    const char *pw = lv_textarea_get_text(s_ta);

    if (user_mgr_verify(s_current_username, pw)) {
        ESP_LOGI(TAG, "%s OK for '%s'", s_relock_mode ? "unlock" : "login", s_current_username);
        user_mgr_set_logged_in(s_current_username);
        // Local unlock — see app_manager.h's own doc comment on
        // app_manager_notify_unlocked(). Idempotent, so calling it again on
        // every relock/unlock cycle (not just the first login) is harmless.
        app_manager_notify_unlocked();
        finish_login_success();
        return;
    }

    ESP_LOGW(TAG, "%s failed for '%s'", s_relock_mode ? "unlock" : "login", s_current_username);
    lv_textarea_set_text(s_ta, "");
    lv_label_set_text(s_status_lbl, "The password is incorrect. Try again.");
    lv_obj_set_style_outline_color(s_ta, COL_ERROR, 0);
    lv_obj_set_style_outline_width(s_ta, 2, 0);
    lv_obj_set_style_outline_opa(s_ta, LV_OPA_COVER, 0);
}

// Continue offline — see this label's own creation-site comment on when
// it's actually shown. No pairing_verify_user() this time: opting into
// offline_access (user_mgr_set_offline_access(), user_mgr.h) IS the "I
// trust this device to act as me without the server reachable" consent,
// already given in advance. Marks the session logged in and unlocks the
// LOCAL registry exactly like an ordinary local unlock would — this is
// what actually satisfies "only already-downloaded LOCAL/HYBRID apps are
// launchable" with no special-case filtering needed at all: claw_
// loader's personal-space storage is keyed by username regardless of
// that account's type, so whatever was already downloaded for this
// identity (app_manager_remote_download(), cheetah_home.c) just shows up
// through the same scan every local unlock already triggers.
static void continue_offline_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "continuing offline as '%s'", s_current_username);
    user_mgr_set_logged_in(s_current_username);
    app_manager_notify_unlocked();
    if (s_offline_lbl) lv_obj_add_flag(s_offline_lbl, LV_OBJ_FLAG_HIDDEN);
    finish_login_success();
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

#if PURR_SYSTEMUI_HAS_SERVER_LOGIN
// ── Server login ─────────────────────────────────────────────────────────
// A near-identical second screen (own username field — a remote identity
// isn't one of this device's local accounts, so no avatar/fixed-name — plus
// a masked password field, a "Server: <name>" line, Go, and Back), built on
// the SAME lv_layer_top() the local login screen uses, but kept as a
// genuinely separate screen (its own lv_obj tree, its own hide/show) rather
// than folded into enter_login_ui() itself — explicit instruction: the
// local login screen stays untouched, this is a second screen next to it.
//
// Target server is always pairing_get_home_base() — reuses homebase.c's
// existing "the default server this device connects to" concept rather
// than adding a new picker (see pairing.h). No home base set means no
// server to log into at all — the label says so and Go stays disabled.
//
// The actual check runs on its own background task (server_login_task()),
// mirroring the exact sequence milkbar's own (now-removed) run_login_flow()
// established: pairing_verify_user() first (the fast path, a key already
// registered here from a prior login) — falling back to
// pairing_request_user_access() -> poll pairing_poll_user_access() until
// approved/denied/timed out -> pairing_register_user_key() ->
// pairing_verify_user() again to actually complete the login with the
// freshly-registered key. Every one of those is a blocking
// proximity_rpc_call() under the hood (pairing_module.c), so — same rule
// every proximity_rpc_call() site in this codebase follows — this never
// runs on cupcake_task or the LVGL render task.
//
// This screen IS raw LVGL, unlike milkbar's old background task (which
// called purr_win_label_set() directly, safe because that portable-API
// backend already defers internally — see cheetah_win.c's own comment on
// this). Raw lv_obj_*/lv_label_set_text() calls get no such protection, so
// every status update and the final hand-off go through lv_async_call() —
// the pattern cheetah_win.c already documents and uses for exactly this
// class of hazard ("destroying a window's object tree synchronously...
// hangs the render task"; mutating a label from the wrong task is the same
// class of problem, not just destroying objects).
//
// On success: hide this screen (same "hide, not destroy" precedent
// enter_login_ui() already uses) and call app_manager_remote_connect(mac)
// directly (async_login_success(), via post_login_success()) — a first-
// class app_manager-level hand-off, not a detour through launching Milkbar
// as an ordinary local app first. app_manager_set_remote() (which that
// wraps) closes the local registry for real before remote opens — see its
// own comment in app_manager.c. Milkbar's own Desktop-button flow (opened
// normally once locally unlocked) is a separate, unchanged path.

#define SERVER_LOGIN_APPROVAL_TIMEOUT_MS (5UL * 60UL * 1000UL)   // matches pairing_module.c's own USERAUTH_REQ_TIMEOUT_MS, not shared directly (that's that file's own internal constant)

typedef struct {
    uint8_t mac[6];
    char    username[USER_MGR_USERNAME_MAX];
    char    password[64];
} server_login_ctx_t;

static void open_server_login_screen(void);

// Runs on the LVGL/render task via lv_async_call() — never call directly
// from server_login_task(). Frees the heap copy post_status() made; the
// label itself may not exist yet if the screen was somehow torn down
// first, hence the s_srv_status_lbl guard (it never actually is torn down —
// same "hide, not destroy" lifetime every screen in this file has — but
// costs nothing to check).
static void async_set_status(void *user_data)
{
    char *msg = (char *)user_data;
    if (s_srv_status_lbl) lv_label_set_text(s_srv_status_lbl, msg);
    free(msg);
}

// Heap-copies `msg` (rather than writing into a shared static buffer) so a
// burst of status updates queued close together on lv_async_call() can
// never have a later free()/overwrite race a still-pending earlier one —
// each carries its own copy, consumed and freed exactly once.
static void post_status(const char *msg)
{
    size_t n = strlen(msg) + 1;
    char *copy = malloc(n);
    if (!copy) return;
    memcpy(copy, msg, n);
    lv_async_call(async_set_status, copy);
}

// Frees the heap copy post_login_success() made — same "each queued call
// carries its own copy, consumed and freed exactly once" reasoning as
// async_set_status()/post_status() just above.
static void async_login_success(void *user_data)
{
    uint8_t *mac = (uint8_t *)user_data;
    if (s_srv_keyboard) lv_obj_add_flag(s_srv_keyboard, LV_OBJ_FLAG_HIDDEN);
    if (s_srv_screen)   lv_obj_add_flag(s_srv_screen, LV_OBJ_FLAG_HIDDEN);
    // Also hide the LOCAL login/relock screen — begin_remote_reconnect()
    // (an automatic Phase C reconnect fired from THAT screen, not this
    // one) lands here too on success, same as a fresh server login does.
    // A harmless no-op on whichever of the two screens wasn't the one
    // actually showing.
    if (s_keyboard) lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    if (s_screen)   lv_obj_add_flag(s_screen, LV_OBJ_FLAG_HIDDEN);
    // Direct hand-off into remote mode, not app_manager_launch_by_name(
    // "milkbar") any more — see systemui's own CMakeLists.txt comment on
    // app_manager_remote_connect() for why: the local registry really
    // closes (app_manager_set_remote()'s own local-app sweep) before
    // remote opens, with no intermediate local-app launch step. Milkbar's
    // own Desktop-button flow (opened normally once locally unlocked)
    // still works exactly as before and is untouched by this.
    app_manager_remote_connect(mac);
    free(mac);
}

// Heap-copies `mac` for the same reason post_status() heap-copies its
// message — lv_async_call() just queues the callback, it doesn't block,
// so by the time async_login_success() actually runs on the LVGL task,
// server_login_task()'s own `ctx` may already be freed (see its `done:`
// label). A fresh 6-byte copy, freed once inside async_login_success(),
// avoids that use-after-free.
static void post_login_success(const uint8_t mac[6])
{
    uint8_t *copy = malloc(6);
    if (!copy) return;
    memcpy(copy, mac, 6);
    lv_async_call(async_login_success, copy);
}

// ── Automatic remote reconnect (relock/boot for a REMOTE identity) ──────
// Mirrors async_set_status()/post_status() above exactly, just targeting
// the LOCAL login/relock screen's own s_status_lbl instead of the server
// screen's s_srv_status_lbl — begin_remote_reconnect() below always runs
// from the local screen's context (enter_login_ui(), try_unlock(),
// apply_current_identity()), never the server one.
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

// Reveals s_offline_lbl — reconnect_task()'s own failure branch is the
// only caller, on its background task, so this goes through lv_async_
// call() same as every other LVGL touch that background task makes
// (post_local_status()'s own pair, just above). No data to carry (the
// label itself has nothing to say, just something to show), so
// user_data is unused — lv_async_call() still requires a non-conflicting
// callback signature, not a reason to invent a payload.
static void async_show_offline_option(void *user_data)
{
    (void)user_data;
    if (s_offline_lbl) lv_obj_clear_flag(s_offline_lbl, LV_OBJ_FLAG_HIDDEN);
}

typedef struct {
    uint8_t mac[6];
    char    username[USER_MGR_USERNAME_MAX];
} reconnect_ctx_t;

// Owns `ctx` from here on, same as server_login_task() owns its own ctx.
// Plain xTaskCreate()/vTaskDelete() below, not the WithCaps/PSRAM pair —
// pairing_verify_user() on success calls user_mgr_create_remote() ->
// save_all(), an NVS write, same reasoning submit_server_login()'s own
// task-creation comment documents (and the same real crash this file
// already fixed once, on real hardware, for that exact call chain).
static void reconnect_task(void *arg)
{
    reconnect_ctx_t *ctx = (reconnect_ctx_t *)arg;
    if (pairing_verify_user(ctx->mac, ctx->username)) {
        post_login_success(ctx->mac);
    } else {
        // Server unreachable (out of range, powered off, etc.) is the
        // expected failure mode here, not an error — this is exactly the
        // "soft lock" case: falls back to a real path forward (Switch
        // User, and — only if this specific account opted in,
        // user_mgr_get_offline_access() — Continue offline too) instead
        // of a password field that could never have succeeded for this
        // account type anyway.
        bool offline_ok = user_mgr_get_offline_access(ctx->username);
        post_local_status(offline_ok
            ? "Couldn't reach the server \xE2\x80\x94 tap Switch User, or Continue offline"   // em dash
            : "Couldn't reach the server \xE2\x80\x94 tap Switch User to log in locally");     // em dash
        if (offline_ok) lv_async_call(async_show_offline_option, NULL);
    }
    free(ctx);
    vTaskDelete(NULL);
}

static void begin_remote_reconnect(const char *username)
{
    uint8_t mac[6];
    if (!user_mgr_get_remote_mac(username, mac)) {
        post_local_status("No server on record for this account \xE2\x80\x94 tap Switch User");   // em dash
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
        if (s_status_lbl) lv_label_set_text(s_status_lbl, "Could not start reconnect \xE2\x80\x94 tap Switch User");   // em dash
    }
}

// Never call directly — only ever the body of a task spawned by
// submit_server_login(), which owns `ctx` from here on (frees it, on every
// exit path).
static void server_login_task(void *arg)
{
    server_login_ctx_t *ctx = (server_login_ctx_t *)arg;

    // Fast path — this device already registered a key for (mac,
    // username) from a prior login, most logins take this single call and
    // skip everything below entirely.
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

    post_status("Approved \xE2\x80\x94 connecting...");   // em dash
    if (!pairing_register_user_key(ctx->mac, ctx->username) || !pairing_verify_user(ctx->mac, ctx->username)) {
        post_status("Connection failed \xE2\x80\x94 try again");
        goto done;
    }
    post_status("Connected");
    post_login_success(ctx->mac);

done:
    memset(ctx->password, 0, sizeof(ctx->password));   // done with it — no reason to keep a plaintext password in RAM longer than necessary
    free(ctx);
    s_srv_login_in_flight = false;
    // Plain vTaskDelete() — must match how this task's own stack was
    // created (plain xTaskCreate() above, internal RAM, not the WithCaps/
    // PSRAM allocator) — see app_manager.c's app_manager_stop() comment on
    // why mismatching the two is a real bug, not a style nit.
    vTaskDelete(NULL);
}

// Runs on the LVGL/render task (a button click / textarea READY event) —
// safe to touch s_srv_* widgets directly here, only server_login_task()
// itself needs lv_async_call().
static void submit_server_login(void)
{
    if (!s_srv_have_server) return;
    if (s_srv_login_in_flight) return;   // already running — ignore a double-tap

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
    // Plain xTaskCreate() — internal-RAM stack, NOT xTaskCreateWithCaps(...,
    // MALLOC_CAP_SPIRAM). server_login_task() calls pairing_verify_user() ->
    // user_mgr_create_remote() -> save_all(), which writes to NVS — a task
    // touching NVS/flash must run on an internal-RAM stack (ESP-IDF asserts
    // esp_task_stack_is_sane_cache_disabled() when the flash cache gets
    // disabled for the write). Confirmed live on real hardware: a PSRAM
    // stack here crashed with exactly that assert the moment a server login
    // actually succeeded and tried to persist the new remote user, decoded
    // via xtensa-esp32s3-elf-addr2line against the real backtrace. Same
    // established pattern this codebase already uses elsewhere for any
    // NVS-touching task — see app_manager.c's own "Static stack pool for
    // apps that touch NVS/flash directly" comment and meshtastic_module.c's
    // mesh_persist_task().
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

// The shared keyboard follows whichever of the two fields currently has
// focus — unlike the local login screen (one field, one permanent binding),
// this screen has two editable fields to switch between.
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

// Reads pairing_get_home_base() fresh every open — the paired/home-base
// list can change between visits to this screen (Nearby's Set Home, or a
// forget) far more plausibly than within a single visit, so this only
// needs to run on open, not on every frame.
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
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_HIDDEN);   // local login screen was hidden underneath, not destroyed
    lv_obj_move_foreground(s_screen);
    lv_group_t *g = s_host->group ? s_host->group() : NULL;
    if (g) lv_group_focus_obj(s_ta);
}

// Reused directly as the user picker's own "Log in to a server" row click
// handler (show_user_picker(), further down) — the merge point for what
// used to be two separate buttons on the main login/relock screen.
static void srv_login_btn_cb(lv_event_t *e)
{
    (void)e;
    if (s_picker_screen) lv_obj_add_flag(s_picker_screen, LV_OBJ_FLAG_HIDDEN);
    open_server_login_screen();
}

static void open_server_login_screen(void)
{
    uint16_t w = s_host->width();
    uint16_t h = s_host->height();

    if (!s_srv_built) {
        s_srv_screen = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(s_srv_screen);
        lv_obj_set_size(s_srv_screen, w, h);
        lv_obj_set_pos(s_srv_screen, 0, 0);
        lv_obj_set_style_bg_color(s_srv_screen, COL_BG_TOP, 0);
        lv_obj_set_style_bg_grad_color(s_srv_screen, COL_BG_BOTTOM, 0);
        lv_obj_set_style_bg_grad_dir(s_srv_screen, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_opa(s_srv_screen, LV_OPA_COVER, 0);
        lv_obj_clear_flag(s_srv_screen, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(s_srv_screen, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(s_srv_screen, LV_OBJ_FLAG_HIDDEN);   // stays hidden until open_server_login_screen() shows it

        lv_obj_t *title = lv_label_create(s_srv_screen);
        lv_label_set_text(title, "Log in to a server");
        lv_obj_set_style_text_color(title, COL_LABEL, 0);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_32, 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, (lv_coord_t)(PURR_SYSTEMUI_STATUS_H + 2));

        lv_coord_t urow_y = (lv_coord_t)(PURR_SYSTEMUI_STATUS_H + 60);
        s_srv_user_ta = lv_textarea_create(s_srv_screen);
        lv_textarea_set_one_line(s_srv_user_ta, true);
        lv_textarea_set_placeholder_text(s_srv_user_ta, "Username");
        lv_obj_set_size(s_srv_user_ta, (lv_coord_t)(w * 4 / 5), 36);
        lv_obj_align(s_srv_user_ta, LV_ALIGN_TOP_MID, 0, urow_y);
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
        lv_obj_add_event_cb(s_srv_pass_ta, srv_ta_ready_cb, LV_EVENT_READY, NULL);
        lv_obj_add_event_cb(s_srv_pass_ta, srv_pass_focus_cb, LV_EVENT_FOCUSED, NULL);

        s_srv_go_btn = lv_btn_create(prow);
        lv_obj_set_size(s_srv_go_btn, 36, 36);
        purr_fx_radius(s_srv_go_btn, LV_RADIUS_CIRCLE);
        lv_obj_set_style_bg_color(s_srv_go_btn, COL_GO_BTN, 0);
        lv_obj_add_event_cb(s_srv_go_btn, srv_go_btn_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *arrow = lv_label_create(s_srv_go_btn);
        lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
        lv_obj_center(arrow);

        s_srv_server_lbl = lv_label_create(s_srv_screen);
        lv_obj_set_style_text_color(s_srv_server_lbl, COL_LABEL_DIM, 0);
        lv_obj_set_style_text_font(s_srv_server_lbl, &lv_font_montserrat_14, 0);
        lv_obj_align(s_srv_server_lbl, LV_ALIGN_TOP_MID, 0, (lv_coord_t)(prow_y + 44));

        s_srv_status_lbl = lv_label_create(s_srv_screen);
        lv_label_set_text(s_srv_status_lbl, "");
        lv_obj_set_style_text_color(s_srv_status_lbl, COL_ERROR, 0);
        lv_obj_set_style_text_font(s_srv_status_lbl, &lv_font_montserrat_14, 0);
        lv_obj_align(s_srv_status_lbl, LV_ALIGN_TOP_MID, 0, (lv_coord_t)(prow_y + 66));

        lv_coord_t btn_y = (lv_coord_t)(h - ICON_BTN_SZ - 6);

        lv_obj_t *back_btn = lv_obj_create(s_srv_screen);
        lv_obj_remove_style_all(back_btn);
        lv_obj_set_size(back_btn, 72, ICON_BTN_SZ);
        lv_obj_set_pos(back_btn, 8, btn_y);
        lv_obj_set_style_radius(back_btn, 4, 0);
        lv_obj_set_style_bg_color(back_btn, COL_ICON_BTN, 0);
        lv_obj_set_style_bg_opa(back_btn, LV_OPA_COVER, 0);
        lv_obj_clear_flag(back_btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(back_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(back_btn, back_from_server_login_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *back_lbl = lv_label_create(back_btn);
        lv_label_set_text(back_lbl, "< Back");
        lv_obj_set_style_text_color(back_lbl, lv_color_white(), 0);
        lv_obj_center(back_lbl);
        lv_obj_clear_flag(back_lbl, LV_OBJ_FLAG_CLICKABLE);

        s_srv_osk_btn = lv_obj_create(s_srv_screen);
        lv_obj_remove_style_all(s_srv_osk_btn);
        lv_obj_set_size(s_srv_osk_btn, ICON_BTN_SZ, ICON_BTN_SZ);
        lv_obj_set_pos(s_srv_osk_btn, (lv_coord_t)(8 + 72 + 8), btn_y);
        purr_fx_radius(s_srv_osk_btn, LV_RADIUS_CIRCLE);
        lv_obj_set_style_bg_color(s_srv_osk_btn, COL_ICON_BTN, 0);
        lv_obj_set_style_bg_opa(s_srv_osk_btn, LV_OPA_COVER, 0);
        lv_obj_clear_flag(s_srv_osk_btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(s_srv_osk_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_srv_osk_btn, srv_osk_toggle_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *osk_glyph = lv_label_create(s_srv_osk_btn);
        lv_label_set_text(osk_glyph, LV_SYMBOL_KEYBOARD);
        lv_obj_set_style_text_color(osk_glyph, lv_color_white(), 0);
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
// Stub for begin_remote_reconnect()'s forward declaration above — see its
// own comment. Structurally never reached: no pairing subsystem exists on
// this target (esp32p4/tab5), so user_mgr_account_type() can never report
// USER_ACCOUNT_REMOTE for a real account here. Logs loudly rather than
// silently no-op-ing, in case that invariant is ever wrong.
static void begin_remote_reconnect(const char *username)
{
    ESP_LOGE(TAG, "begin_remote_reconnect('%s') called on a target with no "
                   "server-login support — should be unreachable", username);
}
#endif // PURR_SYSTEMUI_HAS_SERVER_LOGIN

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

        // "Switch user" — unlike OSK/power/server-login below, visible in
        // BOTH login and relock mode (never toggled by the relock branch
        // further down): the whole point is a way out when the identity
        // currently shown is stuck (a REMOTE account whose server isn't
        // reachable — see begin_remote_reconnect()), which can happen at
        // either boot or relock.
        s_switch_user_lbl = lv_label_create(s_screen);
        lv_label_set_text(s_switch_user_lbl, "Switch user");
        lv_obj_set_style_text_color(s_switch_user_lbl, COL_LABEL_DIM, 0);
        lv_obj_set_style_text_font(s_switch_user_lbl, &lv_font_montserrat_14, 0);
        lv_obj_align(s_switch_user_lbl, LV_ALIGN_TOP_MID, 0, (lv_coord_t)(row_y + 58));
        lv_obj_add_flag(s_switch_user_lbl, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_switch_user_lbl, switch_user_cb, LV_EVENT_CLICKED, NULL);

        // "Continue offline" — hidden by default, revealed only after a
        // reconnect attempt actually fails for an account with offline_
        // access enabled (post_offline_option(), reconnect_task()'s own
        // failure branch) — never shown speculatively/always-on, since
        // most accounts don't have it enabled and it would be a confusing
        // no-op for a LOCAL account or an offline_access-disabled REMOTE
        // one either way.
        s_offline_lbl = lv_label_create(s_screen);
        lv_label_set_text(s_offline_lbl, "Continue offline");
        lv_obj_set_style_text_color(s_offline_lbl, COL_LABEL_DIM, 0);
        lv_obj_set_style_text_font(s_offline_lbl, &lv_font_montserrat_14, 0);
        lv_obj_align(s_offline_lbl, LV_ALIGN_TOP_MID, 0, (lv_coord_t)(row_y + 76));
        lv_obj_add_flag(s_offline_lbl, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_offline_lbl, continue_offline_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_add_flag(s_offline_lbl, LV_OBJ_FLAG_HIDDEN);

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

        // "Log in to a server" no longer has its own button here — merged
        // into the user picker (Switch user), see show_user_picker()'s own
        // "Log in to a server" row.

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
            lv_group_add_obj(g, s_switch_user_lbl);
            lv_group_add_obj(g, s_offline_lbl);
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
    // Never lingers for a different identity than the one that actually
    // triggered it (begin_remote_reconnect()'s own failure branch reveals
    // it fresh, per-attempt, if relevant to THIS identity).
    if (s_offline_lbl) lv_obj_add_flag(s_offline_lbl, LV_OBJ_FLAG_HIDDEN);

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

    // Deliberately NOT auto-firing begin_remote_reconnect() here for a
    // REMOTE identity, even though nothing typed into s_ta above can
    // actually verify one (see try_unlock()'s own comment) — an earlier
    // version of this fix did fire it automatically, the instant this
    // screen showed, and that was a real security regression: Phase C's
    // registered key already lives on this device, so anyone with
    // physical possession got back into the remote session with zero
    // input at all, defeating the entire point of a lock screen. A tap
    // on Go/Enter (try_unlock()'s own REMOTE branch) is still required
    // to actually fire it — this screen just sits showing the identity
    // and waits, same as it does for a LOCAL account.
}

// ── User picker (definitions) ────────────────────────────────────────────
// Built lazily on lv_layer_top(), same "hide, not destroy" lifetime as
// the login screen itself — an overlay ON TOP of whichever screen is
// currently showing (s_screen; s_srv_screen has its own way back via its
// own Back button already), not a replacement for it.

// Re-applies enter_login_ui()'s own "reset for this show" step without
// rebuilding the whole screen — used when the picker switches identity
// while the login/relock screen is already up. s_screen/s_ta/etc. are
// guaranteed built by the time this can run (the picker only ever opens
// from show_user_picker(), itself gated on s_screen already existing).
static void apply_current_identity(void)
{
    char initial[2] = { (char)toupper((unsigned char)s_current_username[0]), '\0' };
    lv_label_set_text(s_avatar_initial_lbl, initial);
    lv_label_set_text(s_name_lbl, s_current_username);
    lv_textarea_set_text(s_ta, "");
    lv_label_set_text(s_status_lbl, "");
    lv_obj_set_style_outline_opa(s_ta, LV_OPA_TRANSP, 0);
    if (s_offline_lbl) lv_obj_add_flag(s_offline_lbl, LV_OBJ_FLAG_HIDDEN);

    if (user_mgr_account_type(s_current_username) == USER_ACCOUNT_REMOTE) {
        begin_remote_reconnect(s_current_username);
    }
}

// user_data is one of s_picker_names[]'s own row buffers (see that
// array's own comment) — stable for the picker's whole lifetime, never a
// dangling per-row loop local.
static void picker_row_cb(lv_event_t *e)
{
    const char *name = (const char *)lv_event_get_user_data(e);
    strncpy(s_current_username, name, sizeof(s_current_username) - 1);
    s_current_username[sizeof(s_current_username) - 1] = '\0';
    if (s_picker_screen) lv_obj_add_flag(s_picker_screen, LV_OBJ_FLAG_HIDDEN);
    apply_current_identity();
}

static void show_user_picker(void)
{
    if (!s_screen) return;   // login/relock screen isn't up — nothing to overlay onto

    if (!s_picker_built) {
        uint16_t w = s_host ? s_host->width() : 240;
        uint16_t h = s_host ? s_host->height() : 320;

        s_picker_screen = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(s_picker_screen);
        lv_obj_set_size(s_picker_screen, w, h);
        lv_obj_set_pos(s_picker_screen, 0, 0);
        lv_obj_set_style_bg_color(s_picker_screen, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(s_picker_screen, LV_OPA_70, 0);
        lv_obj_clear_flag(s_picker_screen, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(s_picker_screen, LV_OBJ_FLAG_CLICKABLE);   // eats touches — tapping outside a row does nothing, no dismiss gesture needed for a first pass

        lv_obj_t *title = lv_label_create(s_picker_screen);
        lv_label_set_text(title, "Switch user");
        lv_obj_set_style_text_color(title, COL_LABEL, 0);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

        s_picker_list = lv_obj_create(s_picker_screen);
        lv_obj_remove_style_all(s_picker_list);
        lv_obj_set_size(s_picker_list, (lv_coord_t)(w * 4 / 5), (lv_coord_t)(h - 80));
        lv_obj_align(s_picker_list, LV_ALIGN_TOP_MID, 0, 36);
        lv_obj_set_layout(s_picker_list, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(s_picker_list, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(s_picker_list, 6, 0);
        lv_obj_set_scroll_dir(s_picker_list, LV_DIR_VER);

        s_picker_built = true;
    }

    // Rebuilt fresh every show — cheap (USER_MGR_MAX_USERS is 8), and
    // avoids ever needing to diff against a stale row set from the last
    // time this was open (an account created/removed meanwhile, etc.).
    lv_obj_clean(s_picker_list);
    int n = user_mgr_count();
    if (n > USER_MGR_MAX_USERS) n = USER_MGR_MAX_USERS;   // s_picker_names[]'s own bound
    for (int i = 0; i < n; i++) {
        if (!user_mgr_at(i, s_picker_names[i], sizeof(s_picker_names[i]))) continue;

        lv_obj_t *row = lv_btn_create(s_picker_list);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, 36);
        lv_obj_add_event_cb(row, picker_row_cb, LV_EVENT_CLICKED, s_picker_names[i]);

        bool is_remote = user_mgr_account_type(s_picker_names[i]) == USER_ACCOUNT_REMOTE;
        char label[USER_MGR_USERNAME_MAX + 16];
        snprintf(label, sizeof(label), "%s%s", s_picker_names[i], is_remote ? " (network)" : " (local)");
        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, label);
        lv_obj_center(lbl);
    }

#if PURR_SYSTEMUI_HAS_SERVER_LOGIN
    // The merge point for what used to be a separate "Log in to a server"
    // button on the main login/relock screen — one entry point (Switch
    // user) now covers both "become a known account" and "authenticate
    // against a new/different server". srv_login_btn_cb() itself hides
    // this picker before opening the server-login screen.
    lv_obj_t *srv_row = lv_btn_create(s_picker_list);
    lv_obj_set_width(srv_row, LV_PCT(100));
    lv_obj_set_height(srv_row, 36);
    lv_obj_add_event_cb(srv_row, srv_login_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *srv_lbl = lv_label_create(srv_row);
    lv_label_set_text(srv_lbl, "Log in to a server");
    lv_obj_center(srv_lbl);
#endif // PURR_SYSTEMUI_HAS_SERVER_LOGIN

    lv_obj_clear_flag(s_picker_screen, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_picker_screen);
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
