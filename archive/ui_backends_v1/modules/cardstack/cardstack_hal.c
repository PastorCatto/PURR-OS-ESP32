// cardstack_hal.c — LVGL ↔ catcall_display / catcall_touch / catcall_input bridge
//
// Same proven flush/touch pattern as every other LVGL UI module here. Touch
// is a plain passthrough — gt911.c already hands back final screen-pixel
// coordinates (swap/mirror/rescale lives in the driver now, not the HAL).
// Trackball is NOT registered as an LVGL indev: cardstack only needs it to
// step the card stack up/down a page at a time, which cardstack_ui.c polls
// directly via cardstack_hal_poll_trackball() rather than going through
// LVGL's pointer/encoder abstractions.

#include "cardstack.h"
#include "../../kernel/core/purr_kernel.h"
#include "../../kernel/catcalls/catcall_input.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "cardstack_hal";

#ifndef CARDSTACK_BUF_LINES
#define CARDSTACK_BUF_LINES 20
#endif
#define CARDSTACK_BUF_WIDTH 480

static lv_color_t s_buf1[CARDSTACK_BUF_WIDTH * CARDSTACK_BUF_LINES];
static lv_color_t s_buf2[CARDSTACK_BUF_WIDTH * CARDSTACK_BUF_LINES];

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

static void touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    const catcall_touch_t *t = purr_kernel_touch();
    if (t && t->is_pressed()) {
        uint16_t x = 0, y = 0;
        t->read_point(&x, &y);
        data->point.x = (lv_coord_t)x;
        data->point.y = (lv_coord_t)y;
        data->state   = LV_INDEV_STATE_PR;
    } else {
        data->state = LV_INDEV_STATE_REL;
    }
}

// ── Trackball polling (not an LVGL indev — consumed directly by cardstack_ui) ──

bool cardstack_hal_poll_trackball(int16_t *out_dy)
{
    const catcall_input_t *inp = purr_kernel_input();
    if (!inp || !inp->poll_event) return false;

    int32_t accum = 0;
    bool    any   = false;
    input_event_t ev;
    int drained = 0;
    while (drained++ < 16 && inp->poll_event(&ev)) {
        if (ev.type == INPUT_EVENT_POINTER) {
            accum += ev.delta_y;
            any = true;
        }
    }
    if (!any) return false;
    if (accum > 127)  accum = 127;
    if (accum < -127) accum = -127;
    *out_dy = (int16_t)accum;
    return true;
}

static void lv_tick_task(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5));
        lv_tick_inc(5);
    }
}

int cardstack_hal_init(void)
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

    lv_disp_draw_buf_init(&s_draw_buf, s_buf1, s_buf2, s_disp_w * CARDSTACK_BUF_LINES);

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
        ESP_LOGW(TAG, "no touch catcall — cardstack will run touchless");
    }

    BaseType_t tr = xTaskCreate(lv_tick_task, "cardstack_tick", 4096, NULL, 1, NULL);
    if (tr != pdPASS) {
        ESP_LOGE(TAG, "failed to create tick task");
        return -1;
    }

    ESP_LOGI(TAG, "HAL init complete");
    return 0;
}

uint16_t cardstack_hal_width(void)  { return s_disp_w; }
uint16_t cardstack_hal_height(void) { return s_disp_h; }
