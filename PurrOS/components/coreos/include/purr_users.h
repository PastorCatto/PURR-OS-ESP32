/*
 * purr_users.h - accounts (Users/SPEC.md).
 *
 * Plain C, no filesystem or crypto library dependency: the account list and the
 * password records are separate, matching the spec's root-only /etc/passwd and
 * /etc/shadow split, each with its own flat text format (one line per user, tab
 * separated, the same style as purr_wifi's saved list). Root (uid 0) is never a row
 * in either file: `su` re-checks the caller's own password instead of needing one
 * for root, per the spec ("its own login is locked").
 */
#ifndef PURR_USERS_H
#define PURR_USERS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PURR_USER_NAME_LEN   32
#define PURR_USER_MAX        16
#define PURR_SALT_LEN        16
#define PURR_HASH_LEN        32
#define PURR_PBKDF2_ITERATIONS 10000   /* Open in Users/SPEC.md section 8; a starting point */

typedef enum {
    PURR_ROLE_USER_STANDARD = 0,
    PURR_ROLE_USER_ADMIN = 1,
} purr_user_role_t;

typedef struct {
    char name[PURR_USER_NAME_LEN];
    uint8_t uid;                     /* 1..255; 0 is root and is never stored here */
    purr_user_role_t role;
    uint32_t fail_count;             /* consecutive wrong passwords, for the growing delay */
    uint32_t next_allowed_time;      /* unix seconds; a login before this is refused outright */
} purr_user_t;

typedef struct {
    purr_user_t users[PURR_USER_MAX];
    int count;
} purr_user_list_t;

typedef struct {
    char name[PURR_USER_NAME_LEN];
    uint8_t salt[PURR_SALT_LEN];
    uint32_t iterations;
    uint8_t hash[PURR_HASH_LEN];
} purr_shadow_entry_t;

typedef struct {
    purr_shadow_entry_t entries[PURR_USER_MAX];
    int count;
} purr_shadow_list_t;

void purr_user_list_init(purr_user_list_t *l);
void purr_shadow_list_init(purr_shadow_list_t *l);

/* The next free uid (1, 2, 3, ...), or 0 if the list is full. */
uint8_t purr_user_next_uid(const purr_user_list_t *l);

enum {
    PURR_USER_ADDED = 1,
    PURR_USER_INVALID = -1,     /* an empty name, a tab/newline in it, or uid 0 */
    PURR_USER_EXISTS = -2,      /* the name is already taken */
    PURR_USER_FULL = -3,
};

/* F-11: letters, digits, '-', '_' only, bounded length, not "root". purr_user_list_add()
 * already enforces this; exposed so a caller (first_time_setup, useradd) can re-prompt
 * before trying to add, instead of finding out from the rejected result. */
int purr_user_name_valid(const char *name);

int purr_user_list_add(purr_user_list_t *l, const char *name, uint8_t uid, purr_user_role_t role);
int purr_user_list_remove(purr_user_list_t *l, const char *name);   /* 1 removed, 0 not found */
purr_user_t *purr_user_list_find(purr_user_list_t *l, const char *name);
purr_user_t *purr_user_list_find_uid(purr_user_list_t *l, uint8_t uid);

/* True if this is the only admin account left (refuse removing or demoting them: Users/SPEC.md
 * says nothing here directly, but leaving zero admins locks the device out). */
int purr_user_is_last_admin(const purr_user_list_t *l, const char *name);

/* One line per user: name\tuid\trole\tfail_count\tnext_allowed_time\n */
int purr_user_list_format(const purr_user_list_t *l, char *buf, size_t bufsize);   /* bytes, or -1 */
void purr_user_list_parse(purr_user_list_t *l, const char *buf, size_t len);       /* bad lines skipped */

/* Sets (or replaces) an entry's password: hashes it with a fresh salt (caller-supplied,
 * e.g. from a hardware RNG) and the given iteration count. */
void purr_shadow_set(purr_shadow_list_t *l, const char *name, const char *password,
                     const uint8_t salt[PURR_SALT_LEN], uint32_t iterations);
int purr_shadow_list_remove(purr_shadow_list_t *l, const char *name);
const purr_shadow_entry_t *purr_shadow_find(const purr_shadow_list_t *l, const char *name);

/* Constant-time comparison against the stored hash. Returns 1 on a match, 0 otherwise
 * (including "no such entry"). */
int purr_shadow_check(const purr_shadow_list_t *l, const char *name, const char *password);

/* name\tsalt_hex\titerations\thash_hex\n */
int purr_shadow_list_format(const purr_shadow_list_t *l, char *buf, size_t bufsize);
void purr_shadow_list_parse(purr_shadow_list_t *l, const char *buf, size_t len);

/*
 * The growing delay after `fail_count` consecutive wrong passwords: 1, 2, 4, 8...
 * seconds, capped at 60 (Users/SPEC.md section 1). fail_count 0 means no delay.
 */
uint32_t purr_login_delay_seconds(uint32_t fail_count);

#ifdef __cplusplus
}
#endif

#endif /* PURR_USERS_H */
