// lvgldebug_hal.c — LVGL <-> catcall_display / catcall_touch bridge
//
// Same display flush pattern as meow420_hal.c. Touch uses the same
// swap+mirror+scale derived from LilyGo's own reference driver for this
// exact board (SensorLib's TouchDrvGT911: setSwapXY(true), setMirrorXY
// (false, true)) — this module's whole purpose is to verify that mapping
// visually, so it deliberately reuses the current best-guess transform
// rather than introducing a different one.

#include "lvgldebug.h"
#include "../../kernel/core/purr_kernel.h"
#include "gt911.h"
#include "lvgl.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "lvgldebug_hal";

#ifndef LVD_BUF_LINES
#define LVD_BUF_LINES 20
#endif
#define LVD_BUF_WIDTH 480

static lv_color_t s_buf1[LVD_BUF_WIDTH * LVD_BUF_LINES];
static lv_color_t s_buf2[LVD_BUF_WIDTH * LVD_BUF_LINES];

static lv_disp_draw_buf_t s_draw_buf;
static lv_disp_drv_t      s_disp_drv;
static lv_indev_drv_t     s_touch_drv;

static uint16_t s_disp_w = 320;
static uint16_t s_disp_h = 240;

static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p)
{
    const catcall_display_t *d = purr_kernel_display();
    if (d && d->push_pixels) {
        int32_t w = area->x2 - area->x1 + 1;
        int32_t h = area->y2 - area->y1 + 1;
        d->push_pixels(area->x1, area->y1, (int)w, (int)h, (uint16_t *)color_p);
    }
    lv_disp_flush_ready(drv);
}

// ── Touch: driver now hands back display-pixel coordinates directly ──────────
// gt911_read_point() (source/drivers/touch/gt911/gt911.c) does the raw-to-
// pixel scaling itself now, hard-clamped to the panel size — no transform
// needed here anymore, just pass through.

static uint16_t s_last_raw_x, s_last_raw_y;
static int32_t  s_last_mapped_x, s_last_mapped_y;
static bool     s_last_pressed;

// True-raw history ring — straight off the chip, before any swap/mirror/
// scale/outlier-rejection in gt911.c. Every single sample goes in here and
// to serial unconditionally, so we can see exactly what the hardware is
// doing rather than what our own transform thinks it should look like.
static uint16_t s_hist_x[LVD_HISTORY_LEN];
static uint16_t s_hist_y[LVD_HISTORY_LEN];
static uint8_t  s_hist_head;
static uint8_t  s_hist_count;

static void touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    const catcall_touch_t *t = purr_kernel_touch();
    if (t && t->is_pressed()) {
        uint16_t rx = 0, ry = 0;
        t->read_point(&rx, &ry);

        uint16_t true_raw_x = 0, true_raw_y = 0;
        gt911_get_last_true_raw(&true_raw_x, &true_raw_y);
        ESP_LOGI(TAG, "DBG true_raw=(%u,%u) driver_out=(%u,%u)",
                 true_raw_x, true_raw_y, rx, ry);

        s_hist_x[s_hist_head] = true_raw_x;
        s_hist_y[s_hist_head] = true_raw_y;
        s_hist_head = (uint8_t)((s_hist_head + 1) % LVD_HISTORY_LEN);
        if (s_hist_count < LVD_HISTORY_LEN) s_hist_count++;

        int32_t px = (int32_t)rx;
        int32_t py = (int32_t)ry;

        if (px < 0) px = 0;
        if (py < 0) py = 0;
        if (px >= s_disp_w) px = s_disp_w - 1;
        if (py >= s_disp_h) py = s_disp_h - 1;

        s_last_raw_x = rx;
        s_last_raw_y = ry;
        s_last_mapped_x = px;
        s_last_mapped_y = py;
        s_last_pressed = true;

        data->point.x = (lv_coord_t)px;
        data->point.y = (lv_coord_t)py;
        data->state   = LV_INDEV_STATE_PR;
    } else {
        s_last_pressed = false;
        data->state = LV_INDEV_STATE_REL;
    }
}

void lvgldebug_hal_get_touch_debug(uint16_t *raw_x, uint16_t *raw_y,
                                   int32_t *mapped_x, int32_t *mapped_y,
                                   bool *pressed)
{
    if (raw_x) *raw_x = s_last_raw_x;
    if (raw_y) *raw_y = s_last_raw_y;
    if (mapped_x) *mapped_x = s_last_mapped_x;
    if (mapped_y) *mapped_y = s_last_mapped_y;
    if (pressed) *pressed = s_last_pressed;
}

// Returns up to LVD_HISTORY_LEN entries, oldest first. out_x/out_y must each
// have room for LVD_HISTORY_LEN entries.
uint8_t lvgldebug_hal_get_touch_history(uint16_t *out_x, uint16_t *out_y)
{
    uint8_t n = s_hist_count;
    uint8_t start = (uint8_t)((s_hist_head + LVD_HISTORY_LEN - n) % LVD_HISTORY_LEN);
    for (uint8_t i = 0; i < n; i++) {
        uint8_t idx = (uint8_t)((start + i) % LVD_HISTORY_LEN);
        out_x[i] = s_hist_x[idx];
        out_y[i] = s_hist_y[idx];
    }
    return n;
}

static void lv_tick_task(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5));
        lv_tick_inc(5);
    }
}

int lvgldebug_hal_init(void)
{
    const catcall_display_t *disp = purr_kernel_display();
    if (!disp) {
        ESP_LOGE(TAG, "no display catcall");
        return -1;
    }

    display_info_t info = {0};
    if (disp->get_info) {
        disp->get_info(&info);
        s_disp_w = info.width  ? info.width  : 320;
        s_disp_h = info.height ? info.height : 240;
    }
    ESP_LOGI(TAG, "display: %ux%u", s_disp_w, s_disp_h);

    lv_init();

    lv_disp_draw_buf_init(&s_draw_buf, s_buf1, s_buf2, s_disp_w * LVD_BUF_LINES);

    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res      = (lv_coord_t)s_disp_w;
    s_disp_drv.ver_res      = (lv_coord_t)s_disp_h;
    s_disp_drv.flush_cb     = flush_cb;
    s_disp_drv.draw_buf     = &s_draw_buf;
    s_disp_drv.full_refresh = 0;
    lv_disp_drv_register(&s_disp_drv);

    const catcall_touch_t *touch = purr_kernel_touch();
    if (touch) {
        lv_indev_drv_init(&s_touch_drv);
        s_touch_drv.type    = LV_INDEV_TYPE_POINTER;
        s_touch_drv.read_cb = touch_read_cb;
        lv_indev_drv_register(&s_touch_drv);
        ESP_LOGI(TAG, "touch input registered");
    } else {
        ESP_LOGW(TAG, "no touch catcall — lvgldebug will run touchless");
    }

    BaseType_t tr = xTaskCreate(lv_tick_task, "lvd_tick", 4096, NULL, 1, NULL);
    if (tr != pdPASS) {
        ESP_LOGE(TAG, "failed to create tick task");
        return -1;
    }

    ESP_LOGI(TAG, "HAL init complete");
    return 0;
}

uint16_t lvgldebug_hal_width(void)  { return s_disp_w; }
uint16_t lvgldebug_hal_height(void) { return s_disp_h; }
