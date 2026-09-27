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

#endif
