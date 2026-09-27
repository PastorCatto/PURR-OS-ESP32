#ifndef PURR_BOOTPKG_H
#define PURR_BOOTPKG_H

#include "bootloader_utility.h"

/*
 * Load and run the boot package, if there is a valid one.
 * `preferred` is the app slot the bootloader would boot. Returns the slot to boot
 * (the user's choice), or -1 to boot normally.
 */
int purr_bootpkg_run(const bootloader_state_t *bs, int preferred);

#endif
