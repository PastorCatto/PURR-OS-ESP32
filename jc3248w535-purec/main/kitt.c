#include "kitt.h"
#include "device_config.h"
#include "display_axs.h"
#include "wifi_mgr.h"
#include "power_mgr.h"
#include "touch_axs.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "kitt";

static device_config_t cfg;
static bool     kitt_ready = false;
static char     os_name_buf[16] = "PUR OS";   // upgraded to "PURR OS" if LoRa present
static uint32_t last_heartbeat_ms = 0;
static uint8_t  log_row = 2;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static uint16_t to565(uint32_t c) {
    uint8_t r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static bool display_is_axs(void) {
    return strcmp(cfg.display, "axs15231b") == 0 ||
           strcmp(cfg.display, "st7796")    == 0;   // accept legacy key too
}

// ── Display text passthrough ─────────────────────────────────────────────────
void kitt_text_print(uint8_t row, const char* text) {
    if (display_is_axs()) display_axs_text(row, text);
}
void kitt_text_clear(void) {
    if (display_is_axs()) display_axs_clear();
}
void kitt_text_set_color(uint32_t fg_hex, uint32_t bg_hex) {
    if (display_is_axs()) display_axs_set_text_colors(to565(fg_hex), to565(bg_hex));
}

void kitt_show_boot_splash(void) {
    kitt_text_clear();
    kitt_text_print(0, "PURR OS");
    kitt_text_print(1, cfg.device);
}

void kitt_emergency_text(const char* l1, const char* l2, const char* l3) {
    ESP_LOGE(TAG, "EMERGENCY: %s | %s | %s", l1, l2, l3);
    kitt_text_clear();
    kitt_text_print(0, l1);
    kitt_text_print(1, l2);
    kitt_text_print(2, l3);
}

void kitt_log(const char* tag, const char* message) {
    ESP_LOGI(tag, "%s", message);
    if (cfg.verbose_boot && cfg.display_w > 128) {
        char buf[48];
        snprintf(buf, sizeof(buf), "[%s] %s", tag, message);
        kitt_text_print(log_row % 6 + 2, buf);
        log_row++;
    }
}

// ── NVS helpers ──────────────────────────────────────────────────────────────
static void nvs_put_u8(const char* ns, const char* key, uint8_t v) {
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, key, v);
        nvs_commit(h);
        nvs_close(h);
    }
}
static void nvs_put_u32(const char* ns, const char* key, uint32_t v) {
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, key, v);
        nvs_commit(h);
        nvs_close(h);
    }
}

// ── Boot ─────────────────────────────────────────────────────────────────────
bool kitt_init(const char* device_json_path) {
    ESP_LOGI(TAG, "PURR OS v%s  KITT v%s  boot start", PURR_OS_VERSION, KITT_VERSION);

    // device.json (always succeeds: falls back to compiled defaults)
    device_config_load(device_json_path, &cfg);

    // Display
    if (display_is_axs()) {
        display_axs_init();
        display_axs_set_text_colors(to565(0xFFFFFF), to565(0x000000));
        kitt_log("KITT", "display AXS15231B OK");
    } else {
        ESP_LOGE(TAG, "ERR 0x02 unknown display '%s'", cfg.display);
        return false;
    }

    // Boot splash / verbose
    if (cfg.verbose_boot) {
        kitt_text_clear();
        kitt_text_print(0, "PURR OS");
        kitt_text_print(1, cfg.device);
        kitt_log("KITT", "boot verbose mode");
    } else {
        kitt_show_boot_splash();
    }

    // WiFi
    if (cfg.wifi) {
        wifi_mgr_init();
        kitt_log("KITT", "wifi OK");
    }

    // Power
    power_mgr_init(cfg.cpu_max_mhz);
    last_heartbeat_ms = now_ms();

    // Touch
    if (strcmp(cfg.touch, "axs15231b") == 0 || strcmp(cfg.touch, "gt911") == 0) {
        touch_axs_init();
        kitt_log("KITT", "touch OK");
    }

    // Boot flags
    nvs_put_u8("kitt_boot", "kitt_ready", 1);
    nvs_put_u8("purr_bl",   "boot_tries", 0);
    nvs_put_u32("kitt_hb",  "kitt_hb", last_heartbeat_ms);

    strlcpy(os_name_buf, "PURR OS", sizeof(os_name_buf));
    kitt_ready = true;
    kitt_log("KITT", "ready");
    return true;
}

void kitt_update(void) {
    uint32_t now = now_ms();
    if (now - last_heartbeat_ms >= 500) {
        nvs_put_u32("kitt_hb", "kitt_hb", now);
        last_heartbeat_ms = now;
    }

    touch_event_t ev;
    if (touch_axs_get_event(&ev) && ev.pressed) {
        ESP_LOGI(TAG, "touch @ %d,%d", ev.x, ev.y);
    }
}

bool        kitt_is_ready(void)   { return kitt_ready; }
const char* kitt_device_name(void){ return cfg.device; }
const char* kitt_os_name(void)    { return os_name_buf; }
