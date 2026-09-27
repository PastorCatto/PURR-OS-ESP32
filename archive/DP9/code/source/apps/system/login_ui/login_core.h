#pragma once
// login_core.h — backend-agnostic login state machine, shared by both
// render backends (login_render_fb.c / login_render_lvgl.c). No drawing,
// no input polling, no catcall access at all — a render backend feeds it
// keystrokes and reads its state back to decide what to draw.
//
// Compiled standalone via catstrap's "sysclaw" tier (see app.pcat's own
// comment) — no #include of real ESP-IDF/kernel headers anywhere in this
// package. Ordinary, freestanding-safe C only.

#include <stdbool.h>

#define LOGIN_USERNAME_MAX 32   // matches USER_MGR_USERNAME_MAX (user_mgr.h)
#define LOGIN_PASSWORD_MAX 64

typedef enum {
    LOGIN_STATE_USERNAME,   // typing the username field
    LOGIN_STATE_PASSWORD,   // typing the password field
    LOGIN_STATE_ERROR,      // last attempt failed — error_msg is set
    LOGIN_STATE_SUCCESS,    // logged in — app_manager_notify_unlocked() already fired
} login_state_t;

#define LOGIN_MOTD_MAX 64

typedef struct {
    login_state_t state;
    char username[LOGIN_USERNAME_MAX];
    char password[LOGIN_PASSWORD_MAX];
    int  u_len;
    int  p_len;
    const char *error_msg;   // valid only while state == LOGIN_STATE_ERROR
    // Set by login_ui_main.c before the render loop starts (read from this
    // package's own staged assets — see catstrap.py's _stage_sysclaw_
    // assets() / this package's assets/motd.txt), never by login_core.c
    // itself. Empty string if no assets were found — a render backend
    // must treat that as "nothing to show", not an error.
    char motd[LOGIN_MOTD_MAX];
} login_core_t;

// Zeroes `lc`, sets state to LOGIN_STATE_USERNAME, and seeds the username
// field from user_mgr_default_username() (the same "identity to seed a
// login flow with" convenience the archived systemui_login.c used) so a
// single-account device can go straight to typing a password.
void login_core_init(login_core_t *lc);

// Appends one printable character to whichever field is currently being
// typed (username or password) — a no-op in LOGIN_STATE_SUCCESS, and in
// LOGIN_STATE_ERROR clears the error and resumes at the password field
// first (same "any key clears the error" behavior purr_console_login.c's
// own always-re-prompt already established).
void login_core_key(login_core_t *lc, char c);

// Removes the last character of whichever field is currently being typed.
void login_core_backspace(login_core_t *lc);

// Enter/Go. On the username field: advances to the password field (no-op
// if the username is empty). On the password field: verifies via
// user_mgr_verify() — success calls user_mgr_set_logged_in() +
// app_manager_notify_unlocked() and moves to LOGIN_STATE_SUCCESS; failure
// clears the password and moves to LOGIN_STATE_ERROR with error_msg set.
void login_core_submit(login_core_t *lc);
