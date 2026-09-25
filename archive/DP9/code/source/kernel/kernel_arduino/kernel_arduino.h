#pragma once
// kernel_arduino.h — shared header for Arduino-backed PURR OS kernels
//
// Kernels that include arduino-esp32 use Wire for I2C instead of the
// IDF 5.3 i2c_master API (which has known NACK/state regressions).
// All PURR catcall registration still happens via purr_kernel.h.

#ifdef __cplusplus
extern "C" {
#endif

#include "purr_kernel.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Shared NVS + SPIFFS init used by all Arduino kernels
static inline void arduino_kernel_nvs_init(void)
{
    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
}

static inline void arduino_kernel_spiffs_init(const char *tag)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path              = "/flash",
        .partition_label        = NULL,
        .max_files              = 12,
        .format_if_mount_failed = true,
    };
    esp_err_t r = esp_vfs_spiffs_register(&conf);
    if (r != ESP_OK) {
        ESP_LOGW(tag, "SPIFFS mount failed (%s)", esp_err_to_name(r));
    } else {
        size_t total = 0, used = 0;
        esp_spiffs_info(NULL, &total, &used);
        ESP_LOGI(tag, "flash VFS: %u KB / %u KB",
                 (unsigned)(used / 1024), (unsigned)(total / 1024));
    }
}

#ifdef __cplusplus
}
#endif
