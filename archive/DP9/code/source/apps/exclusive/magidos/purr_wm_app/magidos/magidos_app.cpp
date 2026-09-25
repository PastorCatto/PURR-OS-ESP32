#include "magidos_app.h"
#include "magidos_filepicker.h"
#include "magidos_cga.h"
#include "drv_8086.h"
#include "purr_dos_ipc.h"
#include "purr_wm.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "magidos_app";

// MiniWin window handle
static purr_wm_window_t *s_win = NULL;

// RGB565 pixel buffer for the window's draw area
// CYD window size: we use 240x200 to keep the CGA aspect ratio reasonable
#define WIN_W   240
#define WIN_H   200
static uint16_t s_framebuf[WIN_W * WIN_H];

// ---------------------------------------------------------------------------
// CGA frame callback — called by drv_8086 when video RAM changes
// ---------------------------------------------------------------------------
static void _on_frame(const uint8_t *vram, int cols, int rows)
{
    magidos_cga_render(vram, cols, rows, s_framebuf, WIN_W, WIN_H);
    purr_wm_window_blit(s_win, s_framebuf, WIN_W, WIN_H);
}

// ---------------------------------------------------------------------------
// Touch callback from WM — forward to DOS keyboard buffer as cursor keys
// (touch in text mode = tap a line = move cursor, Enter on that line)
// ---------------------------------------------------------------------------
static void _on_touch(purr_wm_window_t *win, int x, int y, bool pressed)
{
    (void)win;
    if (!pressed) return;
    // Map tap Y coordinate to a text row, send cursor-down/up then Enter
    // Simple heuristic: tap below midpoint = Down, above = Up
    if (y > WIN_H / 2)
        drv_8086_key(0x00, 0x50);   // cursor down scancode
    else
        drv_8086_key(0x00, 0x48);   // cursor up scancode
}

// ---------------------------------------------------------------------------
// Main app task — shows file picker, loads selected exec, runs emulator loop
// ---------------------------------------------------------------------------
static void _magidos_task(void *arg)
{
    (void)arg;

    // Init emulator
    drv_8086_config_t cfg = { .bios_path = NULL, .mem_size = 0 };
    if (drv_8086_init(&cfg) != ESP_OK) {
        ESP_LOGE(TAG, "8086 init failed");
        purr_wm_window_close(s_win);
        vTaskDelete(NULL);
        return;
    }
    drv_8086_set_frame_callback(_on_frame);

    // Show file picker until the user picks something (or SD has no files)
    const char *path = magidos_filepicker_show();
    if (!path) {
        // Draw "No DOS programs on SD card" message and close after 3s
        purr_wm_window_draw_text(s_win, 4, 80, "No .COM/.EXE on SD card");
        vTaskDelay(pdMS_TO_TICKS(3000));
        purr_wm_window_close(s_win);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "loading %s", path);
    if (drv_8086_load_file(path) != ESP_OK) {
        purr_wm_window_draw_text(s_win, 4, 80, "Load failed");
        vTaskDelay(pdMS_TO_TICKS(2000));
        purr_wm_window_close(s_win);
        vTaskDelete(NULL);
        return;
    }

    // Emulator run loop — step until program exits
    while (drv_8086_step()) {
        vTaskDelay(pdMS_TO_TICKS(1));  // yield to WM and other tasks
    }

    ESP_LOGI(TAG, "DOS program exited");
    purr_wm_window_draw_text(s_win, 4, 90, "Program exited. Tap to close.");
    // Wait for a tap to dismiss
    vTaskDelay(pdMS_TO_TICKS(5000));
    purr_wm_window_close(s_win);
    s_win = NULL;
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// WM app launch callback — called when the user taps the MagiDOS icon
// ---------------------------------------------------------------------------
static void _magidos_launch(void)
{
    if (s_win) {
        purr_wm_window_focus(s_win);
        return;
    }

    purr_wm_window_cfg_t wcfg = {
        .title  = "MagiDOS",
        .x      = 0,
        .y      = 20,    // below WM title bar
        .width  = WIN_W,
        .height = WIN_H,
        .on_touch = _on_touch,
    };
    s_win = purr_wm_window_open(&wcfg);
    if (!s_win) {
        ESP_LOGE(TAG, "could not open WM window");
        return;
    }

    xTaskCreatePinnedToCore(_magidos_task, "magidos", 8192, NULL, 2, NULL, 0);
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------
void magidos_register(void)
{
    purr_wm_app_t app = {
        .name    = "MagiDOS",
        .icon    = NULL,   // TODO: 16x16 icon bitmap
        .launch  = _magidos_launch,
    };
    purr_wm_register_app(&app);
    ESP_LOGI(TAG, "registered with purr_wm");
}
