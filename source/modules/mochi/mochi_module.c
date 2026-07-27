// mochi_module.c — Mochi kernel module entry + render loop.
//
// Same task/lock/tick shape as cupcake_module.c, and for the same reasons —
// see the stack-placement comment below, which is not a stylistic choice.

#include "mochi.h"
#include "../../kernel/core/purr_module.h"
#include "../../kernel/core/purr_kernel.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "mochi";

static TaskHandle_t s_task = NULL;

// Dedicated static INTERNAL-RAM stack. mochi_task is the LVGL render/dispatch
// loop that every app's widget callback ultimately runs on (lv_timer_handler
// -> lv_event_send -> the app's purr_win_cb_t), including callbacks that touch
// NVS/flash — e.g. settings.c writing a theme or brightness value. Loading
// from flash briefly disables the flash cache, and a PSRAM-backed stack is
// unreachable while that is true, so such a callback crashes instantly on a
// PSRAM stack. This is core UI-kernel dispatch structure, not "just another
// app task", and belongs in the same protected category as settings/fileman's
// own static stacks.
//
// 8192 matches the size already proven safe for those, and stays within the
// internal DRAM budget — 12288 here on top of the existing static stacks
// overflowed the segment at link time on this device.
#define MOCHI_STACK_SIZE 8192
static StackType_t  s_mochi_stack[MOCHI_STACK_SIZE];
static StaticTask_t s_mochi_tcb;

// ── Frame-time instrumentation (DP8_CHECKLIST.md) ───────────────────────────
//
// TEMPORARY. Remove with the st7789 flush counters once the perf work is done.
//
// This replaces a per-frame "lv_timer_handler() took Nms" warning, which was
// wrong in two ways that between them produced a genuinely misleading picture:
//
//   1. CENSORED SAMPLE. It only fired above a threshold, so it never observed a
//      fast frame. Its "median" was the median of the SLOW frames only, and
//      comparing that across two builds with different hit rates compares two
//      different populations. It made a build with 239 uniformly-mediocre
//      frames look better than one with 108 slow frames and a fast majority,
//      when the second is arguably the nicer UI.
//   2. IT PERTURBED WHAT IT MEASURED. ESP_LOG writes are synchronous to a
//      115200-baud UART — roughly 5ms of blocked render task per line. Emitting
//      one per slow frame meant the build with more slow frames also paid more
//      logging tax, exaggerating the very difference being measured.
//
// So: bucket EVERY frame, and emit ONE line per window. The histogram is in
// frame-rate terms rather than round numbers, because "how many frames missed
// 60fps / 30fps / 20fps" is the question actually being asked.
#define FRAME_WINDOW      120     // RENDERED frames per log line
                                  // (was 600, which took minutes once
                                  //  no-op iterations stopped counting)
#define FRAME_BUCKETS     8
// Upper bound (ms, exclusive) of each bucket. 8/16 = better than 120/60fps,
// 33 = 30fps, 50 = 20fps, then the hitch territory.
static const uint16_t k_frame_bucket_ms[FRAME_BUCKETS] =
    { 8, 16, 33, 50, 100, 200, 400, 0xFFFF };
static uint32_t s_frame_hist[FRAME_BUCKETS];
static uint32_t s_frame_count;
static uint64_t s_frame_total_us;
static uint32_t s_frame_max_us;
static int64_t  s_frame_window_start_us;
// Iterations where lv_timer_handler() had nothing to do. Reported so the
// render rate can be read against how hard the loop was actually spinning.
static uint32_t s_idle_iters;

// An iteration only counts as a FRAME if LVGL actually drew something.
//
// This threshold is the whole point. lv_timer_handler() returns in microseconds
// when there is nothing to redraw, and the loop spins ~185 times a second doing
// exactly that. Counting those as frames produced "185 fps, 585/600 under 8ms"
// while the screen was visibly managing a few updates a second during a scroll —
// the average was dominated by no-ops and said nothing about rendering at all.
//
// 1ms is comfortably above a no-op handler and far below any real render (the
// cheapest observed is ~15ms).
#define FRAME_DREW_MIN_US 1000

// ── Busy fps: what you actually get while scrolling ─────────────────────────
//
// RENDER-fps below is frames divided by the WHOLE window, idle time included.
// That is the right number for "how busy is the UI overall" and the wrong one
// for "how fast is it while I am dragging", which is the question that matters
// when someone is looking at the screen and it feels slow.
//
// Measured as the GAP BETWEEN CONSECUTIVE RENDERED FRAMES, not as runs of
// back-to-back active iterations. The first attempt used the latter and reported
// a flat zero on hardware: LVGL's refresh period is longer than this loop's poll
// interval, so lv_timer_handler() returns having drawn nothing most times it is
// called. There is essentially ALWAYS an idle iteration between two renders,
// even mid-scroll, so "consecutive active frames" is a state that never occurs.
//
// The gap between renders has no such problem. When the user is dragging, frames
// arrive one after another with only the refresh period between them; when the
// UI is idle, the gap is arbitrarily long. So: any two renders closer together
// than BUSY_GAP_MAX_US are treated as part of the same sustained motion, and the
// reported figure is frames divided by the time actually spent in motion.
//
// Reported alongside RENDER-fps rather than replacing it, because the gap
// between the two is itself informative: if they are close the UI is saturated;
// if BUSY is much higher the device is mostly idling between bursts.
#define BUSY_GAP_MAX_US 250000   // 250ms — well beyond a slow frame, well under idle

static int64_t  s_last_frame_us = 0;
static uint32_t s_busy_frames   = 0;
static uint64_t s_busy_span_us  = 0;
static uint32_t s_busy_worst_us = 0;   // slowest gap counted as motion

static void frame_record(int64_t handler_us)
{
    // Iterations that drew nothing are counted only as loop spin, so the
    // reported fps is renders per second rather than iterations per second.
    if (handler_us < FRAME_DREW_MIN_US) { s_idle_iters++; return; }

    // Fold this frame into the motion total if it followed closely enough on the
    // previous one to be part of the same drag/scroll.
    int64_t frame_now = esp_timer_get_time();
    if (s_last_frame_us) {
        int64_t gap = frame_now - s_last_frame_us;
        if (gap > 0 && gap <= BUSY_GAP_MAX_US) {
            s_busy_frames++;
            s_busy_span_us += (uint64_t)gap;
            if ((uint32_t)gap > s_busy_worst_us) s_busy_worst_us = (uint32_t)gap;
        }
    }
    s_last_frame_us = frame_now;

    uint32_t ms = (uint32_t)(handler_us / 1000);
    int b = 0;
    while (b < FRAME_BUCKETS - 1 && ms >= k_frame_bucket_ms[b]) b++;
    s_frame_hist[b]++;
    s_frame_count++;
    s_frame_total_us += (uint64_t)handler_us;
    if ((uint32_t)handler_us > s_frame_max_us) s_frame_max_us = (uint32_t)handler_us;

    if (s_frame_count < FRAME_WINDOW) return;

    int64_t now  = esp_timer_get_time();
    int64_t span = now - s_frame_window_start_us;
    // Real frames per second over wall time — NOT 1000/mean_handler. The loop
    // also spends a fixed vTaskDelay plus whatever else runs, so deriving fps
    // from handler time alone would overstate it.
    uint32_t fps_x10 = span > 0 ? (uint32_t)((int64_t)s_frame_count * 10000000LL / span) : 0;

    uint32_t busy_x10 = s_busy_span_us > 0
        ? (uint32_t)((uint64_t)s_busy_frames * 10000000ULL / s_busy_span_us) : 0;

    ESP_LOGW(TAG,
        "[frames] rendered=%lu  RENDER-fps=%lu.%lu  BUSY-fps=%lu.%lu (%lu fr, worst gap %lums)  "
        "idle_iters=%lu  mean=%lums  max=%lums  "
        "| <8:%lu 8-16:%lu 16-33:%lu 33-50:%lu 50-100:%lu 100-200:%lu 200-400:%lu 400+:%lu",
        (unsigned long)s_frame_count,
        (unsigned long)(fps_x10 / 10), (unsigned long)(fps_x10 % 10),
        (unsigned long)(busy_x10 / 10), (unsigned long)(busy_x10 % 10),
        (unsigned long)s_busy_frames, (unsigned long)(s_busy_worst_us / 1000),
        (unsigned long)s_idle_iters,
        (unsigned long)(s_frame_total_us / s_frame_count / 1000),
        (unsigned long)(s_frame_max_us / 1000),
        (unsigned long)s_frame_hist[0], (unsigned long)s_frame_hist[1],
        (unsigned long)s_frame_hist[2], (unsigned long)s_frame_hist[3],
        (unsigned long)s_frame_hist[4], (unsigned long)s_frame_hist[5],
        (unsigned long)s_frame_hist[6], (unsigned long)s_frame_hist[7]);

    for (int i = 0; i < FRAME_BUCKETS; i++) s_frame_hist[i] = 0;
    s_frame_count           = 0;
    s_frame_total_us        = 0;
    s_frame_max_us          = 0;
    s_idle_iters            = 0;
    s_frame_window_start_us = now;

    s_busy_frames   = 0;
    s_busy_span_us  = 0;
    s_busy_worst_us = 0;
    // s_last_frame_us is deliberately NOT reset: a scroll spanning a window
    // boundary should keep being measured as one continuous motion.
}

static void mochi_task(void *arg)
{
    (void)arg;

    // app_manager's registry must be final before the shell builds its app
    // list from it, or the launcher comes up short a few entries.
    while (!purr_kernel_boot_ready()) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    mochi_springboard_init();

    // Explicit self-subscribe rather than the default idle-task watch: the
    // project-wide TWDT is off by default to avoid false positives during long
    // boot/module-init sequences, so tasks opt in individually. This is the
    // task every UI hang ultimately surfaces as, and subscribing gets a real
    // backtrace of whatever it is stuck in instead of only a breadcrumb.
    esp_task_wdt_add(NULL);

    // Anchor the first window here, not at 0 — otherwise the first fps figure
    // is computed against all of boot and reads absurdly low.
    s_frame_window_start_us = esp_timer_get_time();

    uint32_t tick = 0;
    while (1) {
        purr_kernel_ui_lock();
        lv_tick_inc(5);
        purr_kernel_ui_breadcrumb("timer_handler");
        int64_t t0 = esp_timer_get_time();
        lv_timer_handler();
        int64_t handler_us = esp_timer_get_time() - t0;
        // Every frame is recorded; one aggregate line is emitted per window.
        // See frame_record() for why the old per-frame threshold warning was
        // both a censored sample and a source of its own distortion.
        frame_record(handler_us);
        // Genuine-hang tripwire only. Well clear of any legitimate frame, so it
        // fires when something is actually stuck rather than merely slow, and
        // it is rare enough that its own UART cost does not matter.
        if (handler_us > 500000) {
            ESP_LOGE(TAG, "lv_timer_handler() STALLED %lldms (tick=%lu)",
                     (long long)(handler_us / 1000), (unsigned long)tick);
        }
        if (++tick % 40 == 0) {
            purr_kernel_ui_breadcrumb("springboard_tick");
            mochi_springboard_tick();   // ~200ms
        }
        purr_kernel_ui_breadcrumb("idle");
        purr_kernel_ui_unlock();
        purr_kernel_ui_heartbeat();
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

int mochi_init(void)
{
#ifndef CONFIG_PURR_UI_BACKEND_MOCHI
    ESP_LOGI(TAG, "Mochi built-in but not selected for this device — skipping");
    return 0;
#endif

    if (purr_kernel_ui()) {
        ESP_LOGW(TAG, "UI catcall already registered — skipping Mochi");
        return 0;
    }

    if (mochi_hal_init() != 0) return -1;

    mochi_win_register();

    // Core 1, grouping with the mesh tasks and away from core 0's app tasks +
    // WiFi/BT — same placement cupcake_task uses.
    s_task = xTaskCreateStaticPinnedToCore(mochi_task, "mochi", MOCHI_STACK_SIZE, NULL, 4,
                                            s_mochi_stack, &s_mochi_tcb, 1);
    if (!s_task) {
        ESP_LOGE(TAG, "xTaskCreateStatic failed for mochi task");
        return -1;
    }

    ESP_LOGI(TAG, "Mochi ready (%ux%u)", mochi_hal_width(), mochi_hal_height());
    return 0;
}

void mochi_deinit(void)
{
    // Unsubscribe from the task watchdog BEFORE deleting the task.
    //
    // esp_task_wdt_add(NULL) in the task above registers its handle with the
    // TWDT. vTaskDelete() does not unregister it, so the watchdog keeps waiting
    // for resets from a task that no longer exists and panics when
    // CONFIG_ESP_TASK_WDT_TIMEOUT_S (4s here) elapses.
    //
    // This never mattered while this module was only unloaded at shutdown. Game
    // mode unloads it at runtime, and the result was a hard panic roughly four
    // seconds into every game-mode session, which then struck the crash guard
    // and rebooted. Harmless if the task was never subscribed - delete just
    // returns ESP_ERR_NOT_FOUND.
    //
    // Drain any asynchronous flush FIRST, for a closely related reason. The SPI
    // bus can now be held across a push_pixels_async() return, so deleting the
    // task mid-transfer leaves the bus acquire/release unbalanced and trips
    // `assert failed: spi_device_release_bus` — the same assert the UI lock
    // used to prevent, back when a flush began and ended inside one call.
    mochi_hal_wait_flush_idle();

    if (s_task) {
        esp_task_wdt_delete(s_task);
        vTaskDelete(s_task);
        s_task = NULL;
    }

    // Release the UI catcall, or this module can never be loaded again.
    //
    // init() above begins with "if (purr_kernel_ui()) skip — something else owns
    // the screen". Leaving the registration in place meant game mode restored
    // Mochi into exactly that branch: the kernel logged it loaded, but no HAL,
    // no launcher and no render task. Six seconds later the crash guard reported
    // "UI TASK UNRESPONSIVE @ idle" — correctly, about a task never created.
    //
    // After vTaskDelete, so nothing can observe a live task with no registration.
    mochi_win_unregister();

    // lv_deinit() only exists when LV_MEM_CUSTOM is off (or GC is on) — see
    // lv_obj.c's matching guard.
#if LV_ENABLE_GC || !LV_MEM_CUSTOM
    lv_deinit();
#endif
}

PURR_MODULE_REGISTER(mochi) = {
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_UI,
    .load_priority     = PURR_PRIORITY_IMPORTANT,
    .name              = "mochi",
    .version           = "0.1.0",
    .kernel_min        = "0.11.1",
    .kernel_max        = "",
    .provided_catcalls = 0,
    .required_catcalls = CATCALL_FLAG_DISPLAY,
    .init              = mochi_init,
    .deinit            = mochi_deinit,
};
