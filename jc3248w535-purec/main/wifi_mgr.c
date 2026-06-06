#include "wifi_mgr.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

static const char *TAG = "wifi_mgr";
static bool s_ready = false;

void wifi_mgr_init(void) {
    if (s_ready) return;

    ESP_ERROR_CHECK(esp_netif_init());
    if (esp_event_loop_create_default() != ESP_OK) {
        // already created elsewhere — fine
    }
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    s_ready = true;
    ESP_LOGI(TAG, "STA interface up (no credentials configured)");
}

bool wifi_mgr_ready(void) { return s_ready; }
