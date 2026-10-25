#include "purr_lvgl.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "purr_display.h"
#include "purr_kernel.h"
#include "purr_ui_service.h"

static const char *TAG = "lvgl";

#define LVGL_TICK_PERIOD_MS 5

/* purr_display_v2_t's blit()/fill() already do their own panel-specific chunking
 * internally (st7789.c's d_blit() loops over its own DMA buffer size) -- the flush
 * callback below can hand it any area in one call, no chunking of its own needed. */
static const purr_display_v2_t *s_display;
static lv_disp_draw_buf_t s_draw_buf;
static lv_disp_drv_t s_disp_drv;
static lv_indev_drv_t s_indev_drv;
static volatile int s_critical_error;

static void tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p)
{
    (void)drv;
    int w = area->x2 - area->x1 + 1;
    int h = area->y2 - area->y1 + 1;
    /* lv_color_t at CONFIG_LV_COLOR_DEPTH=16 is laid out exactly like this project's own
     * host-endian RGB565 (purr_display.h's own convention) -- no conversion needed. */
    int e = s_display->blit(area->x1, area->y1, w, h, (const uint16_t *)color_p);
    if (e != 0) {
        /* This callback used to ignore blit()'s result, so a failing flush left no trace at all
         * (found chasing a white screen, 2026-10-25 -- which turned out to be elsewhere, but the
         * silence cost a lot of time). Still ends in flush_ready so LVGL does not stall. */
        ESP_LOGW(TAG, "flush (%d,%d %dx%d): blit failed (%d)", area->x1, area->y1, w, h, e);
    }
    lv_disp_flush_ready(drv);
}

/* The first real input device this project has wired to LVGL -- every button built so far
 * (chunks B/F/G, memory/[[project_appruntime_fullstack_plan]]) could create a tap handler
 * but nothing could ever trigger one, since no touch driver existed at all until now.
 * purr_kernel_touch_read() already returns screen-pixel coordinates (purr_kernel.c's own
 * swap+mirror+rescale), so this callback is just a type conversion, no real logic of its
 * own. LVGL polls this on its own internal timer once registered -- nothing else to drive. */
static void touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    (void)drv;
    uint16_t x, y;
    if (purr_kernel_touch_read(&x, &y)) {
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

esp_err_t purr_lvgl_init(void)
{
    s_critical_error = 0;
    s_display = purr_kernel_display();
    if (s_display == NULL) {
        ESP_LOGW(TAG, "no display");
        return ESP_FAIL;
    }
    purr_display_info_t info;
    if (s_display->get_info(&info) != ESP_OK) {
        ESP_LOGW(TAG, "get_info failed");
        return ESP_FAIL;
    }

    size_t pixels = (size_t)info.width * info.height;
    lv_color_t *buf = heap_caps_malloc(pixels * sizeof(lv_color_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        ESP_LOGW(TAG, "draw buffer alloc failed (%u pixels)", (unsigned)pixels);
        return ESP_FAIL;
    }

    lv_init();
    lv_disp_draw_buf_init(&s_draw_buf, buf, NULL, pixels);

    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.draw_buf = &s_draw_buf;
    s_disp_drv.flush_cb = flush_cb;
    s_disp_drv.hor_res = info.width;
    s_disp_drv.ver_res = info.height;
    lv_disp_drv_register(&s_disp_drv);

    lv_indev_drv_init(&s_indev_drv);
    s_indev_drv.type = LV_INDEV_TYPE_POINTER;
    s_indev_drv.read_cb = touch_read_cb;
    lv_indev_drv_register(&s_indev_drv);

    const esp_timer_create_args_t targs = {.callback = tick_cb, .name = "lv_tick"};
    esp_timer_handle_t timer;
    if (esp_timer_create(&targs, &timer) != ESP_OK ||
        esp_timer_start_periodic(timer, LVGL_TICK_PERIOD_MS * 1000) != ESP_OK) {
        ESP_LOGW(TAG, "tick timer failed to start");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "up: %ux%u, %u byte draw buffer (PSRAM)",
             (unsigned)info.width, (unsigned)info.height, (unsigned)(pixels * sizeof(lv_color_t)));

    /* The service task is what actually drives LVGL from here on (purr_ui_service.h's own
     * comment on why) -- started here, not left for the caller to remember, so the two can
     * never be paired wrong. */
    return purr_ui_service_start();
}

void purr_lvgl_demo_screen(void)
{
    purr_ui_request_t req = {.op = PURR_UI_OP_CLEAR_SCREEN};
    purr_ui_service_request(&req, NULL);

    req = (purr_ui_request_t){.op = PURR_UI_OP_ADD_WIDGET};
    req.widget.type = PURR_UI_WIDGET_LABEL;
    snprintf(req.widget.text, sizeof(req.widget.text), "PURR OS\nLVGL is alive.");
    req.widget.color = PURR_UI_COLOR_UNSET;
    purr_ui_service_request(&req, NULL);
}

/* The service task (purr_ui_service.c) now owns lv_timer_handler() -- these just wait for a
 * critical error to be reported, instead of driving LVGL themselves. */
void purr_lvgl_run(void)
{
    while (!s_critical_error) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGW(TAG, "stopping: a critical error was reported");
}

void purr_lvgl_run_for_ms(uint32_t ms)
{
    int64_t until = esp_timer_get_time() + (int64_t)ms * 1000;
    while (esp_timer_get_time() < until && !s_critical_error) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void purr_lvgl_report_critical_error(const char *why)
{
    ESP_LOGE(TAG, "critical error: %s", why ? why : "(no reason given)");
    s_critical_error = 1;
}

int purr_lvgl_critical_error_pending(void)
{
    return s_critical_error;
}
