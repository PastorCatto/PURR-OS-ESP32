#include "umac_core.h"
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// umac + musashi headers (vendored under this component)
#include "umac/umac.h"
#include "musashi/m68k.h"

static const char *TAG = "drv_umac";

static uint8_t          *s_rom  = NULL;
static uint8_t          *s_ram  = NULL;
static umac_frame_cb_t   s_frame_cb = NULL;

// ---------------------------------------------------------------------------
// Frame output — umac calls this when the video framebuffer changes
// ---------------------------------------------------------------------------
static void _umac_video_update(const uint8_t *buf, int w, int h)
{
    if (s_frame_cb) s_frame_cb(buf, w, h);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

esp_err_t umac_init(const umac_config_t *cfg)
{
    ESP_LOGI(TAG, "init — ROM: %s  RAM: %lu KB", cfg->rom_path, cfg->ram_size / 1024);

    // Load ROM from SPIFFS
    FILE *f = fopen(cfg->rom_path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "ROM not found at %s", cfg->rom_path);
        return ESP_ERR_NOT_FOUND;
    }
    s_rom = heap_caps_malloc(UMAC_ROM_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_rom) s_rom = malloc(UMAC_ROM_SIZE);
    if (!s_rom) { fclose(f); return ESP_ERR_NO_MEM; }
    fread(s_rom, 1, UMAC_ROM_SIZE, f);
    fclose(f);

    // Allocate emulated RAM (prefer PSRAM)
    // System 3 fits in 512KB internal SRAM; only fall back to PSRAM if needed
    s_ram = malloc(cfg->ram_size);
    if (!s_ram) s_ram = heap_caps_malloc(cfg->ram_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_ram) { free(s_rom); return ESP_ERR_NO_MEM; }
    memset(s_ram, 0, cfg->ram_size);

    umac_init_core(s_rom, UMAC_ROM_SIZE, s_ram, cfg->ram_size, _umac_video_update);

    ESP_LOGI(TAG, "ready");
    return ESP_OK;
}

static void _umac_task(void *arg)
{
    (void)arg;
    umac_run();      // runs indefinitely, calls _umac_video_update each frame
    vTaskDelete(NULL);
}

void umac_start(void)
{
    xTaskCreatePinnedToCore(_umac_task, "umac", 8192, NULL, 2, NULL, 0);
}

void umac_stop(void)
{
    // TODO: signal umac_run() to exit, free buffers
}

void umac_set_frame_callback(umac_frame_cb_t cb)
{
    s_frame_cb = cb;
}

void umac_mouse_event(int x, int y, bool pressed)
{
    // Scale CYD touch coords to Mac Plus 512x342
    int mx = (x * UMAC_DISPLAY_W) / 240;
    int my = (y * UMAC_DISPLAY_H) / 320;
    umac_mouse(mx, my, pressed ? 1 : 0);
}
