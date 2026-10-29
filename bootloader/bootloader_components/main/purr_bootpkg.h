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
bool purr_bootpkg_run(const bootloader_state_t *bs, int preferred, int *boot_index,
                      bool (*usable)(int index));

/* The normal way in (boot package 0.3 and later; an older one gets the old menu, none gets nothing). Decides whether any screen is needed at all.
 *  fast: Fast startup is on and nothing is wrong, so no package is loaded and nothing is drawn unless trouble turns up.
 *  why:  PURR_MENU_WHY_* if the caller already knows the menu is needed (a failed last start, Settings asking), else 0.
 *  fail_slot: the slot that should have started but did not pass its check, or -1 (used for PURR_MENU_WHY_NO_VERIFY's text).
 * Returns 0 to boot *boot_index (the slot that was passed in), 1 if the menu chose *boot_index, 2 for "restart", -1 when no screen could be
 * shown (carry on without one). */
int purr_bootpkg_start(const bootloader_state_t *bs, int preferred, bool fast, uint32_t why, int fail_slot,
                       int *boot_index, bool (*usable)(int index));

/* Two short lines saying why a slot failed its check (purr_boot.c); the pointers are to static text. */
void purr_slot_failure_text(int index, const char **line1, const char **line2);

/* Whether a slot may be launched under the current secure_mode (purr_boot.c): it looks like
 * an app image and, in enforce mode, its signature trailer verifies. The menu takes this as
 * `usable` so it never offers a slot the bootloader would refuse. */
bool purr_slot_usable(int index);

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
