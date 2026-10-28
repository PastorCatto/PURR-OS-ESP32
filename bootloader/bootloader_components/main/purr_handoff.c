// The bootloader-to-OS handoff (bootloader/SPEC.md section 7).
//
// A purr_handoff_t sits in the custom area of IDF's RTC FAST retain block. That block has
// a fixed address both builds agree on, survives a soft reset, and does not survive power
// loss -- so a cold boot reads as "no handoff", the same as a stock bootloader.
#include <string.h>

#include "bootloader_common.h"
#include "sdkconfig.h"

#include "purr_abi.h"
#include "purr_cfg.h"
#include "purr_handoff.h"

#if !CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC
#error "the handoff needs CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC (see bootloader/sdkconfig.defaults)"
#endif
#if CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC_IN_CRC
#error "the handoff relies on the custom RTC area being outside IDF's CRC"
#endif
_Static_assert(CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC_SIZE >= sizeof(purr_handoff_t),
               "CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC_SIZE is smaller than purr_handoff_t");

void purr_handoff_write(uint8_t boot_state, uint8_t key_id_used, uint8_t flags_used,
                        uint8_t secure_state, uint16_t recovery_reasons)
{
    purr_handoff_t h;
    purr_handoff_make(&h, boot_state, key_id_used, flags_used, PURR_BOOTLOADER_VERSION, secure_state);
    purr_handoff_set_reasons(&h, recovery_reasons);

    rtc_retain_mem_t *rtc = bootloader_common_get_rtc_retain_mem();

    // IDF's retain block carries its own CRC, and bootloader_utility_load_boot_image() ends by
    // wiping the WHOLE block (custom area included) if that CRC is not valid -- which it is not
    // on a cold power-on, when the memory holds garbage. Writing the handoff first and letting
    // IDF find the block invalid afterwards would erase it on exactly the boots it matters most
    // for. So make the block valid first. The custom area is not part of the CRC
    // (CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC_IN_CRC stays off), so writing it afterwards needs no
    // reseal, and IDF's own later update leaves it alone. A valid block always has a nonzero
    // reboot counter, so a zero counter means invalid (or never counted): start it clean.
    if (bootloader_common_get_rtc_retain_mem_reboot_counter() == 0) {
        bootloader_common_reset_rtc_retain_mem();
        bootloader_common_update_rtc_retain_mem(NULL, false);
    }
    memcpy(rtc->custom, &h, sizeof(h));
}
