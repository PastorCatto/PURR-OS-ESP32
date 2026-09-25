// nougat_hal.c — LVGL v9 ↔ catcall_display / catcall_touch bridge
//
// Ported from cupcake_hal.c onto LVGL v9's real display/indev registration
// API (lv_display_create()/lv_display_set_flush_cb()/lv_display_set_buffers()
// replace v8's lv_disp_drv_t+lv_disp_drv_register(); lv_indev_create()
// replaces lv_indev_drv_t+lv_indev_drv_register()) — every function name and
// signature below was confirmed against this project's actual resolved
// LVGL 9.5.0 headers (CoreOS/managed_components/lvgl__lvgl/src/display/
// lv_display.h, src/indev/lv_indev.h), not assumed from general v8→v9
// knowledge. Same responsibilities as cupcake_hal.c otherwise: buffer
// allocation, input registration, activity tracking.

#include "nougat.h"
#include "../../kernel/core/purr_kernel.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "nougat_hal";

// Tab5's DPI scan geometry is 720(w) x 1280(h) — see m5tab5_bsp.c's header
// comment on why that's "landscape" despite the numbers looking portrait.
// One buffer line's worth of width covers the actual panel; height is a
// partial-render strip like cupcake_hal.c's CUPCAKE_BUF_LINES, not the
// whole 720x1280 frame (that would be 1.8MB per buffer).
#ifndef NOUGAT_BUF_LINES
#define NOUGAT_BUF_LINES 80
#endif
#define NOUGAT_BUF_WIDTH 720

static uint8_t *s_buf1;
static uint8_t *s_buf2;

static lv_display_t *s_disp;
static lv_indev_t    *s_touch_indev;
static lv_indev_t    *s_keypad_indev;
static lv_group_t    *s_kb_group = NULL;

static uint16_t s_disp_w = NOUGAT_BUF_WIDTH;
static uint16_t s_disp_h = 1280;

// Idle-timeout tracking, same shape as cupcake_hal.c's — not consumed by
// anything yet (Nougat has no lock screen, explicitly out of scope per the
// Nougat plan) but kept so future chrome work isn't blocked on adding it.
static uint64_t s_last_activity_ms = 0;

static void mark_activity(void)
{
    s_last_activity_ms = purr_kernel_uptime_ms();
}

uint64_t nougat_hal_last_activity_ms(void) { return s_last_activity_ms; }

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    const catcall_display_t *d = purr_kernel_display();
    if (d && d->push_pixels) {
        int32_t w = area->x2 - area->x1 + 1;
        int32_t h = area->y2 - area->y1 + 1;
        d->push_pixels(area->x1, area->y1, (int)w, (int)h, (const uint16_t *)px_map);
    }
    lv_display_flush_ready(disp);
}

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    const catcall_touch_t *t = purr_kernel_touch();
    if (t && t->is_pressed()) {
        uint16_t x = 0, y = 0;
        t->read_point(&x, &y);
        data->point.x = (lv_coord_t)x;
        data->point.y = (lv_coord_t)y;
        data->state   = LV_INDEV_STATE_PRESSED;
        mark_activity();
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

// Physical keyboard bridge — mirrors cupcake_hal.c's keypad_read_cb exactly
// (same purr_kernel_input_*() scan, same key remap, same trackball-as-
// LV_KEY_PREV/NEXT translation). Tab5 has no input catcall registered as of
// Phase 1 (touch only), so purr_kernel_input_count() is 0 and this is inert
// — kept for parity with Cupcake and any future Tab5 input accessory.
static void keypad_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    static bool     s_pending_release = false;
    static uint32_t s_pending_key     = 0;

    if (s_pending_release) {
        data->key   = s_pending_key;
        data->state = LV_INDEV_STATE_RELEASED;
        s_pending_release = false;
        return;
    }

    int n = purr_kernel_input_count();
    for (int i = 0; i < n; i++) {
        const catcall_input_t *in = purr_kernel_input_at(i);
        if (!in || !in->poll_event) continue;
        input_event_t ev;
        while (in->poll_event(&ev)) {
            uint32_t key;
            if (ev.type == INPUT_EVENT_KEY_DOWN) {
                switch (ev.keycode) {
                    case 0x08: case 0x7F: key = LV_KEY_BACKSPACE; break;
                    case 0x0D: case 0x0A: key = LV_KEY_ENTER;     break;
                    case 0x1B:            key = LV_KEY_ESC;       break;
                    default:              key = ev.keycode;       break;
                }
            } else if (ev.type == INPUT_EVENT_POINTER) {
                if (ev.delta_y > 0)      key = LV_KEY_PREV;
                else if (ev.delta_y < 0) key = LV_KEY_NEXT;
                else if (ev.delta_x > 0) key = LV_KEY_PREV;
                else if (ev.delta_x < 0) key = LV_KEY_NEXT;
                else continue;
            } else {
                continue;
            }
            s_pending_key     = key;
            s_pending_release = true;
            data->key   = key;
            data->state = LV_INDEV_STATE_PRESSED;
            mark_activity();
            return;
        }
    }
    data->state = LV_INDEV_STATE_RELEASED;
}

int nougat_hal_init(void)
{
    const catcall_display_t *disp = purr_kernel_display();
    if (!disp) {
        ESP_LOGE(TAG, "no display catcall");
        return -1;
    }

    display_info_t info = {0};
    if (disp->get_info) {
        disp->get_info(&info);
        s_disp_w = info.width  ? info.width  : NOUGAT_BUF_WIDTH;
        s_disp_h = info.height ? info.height : 1280;
    }
    ESP_LOGI(TAG, "display: %ux%u", s_disp_w, s_disp_h);

    lv_init();

    // RGB565 (2 bytes/pixel) — matches m5tab5_bsp.c's LCD_COLOR_PIXEL_FORMAT_RGB565
    // DPI panel config and catcall_display_t's uint16_t push_pixels contract.
    size_t buf_bytes = 2 * (size_t)NOUGAT_BUF_WIDTH * NOUGAT_BUF_LINES;
    s_buf1 = heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    s_buf2 = heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    if (!s_buf1 || !s_buf2) {
        ESP_LOGE(TAG, "PSRAM DMA alloc failed for display buffers (2x %u bytes)", (unsigned)buf_bytes);
        return -1;
    }

    s_disp = lv_display_create((int32_t)s_disp_w, (int32_t)s_disp_h);
    if (!s_disp) {
        ESP_LOGE(TAG, "lv_display_create failed");
        return -1;
    }
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(s_disp, flush_cb);
    lv_display_set_buffers(s_disp, s_buf1, s_buf2, (uint32_t)buf_bytes,
                            LV_DISPLAY_RENDER_MODE_PARTIAL);

    const catcall_touch_t *touch = purr_kernel_touch();
    if (touch) {
        s_touch_indev = lv_indev_create();
        lv_indev_set_type(s_touch_indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(s_touch_indev, touch_read_cb);
        ESP_LOGI(TAG, "touch input registered");
    } else {
        ESP_LOGW(TAG, "no touch catcall — nougat will run touchless");
    }

    if (purr_kernel_input_count() > 0) {
        s_kb_group = lv_group_create();
        s_keypad_indev = lv_indev_create();
        lv_indev_set_type(s_keypad_indev, LV_INDEV_TYPE_KEYPAD);
        lv_indev_set_read_cb(s_keypad_indev, keypad_read_cb);
        lv_indev_set_group(s_keypad_indev, s_kb_group);
        ESP_LOGI(TAG, "keypad input registered (%d input driver(s))", purr_kernel_input_count());
    } else {
        ESP_LOGW(TAG, "no input catcall — physical keyboard unavailable");
    }

    // Must be "now", not left at its 0 initializer — same rationale as
    // cupcake_hal.c's identical line.
    s_last_activity_ms = purr_kernel_uptime_ms();

    ESP_LOGI(TAG, "HAL init complete");
    return 0;
}

uint16_t nougat_hal_width(void)  { return s_disp_w; }
uint16_t nougat_hal_height(void) { return s_disp_h; }

lv_group_t *nougat_hal_keypad_group(void) { return s_kb_group; }
