/*
 * purr_userstore.h - /etc/passwd and /etc/shadow on the root filesystem (Users/SPEC.md).
 *
 * A thin layer over purr_fs and purr_users: load both files into memory, edit through
 * purr_users's functions, save both back. Root-only in spirit; real per-file permission
 * enforcement is not built yet (PurrOS/SPEC.md's filesystem section), so for now anything
 * with shell access can read these, same as the saved Wi-Fi password today.
 */
#ifndef PURR_USERSTORE_H
#define PURR_USERSTORE_H

#include "purr_fs.h"
#include "purr_users.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Loads both files. Missing files are not an error: both lists come back empty, which
 * means "no accounts yet" (first-time setup). A file that is there but cannot be read (after retries) is tried again as its .bak copy (every save keeps one).
 * Returns 0 on success (files read or legitimately absent), PURR_USERSTORE_FROM_BACKUP if a .bak copy had to be used, -1 if the filesystem itself is not usable,
 * PURR_USERSTORE_READ_ERROR if a file exists and neither it nor its backup could be read (the lists are empty then, which must NOT be taken as "no accounts yet":
 * nothing may be created or saved over what could not be read). */
#define PURR_USERSTORE_FROM_BACKUP 1
#define PURR_USERSTORE_READ_ERROR  (-2)
int purr_userstore_load(purr_fs_t *fs, purr_user_list_t *users, purr_shadow_list_t *shadow);

/* Writes both files. Returns 0 on success. */
int purr_userstore_save(purr_fs_t *fs, const purr_user_list_t *users, const purr_shadow_list_t *shadow);

#ifdef __cplusplus
}
#endif

#endif /* PURR_USERSTORE_H */
