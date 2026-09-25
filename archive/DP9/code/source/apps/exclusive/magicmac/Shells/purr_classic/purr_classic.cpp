#include "purr_classic.h"
#include "umac_core.h"
#include "purr_ipc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

// Display HAL for rendering
extern void mw_hal_lcd_colour_bitmap_clip(
    int16_t image_start_x, int16_t image_start_y,
    uint16_t bitmap_width, uint16_t bitmap_height,
    int16_t clip_start_x, int16_t clip_start_y,
    int16_t clip_width, int16_t clip_height,
    const uint8_t *image_data);

extern int16_t mw_hal_lcd_get_display_width(void);
extern int16_t mw_hal_lcd_get_display_height(void);

static const char *TAG = "purr_classic";

// Scale buffer for intermediate scaled framebuffer (320x240 RGB565)
static uint8_t scale_buf[320 * 240 * 2];

// Nearest-neighbor scaling: 1bpp 512x342 → RGB565 320x240
static void scale_mac_framebuffer(const uint8_t *src_1bpp, int src_w, int src_h,
                                  uint8_t *dst_rgb565, int dst_w, int dst_h)
{
    uint16_t *dst = (uint16_t *)dst_rgb565;

    // Precompute scaled colors (black and white in RGB565 format)
    uint16_t col_black = 0x0000;   // Black
    uint16_t col_white = 0xFFFF;   // White

    for (int y = 0; y < dst_h; y++) {
        // Map destination row to source row (nearest-neighbor)
        int src_y = (y * src_h) / dst_h;
        int src_stride = (src_w + 7) / 8;  // 1bpp row stride

        for (int x = 0; x < dst_w; x++) {
            // Map destination column to source column (nearest-neighbor)
            int src_x = (x * src_w) / dst_w;

            // Read 1bpp pixel from source
            uint8_t byte = src_1bpp[src_y * src_stride + (src_x / 8)];
            bool pixel_set = (byte >> (7 - (src_x & 7))) & 1;

            // Write RGB565 pixel to destination
            dst[y * dst_w + x] = pixel_set ? col_black : col_white;
        }
    }
}

// ---------------------------------------------------------------------------
// Frame callback — drv_umac calls this each time the Mac framebuffer changes.
// Mac Plus: 512x342 1bpp → Scale to display 320x240 RGB565
// ---------------------------------------------------------------------------
static void _on_frame(const uint8_t *buf, int w, int h)
{
    if (!buf || w == 0 || h == 0) return;

    int16_t disp_w = mw_hal_lcd_get_display_width();  // 320
    int16_t disp_h = mw_hal_lcd_get_display_height(); // 240

    // Scale Mac framebuffer to display size (nearest-neighbor)
    scale_mac_framebuffer(buf, w, h, scale_buf, disp_w, disp_h);

    // Blit scaled framebuffer to display
    mw_hal_lcd_colour_bitmap_clip(
        0, 0,               // Start at display origin
        disp_w, disp_h,     // Scaled framebuffer size (320x240)
        0, 0,               // No clipping needed (full display)
        disp_w, disp_h,
        scale_buf
    );
}

// ---------------------------------------------------------------------------
// Touch callback — wired from drv_touch interrupt to Mac ADB mouse
// ---------------------------------------------------------------------------
static void _on_touch(int x, int y, bool pressed)
{
    umac_mouse_event(x, y, pressed);
}

// ---------------------------------------------------------------------------
// Shell entry point
// ---------------------------------------------------------------------------
void purr_classic_start(void)
{
    ESP_LOGI(TAG, "starting MagicMac shell");

    // Init IPC bridge (must come before umac so the window is registered)
    purr_ipc_init();

    // Init emulator with ROM from SD card (/sdcard/magicmac/mac.rom)
    // Falls back to SPIFFS if SD card ROM not found
    const char *rom_path = "/sdcard/magicmac/mac.rom";
    FILE *f = fopen(rom_path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "SD card ROM not found, trying SPIFFS fallback");
        rom_path = UMAC_ROM_PATH;  // Falls back to /spiffs/mac.rom
    } else {
        fclose(f);
    }

    umac_config_t cfg = {
        .rom_path = rom_path,
        .ram_size = UMAC_RAM_SIZE,
        .verbose  = false,
    };
    if (umac_init(&cfg) != ESP_OK) {
        ESP_LOGE(TAG, "umac init failed — ROM missing from %s", rom_path);
        return;
    }

    ESP_LOGI(TAG, "umac initialized with ROM from %s", rom_path);

    // Wire frame output
    umac_set_frame_callback(_on_frame);

    // Wire touch (drv_touch callback registration — adjust to actual API)
    // touch_set_callback(_on_touch);

    // Launch IPC dispatch task on core 1 (same as system_task, low priority)
    xTaskCreatePinnedToCore(purr_ipc_task, "purr_ipc", 4096, NULL, 1, NULL, 1);

    // Launch emulator — runs on core 0, does not return
    umac_start();
}
