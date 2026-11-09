/*
 * purr_repo_dev.h - the device's client for a repository of apps (plan8.md): find an SDK server on the
 * local network, read its index, install an app from it. The rules (what may be contacted, what an entry
 * may be) are purr_repo.h's; this is the part that touches the network and the app manager.
 *
 * Every call blocks, so call from a worker task, never the UI task. Only while developer mode is active
 * this boot (purr_security_dev_active()); every function refuses otherwise.
 */
#ifndef PURR_REPO_DEV_H
#define PURR_REPO_DEV_H

#include <stddef.h>

#include "purr_repo.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char base[80];                      /* http://10.0.2.2:47801 */
    char name[32];
    char id[PURR_REPO_ID_LEN];
} purr_repo_server_t;

/* Turns what a person typed ("10.0.2.2:47801", "http://my-mac.local:8000") into a base address the policy
 * accepts, with the host resolved and checked to be a local address. On success *out is a base address
 * with a numeric host (so the address that was checked is the address that is used). 0 or -1 with err. */
int purr_repo_dev_resolve(const char *typed, char *out, size_t cap, char *err, size_t errcap);

/* Asks the local network (a broadcast, and the VM host at 10.0.2.2) which SDK servers are listening.
 * Returns how many answered (up to max). */
int purr_repo_dev_discover(purr_repo_server_t *out, int max, int wait_ms);

/* Fetches and parses base/purr/v1/index.json as an SDK repository. */
int purr_repo_dev_index(const char *base, purr_repo_index_t *idx, char *err, size_t errcap);

/* Downloads one app, checks it against the index entry, and installs it in the user's devapps folder. */
int purr_repo_dev_install(const char *base, const purr_repo_app_t *app, char *msg, size_t cap);

/* The server the owner chose last, and the id they confirmed (kept in /etc/system.conf). */
int purr_repo_dev_last(purr_repo_server_t *out);                 /* 0 if there is one */
void purr_repo_dev_remember(const purr_repo_server_t *s);       /* also records the id as trusted */
int purr_repo_dev_trusted(const char *id);                      /* the owner confirmed this server before */

#ifdef __cplusplus
}
#endif

#endif /* PURR_REPO_DEV_H */
