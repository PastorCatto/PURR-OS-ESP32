/*
 * purr_preload.h - apps that ship inside the image, to be planted at first start (the second-stage OOBE, plan1.md "Ready-to-go image").
 *
 * An image made for a device that is not to need a network install carries signed app packages in /boot/preload (one `<name>.cat` each, put there when the image's
 * root file system is built). The first-run wizard lists them, the person ticks the ones they want, and once the account exists they are installed under that account's
 * apps folder, exactly as an app installed by hand is (purr_appmgr_add: the signature is checked, a package that does not verify is refused and the rest carry on). The files stay in
 * /boot/preload afterwards, so an app that was left out can be added later.
 *
 * Plain C over purr_appmgr_fs_t, tested on the PC (test_preload.c).
 */
#ifndef PURR_PRELOAD_H
#define PURR_PRELOAD_H

#include <stdint.h>

#include "purr_appmgr.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PURR_PRELOAD_DIR  "/boot/preload"
#define PURR_PRELOAD_MAX  16                 /* the wizard's page shows this many, a long list scrolls */

typedef struct {
    char file[PURR_APP_NAME_LEN + 8];        /* "notes.cat" */
    char name[PURR_APP_NAME_LEN];            /* from the package's own header */
    char version[PURR_VERSION_LEN];
    uint32_t size;
} purr_preload_app_t;

/* The apps in `dir`: every `*.cat` that is a PURR app image for this chip (the header is read, nothing is verified here: planting verifies). A file that is not one, or is
 * larger than the scratch buffer, is left out, never fatal. Sorted by file name. Returns how many (at most `max`). `scratch` is the buffer a package is read into. */
int purr_preload_list(const purr_appmgr_fs_t *fs, const char *dir, uint16_t chip_id, uint8_t *scratch, uint32_t cap, purr_preload_app_t *out, int max);

/* Installs one of them under `root` (the account's apps folder): the package read from `dir/file`, then purr_appmgr_add. PURR_APP_OK, PURR_APP_EXISTS_NEWER when this version or a newer
 * one is already installed (which is fine: it counts as done), or the reason it was refused. `reg` is the registry of `root` as it is now (purr_appmgr_scan). */
purr_app_result_t purr_preload_plant(const purr_appmgr_fs_t *fs, const char *dir, const char *file, const char *root, const purr_appmgr_env_t *env,
                                     const purr_cfg_t *cfg, const purr_app_registry_t *reg, uint8_t *scratch, uint32_t cap);

#ifdef __cplusplus
}
#endif

#endif /* PURR_PRELOAD_H */
