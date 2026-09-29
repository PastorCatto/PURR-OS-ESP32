#ifndef PURR_BOOTPKG_H
#define PURR_BOOTPKG_H

#include "bootloader_utility.h"

#include <stdbool.h>

/*
 * Load and run the boot package, if there is a valid one.
 * `preferred` is the slot the bootloader would boot (an ota index, or FACTORY_INDEX for
 * KittenOS). Returns true and sets *boot_index if the menu chose a slot, or false to boot
 * normally.
 */
bool purr_bootpkg_run(const bootloader_state_t *bs, int preferred, int *boot_index);

/* Find a custom raw data partition (subtype 0x40) by name in the partition table. */
bool purr_find_partition(const char *name, uint32_t *offset, uint32_t *size);

/* Shallow "does this look like a real app image" check (first word is an ESP app image
 * magic byte, 0xE9) -- the same check the boot menu already uses to gray out a dead slot.
 * Shared with purr_boot.c for the kernel -> KittenOS -> recovery-loader fallback chain
 * (PurrOS/SPEC.md's design). A real PURR-signature check before boot is designed
 * (bootloader/SPEC.md section 6) but not yet built; this is the mechanism that already
 * exists and is already proven on hardware, reused for the automatic decision instead of
 * just labeling menu entries. */
uint8_t purr_looks_bootable(const esp_partition_pos_t *pos);

#endif
