#include "purr_lvgl.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "purr_display.h"
#include "purr_kernel.h"
#include "purr_power_plat.h"
#include "purr_ui_service.h"

static const char *TAG = "lvgl";

/* purr_display_v2_t's blit()/fill() already do their own panel-specific chunking
 * internally (st7789.c's d_blit() loops over its own DMA buffer size) -- the flush
 * callback below can hand it any area in one call, no chunking of its own needed. */
static const purr_display_v2_t *s_display;
static lv_display_t *s_lv_display;
static lv_indev_t *s_lv_indev;
static volatile int s_critical_error;

/* LVGL 9 asks for the time instead of being told it (8.x's lv_tick_inc timer is gone). */
static uint32_t tick_get_cb(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* Rotation (purr_lvgl_set_rotation): LVGL renders into the buffer in the rotated orientation and
 * hands each area to the flush callback in logical coordinates. The panel only knows its own
 * orientation, so the area and its pixels are turned back before they go out. */
static uint8_t *s_rot_buf;
static size_t s_rot_buf_bytes;

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    lv_display_rotation_t rot = lv_display_get_rotation(disp);
    lv_area_t turned;
    if (rot != LV_DISPLAY_ROTATION_0 && s_rot_buf != NULL) {
        lv_color_format_t cf = lv_display_get_color_format(disp);
        turned = *area;
        lv_display_rotate_area(disp, &turned);
        int32_t src_w = lv_area_get_width(area);
        int32_t src_h = lv_area_get_height(area);
        uint32_t src_stride = lv_draw_buf_width_to_stride(src_w, cf);
        uint32_t dst_stride = lv_draw_buf_width_to_stride(lv_area_get_width(&turned), cf);
        if ((size_t)dst_stride * lv_area_get_height(&turned) <= s_rot_buf_bytes) {
            lv_draw_sw_rotate(px_map, s_rot_buf, src_w, src_h, src_stride, dst_stride, rot, cf);
            area = &turned;
            px_map = s_rot_buf;
        }
    }
    int w = area->x2 - area->x1 + 1;
    int h = area->y2 - area->y1 + 1;
    /* LVGL's RGB565 (LV_COLOR_FORMAT_RGB565, 16-bit colour depth) is laid out exactly like this
     * project's own host-endian RGB565 (purr_display.h's own convention): no conversion needed. */
    int e = s_display->blit(area->x1, area->y1, w, h, (const uint16_t *)px_map);
    if (e != 0) {
        /* This callback used to ignore blit()'s result, so a failing flush left no trace at all
         * (found chasing a white screen, 2026-10-25 -- which turned out to be elsewhere, but the
         * silence cost a lot of time). Still ends in flush_ready so LVGL does not stall. */
        ESP_LOGW(TAG, "flush (%d,%d %dx%d): blit failed (%d)", (int)area->x1, (int)area->y1, w, h, e);
    }
    lv_display_flush_ready(disp);
}

int purr_lvgl_set_rotation(int degrees)
{
    lv_display_rotation_t r;
    switch (degrees) {
    case 0:   r = LV_DISPLAY_ROTATION_0; break;
    case 90:  r = LV_DISPLAY_ROTATION_90; break;
    case 180: r = LV_DISPLAY_ROTATION_180; break;
    case 270: r = LV_DISPLAY_ROTATION_270; break;
    default:  return -1;
    }
    if (s_lv_display == NULL) {
        return -1;
    }
    lv_display_set_rotation(s_lv_display, r);
    lv_obj_invalidate(lv_screen_active());
    return 0;
}

int purr_lvgl_get_rotation(void)
{
    if (s_lv_display == NULL) {
        return 0;
    }
    return (int)lv_display_get_rotation(s_lv_display) * 90;
}

/* The first real input device this project has wired to LVGL -- every button built so far
 * (chunks B/F/G, memory/[[project_appruntime_fullstack_plan]]) could create a tap handler
 * but nothing could ever trigger one, since no touch driver existed at all until now.
 * purr_kernel_touch_read() already returns screen-pixel coordinates (purr_kernel.c's own
 * swap+mirror+rescale), so this callback is just a type conversion, no real logic of its
 * own. LVGL polls this on its own internal timer once registered -- nothing else to drive. */
static int s_touch_swallow;

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    uint16_t x, y;
    if (purr_kernel_touch_read(&x, &y)) {
        if (purr_power_sys_activity()) {
            s_touch_swallow = 1;                /* this touch only woke the screen: it must not also tap something */
        }
        if (s_touch_swallow) {
            data->state = LV_INDEV_STATE_RELEASED;
            return;
        }
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        s_touch_swallow = 0;
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

/* The hardware keyboard. LVGL wants press and release as separate reads, so a key is reported as
 * pressed on one poll and released on the next. */
static lv_indev_t *s_lv_kbd;
static uint32_t s_kbd_key;
static int s_kbd_down;

static void kbd_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    if (s_kbd_down) {
        s_kbd_down = 0;
        data->state = LV_INDEV_STATE_RELEASED;
        data->key = s_kbd_key;
        return;
    }
    char c = purr_kernel_key();
    if (c == 0) {
        data->state = LV_INDEV_STATE_RELEASED;
        data->key = s_kbd_key;
        return;
    }
    if (purr_power_sys_activity()) {            /* the key only woke the screen: it is not delivered */
        data->state = LV_INDEV_STATE_RELEASED;
        data->key = s_kbd_key;
        return;
    }
    uint32_t k = (uint8_t)c;
    switch (c) {
    case '\r':
    case '\n': k = LV_KEY_ENTER; break;
    case 8:
    case 127: k = LV_KEY_BACKSPACE; break;
    case 27:  k = LV_KEY_ESC; break;
    case '\t': k = LV_KEY_NEXT; break;
    default: break;
    }
    s_kbd_key = k;
    s_kbd_down = 1;
    data->key = k;
    data->state = LV_INDEV_STATE_PRESSED;
}

struct _lv_indev_t *purr_lvgl_keypad(void)
{
    return s_lv_kbd;
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
    size_t bytes = pixels * sizeof(uint16_t);        /* RGB565 */
    uint8_t *buf = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        ESP_LOGW(TAG, "draw buffer alloc failed (%u pixels)", (unsigned)pixels);
        return ESP_FAIL;
    }

    s_rot_buf = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_rot_buf_bytes = s_rot_buf ? bytes : 0;     /* without it a rotation request just does nothing */

    lv_init();
    lv_tick_set_cb(tick_get_cb);

    s_lv_display = lv_display_create(info.width, info.height);
    lv_display_set_color_format(s_lv_display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(s_lv_display, flush_cb);
    /* One screen-sized buffer, partial render mode: the same single full-size buffer 8.3 used. */
    lv_display_set_buffers(s_lv_display, buf, NULL, bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);

    s_lv_indev = lv_indev_create();
    lv_indev_set_type(s_lv_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_lv_indev, touch_read_cb);

    s_lv_kbd = lv_indev_create();
    lv_indev_set_type(s_lv_kbd, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(s_lv_kbd, kbd_read_cb);

    ESP_LOGI(TAG, "up: %ux%u, %u byte draw buffer (PSRAM), LVGL %d.%d.%d",
             (unsigned)info.width, (unsigned)info.height, (unsigned)bytes, LVGL_VERSION_MAJOR,
             LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);

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
