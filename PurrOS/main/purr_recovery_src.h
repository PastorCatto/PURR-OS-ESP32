/*
 * purr_recovery_src.h - where recovery pulls its images from: the official server (the Kconfig URLs), or, when the bootloader
 * allowed it, an SDK server on the local network that the owner picked and confirmed on the recovery setup page.
 *
 * The bootloader's DEV_RECOVERY flag is the gate (purr_abi.h): it only takes effect in development mode, and reaches here as
 * PURR_SECST_DEVREC in the hand-off. Without it every function here answers "the official server" and a saved choice is
 * ignored. Either way the images are checked against the boot key before anything is written.
 */
#ifndef PURR_RECOVERY_SRC_H
#define PURR_RECOVERY_SRC_H

#include <stddef.h>

#include "purr_lan_discover.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 1 if the bootloader allowed a dev server for recovery on this boot. */
int purr_recovery_dev_allowed(void);

/* Asks the server at base (a purr_repo.h-approved local address) for its recovery manifest and checks it has PURR OS for this
 * board. 0 if so; else -1 and msg says why, in plain words. msg also says what it found when it is fine. Blocks: a worker task. */
int purr_recovery_verify(const char *base, char *msg, size_t cap);

/* Use this server from now on (remembered for the next boot), or NULL for the official one. Ignored unless allowed. */
void purr_recovery_use(const purr_lan_server_t *server);

/* The files to fetch. Returned pointers stay valid until the next call to the same function. */
const char *purr_recovery_manifest_url(void);
const char *purr_recovery_modules_url(void);
const char *purr_recovery_legal_url(void);

/* The chosen dev server's name, or NULL when using the official server. */
const char *purr_recovery_source_name(void);

#ifdef __cplusplus
}
#endif

#endif /* PURR_RECOVERY_SRC_H */
