/*
 * purr_bootupdate.h - writing the second-stage bootloader (flash 0x0) safely (bootloader/SPEC.md section 9.2). KittenOS only.
 *
 * There is no second copy of the bootloader on this chip: the ROM starts whatever is at 0x0. So this is the most careful write in the system:
 * only sectors that differ are touched, they are written last to first, and every sector is read back. The caller (commands.c's bootloader swap)
 * has already verified the image's signature and shape, saved a backup of what is there now, and checked the power.
 */
#ifndef PURR_BOOTUPDATE_H
#define PURR_BOOTUPDATE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The first `len` bytes of the flash (the bootloader area is below the partition table at 0xA000). 0 or -1. */
int purr_bootupdate_read(uint8_t *buf, size_t len);

/* Makes flash [0, len) equal img: erases and writes only the 4 KB sectors that differ, last sector first, then reads everything back.
 * 0 when the read-back matches, -1 otherwise (the caller puts the backup back). */
int purr_bootupdate_write(const uint8_t *img, size_t len);

/* 1 if it is safe to rewrite the bootloader now: no battery reading (running from USB), or at least 50 %. Otherwise 0 and a sentence in why. */
int purr_bootupdate_power_ok(char *why, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
