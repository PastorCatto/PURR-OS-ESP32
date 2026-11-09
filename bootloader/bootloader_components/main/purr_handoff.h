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
#define PURR_BOOTLOADER_VERSION 0x00050000u   /* 0.5.0, packed like PURR_VERSION (major<<24 | minor<<16 | patch). 0.5.0: the key rotation (plan3.md, phase 4): the new boot, developer and system public keys (ids 3, 4, 5) are compiled in beside the old ones (ids 1, 2), so a trust block signed by the new boot key and images signed by the new keys verify with no key update; the old keys are still accepted until 0.6.0 revokes them. 0.4.0: the 0.3.1 watchdog fix, renumbered as a minor release by the owner. 0.3.1: the boot menu no longer trips the RTC watchdog (it fed nothing, so a menu left for about 5 s restarted the device instead of falling through after 20 s). 0.3.0: the quiet start (boot package 0.3). Earlier builds report 0x00000200 for 0.2.0, which reads as older. */

#endif
