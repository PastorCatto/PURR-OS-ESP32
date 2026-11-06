/*
 * login.h - the login gate (Users/SPEC.md section 4) and the session it starts.
 *
 * Runs once at boot, before the shell: first-time setup if there are no accounts yet,
 * otherwise a login prompt. The shell's account commands (whoami, id, su, passwd,
 * useradd, userdel, usermod, logout) read and mutate the same session and account lists
 * through the accessors here.
 */
#ifndef PURR_LOGIN_H
#define PURR_LOGIN_H

#include "purr_users.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Loads the accounts, runs first-time setup or the login prompt (looping on a wrong
 * password, with the growing delay) until one succeeds, and starts the session. */
void purr_login_run(void);


/* Loads the accounts (so useradd etc. still see them if any exist) but skips the login
 * gate entirely, starting an in-memory session that is not one of the stored accounts.
 * KittenOS uses this when there is no PURR OS installed yet: requiring an account to
 * reach the tools that would install one is backwards. */
void purr_login_skip(const char *name, purr_user_role_t role);

/* The in-memory account lists, shared with the login gate. Callers that change them
 * must call purr_login_persist() to write both files back. */
purr_user_list_t *purr_login_users(void);
purr_shadow_list_t *purr_login_shadow(void);
int purr_login_persist(void);          /* 0 on success */

/* The current session. Valid only after purr_login_run(). */
const purr_user_t *purr_login_current(void);
int purr_login_is_root(void);          /* an admin has su'd in this session */
void purr_login_set_root(int is_root);

/* Whether any account exists (loads the account files the first time it is asked). */
int purr_login_has_accounts(void);
/* 1 (and the account's name in `name`) while the default administrator, MEOW with the password MEOW, is still in force; 0 once its password has been changed or it has been removed. */
int purr_login_default_account(char *name, size_t cap);

typedef enum {
    PURR_LOGIN_OK = 0,       /* signed in: purr_login_current() is the account */
    PURR_LOGIN_BAD = 1,      /* wrong name or password */
    PURR_LOGIN_WAIT = 2,     /* too many wrong tries: try again in *wait_s seconds */
} purr_login_result_t;

/* One sign-in attempt, for a UI that cannot sit in a prompt loop. The same rules as the text prompt:
 * "root" is never a login, every wrong password makes the next wait longer (1, 2, 4 ... 60
 * seconds), and the counter is kept in the account file. It does not sleep: while a wait is running
 * it answers PURR_LOGIN_WAIT without checking the password. The check itself is a PBKDF2 hash, which
 * takes a while, so call it from a worker task and not from a UI callback. */
purr_login_result_t purr_login_try(const char *name, const char *password, uint32_t *wait_s);

/* Recovery (KittenOS): the built-in standard session "kitten", and user account control. Elevating turns the session into the
 * administrator's until the device restarts; a failed or non-administrator attempt changes nothing. Worker task only (hashing). */
void purr_login_kitten(void);
purr_login_result_t purr_login_elevate(const char *name, const char *password, uint32_t *wait_s);
int purr_login_elevated(void);         /* the session was elevated since it started */

/* Account changes without a prompt, for a UI (Settings) and anything else that cannot sit in a text
 * prompt. The rules are the shell commands' rules, enforced here and not left to the caller: only an
 * administrator (or an admin who has su'd) adds or removes accounts, changes anyone else's password or
 * role; anyone may change their own password given the current one; the account you are signed in as
 * cannot be removed, and the last administrator can be neither removed nor demoted. Each of these
 * hashes a password (PBKDF2), so call them from a worker task and not from a UI callback. A removed
 * account's home folder is kept. */
typedef enum {
    PURR_ACCT_OK = 0,
    PURR_ACCT_DENIED,        /* the signed-in account may not do this */
    PURR_ACCT_INVALID,       /* the name is not allowed */
    PURR_ACCT_EXISTS,
    PURR_ACCT_FULL,
    PURR_ACCT_NOUSER,
    PURR_ACCT_LAST_ADMIN,
    PURR_ACCT_SELF,          /* cannot remove the account you are signed in as */
    PURR_ACCT_BADPASS,       /* empty password, or the current password was wrong */
    PURR_ACCT_SAVE,          /* could not write the account files */
    PURR_ACCT_DEVMODE,       /* automatic sign-in needs developer mode, active this boot */
} purr_acct_result_t;

purr_acct_result_t purr_login_account_add(const char *name, const char *password, int admin);
purr_acct_result_t purr_login_account_add_first(const char *name, const char *password);   /* only with no accounts yet */
purr_acct_result_t purr_login_account_set_password(const char *name, const char *new_password,
                                                   const char *current_password /* own account only */);
purr_acct_result_t purr_login_account_remove(const char *name);
purr_acct_result_t purr_login_account_set_admin(const char *name, int admin);
const char *purr_login_account_error(purr_acct_result_t r);

/* Automatic sign-in works ONLY while developer mode is active for this boot (the bootloader's handoff;
 * a signed trust block that locks developer mode out makes it impossible), so a secure device never
 * signs in on its own. Called once at boot, before the sign-in screen: if a user is set for it (setting
 * login.autologin) and still exists, or the test-image account is built in (CONFIG_PURR_TEST_AUTOLOGIN,
 * VM images only), the session starts as that user with no password asked, and this returns 1. Holding
 * space at boot skips it (main.c does not call this then). It weakens the sign-in screen for that account,
 * so turning it on needs an administrator and their password. */
int purr_login_autologin(void);
int purr_login_autologin_used(void);                       /* this session started automatically */
void purr_login_autologin_user(char *out, size_t cap);     /* the configured user, "" if none */
/* name = NULL turns it off. admin_password confirms the acting administrator (NULL skips the check, for a
 * caller that has already authenticated them, such as the shell's `settings`). */
purr_acct_result_t purr_login_set_autologin(const char *name, const char *admin_password);

/* A fresh random salt, from the hardware RNG. */
void purr_login_random_salt(uint8_t salt[PURR_SALT_LEN]);

/* Set by the "logout" command; main.c's shell loop checks this to return to login. */
void purr_login_request_logout(void);
int purr_login_take_logout(void);      /* 1 if requested, and clears it */

#ifdef __cplusplus
}
#endif

#endif /* PURR_LOGIN_H */
