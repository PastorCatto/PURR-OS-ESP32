// PURR OS — JC3248W535 (ESP32-S3) — pure-C ESP-IDF entry point.
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_spiffs.h"
#include "nvs_flash.h"
#include "kitt.h"

static const char *TAG = "purr";

static void mount_spiffs(void) {
    esp_vfs_spiffs_conf_t conf = {
        .base_path              = "/spiffs",
        .partition_label        = "spiffs",
        .max_files              = 8,
        .format_if_mount_failed = true,
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SPIFFS mount failed (%s) — continuing with defaults",
                 esp_err_to_name(err));
    } else {
        size_t total = 0, used = 0;
        esp_spiffs_info("spiffs", &total, &used);
        ESP_LOGI(TAG, "SPIFFS mounted: %u/%u bytes used", (unsigned)used, (unsigned)total);
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "boot");

    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    mount_spiffs();

    if (!kitt_init("/spiffs/system/kernel/device.json")) {
        ESP_LOGE(TAG, "KITT init failed");
        kitt_emergency_text("PURR OS", "KITT INIT FAIL", "see serial log");
    }

    while (1) {
        kitt_update();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
