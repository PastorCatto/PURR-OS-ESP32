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

#endif
