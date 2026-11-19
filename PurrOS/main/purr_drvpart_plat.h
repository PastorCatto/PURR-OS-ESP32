/*
 * purr_drvpart_plat.h - the `drivers` partition on the device (Drivers/SPEC.md, "The driver partition"; the format is components/coreos/include/purr_drvpart.h).
 *
 * The kernel plants these into /boot/drivers at boot; the loader and KittenOS, which have no file system, load straight from the slots.
 */
#ifndef PURR_DRVPART_PLAT_H
#define PURR_DRVPART_PLAT_H

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

#ifdef __cplusplus
}
#endif

#endif /* PURR_DRVPART_PLAT_H */
