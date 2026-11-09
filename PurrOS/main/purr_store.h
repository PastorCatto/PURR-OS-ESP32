/*
 * purr_store.h - the repository client and installer (Repositories/SPEC.md): trusted repositories, signed indexes, installing and updating apps, assets and
 * extras, and the loader's question "does a trusted repository vouch for this package?". The `store` catcall and the `store` shell command sit on top.
 */
#ifndef PURR_STORE_H
#define PURR_STORE_H

#include <stddef.h>
#include <stdint.h>

#include "purr_appmgr.h"
#include "purr_cli.h"

/* Registers the `store` catcall. Call once at start-up (commands.c, next to the files catcall). */
void purr_catrt_store_init(void);

/* The loader's rule 2 (SPEC section 5): 1 if `path` (/home/<user>/apps/<name>/package.cat) has a receipt from a trusted repository whose signed index lists
 * exactly these bytes. Never 1 for a repository that is untrusted unless developer mode is on. */
int purr_store_vouches(const char *path, const uint8_t *data, size_t len);

/* Marks the entries of a scan of /home/<user>/apps that a trusted repository vouches for as verified, so they show and run like any app. */
void purr_store_mark_vouched(purr_app_registry_t *reg, const char *apps_root);

/* The shell command: store [repos | add <url> | confirm | refresh [id] | list | install <name> <repo-id> | remove <name> <repo-id> | check | bootstrap]. */
int purr_store_cmd(purr_cli_t *cli, int argc, char **argv);

/* First-run setup: add the official repository and install the Store and its companion. Blocking; call from a worker with a big stack. 0, or -1 with msg. */
int purr_store_bootstrap(char *msg, size_t cap);


/* The Store and its companion, built into the firmware (tools/release/gen_builtin_apps.py), so a new account has them without a network. */
typedef struct {
    const char *name;
    const char *version;
    const uint8_t *data;
    uint32_t len;
    uint8_t sha256[32];
} purr_builtin_app_t;
extern const purr_builtin_app_t purr_builtin_apps[];
extern const int purr_builtin_apps_count;

/* After a sign-in: puts the built-in apps in this user's apps folder, once (a marker in their home remembers, so removing the Store sticks), gives the
 * companion the one permission it needs, and starts it. Runs on its own task. */
void purr_store_session_started(void);

#endif
