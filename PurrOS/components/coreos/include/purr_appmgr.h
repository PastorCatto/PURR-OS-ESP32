/*
 * purr_appmgr.h - storage and registry for installed apps (AppManager/SPEC.md sections 4-5).
 *
 * Plain C over purr_fs (already host-testable against a RAM disk), so this is too. Local
 * apps only: add, remove, list. No running an app (AppRuntime's job, not built) and no
 * transport (MTP etc.; a caller hands over a package already fully in memory).
 *
 * Layout on the apps filesystem: one folder per app, `<name>/package.cat` holding the
 * signed image. The filesystem is the source of truth; the registry here is a cache built
 * by scanning it, so it is always rebuildable and never has to be migrated.
 */
#ifndef PURR_APPMGR_H
#define PURR_APPMGR_H

#include <stddef.h>
#include <stdint.h>

#include "purr_abi.h"
#include "purr_cfg.h"
#include "purr_fs.h"
#include "purr_verify.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PURR_APP_MAX      32     /* Users/SPEC.md's MAX_APPS-style cap; a fixed table, no heap */
#define PURR_APP_NAME_LEN 32

typedef struct {
    char name[PURR_APP_NAME_LEN];
    char version[PURR_VERSION_LEN];
    uint32_t size;                /* the whole .cat file, not just the payload */
    uint8_t chip_ok;               /* its chip_id matches this device, or PURR_CHIP_ANY */
    uint8_t signer_role;           /* PURR_ROLE_*, or PURR_ROLE_NONE if unsigned */
    uint8_t verified;              /* passed purr_image_verify */
} purr_app_entry_t;

typedef struct {
    purr_app_entry_t apps[PURR_APP_MAX];
    int count;
    int dropped;                   /* app folders seen but not usable: unreadable or a bad
                                    * container. Never fatal; just not listed. */
} purr_app_registry_t;

/* What verification needs: the same pieces the bootloader and the kernel build. */
typedef struct {
    uint16_t chip_id;
    const purr_keybag_t *bag;
    const purr_crypto_t *crypto;
} purr_appmgr_env_t;

typedef enum {
    PURR_APP_OK = 0,
    PURR_APP_BAD_MAGIC,
    PURR_APP_BAD_TYPE,             /* not image_type == PURR_IMG_APP */
    PURR_APP_BAD_CHIP,             /* no payload for this device */
    PURR_APP_BAD_VERIFY,           /* failed verification, and secure_mode is not off */
    PURR_APP_NAME_TOO_LONG,
    PURR_APP_EXISTS_NEWER,         /* already installed at this version or newer */
    PURR_APP_REGISTRY_FULL,
    PURR_APP_IO_ERROR,
} purr_app_result_t;

const char *purr_app_result_name(purr_app_result_t r);

/* Deletes any "<name>.tmp" folder left over from a cut install. Call once at boot, before
 * purr_appmgr_scan. Never fails outright; a folder it cannot remove is just left for the
 * next boot to try again. */
void purr_appmgr_recover(purr_fs_t *fs);

/*
 * Rebuilds the registry by scanning the filesystem's root: one subfolder per app, each
 * expected to hold "package.cat". A folder that does not parse as an app image, or whose
 * package is bigger than `scratch_cap`, is skipped (reg->dropped counts it), not fatal to
 * the scan. `scratch` is used to read one package at a time for verification; how big to
 * make it is a caller decision (PSRAM lets it be generous, a board with none does not).
 */
void purr_appmgr_scan(purr_fs_t *fs, const purr_appmgr_env_t *env, uint8_t *scratch,
                      uint32_t scratch_cap, purr_app_registry_t *reg);

/*
 * Installs a package already read into memory. Verifies it, checks its chip has a usable
 * payload, and, if a version of the same name is already installed, requires the new one
 * to be newer. Stages at "<name>.tmp/package.cat", checks the copy's hash, then renames
 * into place (replacing an older version atomically). `cfg` supplies secure_mode for the
 * accept-unsigned rule, same as everywhere else.
 */
purr_app_result_t purr_appmgr_add(purr_fs_t *fs, const purr_appmgr_env_t *env, const purr_cfg_t *cfg,
                                  const purr_app_registry_t *reg, const uint8_t *data, uint32_t len);

/* Removes "<name>/" entirely. 0 on success, nonzero if there is no such app or it could
 * not be fully removed (a partial removal is safe: the next scan just drops it as
 * incomplete, the same as a cut install). */
int purr_appmgr_remove(purr_fs_t *fs, const char *name);

const purr_app_entry_t *purr_appmgr_find(const purr_app_registry_t *reg, const char *name);

#ifdef __cplusplus
}
#endif

#endif /* PURR_APPMGR_H */
