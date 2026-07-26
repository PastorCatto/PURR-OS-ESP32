// mochi_hal.c — LVGL ↔ catcall_display / catcall_touch / catcall_input bridge.
//
// Three input devices, deliberately kept separate rather than merged into one
// read_cb the way cupcake_hal.c does it:
//
//   touch     → LV_INDEV_TYPE_POINTER   (absolute x/y, press state)
//   keyboard  → LV_INDEV_TYPE_KEYPAD    (characters + Enter/Esc/Backspace)
//   trackball → LV_INDEV_TYPE_KEYPAD    (arrow keys + Enter on click)
//
// The split matters. cupcake_hal.c polls *every* registered input catcall in
// one callback and rewrites trackball deltas into synthetic LV_KEY_PREV/NEXT
// presses, which loses the distinction between "the user typed a character"
// and "the user rolled the ball" — the two arrive indistinguishable. Keeping
// them on separate indevs keeps that distinction at the LVGL level.
//
// The trackball is a KEYPAD emitting the four arrow keys, not an ENCODER.
// An encoder is one-dimensional (a signed enc_diff LVGL renders as LEFT/RIGHT),
// so folding a two-axis ball onto it loses the axis — which shipped once and
// showed up as the cursor moving sideways when the ball was rolled up. See
// ball_read_cb below. Tabby still uses an encoder, correctly, because its shell
// is a single-column list where one axis is all there is.
//
// Which physical driver is which is decided by capability, not by name: a
// keyboard-class driver implements set_backlight (bbq20's under-key LEDs), a
// trackball does not. Same test cupcake_win.c's ck_has_physical_keyboard()
// already uses. A device with only one of the two still works — the missing
// indev simply isn't registered.

#include "mochi.h"
#include "../systemui/systemui.h"
#include "../../kernel/core/purr_kernel.h"
#include "../../kernel/catcalls/catcall_input.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "mochi_hal";

// Rows of pixels per flush buffer. Same PSRAM+DMA reasoning as
// cupcake_hal.c's equivalent: these are pure pixel payloads pushed out over
// SPI DMA, nothing that needs scarce internal SRAM, and ESP32-S3's GDMA can
// DMA straight out of PSRAM. MALLOC_CAP_DMA alongside MALLOC_CAP_SPIRAM is
// what actually guarantees a DMA-usable PSRAM allocation — MALLOC_CAP_SPIRAM
// on its own does not.
// Sized to FIT THE DATA CACHE, which is the constraint that actually binds here.
//
// CONFIG_ESP32S3_DATA_CACHE_SIZE is 0x8000 = 32,768 bytes on this device. At 80
// lines the buffer is 320 x 80 x 2 = 51,200 bytes — 1.6x the entire data cache,
// and it lives in PSRAM. Every render pass therefore streams the whole buffer
// through a cache too small to hold any of it, so nothing survives to be reused
// and every access is a miss.
//
// 48 lines is 320 x 48 x 2 = 30,720 bytes, which fits with headroom, and gives 5
// passes per screen (240 / 48).
//
// This is a deliberate middle between two measured points, not a guess:
//   16 lines (15 passes) — much WORSE. Per-pass cost dominates: each pass
//                          re-walks the object tree and re-clips every widget.
//                          Measured fps 48-72 vs 125-160.
//   80 lines (3 passes)  — fewest passes, but exceeds the data cache.
// If per-pass cost and cache residency are both real, the optimum is between
// them, and 48 is the largest size that still fits the cache.
//
// Re-measure with the frame histogram (mochi_module.c) before changing this.
// Both earlier buffer-size conclusions in this project were drawn from a broken
// instrument — see DP8_CHECKLIST.md F12.
#ifndef MOCHI_BUF_LINES
#define MOCHI_BUF_LINES 48
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

uint64_t mochi_hal_last_activity_ms(void) { return s_last_activity_ms; }
lv_group_t *mochi_hal_group(void)         { return s_group; }
bool mochi_hal_has_physical_keyboard(void) { return s_kbd != NULL; }

// Set once at init if the display driver offers the async push pair. Kept as a
// separate flag rather than re-testing the members every flush so the hot path
// stays a single branch.
static bool s_async_flush = false;

// Fired by the display driver once a band has actually gone out. This is what
// tells LVGL it may reuse the buffer it handed us, and it is the entire reason
// double buffering does anything here.
//
// Runs on the driver's completion task, NOT an ISR — see catcall_display.h. The
// only thing it may touch is the flush-ready signal.
static void flush_done(void *user)
{
    lv_disp_flush_ready((lv_disp_drv_t *)user);
}

static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p)
{
    const catcall_display_t *d = purr_kernel_display();
    if (!d) { lv_disp_flush_ready(drv); return; }

    int32_t w = area->x2 - area->x1 + 1;
    int32_t h = area->y2 - area->y1 + 1;

    // Async: hand the band over and return NOW. LVGL immediately starts
    // rendering the next band into the other draw buffer while this one is
    // still being clocked out over SPI, and flush_done() releases it when the
    // transfer finishes. On T-Deck Plus that overlap is worth roughly half the
    // frame, because flushing and rendering were previously strictly serialised.
    if (s_async_flush) {
        d->push_pixels_async(area->x1, area->y1, (int)w, (int)h, (uint16_t *)color_p);
        return;   // flush_done() calls lv_disp_flush_ready(), not us
    }

    if (d->push_pixels) {
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

// Trackball → a SECOND keypad indev emitting real arrow keys.
//
// This was originally an LV_INDEV_TYPE_ENCODER, which was wrong for a grid and
// is worth recording so it doesn't get "simplified" back. LVGL's encoder model
// is one-dimensional: a read reports enc_diff, a signed detent count, and LVGL
// turns that into LV_KEY_LEFT / LV_KEY_RIGHT. Folding a two-axis ball onto that
// single axis throws away which way it actually rolled, so rolling UP moved the
// springboard cursor SIDEWAYS — confirmed on hardware as "the trackball isn't
// mapping correctly".
//
// An encoder is still the right model for a one-dimensional list (Tabby uses
// one deliberately). A springboard is a grid, so both axes have to survive,
// which means emitting the four arrow keys directly. LVGL's keypad handling
// forwards anything that isn't NEXT/PREV to the focused object as LV_EVENT_KEY,
// which is exactly what the springboard's key handler consumes.
//
// This also removes the encoder's edit-mode requirement entirely: edit mode
// only existed to stop LVGL consuming enc_diff as focus-stepping, and a keypad
// has no such behaviour.
//
// Sign convention: trackball.c flips dx/dy for pointer-style feel (see its
// update_state() comment), so dy > 0 means the ball rolled UP and dx > 0 means
// LEFT. Both axes are checked per event, so a diagonal roll produces the
// stronger of the two rather than nothing.
// Note: there is deliberately NO rate limit here any more.
//
// One was added first, when a roll produced three to six cursor steps. That
// turned out to be a driver bug — trackball.c emitted a movement event every
// 120ms for as long as the switch stayed closed, with no distinction between a
// single roll and a deliberate hold — and it is fixed there now (see its
// typematic repeat-model comment). Throttling here as well would just
// double-limit and make the ball feel sluggish, and it papered over a fault
// every other consumer of that driver was also suffering.
//
// What remains is still worth keeping: drain the whole queue per read and sum
// it, so a burst folds into one coherent direction rather than replaying as
// several steps on later reads.
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

    // Drain the whole queue every read. Leaving events queued was the other
    // half of the jumping: the backlog simply replayed as extra steps on
    // subsequent reads instead of being folded into the current motion.
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

    // A click is a discrete intent — never rate-limited, and it wins over any
    // motion drained in the same pass so a slightly-rolled press still opens
    // the thing the user was pointing at.
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

    // Dominant axis wins, so a slightly off-axis roll reads as the direction
    // intended rather than cancelling out. Magnitude is deliberately discarded:
    // the accelerated ±3 means "still held", not "move three cells".
    //
    // The comparison is STRICT, so an exact tie resolves horizontally. It was
    // >= at first, which resolved ties vertically — and because the ball's four
    // directions are separate GPIOs with some mechanical crosstalk, a
    // left/right roll that picked up a single stray vertical tick tied at 1:1
    // and jumped the cursor up or down instead. Reported as "when I swipe left
    // and right, sometimes it'll sprint up and down".
    int32_t ax = sum_x < 0 ? -sum_x : sum_x;
    int32_t ay = sum_y < 0 ? -sum_y : sum_y;
    uint32_t key = (ay > ax) ? ((sum_y > 0) ? LV_KEY_UP   : LV_KEY_DOWN)
                              : ((sum_x > 0) ? LV_KEY_LEFT : LV_KEY_RIGHT);

    s_pending_key     = key;
    s_pending_release = true;
    data->key   = key;
    data->state = LV_INDEV_STATE_PR;
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

int mochi_hal_init(void)
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

    // Draw buffer: ONE buffer, 16 lines, reserved in INTERNAL RAM at link time.
    //
    // Three deliberate departures from the obvious (two big PSRAM buffers), all
    // of them measured — see DP8_CHECKLIST.md F3/F9:
    //
    // INTERNAL, not PSRAM. This buffer is not merely a DMA source; it is LVGL's
    // RENDER TARGET, blended into with heavy read-modify-write, and internal RAM
    // is roughly an order of magnitude better for that pattern. The st7789
    // driver also reads it pixel-by-pixel to byte-swap, so it is read twice per
    // frame by the CPU either way.
    //
    // RESERVED IN .bss, not malloc'd. Asking the heap here is far too late: this
    // runs at t=8.3s, free internal DRAM flatlines at ~1-2KB by ~14s once WiFi,
    // NimBLE and the mesh stack have taken their share, and the allocator
    // refused even 12 lines — silently falling back to PSRAM, i.e. exactly what
    // this is trying to avoid. The linker cannot lose that race. Same technique,
    // and same reason, as this module's static render-task stack.
    //
    // DOUBLE, when the driver supports it. This was single for most of DP8, and
    // correctly so: LVGL's double buffering pays off only if flush_cb returns
    // immediately, and while flush_cb was synchronous a second buffer was memory
    // spent for no benefit whatsoever. That precondition is now met — see
    // catcall_display_t::push_pixels_async — so the second buffer earns its
    // keep: LVGL renders band N+1 into it while band N is still going out.
    //
    // If the driver does not offer the async pair we stay single-buffered, for
    // the original reason, which still holds.
    //
    // ── Why the size is small, and why that is NOT obviously a loss ──────────
    // 16 lines means ~15 flushes per full screen rather than 3. Measured at -Og
    // that was a clear regression (median lv_timer_handler() 146ms -> 164ms) and
    // it was reverted on that basis. That measurement is no longer valid: it
    // predates the -O2 switch, which cut frame time ~3.3x and changed what the
    // extra flushes are being weighed against. The build that measured best on
    // hardware (median 49ms, max 53ms, a single tight 40-59ms band) had THIS
    // buffer configuration in it — the revert was applied afterwards, on the
    // strength of the stale pre--O2 number, and was never re-measured.
    //
    // So: keep the two variables straight. If this is ever changed again,
    // re-measure at the CURRENT optimisation level rather than trusting the
    // paragraph above.
    //
    // A width wider than the array would render past it and corrupt adjacent
    // .bss, so the guard below is load-bearing, not defensive style.
    // EXPERIMENT (DP8_CHECKLIST.md): pass count vs buffer locality.
    //
    // Measured with the FIXED frame instrument, a full-screen redraw costs
    // ~150ms even with effects off — about 470 CPU cycles per pixel, which is
    // an order of magnitude too high to be per-pixel blending. That points at
    // PER-PASS cost, not per-pixel: at 16 lines a full screen is 15 render
    // passes, and every pass re-walks the whole object tree and re-clips every
    // widget against its band.
    //
    // Set to 0 to use the large PSRAM buffer (3 passes, slower memory) instead
    // of the small internal one (15 passes, faster memory). The earlier
    // comparison of these two was made with the old censored per-frame warning
    // and is not trustworthy — see frame_record() in mochi_module.c for why
    // that instrument was misleading.
    #define MOCHI_USE_INTERNAL_DRAW_BUF 0

    #define MOCHI_DRAW_LINES 16
    static lv_color_t s_draw_mem[320 * MOCHI_DRAW_LINES];

    size_t buf_bytes;
    int    lines;

    if (MOCHI_USE_INTERNAL_DRAW_BUF && s_disp_w <= 320) {
        s_buf1    = s_draw_mem;
        buf_bytes = sizeof(s_draw_mem);
        lines     = (int)(sizeof(s_draw_mem) / sizeof(lv_color_t) / s_disp_w);
        ESP_LOGW(TAG, "[perf] draw buffer: INTERNAL .bss, single, %d lines (%u bytes), "
                      "%d passes per screen",
                 lines, (unsigned)buf_bytes, (s_disp_h + lines - 1) / lines);
    } else {
        // Large PSRAM buffer: fewer, larger passes. Also the only option on a
        // panel wider than the static array was sized for.
        lines     = MOCHI_BUF_LINES;
        buf_bytes = sizeof(lv_color_t) * s_disp_w * lines;
        s_buf1    = heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
        ESP_LOGW(TAG, "[perf] draw buffer: PSRAM, single, %d lines (%u bytes), %d passes per screen",
                 lines, (unsigned)buf_bytes, (s_disp_h + lines - 1) / lines);
    }
    if (!s_buf1) {
        ESP_LOGE(TAG, "draw buffer alloc failed (%u bytes)", (unsigned)buf_bytes);
        return -1;
    }
    // Ask the display driver for an async path sized to one flush, and only
    // allocate the second draw buffer if it agrees. Both must succeed together:
    // a second buffer without async flushing is wasted memory, and async
    // flushing without a second buffer gains nothing because LVGL would have
    // nowhere to render while the first is in flight.
    s_buf2 = NULL;
    const catcall_display_t *dsp = purr_kernel_display();
    if (dsp && dsp->push_pixels_async && dsp->flush_done_cb) {
        s_buf2 = heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
        if (s_buf2) {
            dsp->flush_done_cb(flush_done, &s_disp_drv);
            s_async_flush = true;
            ESP_LOGW(TAG, "[perf] async flush + double buffering ON (2 x %u bytes)",
                     (unsigned)buf_bytes);
        } else {
            ESP_LOGW(TAG, "[perf] second draw buffer alloc failed — staying single/sync");
        }
    }

    lv_disp_draw_buf_init(&s_draw_buf, s_buf1, s_buf2, s_disp_w * lines);

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
        ESP_LOGW(TAG, "no touch catcall — Mochi will run keyboard/ball only");
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
        // KEYPAD, not ENCODER — see ball_read_cb's comment for why a grid
        // needs both axes and an encoder cannot carry them.
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

    // Must be "now", not the 0 initializer: boot (WiFi/BT/LoRa bring-up) can
    // outlast the shortest idle timeout, which would make the system UI's idle
    // check see an already-expired timer and lock the screen before the user
    // ever touches the device.
    s_last_activity_ms = purr_kernel_uptime_ms();

    // lv_tick_inc()/lv_timer_handler() are driven solely by mochi_task
    // (mochi_module.c) under purr_kernel_ui_lock(). Deliberately no separate
    // tick task here — cupcake_hal.c used to have one and it both
    // double-counted LVGL's clock and raced the render loop's own LVGL calls.
    ESP_LOGI(TAG, "HAL init complete");
    return 0;
}

uint16_t mochi_hal_width(void)  { return s_disp_w; }
uint16_t mochi_hal_height(void) { return s_disp_h; }
