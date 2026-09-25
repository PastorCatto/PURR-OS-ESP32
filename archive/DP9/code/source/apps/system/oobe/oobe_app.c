// oobe_app.c — PURR OS out-of-box setup (.claw)
//
// First-run only: app_manager_init()'s autorun_oobe() (app_manager.c) launches
// this automatically, exactly once, gated purely on user_mgr_oobe_completed()
// — see that function's own doc comment in user_mgr.h for why that is a
// separate, explicit marker rather than inferred from account state. This app
// owns setting that marker; nothing else does, the same way every other app
// owns its own exit path.
//
// Lets the person keep the zero-config bootstrap account
// (USER_MGR_BOOTSTRAP_USER, no password) or replace it with a real
// Unix-style username + optional password. Either choice marks OOBE
// complete — "keep the default" is a completed setup, not a skipped one.

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "purr_win.h"
#include "purr_module.h"
#include "purr_kernel.h"
#include "app_manager.h"
#include "user_mgr.h"

static const char *TAG = "oobe";

static purr_win_t s_win        = 0;
static purr_wid_t s_status_lbl = 0;
static purr_wid_t s_user_input = 0;
static purr_wid_t s_pass_input = 0;
static purr_wid_t s_pass2_input = 0;

static void set_status(const char *msg) {
    if (s_status_lbl) purr_win_label_set(s_status_lbl, msg);
}

// Destroys the window and reports the exit the same way any other app
// would — see app_manager_notify_exited()'s own "safe to call from the
// exiting task" doc comment; a button callback runs on the UI/render task,
// not a dedicated app task, but the operation is the same bookkeeping
// update either way.
static void finish(void) {
    user_mgr_set_oobe_completed();
    if (s_win) { purr_win_destroy(s_win); s_win = 0; }
    s_status_lbl = s_user_input = s_pass_input = s_pass2_input = 0;
    app_manager_notify_exited("oobe");
}

static void on_skip(purr_wid_t w, purr_event_t e, void *u) {
    (void)w; (void)e; (void)u;
    ESP_LOGI(TAG, "setup skipped — keeping default account '%s'", user_mgr_default_username());
    // Bootstrap default has no password (see user_mgr.h) — this is exactly
    // the "milkaholic, no password" zero-friction path, unchanged.
    user_mgr_set_logged_in(user_mgr_default_username());
    // OOBE bypasses systemui_login.c's real screen entirely (and its own
    // try_unlock() -> app_manager_notify_unlocked() call) — without this,
    // finishing setup would leave the local registry permanently idle/
    // empty. See app_manager.h's own doc comment on this function.
    app_manager_notify_unlocked();
    finish();
}

static void on_continue(purr_wid_t w, purr_event_t e, void *u) {
    (void)w; (void)e; (void)u;

    const char *username = s_user_input ? purr_win_textarea_get(s_user_input) : "";
    const char *password  = s_pass_input  ? purr_win_textarea_get(s_pass_input)  : "";
    const char *password2 = s_pass2_input ? purr_win_textarea_get(s_pass2_input) : "";
    if (!username) username = "";
    if (!password)  password  = "";
    if (!password2) password2 = "";

    if (!user_mgr_valid_username(username)) {
        set_status("Username: lowercase letters/numbers/_, must start with a letter.");
        return;
    }
    if (strcmp(password, password2) != 0) {
        set_status("Passwords don't match.");
        return;
    }

    const char *current_default = user_mgr_default_username();
    bool renaming = strcmp(username, current_default) != 0;

    if (renaming) {
        if (user_mgr_exists(username)) {
            set_status("That username is already taken.");
            return;
        }
        if (!user_mgr_create(username, password)) {
            set_status("Could not create that account — try again.");
            return;
        }
        // Drop the now-unused bootstrap placeholder rather than leaving it
        // sitting alongside the real account forever. Only ever removes
        // the bootstrap default specifically — see user_mgr.h's own doc
        // comment on why this and the "keep it" path are equally valid.
        if (strcmp(current_default, USER_MGR_BOOTSTRAP_USER) == 0) {
            user_mgr_remove(USER_MGR_BOOTSTRAP_USER);
        }
    } else {
        // Same username as the current default — just set (or clear) its password.
        if (!user_mgr_set_password(username, password)) {
            set_status("Could not set that password — try again.");
            return;
        }
    }

    user_mgr_set_logged_in(username);
    // Same reasoning as on_skip() above — this path bypasses
    // systemui_login.c entirely too.
    app_manager_notify_unlocked();
    ESP_LOGI(TAG, "setup complete — account '%s' (%s)", username,
             password[0] ? "password set" : "no password");
    finish();
}

static int oobe_init(void) {
    // Only the AUTOMATIC boot-time launch is gated on user_mgr_oobe_
    // completed() (see autorun_oobe() in app_manager.c) — this app can
    // still be launched deliberately by name (e.g. from Terminal, to redo
    // setup later) with no extra guard needed here: app_manager_launch_
    // path()'s own `state == APP_STATE_RUNNING` early return already
    // prevents a second concurrent launch while this one is showing.
    s_win = purr_win_create("Welcome to PURR OS");

    char welcome[96];
    snprintf(welcome, sizeof(welcome), "Set up your account, or keep the default (%s, no password).",
             USER_MGR_BOOTSTRAP_USER);
    purr_win_label(s_win, welcome);

    purr_win_label(s_win, "Username:");
    s_user_input = purr_win_textarea(s_win, 90, 20);
    purr_win_textarea_set(s_user_input, user_mgr_default_username());

    purr_win_label(s_win, "Password (optional):");
    s_pass_input = purr_win_textarea(s_win, 90, 20);

    purr_win_label(s_win, "Confirm password:");
    s_pass2_input = purr_win_textarea(s_win, 90, 20);

    s_status_lbl = purr_win_label(s_win, "");

    purr_wid_t row = purr_win_row(s_win, 4);
    purr_win_button(s_win, "Continue",    on_continue, NULL);
    purr_win_button(s_win, "Skip Setup",  on_skip,     NULL);
    purr_win_layout_end(row);

    purr_win_textarea_focus(s_user_input);
    purr_win_show(s_win);
    purr_win_keyboard_show(s_win, s_user_input);
    return 0;
}

static void oobe_deinit(void) {
    if (s_win) { purr_win_destroy(s_win); s_win = 0; }
    s_status_lbl = s_user_input = s_pass_input = s_pass2_input = 0;
}

// ── Module header ─────────────────────────────────────────────────────────────

PURR_MODULE_REGISTER(oobe) = {
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_APP,
    .load_priority     = PURR_PRIORITY_OPTIONAL,
    .name              = "oobe",
    .version           = "1.0.0",
    .kernel_min        = "0.11.1",
    .provided_catcalls = 0,
    .required_catcalls = 0,
    .init              = oobe_init,
    .deinit            = oobe_deinit,
};
