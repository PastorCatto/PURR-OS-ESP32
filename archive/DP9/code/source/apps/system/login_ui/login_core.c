// login_core.c — see login_core.h for the full picture.
#include "login_core.h"
#include <string.h>

// Hand-declared externs, not #include "user_mgr.h"/"app_manager.h" — this
// file compiles standalone via catstrap's "sysclaw" tier (see app.pcat's
// own comment), the same restricted context every claw_loader_selftest.c
// guest object already established the convention for. Signatures
// confirmed against the real headers; all four names are already in
// claw_imports_generated.h's import table (purr_kernel.h/user_mgr.h/
// app_manager.h are all scanned — see purrstrap.py's _generate_claw_
// imports()), so catstrap's own build-time import check passes.
extern bool        user_mgr_verify(const char *username, const char *password);
extern bool        user_mgr_set_logged_in(const char *username);
extern const char *user_mgr_default_username(void);
extern void        app_manager_notify_unlocked(void);

void login_core_init(login_core_t *lc)
{
    memset(lc, 0, sizeof(*lc));
    lc->state = LOGIN_STATE_USERNAME;

    const char *def = user_mgr_default_username();
    if (def && def[0]) {
        int i = 0;
        while (def[i] && i < (int)sizeof(lc->username) - 1) {
            lc->username[i] = def[i];
            i++;
        }
        lc->username[i] = '\0';
        lc->u_len = i;
    }
}

static void reset_password(login_core_t *lc)
{
    memset(lc->password, 0, sizeof(lc->password));
    lc->p_len = 0;
}

void login_core_key(login_core_t *lc, char c)
{
    if (lc->state == LOGIN_STATE_ERROR) {
        // Any keypress after a failed attempt clears the error and drops
        // back to a fresh password prompt — same field the error was
        // actually about, matching purr_console_login.c's own always-
        // re-prompt convention rather than sending the user all the way
        // back to the username field.
        lc->state = LOGIN_STATE_PASSWORD;
        reset_password(lc);
    }

    if (lc->state == LOGIN_STATE_USERNAME) {
        if (lc->u_len < (int)sizeof(lc->username) - 1) {
            lc->username[lc->u_len++] = c;
            lc->username[lc->u_len] = '\0';
        }
    } else if (lc->state == LOGIN_STATE_PASSWORD) {
        if (lc->p_len < (int)sizeof(lc->password) - 1) {
            lc->password[lc->p_len++] = c;
            lc->password[lc->p_len] = '\0';
        }
    }
    // LOGIN_STATE_SUCCESS: no-op — a terminal state, nothing more to type.
}

void login_core_backspace(login_core_t *lc)
{
    if (lc->state == LOGIN_STATE_USERNAME && lc->u_len > 0) {
        lc->username[--lc->u_len] = '\0';
    } else if (lc->state == LOGIN_STATE_PASSWORD && lc->p_len > 0) {
        lc->password[--lc->p_len] = '\0';
    }
}

void login_core_submit(login_core_t *lc)
{
    if (lc->state == LOGIN_STATE_USERNAME) {
        if (lc->u_len == 0) return;   // Enter on an empty username is a no-op
        lc->state = LOGIN_STATE_PASSWORD;
        reset_password(lc);
        return;
    }

    if (lc->state == LOGIN_STATE_PASSWORD) {
        if (user_mgr_verify(lc->username, lc->password)) {
            user_mgr_set_logged_in(lc->username);
            app_manager_notify_unlocked();
            reset_password(lc);
            lc->state = LOGIN_STATE_SUCCESS;
        } else {
            reset_password(lc);
            lc->error_msg = "Incorrect password. Try again.";
            lc->state = LOGIN_STATE_ERROR;
        }
        return;
    }
    // LOGIN_STATE_ERROR: login_core_key() moves this back to PASSWORD on
    // the next keypress; Enter with no keypress first has nothing to
    // submit. LOGIN_STATE_SUCCESS: terminal, no-op.
}
