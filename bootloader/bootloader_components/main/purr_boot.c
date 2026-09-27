// PURR OS second-stage bootloader.
//
// The ROM bootloader loads this from flash. It does the bare minimum:
// initialise hardware, read the partition table, pick an app slot
// (otadata-aware) and jump to it. Nothing else lives here.
#include <stdbool.h>
#include "esp_log.h"
#include "bootloader_init.h"
#include "bootloader_utility.h"
#include "purr_bootcfg.h"
#include "purr_bootpkg.h"

static const char *TAG = "purr_boot";

void __attribute__((noreturn)) call_start_cpu0(void)
{
    if (bootloader_init() != ESP_OK) {
        bootloader_reset();
    }

    ESP_LOGI(TAG, "PURR OS bootloader");

    bootloader_state_t bs = {0};
    if (!bootloader_utility_load_partition_table(&bs)) {
        ESP_LOGE(TAG, "failed to load partition table");
        bootloader_reset();
    }

    int boot_index = bootloader_utility_get_selected_boot_partition(&bs);
    if (boot_index == INVALID_INDEX) {
        ESP_LOGE(TAG, "no bootable app found");
        bootloader_reset();
    }

    /* A recovery request (the shell's "reboot recovery") skips the menu and starts KittenOS
     * once. "reboot loader" does the same for the recovery loader (the "test" slot,
     * RecoveryLoader/SPEC.md). If the target is not there, fall through to the menu as usual. */
    if (purr_bootcfg_take_flag(PURR_CFGF_FORCE_RECOVERY) && bs.factory.size != 0) {
        ESP_LOGI(TAG, "recovery requested: starting KittenOS");
        boot_index = FACTORY_INDEX;
    } else if (purr_bootcfg_take_flag(PURR_CFGF_FORCE_LOADER) && bs.test.size != 0) {
        ESP_LOGI(TAG, "internet recovery requested: starting the recovery loader");
        boot_index = TEST_APP_INDEX;
    } else {
        /* The boot package shows the menu and may pick another slot. Without one, boot normally. */
        int chosen;
        if (purr_bootpkg_run(&bs, boot_index, &chosen)) {
            boot_index = chosen;
        }
    }

    ESP_LOGI(TAG, "booting app slot %d", boot_index);
    bootloader_utility_load_boot_image(&bs, boot_index);
}
