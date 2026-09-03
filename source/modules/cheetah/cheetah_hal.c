// cheetah_hal.c — LVGL ↔ catcall_display / catcall_touch / catcall_input bridge.
//
// A near-direct copy of mochi_hal.c (see that file's own header for the full
// rationale on every decision here — separate indevs per input class, the
// trackball as a keypad rather than an encoder, the draw-buffer sizing).
// Nothing about display/input plumbing is BB10-specific, so re-deriving it
// here rather than reusing it verbatim would just mean re-hitting the same
// hardware bugs mochi_hal.c already fixed.

#include "cheetah.h"
#include "../systemui/systemui.h"
#include "../common/purr_lv_flush.h"
#include "../../kernel/core/purr_kernel.h"
#include "../../kernel/catcalls/catcall_input.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "cheetah_hal";

// Same reasoning as mochi_hal.c's MOCHI_BUF_LINES — see that file for the
// cache-residency measurements this is derived from. Re-measure with
// cheetah_module.c's own instrumentation (if any is added) before changing it.
#ifndef CHEETAH_BUF_LINES
#define CHEETAH_BUF_LINES 48
#endif

static lv_color_t *s_buf1;
static lv_color_t *s_buf2;

static lv_disp_draw_buf_t s_draw_buf;
static lv_disp_drv_t      s_disp_drv;
static lv_indev_drv_t     s_touch_drv;
static lv_indev_drv_t     s_keypad_drv;
static lv_indev_drv_t     s_ball_drv;
static lv_group_t        *s_group = NULL;

static uint16_t s_disp_w = 320;
static uint16_t s_disp_h = 240;

static uint64_t s_last_activity_ms = 0;

// Resolved once at init — see mochi_hal.c's identical comment: this is a
// capability test (set_backlight => keyboard-class), not a name lookup.
static const catcall_input_t *s_kbd  = NULL;
static const catcall_input_t *s_ball = NULL;

// Trackball click keycode — see mochi_hal.c's matching comment. Not ASCII
// Enter; has to stay distinguishable from a real keyboard Enter.
#define TRACKBALL_CLICK_KEYCODE 0x0028

static void mark_activity(void)
{
    s_last_activity_ms = purr_kernel_uptime_ms();
    if (purr_systemui_is_locked()) purr_systemui_wake();
}

uint64_t cheetah_hal_last_activity_ms(void) { return s_last_activity_ms; }
lv_group_t *cheetah_hal_group(void)          { return s_group; }
bool cheetah_hal_has_physical_keyboard(void) { return s_kbd != NULL; }

static purr_lv_flush_t s_flush;

void cheetah_hal_set_shadows_enabled(bool on) { s_flush.shadows_off = !on; }

void cheetah_hal_wait_flush_idle(void)
{
    purr_lv_flush_wait_idle(&s_disp_drv, TAG);
}

static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p)
{
    purr_lv_flush(&s_flush, drv, area, color_p);
}

static void touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    (void)drv;
    const catcall_touch_t *t = purr_kernel_touch();
    if (t && t->is_pressed()) {
        uint16_t x = 0, y = 0;
        t->read_point(&x, &y);
        data->point.x = (lv_coord_t)x;
        data->point.y = (lv_coord_t)y;
        data->state   = LV_INDEV_STATE_PR;
        mark_activity();
    } else {
        data->state = LV_INDEV_STATE_REL;
    }
}

static void keypad_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    (void)drv;
    static bool     s_pending_release = false;
    static uint32_t s_pending_key     = 0;

    if (s_pending_release) {
        data->key   = s_pending_key;
        data->state = LV_INDEV_STATE_REL;
        s_pending_release = false;
        return;
    }

    if (!s_kbd || !s_kbd->poll_event) { data->state = LV_INDEV_STATE_REL; return; }

    input_event_t ev;
    while (s_kbd->poll_event(&ev)) {
        if (ev.type != INPUT_EVENT_KEY_DOWN) continue;
        uint32_t key;
        switch (ev.keycode) {
            case 0x08: case 0x7F: key = LV_KEY_BACKSPACE; break;
            case 0x0D: case 0x0A: key = LV_KEY_ENTER;     break;
            case 0x1B:            key = LV_KEY_ESC;       break;
            default:              key = ev.keycode;       break;
        }
        s_pending_key     = key;
        s_pending_release = true;
        data->key   = key;
        data->state = LV_INDEV_STATE_PR;
        mark_activity();
        return;
    }
    data->state = LV_INDEV_STATE_REL;
}

// Trackball → keypad indev emitting real arrow keys. See mochi_hal.c's
// ball_read_cb for the full story on why this is a KEYPAD, not an ENCODER
// (a grid needs both axes; an encoder only carries one).
static void ball_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    (void)drv;
    static bool     s_pending_release = false;
    static uint32_t s_pending_key     = 0;
    static bool     s_click_pending   = false;

    if (s_pending_release) {
        data->key   = s_pending_key;
        data->state = LV_INDEV_STATE_REL;
        s_pending_release = false;
        return;
    }

    if (!s_ball || !s_ball->poll_event) { data->state = LV_INDEV_STATE_REL; return; }

    int32_t sum_x = 0, sum_y = 0;
    input_event_t ev;
    while (s_ball->poll_event(&ev)) {
        if (ev.type == INPUT_EVENT_POINTER) {
            sum_x += ev.delta_x;
            sum_y += ev.delta_y;
        } else if (ev.type == INPUT_EVENT_KEY_DOWN && ev.keycode == TRACKBALL_CLICK_KEYCODE) {
            s_click_pending = true;
        }
    }

    if (s_click_pending) {
        s_click_pending   = false;
        s_pending_key     = LV_KEY_ENTER;
        s_pending_release = true;
        data->key   = LV_KEY_ENTER;
        data->state = LV_INDEV_STATE_PR;
        mark_activity();
        return;
    }

    if (sum_x == 0 && sum_y == 0) { data->state = LV_INDEV_STATE_REL; return; }

    mark_activity();

    int32_t ax = sum_x < 0 ? -sum_x : sum_x;
    int32_t ay = sum_y < 0 ? -sum_y : sum_y;
    uint32_t key = (ay > ax) ? ((sum_y > 0) ? LV_KEY_UP   : LV_KEY_DOWN)
                              : ((sum_x > 0) ? LV_KEY_LEFT : LV_KEY_RIGHT);

    s_pending_key     = key;
    s_pending_release = true;
    data->key   = key;
    data->state = LV_INDEV_STATE_PR;
}

// purr_kernel_input() returns "first registered (legacy)", which on
// tdeck_plus is the trackball — this classifies by capability instead. See
// mochi_hal.c's identical function for the full reasoning.
static void resolve_inputs(void)
{
    int n = purr_kernel_input_count();
    for (int i = 0; i < n; i++) {
        const catcall_input_t *in = purr_kernel_input_at(i);
        if (!in || !in->poll_event) continue;
        if (in->set_backlight) {
            if (!s_kbd)  s_kbd  = in;
        } else {
            if (!s_ball) s_ball = in;
        }
    }
    ESP_LOGI(TAG, "inputs: keyboard=%s trackball=%s",
             s_kbd  ? (s_kbd->name  ? s_kbd->name  : "yes") : "none",
             s_ball ? (s_ball->name ? s_ball->name : "yes") : "none");
}

int cheetah_hal_init(void)
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

    // Draw buffer: PSRAM, banded to fit the data cache — see mochi_hal.c's
    // extensive measurement comment for why 48 lines is the deliberate
    // middle ground here, not a guess (mochi_hal.c also carries an
    // internal-.bss-buffer experiment path behind MOCHI_USE_INTERNAL_DRAW_BUF;
    // that was a not-yet-settled A/B comparison, not production code, so it
    // isn't reproduced here — this takes the PSRAM-banded path directly).
    // Re-measure before changing.
    int    lines     = CHEETAH_BUF_LINES;
    size_t buf_bytes = sizeof(lv_color_t) * s_disp_w * lines;
    s_buf1 = heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    ESP_LOGW(TAG, "[perf] draw buffer: PSRAM, %d lines (%u bytes), %d render passes per screen",
             lines, (unsigned)buf_bytes, (s_disp_h + lines - 1) / lines);
    if (!s_buf1) {
        ESP_LOGE(TAG, "draw buffer alloc failed (%u bytes)", (unsigned)buf_bytes);
        return -1;
    }

    s_buf2 = NULL;

    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res      = (lv_coord_t)s_disp_w;
    s_disp_drv.ver_res      = (lv_coord_t)s_disp_h;
    s_disp_drv.flush_cb     = flush_cb;
    s_disp_drv.draw_buf     = &s_draw_buf;
    s_disp_drv.full_refresh = 0;

    purr_lv_flush_init(&s_flush, &s_disp_drv, s_disp_w, s_disp_h,
                       &s_buf2, buf_bytes, TAG);

    lv_disp_draw_buf_init(&s_draw_buf, s_buf1, s_buf2, s_disp_w * lines);

    lv_disp_t *lv_disp = lv_disp_drv_register(&s_disp_drv);
    purr_lv_flush_install_theme(&s_flush, lv_disp, TAG);

    resolve_inputs();

    const catcall_touch_t *touch = purr_kernel_touch();
    if (touch) {
        lv_indev_drv_init(&s_touch_drv);
        s_touch_drv.type    = LV_INDEV_TYPE_POINTER;
        s_touch_drv.read_cb = touch_read_cb;
        lv_indev_drv_register(&s_touch_drv);
        ESP_LOGI(TAG, "touch registered (pointer)");
    } else {
        ESP_LOGW(TAG, "no touch catcall — Cheetah will run keyboard/ball only");
    }

    if (s_kbd || s_ball) s_group = lv_group_create();

    if (s_kbd) {
        lv_indev_drv_init(&s_keypad_drv);
        s_keypad_drv.type    = LV_INDEV_TYPE_KEYPAD;
        s_keypad_drv.read_cb = keypad_read_cb;
        lv_indev_t *kp = lv_indev_drv_register(&s_keypad_drv);
        lv_indev_set_group(kp, s_group);
        ESP_LOGI(TAG, "keyboard registered (keypad)");
    }

    if (s_ball) {
        lv_indev_drv_init(&s_ball_drv);
        s_ball_drv.type    = LV_INDEV_TYPE_KEYPAD;
        s_ball_drv.read_cb = ball_read_cb;
        lv_indev_t *ball = lv_indev_drv_register(&s_ball_drv);
        lv_indev_set_group(ball, s_group);
        ESP_LOGI(TAG, "trackball registered (keypad/arrows)");
    }

    if (!s_kbd && !s_ball) {
        ESP_LOGW(TAG, "no input catcalls — keyboard navigation unavailable");
    }

    // "Now", not the 0 initializer — see mochi_hal.c's identical comment:
    // boot can outlast the idle timeout, which would lock the screen before
    // the user ever touches the device.
    s_last_activity_ms = purr_kernel_uptime_ms();

    ESP_LOGI(TAG, "HAL init complete");
    return 0;
}

uint16_t cheetah_hal_width(void)  { return s_disp_w; }
uint16_t cheetah_hal_height(void) { return s_disp_h; }
