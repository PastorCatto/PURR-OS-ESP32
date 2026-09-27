// usb_msc.c — see usb_msc.h for the design rationale.

#include "usb_msc.h"
#include "tinyusb.h"
#include "tusb_console.h"
#include "tusb_msc_storage.h"
#include "esp_vfs_fat.h"
#include "esp_log.h"

static const char *TAG = "usb_msc";

esp_err_t usb_msc_init(void)
{
    // NULL/zeroed descriptor fields fall back to CONFIG_TINYUSB_DESC_*
    // Kconfig values — the default composite descriptor TinyUSB builds from
    // CONFIG_TINYUSB_CDC_ENABLED + CONFIG_TINYUSB_MSC_ENABLED already has
    // exactly the CDC+MSC interfaces this needs, no custom descriptor
    // required (see tinyusb.h's tinyusb_config_t doc comment).
    tinyusb_config_t tusb_cfg = {0};
    esp_err_t ret = tinyusb_driver_install(&tusb_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_driver_install failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Interface 0 — the only CDC channel (CONFIG_TINYUSB_CDC_COUNT=1).
    // Replaces the USB-Serial-JTAG console this board used before: that's a
    // separate internal peripheral from the native USB-OTG controller
    // TinyUSB drives, and they can't both be the active console on the same
    // physical port at once.
    ret = esp_tusb_init_console(0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_tusb_init_console failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "composite CDC+MSC installed (MSC has no backing store yet)");
    return ESP_OK;
}

esp_err_t usb_msc_share_sd(const char *base_path, sdmmc_card_t *card)
{
    if (!base_path || !card) return ESP_ERR_INVALID_ARG;

    // Release our own FATFS mount FIRST — tinyusb_msc_storage_init_sdmmc()
    // is only ever called here, after this succeeds, specifically so there
    // is never a window where both the OS's own VFS mount and TinyUSB's MSC
    // backing store are registered against the same card at once. SD cards
    // don't support the OS and a USB host both touching the same blocks
    // without corruption risk — registering only at this point, instead of
    // at boot, is what guarantees that never happens.
    esp_err_t ret = esp_vfs_fat_sdcard_unmount(base_path, card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "unmount %s failed: %s — not sharing over USB", base_path, esp_err_to_name(ret));
        return ret;
    }

    tinyusb_msc_sdmmc_config_t msc_cfg = {
        .card = card,
        .callback_mount_changed = NULL,
        .callback_premount_changed = NULL,
        .mount_config = VFS_FAT_MOUNT_DEFAULT_CONFIG(),
    };
    // Deliberately never followed by tinyusb_msc_storage_mount() — leaving
    // the card unclaimed by the firmware is what exposes it to the USB
    // host (see tusb_msc_storage.h's doc comment on that function).
    ret = tinyusb_msc_storage_init_sdmmc(&msc_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_msc_storage_init_sdmmc failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "%s now shared over USB MSC", base_path);
    return ESP_OK;
}
