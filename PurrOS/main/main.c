/*
 * PURR OS - display bring-up.
 *
 * Brings up the board and the display, draws a test screen (colour bars, edges,
 * text at two sizes) and animates a square, so size, orientation, colour order
 * and updates can be checked by eye on the real panel.
 */
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "purr_gfx.h"
#include "purr_kernel.h"

static const char *TAG = "purros";

#define BLACK   PURR_RGB565(0, 0, 0)
#define WHITE   PURR_RGB565(255, 255, 255)

static void draw_test_screen(const purr_display_v2_t *d, int w, int h)
{
    static const uint16_t bars[8] = {
        PURR_RGB565(255, 0, 0),   PURR_RGB565(0, 255, 0),   PURR_RGB565(0, 0, 255),
        PURR_RGB565(255, 255, 0), PURR_RGB565(0, 255, 255), PURR_RGB565(255, 0, 255),
        PURR_RGB565(255, 255, 255), PURR_RGB565(128, 128, 128),
    };

    d->fill(0, 0, w, h, BLACK);
    for (int i = 0; i < 8; i++) {
        d->fill(i * (w / 8), 0, w / 8, 40, bars[i]);
    }

    /* A one pixel frame shows any cropping at the edges. */
    d->fill(0, 0, w, 1, WHITE);
    d->fill(0, h - 1, w, 1, WHITE);
    d->fill(0, 0, 1, h, WHITE);
    d->fill(w - 1, 0, 1, h, WHITE);

    const char *title = "PURR OS";
    int tw = purr_gfx_text_width(title, 4);
    purr_gfx_text(d, (w - tw) / 2, 64, title, PURR_RGB565(255, 180, 0), BLACK, 4);

    const char *l1 = "T-Deck Plus  ST7789 320x240";
    purr_gfx_text(d, (w - purr_gfx_text_width(l1, 1)) / 2, 116, l1, WHITE, BLACK, 1);
    const char *l2 = "display bring-up: colours, edges, motion";
    purr_gfx_text(d, (w - purr_gfx_text_width(l2, 1)) / 2, 130, l2, PURR_RGB565(160, 160, 160), BLACK, 1);
    const char *l3 = "ABCDEFGHIJKLMNOPQRSTUVWXYZ 0123456789";
    purr_gfx_text(d, (w - purr_gfx_text_width(l3, 1)) / 2, 150, l3, WHITE, BLACK, 1);
    const char *l4 = "abcdefghijklmnopqrstuvwxyz !?#@%&*()";
    purr_gfx_text(d, (w - purr_gfx_text_width(l4, 1)) / 2, 162, l4, WHITE, BLACK, 1);

    purr_gfx_text(d, 6, 48, "TOP-LEFT", WHITE, BLACK, 1);
    purr_gfx_text(d, w - 6 - purr_gfx_text_width("BOTTOM-RIGHT", 1), h - 16, "BOTTOM-RIGHT", WHITE, BLACK, 1);
}

void app_main(void)
{
    ESP_LOGI(TAG, "PURR OS display bring-up");

    if (purr_kernel_init() != ESP_OK || purr_kernel_display() == NULL) {
        ESP_LOGE(TAG, "no display, staying up on the serial console");
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            ESP_LOGE(TAG, "display did not come up");
        }
    }

    const purr_display_v2_t *d = purr_kernel_display();
    purr_display_info_t info;
    d->get_info(&info);
    ESP_LOGI(TAG, "display: %s %dx%d, features 0x%x", info.name, info.width, info.height,
             (unsigned)d->features);

    if (d->set_brightness) {
        d->set_brightness(255);
    }
    draw_test_screen(d, info.width, info.height);
    ESP_LOGI(TAG, "test screen drawn");

    /* A square bouncing along the bottom, to show updates and motion. */
    int x = 8, dx = 4;
    const int y = info.height - 40, size = 16;
    uint16_t colour = PURR_RGB565(255, 0, 255);
    for (int frame = 0;; frame++) {
        d->fill(x, y, size, size, BLACK);
        x += dx;
        if (x <= 4 || x >= info.width - size - 4) {
            dx = -dx;
        }
        d->fill(x, y, size, size, colour);
        if (frame % 100 == 0) {
            ESP_LOGI(TAG, "alive, frame %d", frame);
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}
