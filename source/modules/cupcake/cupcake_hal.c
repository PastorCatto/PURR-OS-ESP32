// cupcake_hal.c — LVGL ↔ catcall_display / catcall_touch bridge
//
// Forked from cardstack_hal.c. Cupcake is a tap-driven grid launcher (no
// snap-scrolling card stack), so unlike Cardstack this HAL does not poll the
// trackball directly — trackball input, if present, only ever goes through
// LVGL's own indev abstraction here (not registered as one in v1, since
// tile navigation is by touch only).

#include "cupcake.h"
#include "../common/purr_lv_flush.h"
#include "../../kernel/core/purr_kernel.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"

static const char *TAG = "cupcake_hal";

// Sized to FIT THE DATA CACHE, which is the constraint that actually binds.
//
// CONFIG_ESP32S3_DATA_CACHE_SIZE is 0x8000 = 32,768 bytes. At the previous 80
// lines this buffer was 320 x 80 x 2 = 51,200 bytes — 1.6x the entire data
// cache, and it lives in PSRAM, so every render pass streamed the whole thing
// through a cache too small to hold any of it and nothing survived to be reused.
//
// 48 lines is 30,720 bytes, which fits with headroom. Measured on T-Deck Plus
// against Mochi, this is a deliberate middle between two measured points, not a
// guess:
//   16 lines (15 passes) — much WORSE. Per-pass cost dominates: each pass
//                          re-walks the object tree and re-clips every widget.
//   80 lines  (3 passes) — fewest passes, but exceeds the data cache.
//
// Fewer flush round-trips was the original reason for 80, and that reasoning was
// sound before the flush became asynchronous. It no longer applies: the flush is
// overlapped with rendering now, so round-trips are close to free, while the
// cache miss on every pixel written is not.
#ifndef CUPCAKE_BUF_LINES
#define CUPCAKE_BUF_LINES 48
#endif
#define CUPCAKE_BUF_WIDTH 480

// PSRAM-backed instead of static internal-RAM arrays — at the old 20 lines
// these were ~37.5KB combined (480*20*2 bytes each), the single largest
// static consumer of this board's scarce internal SRAM (see the memory-
// pressure investigation this was found in). Pure pixel data pushed out to
// the panel over SPI DMA, nothing that needs internal RAM specifically —
// st7789.c's spi_bus_initialize() already runs with SPI_DMA_CH_AUTO, and
// ESP32-S3's GDMA can DMA directly out of PSRAM (unlike the original
// ESP32). MALLOC_CAP_DMA alongside MALLOC_CAP_SPIRAM is what tells
// heap_caps_malloc() to actually satisfy both — plain MALLOC_CAP_SPIRAM
// alone doesn't guarantee a DMA-usable allocation. Freeing that ~37.5KB
// also made room to grow CUPCAKE_BUF_LINES 4x (20 -> 80), which directly
// cuts the number of flush_cb()/SPI-transfer round-trips per full-screen
// redraw.
static lv_color_t *s_buf1;
static lv_color_t *s_buf2;

static lv_disp_draw_buf_t s_draw_buf;
static lv_disp_drv_t      s_disp_drv;
static lv_indev_drv_t     s_touch_drv;
static lv_indev_drv_t     s_keypad_drv;
static lv_group_t        *s_kb_group = NULL;

static uint16_t s_disp_w = 320;
static uint16_t s_disp_h = 240;

// Idle-timeout tracking for cupcake_ui.c's lock screen (see cupcake.h).
// Stamped here rather than in cupcake_ui.c because this is where every
// input source (touch, bbq20 keys, trackball nav) actually funnels through
// on its way into LVGL — cupcake_ui.c only needs to read it back.
static uint64_t s_last_activity_ms = 0;

static void mark_activity(void)
{
    s_last_activity_ms = purr_kernel_uptime_ms();
    // If the screen is dark because the idle timer already fired, this
    // press is what should make the (still-locked) lock screen visible
    // again — distinct from actually dismissing it, which is the lock
    // overlay's own tap/swipe handler in cupcake_ui.c.
    if (cupcake_ui_is_locked()) cupcake_ui_wake();
}

uint64_t cupcake_hal_last_activity_ms(void) { return s_last_activity_ms; }

// Off-screen compose + async flush + shadow suppression, shared with Mochi.
// See ../common/purr_lv_flush.h — all of it was measured on T-Deck Plus during
// the DP8 display pass, and lived only in mochi_hal.c until this backend was
// brought level with it.
static purr_lv_flush_t s_flush;

void cupcake_hal_set_shadows_enabled(bool on) { s_flush.shadows_off = !on; }

static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p)
{
    // Owns the lv_disp_flush_ready() contract entirely — either it signals, or
    // the driver's completion callback does. Do not signal here as well.
    purr_lv_flush(&s_flush, drv, area, color_p);
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
        mark_activity();
    } else {
        data->state = LV_INDEV_STATE_REL;
    }
}

// Physical keyboard bridge — no LV_INDEV_TYPE_KEYPAD was ever registered
// here before, so BBQ20 keystrokes never reached LVGL at all under Cupcake
// (only the touch pointer indev existed); the on-screen keyboard was the
// only way to type, and even that was separately broken (see
// cupcake_win.c's ck_win_show()/ck_kb_show() ordering note). purr_kernel_
// input() can't be used directly — it returns "first registered (legacy)",
// which on tdeck_plus is the trackball (registered before bbq20 in
// kernel_tdp_boot.c), not the keyboard — so this scans every registered
// input catcall and reacts to both KEY_DOWN (bbq20) and POINTER
// (trackball) events.
//
// bbq20.c only ever emits KEY_DOWN (the RP2040 bridge chip has no key-up
// event), so a single press is reported as one PR cycle followed by one REL
// cycle on the next read — LVGL's keypad indev fires the object's
// LV_EVENT_KEY on the PR edge, so this one-shot pattern is sufficient; it
// doesn't need a real "held" state to drive lv_textarea's normal
// backspace/char-insert handling.
//
// Trackball POINTER deltas are translated to LV_KEY_PREV/LV_KEY_NEXT here
// (group focus stepping) rather than true relative pointer motion — a
// deliberate temporary choice, not a driver limitation: trackball.c itself
// still emits real INPUT_EVENT_POINTER deltas unchanged, so a future
// trackpad-style consumer (a revived cardstack-style UI) can register it
// as a real LV_INDEV_TYPE_POINTER/BUTTON indev without any driver changes.
// UP/LEFT step backward, DOWN/RIGHT step forward — trackball.c's dx/dy
// signs are already flipped for correct pointer-style feel (see its
// update_state() comment), so dy>0/dx>0 here means UP/LEFT was rolled.
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
            data->state = LV_INDEV_STATE_PR;
            mark_activity();
            return;
        }
    }
    data->state = LV_INDEV_STATE_REL;
}

int cupcake_hal_init(void)
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

    // Size from the ACTUAL display width, not the compile-time
    // CUPCAKE_BUF_WIDTH (480): lv_disp_draw_buf_init() below declares
    // s_disp_w * CUPCAKE_BUF_LINES pixels, so on any panel wider than 480
    // LVGL rendered past the allocation — ~100KB of background pixels
    // sprayed over the PSRAM heap per frame on tab5's 640-wide surface,
    // corrupting TLSF free-list metadata (Store access fault in
    // remove_free_block on the next allocation, confirmed live). The two
    // expressions only ever agreed by accident on <=480px devices.
    size_t buf_bytes = sizeof(lv_color_t) * s_disp_w * CUPCAKE_BUF_LINES;
    s_buf1 = heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    if (!s_buf1) {
        ESP_LOGE(TAG, "PSRAM DMA alloc failed for display buffer (%u bytes)", (unsigned)buf_bytes);
        return -1;
    }
    // s_buf2 is now allocated by purr_lv_flush_init(), and ONLY if the driver
    // can flush asynchronously. It used to be allocated unconditionally here,
    // which was pure waste: LVGL's double buffering does nothing while flush_cb
    // is synchronous, because the second buffer can only be rendered into while
    // the first is still being sent. That is DP8_CHECKLIST F3.
    s_buf2 = NULL;
    ESP_LOGW(TAG, "[perf] draw buffer: PSRAM, %d lines (%u bytes), %d render passes per screen",
             CUPCAKE_BUF_LINES, (unsigned)buf_bytes,
             (s_disp_h + CUPCAKE_BUF_LINES - 1) / CUPCAKE_BUF_LINES);

    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res      = (lv_coord_t)s_disp_w;
    s_disp_drv.ver_res      = (lv_coord_t)s_disp_h;
    s_disp_drv.flush_cb     = flush_cb;
    s_disp_drv.draw_buf     = &s_draw_buf;
    s_disp_drv.full_refresh = 0;

    // Before drv_register, because it may allocate s_buf2 and the draw buffer
    // has to be initialised with both pointers already decided.
    purr_lv_flush_init(&s_flush, &s_disp_drv, s_disp_w, s_disp_h,
                       &s_buf2, buf_bytes, TAG);

    lv_disp_draw_buf_init(&s_draw_buf, s_buf1, s_buf2, s_disp_w * CUPCAKE_BUF_LINES);

    lv_disp_t *lv_disp = lv_disp_drv_register(&s_disp_drv);
    purr_lv_flush_install_theme(&s_flush, lv_disp, TAG);

    const catcall_touch_t *touch = purr_kernel_touch();
    if (touch) {
        lv_indev_drv_init(&s_touch_drv);
        s_touch_drv.type    = LV_INDEV_TYPE_POINTER;
        s_touch_drv.read_cb = touch_read_cb;
        lv_indev_drv_register(&s_touch_drv);
        ESP_LOGI(TAG, "touch input registered");
    } else {
        ESP_LOGW(TAG, "no touch catcall — cupcake will run touchless");
    }

    if (purr_kernel_input_count() > 0) {
        s_kb_group = lv_group_create();
        lv_indev_drv_init(&s_keypad_drv);
        s_keypad_drv.type    = LV_INDEV_TYPE_KEYPAD;
        s_keypad_drv.read_cb = keypad_read_cb;
        lv_indev_t *kp_indev = lv_indev_drv_register(&s_keypad_drv);
        lv_indev_set_group(kp_indev, s_kb_group);
        ESP_LOGI(TAG, "keypad input registered (%d input driver(s))", purr_kernel_input_count());
    } else {
        ESP_LOGW(TAG, "no input catcall — physical keyboard unavailable");
    }

    // lv_tick_inc(5) is driven entirely by cupcake_task's own render loop
    // (cupcake_module.c) now, under purr_kernel_ui_lock() — this HAL used
    // to spawn a second, separate task ("cupcake_tick") calling
    // lv_tick_inc(5) on the same 5ms cadence with no lock at all, which
    // both double-counted LVGL's tick rate (running its animation/timer
    // clock at ~2x the intended speed the entire time) and raced
    // cupcake_task's own lv_tick_inc()/lv_timer_handler() calls — a real,
    // unsynchronized concurrent access to shared LVGL state, confirmed via
    // investigation into a live LoadProhibited crash (a looping .meow
    // script's Lua VM state got corrupted, most likely as a downstream
    // effect of this race). Removed rather than locked — cupcake_task
    // already does this correctly, so there is nothing left for a second
    // task to contribute.

    // Must be "now", not left at its 0 initializer — boot (WiFi/BT/LoRa
    // bring-up) can easily take longer than the shortest idle timeout
    // (1 minute), which would otherwise make cupcake_ui.c's idle check
    // see an already-expired timer and lock the screen before the user
    // ever touches it.
    s_last_activity_ms = purr_kernel_uptime_ms();

    ESP_LOGI(TAG, "HAL init complete");
    return 0;
}

uint16_t cupcake_hal_width(void)  { return s_disp_w; }
uint16_t cupcake_hal_height(void) { return s_disp_h; }

lv_group_t *cupcake_hal_keypad_group(void) { return s_kb_group; }
