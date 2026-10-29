// Leaves the handoff block for the OS (bootloader/SPEC.md section 7).
#ifndef PURR_HANDOFF_WRITE_H
#define PURR_HANDOFF_WRITE_H

#include <stdint.h>

// Writes a sealed purr_handoff_t into the RTC FAST memory the IDF reserves for custom use
// (CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC). Call it once, right before jumping to the image.
void purr_handoff_write(uint8_t boot_state, uint8_t key_id_used, uint8_t flags_used,
                        uint8_t secure_state, uint16_t recovery_reasons);

// Packed bootloader version, carried in the handoff. Bump when the bootloader's behaviour
// changes in a way the OS could care about.
#define PURR_BOOTLOADER_VERSION 0x00030000u   /* 0.3.0, packed like PURR_VERSION (major<<24 | minor<<16 | patch). 0.3.0: the quiet start (boot package 0.3). Earlier builds report 0x00000200 for 0.2.0, which reads as older. */

#endif
