/*
 * purr_swap.h - what KittenOS does about a pending system-file update
 * (PurrOS/SPEC.md section 6.1): CoreOS, AppManager, the runtimes, the drivers bundle.
 *
 * kernel, kittenos, loader and bootpkg are partition-level instead (OTA/SPEC.md section 3,
 * PurrOS/main/commands.c's net_install component table) -- this file is only for the
 * components that actually live as files in LittleFS's /boot.
 *
 * Pure decision logic, the same shape as purr_decision.h: no filesystem, no flash. The caller
 * (commands.c, which has purr_fs_t and the verify machinery) does the actual file operations
 * once it knows which action purr_swap_decide() says to take.
 */
#ifndef PURR_SWAP_H
#define PURR_SWAP_H

#include <stdint.h>

#include "purr_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PURR_SWAP_NOTHING = 0,   /* no pending update, or already confirmed/failed: nothing to do */
    PURR_SWAP_DO_SWAP,       /* requested: verify the staged file, then rename it into place */
    PURR_SWAP_RESUME_MOVE,   /* moving: a swap was interrupted mid-rename, finish it */
    PURR_SWAP_RETRY,         /* unconfirmed, tries remain: just boot the normal system again */
    PURR_SWAP_ROLLBACK,      /* unconfirmed, tries exhausted: restore the backup, mark failed */
} purr_swap_action_t;

/*
 * Rules (PurrOS/SPEC.md section 6.1):
 *  - requested -> do the swap.
 *  - moving -> a previous swap was cut short (power loss); resume and finish it, not restart
 *    it from scratch, since one of the two renames may already have happened.
 *  - unconfirmed, attempts < max_attempts -> the swapped-in file hasn't reached healthy yet,
 *    but tries remain: retry (the caller increments attempts and boots normally again).
 *  - unconfirmed, attempts >= max_attempts -> roll back.
 *  - none, staged, confirmed, failed -> nothing to do. ("staged" is a file sitting in /boot
 *    that nothing has requested yet; KittenOS only acts once the requester sets "requested".)
 */
purr_swap_action_t purr_swap_decide(const purr_update_t *update, uint8_t max_attempts);

/* The /boot file name for a component id (PURR_COMP_*), e.g. "coreos.kitt". NULL for a
 * component this mechanism doesn't cover (kernel, kittenos, loader, bootpkg). */
const char *purr_swap_filename(uint8_t component);

#ifdef __cplusplus
}
#endif

#endif /* PURR_SWAP_H */
