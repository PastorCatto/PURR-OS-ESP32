/*
 * purr_drvpart_plat.h - the `drivers` partition on the device (Drivers/SPEC.md, "The driver partition"; the format is components/coreos/include/purr_drvpart.h).
 *
 * The kernel plants these into /boot/drivers at boot; the loader and KittenOS, which have no file system, load straight from the slots.
 */
#ifndef PURR_DRVPART_PLAT_H
#define PURR_DRVPART_PLAT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Called once for each good slot with the file name the driver is planted under and its .cat (valid only during the call). A nonzero return stops the walk and is returned.
 * Empty slots are skipped quietly, damaged ones are logged and skipped. Returns 0 when the walk finished, -1 when the board has no `drivers` partition (a layout from
 * before flash map v3, or the 8 MB board) or there was no memory. */
typedef int (*purr_drvpart_each_fn)(void *ctx, const char *file, const uint8_t *cat, uint32_t len);
int purr_drvpart_each(purr_drvpart_each_fn fn, void *ctx);

/* Whether a good slot carries a driver planted under exactly this file name (headers only: nothing is loaded). */
int purr_drvpart_has_file(const char *file);

/* How many good slots there are (0 with no partition). */
int purr_drvpart_count(void);

/* 1 when the board has a `drivers` partition of the right size (flash map v3). */
int purr_drvpart_present(void);

/* 1 when the partition holds exactly this image: the first `size` bytes hash to `sha256` and everything after them is blank. For a restore that must not rewrite
 * what is already right. 0 with no partition. */
int purr_drvpart_matches(uint32_t size, const uint8_t sha256[32]);

/* Writes a release's `drivers` image (purr_drvpart.h, purr_drvpart_check_image) into the partition: checked first, so a damaged image changes nothing; then the whole
 * partition is erased, written and read back. 0, or -1 with a short reason in `why` (may be NULL). The signatures inside are not checked here: each driver's is checked
 * when it is loaded, like every module, so a blob can cost a driver but never weaken the device. */
int purr_drvpart_install(const uint8_t *image, uint32_t len, char *why, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* PURR_DRVPART_PLAT_H */
