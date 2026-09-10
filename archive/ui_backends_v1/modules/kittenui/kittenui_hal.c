// kittenui_hal.c — LVGL ↔ catcall_display / catcall_touch bridge
//
// This is the only file that touches LVGL internals. Everything above this
// (themes, widgets, app code) goes through LVGL APIs. Everything below goes
// through catcalls — no hardware access here.

#include "kittenui.h"
#include "../../kernel/core/purr_kernel.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "kittenui_hal";

// ── LVGL draw buffer ──────────────────────────────────────────────────────────
// Two buffers of 1/10th screen each → partial refresh, no full-frame RAM needed.
// Adjust KITTENUI_BUF_LINES in menuconfig or here for your RAM budget.

#ifndef KITTENUI_BUF_LINES
#define KITTENUI_BUF_LINES  20
#endif

#define KITTENUI_BUF_WIDTH  480  // max display width we support

static lv_color_t s_buf1[KITTENUI_BUF_WIDTH * KITTENUI_BUF_LINES];
static lv_color_t s_buf2[KITTENUI_BUF_WIDTH * KITTENUI_BUF_LINES];

static lv_disp_draw_buf_t s_draw_buf;
static lv_disp_drv_t      s_disp_drv;
static lv_indev_drv_t     s_touch_drv;

static uint16_t s_disp_w = 320;
static uint16_t s_disp_h = 240;

// ── Display flush callback ────────────────────────────────────────────────────

static int s_flush_count = 0;
static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p)
{
    if (++s_flush_count <= 2) {
        ESP_LOGI(TAG, "flush #%d (%d,%d)-(%d,%d)", s_flush_count,
                 area->x1, area->y1, area->x2, area->y2);
    }
    const catcall_display_t *d = purr_kernel_display();
    if (d && d->push_pixels) {
        int32_t w = area->x2 - area->x1 + 1;
        int32_t h = area->y2 - area->y1 + 1;
        d->push_pixels(area->x1, area->y1, (int)w, (int)h, (uint16_t *)color_p);
    }
    lv_disp_flush_ready(drv);
}

// ── Touch read callback ───────────────────────────────────────────────────────

static void touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    const catcall_touch_t *t = purr_kernel_touch();
    if (t && t->is_pressed()) {
        uint16_t x = 0, y = 0;
        t->read_point(&x, &y);
#ifdef CONFIG_PURR_TOUCH_FLIP_X
        x = (uint16_t)((s_disp_w > x) ? (s_disp_w - 1 - x) : 0);
#endif
#ifdef CONFIG_PURR_TOUCH_FLIP_Y
        y = (uint16_t)((s_disp_h > y) ? (s_disp_h - 1 - y) : 0);
#endif
        data->point.x = (lv_coord_t)x;
        data->point.y = (lv_coord_t)y;
        data->state   = LV_INDEV_STATE_PR;
    } else {
        data->state = LV_INDEV_STATE_REL;
    }
}

// ── LVGL tick (called from timer task) ────────────────────────────────────────

static void lv_tick_task(void *arg)
{
    (void)arg;
    ESP_LOGI("kittenui_hal", "lv_tick_task running");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5));
        lv_tick_inc(5);
    }
}

// ── Init ──────────────────────────────────────────────────────────────────────

int kittenui_hal_init(void)
{
    const catcall_display_t *disp = purr_kernel_display();
    if (!disp) {
        ESP_LOGE(TAG, "no display catcall");
        return -1;
    }

    // Get display dimensions
    display_info_t info = {0};
    if (disp->get_info) {
        disp->get_info(&info);
        s_disp_w = info.width  ? info.width  : 320;
        s_disp_h = info.height ? info.height : 240;
    }
    ESP_LOGI(TAG, "display: %ux%u", s_disp_w, s_disp_h);

    lv_init();

    // Draw buffer — two buffers for DMA double-buffering
    lv_disp_draw_buf_init(&s_draw_buf, s_buf1, s_buf2,
                          s_disp_w * KITTENUI_BUF_LINES);

    // Display driver
    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res    = (lv_coord_t)s_disp_w;
    s_disp_drv.ver_res    = (lv_coord_t)s_disp_h;
    s_disp_drv.flush_cb   = flush_cb;
    s_disp_drv.draw_buf   = &s_draw_buf;
    s_disp_drv.full_refresh = 0;
    lv_disp_drv_register(&s_disp_drv);

    // Touch input driver (optional)
    const catcall_touch_t *touch = purr_kernel_touch();
    if (touch) {
        lv_indev_drv_init(&s_touch_drv);
        s_touch_drv.type    = LV_INDEV_TYPE_POINTER;
        s_touch_drv.read_cb = touch_read_cb;
        lv_indev_drv_register(&s_touch_drv);
        ESP_LOGI(TAG, "touch input registered");
    } else {
        ESP_LOGW(TAG, "no touch catcall — KittenUI will run touchless");
    }

    // LVGL tick task — low priority, wakes every 5ms to advance LVGL clock
    ESP_LOGI(TAG, "creating tick task");
    BaseType_t tr = xTaskCreate(lv_tick_task, "lv_tick", 4096, NULL, 1, NULL);
    ESP_LOGI(TAG, "tick task ret=%d heap=%u", (int)tr, (unsigned)esp_get_free_heap_size());

    ESP_LOGI(TAG, "HAL init complete");
    return 0;
}

uint16_t kittenui_hal_width(void)  { return s_disp_w; }
uint16_t kittenui_hal_height(void) { return s_disp_h; }
