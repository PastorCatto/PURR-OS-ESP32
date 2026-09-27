// tabby_hal.c — LVGL ↔ catcall_display / catcall_touch / catcall_input bridge.
//
// Three input devices, deliberately kept separate rather than merged into one
// read_cb the way cupcake_hal.c does it:
//
//   touch     → LV_INDEV_TYPE_POINTER   (absolute x/y, press state)
//   keyboard  → LV_INDEV_TYPE_KEYPAD    (character + navigation keys)
//   trackball → LV_INDEV_TYPE_ENCODER   (relative steps + click)
//
// The split matters. cupcake_hal.c polls *every* registered input catcall in
// one keypad callback and rewrites trackball deltas into synthetic
// LV_KEY_PREV/LV_KEY_NEXT presses. That works for stepping focus, but it
// throws away the distinction between "the user typed N" and "the user rolled
// the ball one step", which is exactly the distinction a type-to-filter shell
// needs — under that scheme a trackball roll is indistinguishable from a
// keypress and would insert characters into the filter. Registering the ball
// as a real encoder keeps the two channels apart at the LVGL level, and gets
// correct in-app behaviour for free (LVGL's encoder handling already knows to
// step focus within a group and to enter/exit edit mode on click).
//
// Which physical driver is which is decided by capability, not by name: a
// keyboard-class driver implements set_backlight (bbq20's under-key LEDs), a
// trackball does not. Same test cupcake_win.c's ck_has_physical_keyboard()
// already uses. A device with only one of the two still works — the missing
// indev simply isn't registered.

#include "tabby.h"
#include "../systemui/systemui.h"
#include "../../kernel/core/purr_kernel.h"
#include "../../kernel/catcalls/catcall_input.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "tabby_hal";

// Rows of pixels per flush buffer. Same PSRAM+DMA reasoning as
// cupcake_hal.c's equivalent: these are pure pixel payloads pushed out over
// SPI DMA, nothing that needs scarce internal SRAM, and ESP32-S3's GDMA can
// DMA straight out of PSRAM. MALLOC_CAP_DMA alongside MALLOC_CAP_SPIRAM is
// what actually guarantees a DMA-usable PSRAM allocation — MALLOC_CAP_SPIRAM
// on its own does not.
#ifndef TABBY_BUF_LINES
#define TABBY_BUF_LINES 80
#endif

static lv_color_t *s_buf1;
static lv_color_t *s_buf2;

static lv_disp_draw_buf_t s_draw_buf;
static lv_disp_drv_t      s_disp_drv;
static lv_indev_drv_t     s_touch_drv;
static lv_indev_drv_t     s_keypad_drv;
static lv_indev_drv_t     s_enc_drv;
static lv_group_t        *s_group = NULL;

static uint16_t s_disp_w = 320;
static uint16_t s_disp_h = 240;

static uint64_t s_last_activity_ms = 0;

// Resolved once at init — see this file's header comment for the capability
// test. Either may stay NULL on a device that lacks that input class.
static const catcall_input_t *s_kbd  = NULL;
static const catcall_input_t *s_ball = NULL;

// Trackball click, per trackball.c's own header: direction rolls arrive as
// INPUT_EVENT_POINTER deltas, the centre press as KEY_DOWN/KEY_UP carrying
// this keycode. It is NOT ASCII Enter (0x0D/0x0A, what bbq20 sends) — the two
// have to stay distinguishable here, since the shell treats "ball click" and
// "Enter key" the same but a textarea must only receive the latter.
#define TRACKBALL_CLICK_KEYCODE 0x0028

static void mark_activity(void)
{
    s_last_activity_ms = purr_kernel_uptime_ms();
    // A press while the idle lock has already darkened the screen should wake
    // it — distinct from dismissing the lock, which is the lock overlay's own
    // gesture. purr_systemui_wake() is a no-op unless actually locked.
    if (purr_systemui_is_locked()) purr_systemui_wake();
}

uint64_t tabby_hal_last_activity_ms(void) { return s_last_activity_ms; }
lv_group_t *tabby_hal_group(void)         { return s_group; }
bool tabby_hal_has_physical_keyboard(void) { return s_kbd != NULL; }

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

// Keyboard → keypad indev.
//
// bbq20.c only ever emits KEY_DOWN (its RP2040 bridge has no key-up event),
// so each press is reported here as one PR read followed by one REL read on
// the next call. LVGL fires the focused object's LV_EVENT_KEY on the PR edge,
// which is all a type-to-filter shell or an lv_textarea needs; neither wants a
// real held-key repeat state.
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

// Trackball → encoder indev.
//
// LVGL's encoder model is (enc_diff, state): enc_diff is how many detents the
// control moved since the last read, state is the button. Accumulate deltas
// across whatever the driver has queued rather than reporting one per read,
// so a fast roll doesn't lag behind the queue.
//
// trackball.c flips dx/dy signs for pointer-style feel (see its update_state()
// comment), so dy > 0 means the ball rolled UP. Encoder convention is
// "positive = next/forward", and up should move *backward* through a list, so
// dy is negated. Horizontal rolls fold onto the same axis: on a single-column
// list, left/right are far more useful as extra up/down than as a dead input.
static void encoder_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    (void)drv;
    static bool s_click_held = false;

    if (!s_ball || !s_ball->poll_event) {
        data->enc_diff = 0;
        data->state    = s_click_held ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL;
        return;
    }

    int16_t diff = 0;
    input_event_t ev;
    while (s_ball->poll_event(&ev)) {
        if (ev.type == INPUT_EVENT_POINTER) {
            if (ev.delta_y) diff += (ev.delta_y > 0) ? -1 : 1;
            if (ev.delta_x) diff += (ev.delta_x > 0) ? -1 : 1;
            mark_activity();
        } else if (ev.type == INPUT_EVENT_KEY_DOWN && ev.keycode == TRACKBALL_CLICK_KEYCODE) {
            s_click_held = true;
            mark_activity();
        } else if (ev.type == INPUT_EVENT_KEY_UP && ev.keycode == TRACKBALL_CLICK_KEYCODE) {
            s_click_held = false;
        }
    }

    data->enc_diff = diff;
    data->state    = s_click_held ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL;
}

// Classify the registered input catcalls. purr_kernel_input() can't be used —
// it returns "first registered (legacy)", which on tdeck_plus is the trackball
// (registered before bbq20 in kernel_tdp_boot.c), not the keyboard.
static void resolve_inputs(void)
{
    int n = purr_kernel_input_count();
    for (int i = 0; i < n; i++) {
        const catcall_input_t *in = purr_kernel_input_at(i);
        if (!in || !in->poll_event) continue;
        if (in->set_backlight) {
            if (!s_kbd)  s_kbd  = in;      // keyboard-class (bbq20)
        } else {
            if (!s_ball) s_ball = in;      // pointer-class (trackball)
        }
    }
    ESP_LOGI(TAG, "inputs: keyboard=%s trackball=%s",
             s_kbd  ? (s_kbd->name  ? s_kbd->name  : "yes") : "none",
             s_ball ? (s_ball->name ? s_ball->name : "yes") : "none");
}

int tabby_hal_init(void)
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

    // Sized from the real display width, never a compile-time constant — a
    // mismatch here renders past the allocation and sprays pixels across the
    // PSRAM heap (see cupcake_hal.c's note on the tab5 corruption this caused).
    size_t buf_bytes = sizeof(lv_color_t) * s_disp_w * TABBY_BUF_LINES;
    s_buf1 = heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    s_buf2 = heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    if (!s_buf1 || !s_buf2) {
        ESP_LOGE(TAG, "PSRAM DMA alloc failed for display buffers (2x %u bytes)",
                 (unsigned)buf_bytes);
        return -1;
    }

    lv_disp_draw_buf_init(&s_draw_buf, s_buf1, s_buf2, s_disp_w * TABBY_BUF_LINES);

    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res      = (lv_coord_t)s_disp_w;
    s_disp_drv.ver_res      = (lv_coord_t)s_disp_h;
    s_disp_drv.flush_cb     = flush_cb;
    s_disp_drv.draw_buf     = &s_draw_buf;
    s_disp_drv.full_refresh = 0;
    lv_disp_drv_register(&s_disp_drv);

    resolve_inputs();

    const catcall_touch_t *touch = purr_kernel_touch();
    if (touch) {
        lv_indev_drv_init(&s_touch_drv);
        s_touch_drv.type    = LV_INDEV_TYPE_POINTER;
        s_touch_drv.read_cb = touch_read_cb;
        lv_indev_drv_register(&s_touch_drv);
        ESP_LOGI(TAG, "touch registered (pointer)");
    } else {
        ESP_LOGW(TAG, "no touch catcall — Tabby will run keyboard/ball only");
    }

    // One group shared by both the keypad and the encoder: LVGL is happy to
    // drive the same group from several indevs, and it's what makes "roll to
    // the button, then type into the field it focused" behave as one
    // continuous navigation model rather than two competing ones.
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
        lv_indev_drv_init(&s_enc_drv);
        s_enc_drv.type    = LV_INDEV_TYPE_ENCODER;
        s_enc_drv.read_cb = encoder_read_cb;
        lv_indev_t *enc = lv_indev_drv_register(&s_enc_drv);
        lv_indev_set_group(enc, s_group);
        ESP_LOGI(TAG, "trackball registered (encoder)");
    }

    if (!s_kbd && !s_ball) {
        ESP_LOGW(TAG, "no input catcalls — keyboard navigation unavailable");
    }

    // Must be "now", not the 0 initializer: boot (WiFi/BT/LoRa bring-up) can
    // outlast the shortest idle timeout, which would make the system UI's idle
    // check see an already-expired timer and lock the screen before the user
    // ever touches the device.
    s_last_activity_ms = purr_kernel_uptime_ms();

    // lv_tick_inc()/lv_timer_handler() are driven solely by tabby_task
    // (tabby_module.c) under purr_kernel_ui_lock(). Deliberately no separate
    // tick task here — cupcake_hal.c used to have one and it both
    // double-counted LVGL's clock and raced the render loop's own LVGL calls.
    ESP_LOGI(TAG, "HAL init complete");
    return 0;
}

uint16_t tabby_hal_width(void)  { return s_disp_w; }
uint16_t tabby_hal_height(void) { return s_disp_h; }
