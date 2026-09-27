/*
 * purr_cfg.h - purrcfg access (bootloader/SPEC.md section 5).
 *
 * Two 4 KB sectors hold A/B copies of purr_cfg_t. The newest copy with a valid
 * CRC wins. A store writes the other sector, so a power cut at any point
 * leaves either the old copy or the new one, never neither.
 *
 * Flash access is abstract so the same code runs on the device and on the PC.
 */
#ifndef PURR_CFG_H
#define PURR_CFG_H

#include <stdint.h>

#include "purr_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* All return 0 on success, nonzero on error. */
    int (*read)(void *ctx, uint32_t addr, void *buf, uint32_t len);
    int (*erase)(void *ctx, uint32_t addr);    /* erase the 4 KB sector at addr */
    int (*write)(void *ctx, uint32_t addr, const void *buf, uint32_t len);
    void *ctx;
    uint32_t base;                             /* address of sector A, B follows */
} purr_flash_t;

/* Fill in defaults: valid header, secure_mode = warn, everything else zero. */
void purr_cfg_defaults(purr_cfg_t *cfg);

/* Set the CRC over the struct. */
void purr_cfg_seal(purr_cfg_t *cfg);

/* Magic, version, size and CRC are all good. */
int purr_cfg_valid(const purr_cfg_t *cfg);

/*
 * Load the newest valid copy. Returns 0 if one was found, 1 if neither was
 * valid and defaults were filled in, -1 on a read error. If which is not NULL
 * it receives the sector index (0 or 1) of the copy used, or -1 for defaults.
 */
int purr_cfg_load(const purr_flash_t *fl, purr_cfg_t *out, int *which);

/*
 * Store cfg into the sector that does not hold the newest copy. Sets seq to
 * one more than the newest copy's, seals the CRC, writes, and reads back to
 * check. Returns 0 on success, -1 on any error (the old copy is untouched).
 */
int purr_cfg_store(const purr_flash_t *fl, purr_cfg_t *cfg);

/*
 * One-shot flags (PURR_CFGF_*). Both return nonzero if the change was made, zero if the
 * flag was already in that state. Neither writes flash: the caller stores afterwards.
 */
int purr_cfg_set_flag(purr_cfg_t *cfg, uint32_t flag);
int purr_cfg_take_flag(purr_cfg_t *cfg, uint32_t flag);   /* clears it, and says it was set */

#ifdef __cplusplus
}
#endif

#endif /* PURR_CFG_H */
