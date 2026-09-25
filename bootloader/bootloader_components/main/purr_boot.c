// PURR OS second-stage bootloader.
//
// The ROM bootloader loads this from flash. It does the bare minimum:
// initialise hardware, read the partition table, pick an app slot
// (otadata-aware) and jump to it. Nothing else lives here.
#include <stdbool.h>
#include "esp_log.h"
#include "bootloader_init.h"
#include "bootloader_utility.h"

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

    ESP_LOGI(TAG, "booting app slot %d", boot_index);
    bootloader_utility_load_boot_image(&bs, boot_index);
}
