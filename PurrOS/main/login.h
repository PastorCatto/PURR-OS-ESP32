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

/* A fresh random salt, from the hardware RNG. */
void purr_login_random_salt(uint8_t salt[PURR_SALT_LEN]);

/* Set by the "logout" command; main.c's shell loop checks this to return to login. */
void purr_login_request_logout(void);
int purr_login_take_logout(void);      /* 1 if requested, and clears it */

#ifdef __cplusplus
}
#endif

#endif /* PURR_LOGIN_H */
