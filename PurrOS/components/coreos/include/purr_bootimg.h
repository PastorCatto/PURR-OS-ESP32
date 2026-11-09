/*
 * purr_bootimg.h - is this blob a second-stage bootloader the chip's ROM will start? (bootloader/SPEC.md section 9.2)
 *
 * Plain C, no flash and no IDF: KittenOS runs it on the bytes of a bootloader before it writes them over the one that starts the device, and
 * again on the backup before it puts it back. A wrong image written to flash 0x0 leaves a board that will not start without a cable, so this
 * is deliberately strict.
 */
#ifndef PURR_BOOTIMG_H
#define PURR_BOOTIMG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PURR_BOOTIMG_MAX   0xA000u      /* flash 0x0 up to the partition table at 0xA000 */
#define PURR_BOOTIMG_CHIP_ESP32S3 0x0009u

/* NULL if `img` looks like a bootloader for `chip_id`; otherwise a short reason. Checks: the size fits below the partition table, the ESP
 * image magic (0xE9), a believable segment count, the chip id in the header, and the SHA-256 the image carries at its end (when its header says
 * one is appended): every byte before the last 32 must hash to them. A bootloader with a wrong digest is refused by the ROM. */
const char *purr_bootimg_check(const uint8_t *img, size_t len, uint16_t chip_id);

#ifdef __cplusplus
}
#endif

#endif
