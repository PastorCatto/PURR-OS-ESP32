// moy_app.c — the moy console as a PURR OS app.
//
// This file is the whole platform binding, and it is short because
// apps/common/purr_port.h does the translating. Compare it with DOOM's
// compat/, which predates that header and hand-rolls the same five things.
//
// Lifecycle: app_manager enters speed demon before init() (because of
// .speed_demon = 1), we own the panel until the cart quits, and
// app_manager_notify_exited() puts the OS back.

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "purr_kernel.h"
#include "purr_module.h"
#include "app_manager.h"

#include "moy.h"

static const char *TAG = "moy";

static TaskHandle_t  s_task;
static volatile bool s_running;

// ── Persistent state (spec 9 — pmem) ────────────────────────────────────────
//
// 64 int32 slots per cart, keyed by cart title so two carts do not share a save.
// NVS rather than a file on the card: it survives the card being removed, and
// it is transactional, which a half-written save file on a device that can lose
// power mid-frame is not.
#define MOY_NVS_NS "moy"

static void pmem_key(char *out, size_t n)
{
    // NVS keys are limited to 15 characters, so a long cart title is truncated.
    // Collisions between two carts whose titles agree in the first 12 characters
    // are possible and accepted: the alternative is hashing, which makes the
    // stored blob unidentifiable when debugging.
    snprintf(out, n, "pm_%.12s", g_moy.title);
}

static void pmem_load(void)
{
    nvs_handle_t h;
    if (nvs_open(MOY_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;

    char key[16];
    pmem_key(key, sizeof(key));
    size_t sz = sizeof(g_moy.pmem);
    if (nvs_get_blob(h, key, g_moy.pmem, &sz) == ESP_OK)
        ESP_LOGI(TAG, "pmem restored (%u bytes)", (unsigned)sz);
    nvs_close(h);
}

static void pmem_save(void)
{
    if (!g_moy.pmem_dirty) return;

    nvs_handle_t h;
    if (nvs_open(MOY_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;

    char key[16];
    pmem_key(key, sizeof(key));
    if (nvs_set_blob(h, key, g_moy.pmem, sizeof(g_moy.pmem)) == ESP_OK) {
        nvs_commit(h);
        ESP_LOGI(TAG, "pmem saved");
    }
    nvs_close(h);
    g_moy.pmem_dirty = false;
}

// ── Tick loop (spec 5) ──────────────────────────────────────────────────────

static void run_cart(void)
{
    const int64_t period_us = 1000000 / g_moy.fps;
    int64_t next  = esp_timer_get_time();
    int64_t prev  = next;

    int      frames = 0;
    int64_t  fps_t0 = next;

    moy_lua_call_init();

    while (s_running && !g_moy.quit && moy_lua_ok()) {
        int64_t now = esp_timer_get_time();

        // dt in seconds, clamped. A long stall — an SD read, a GC pause — would
        // otherwise hand the cart a huge dt and teleport everything through
        // walls. Clamping to two frames makes a slow host look slow rather than
        // broken, which is the behaviour spec 5 asks for.
        float dt = (float)(now - prev) / 1000000.0f;
        if (dt > 2.0f / g_moy.fps) dt = 2.0f / g_moy.fps;
        prev = now;

        moy_input_poll();

        // The host owns exit (spec 7.3). Checked before _update so the gesture
        // works even on a cart that is busy or misbehaving, and honoured
        // regardless of whether the cart implements quit() — most do not.
        const char *how = NULL;
        if (moy_exit_requested(&how)) {
            ESP_LOGI(TAG, "host exit gesture (%s) - leaving '%s'", how, g_moy.title);
            break;
        }

        moy_lua_call_update(dt);
        if (!moy_lua_ok()) break;

        moy_lua_call_draw();
        purr_port_present(&g_port);

        purr_port_heartbeat();      // 1Hz-throttled inside; safe per frame

        // Frame pacing. Absolute deadline rather than "sleep period_us", so a
        // frame that overruns is absorbed instead of compounding into drift.
        next += period_us;
        int64_t slack = next - esp_timer_get_time();
        if (slack > 0) {
            vTaskDelay(pdMS_TO_TICKS((uint32_t)(slack / 1000)));
        } else {
            // Behind. Give the scheduler a tick anyway — this task is pinned to
            // core 1 at a priority above idle, and never yielding would starve
            // the watchdog on a cart that simply cannot hit its frame rate.
            next = esp_timer_get_time();
            vTaskDelay(1);
        }

        if (++frames >= 120) {
            int64_t el = esp_timer_get_time() - fps_t0;
            ESP_LOGI(TAG, "[perf] %.1f fps (target %d)",
                     frames * 1000000.0 / (double)el, g_moy.fps);
            frames = 0;
            fps_t0 = esp_timer_get_time();
        }
    }

    if (!moy_lua_ok()) {
        // spec 4.3: a cart error stops the cart, not the console. Show it.
        ESP_LOGE(TAG, "cart error: %s", moy_lua_error());
        purr_port_fail_screen("MOY", moy_lua_error(), "Press any key to exit");
    }
}

// ── Task ────────────────────────────────────────────────────────────────────

static void moy_task(void *arg)
{
    (void)arg;

    size_t psram_at_start = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "internal free %u, psram free %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)psram_at_start);

    // Every static in this app survives the app exiting — it is compiled into
    // the firmware, so its lifetime is the boot, not the launch. Anything
    // stateful must therefore be cleared HERE rather than relying on
    // zero-initialisation, which only happens once.
    moy_input_reset();

    // 8bpp: the console is palette-indexed, so this is exactly the fb8 path.
    // purr_port handles the panel size, the internal-then-PSRAM fallback and
    // the "which did I get" log line.
    if (!purr_port_open(&g_port, 8)) {
        ESP_LOGE(TAG, "purr_port_open failed");
        goto done;
    }
    purr_port_set_palette_rgb888(&g_port, moy_palette_rgb888, MOY_PAL_N);

    // Draw state must be live BEFORE anything draws, and the picker draws.
    //
    // This used to be done only inside moy_cart_load(), which runs after the
    // picker — so the picker rendered against a zeroed g_moy: clip_w/clip_h
    // were 0, so plot() rejected every pixel, and pal_map[1] was 0, so cls(1)
    // filled with black. The result was a completely black menu that still
    // responded to input, which is a confusing thing to look at.
    moy_draw_reset();

    purr_splash_show("MOY", 1);
    purr_splash_status("Looking for a cart...");
    purr_port_heartbeat();

    // Scan first, then let the player choose. moy_cart_scan() only reads the
    // directory, so a card with a dozen carts still reaches the picker quickly —
    // manifests and art are parsed for the ONE cart that gets chosen.
    static moy_cart_list_t carts;
    moy_err_t err = moy_cart_scan(&carts);
    if (err != MOY_OK) {
        // Recoverable failure: the launcher is unloaded, so this must draw
        // something and wait rather than returning into a black screen.
        purr_port_fail_screen("MOY", moy_err_str(err), "Press any key to exit");
        goto done;
    }

    const char *chosen = moy_menu_pick(&carts);
    if (!chosen) { ESP_LOGI(TAG, "no cart chosen - leaving"); goto done; }

    err = moy_cart_load(chosen);
    if (err != MOY_OK) {
        purr_port_fail_screen("MOY", moy_err_str(err), "Press any key to exit");
        goto done;
    }

    purr_splash_status(g_moy.title);
    pmem_load();

    if (!moy_lua_start()) {
        purr_port_fail_screen("MOY", moy_lua_error(), "Press any key to exit");
        goto done;
    }

    ESP_LOGI(TAG, "running '%s' at %d fps", g_moy.title, g_moy.fps);
    run_cart();

    pmem_save();
    moy_lua_stop();

done:
    moy_cart_free();
    purr_port_close(&g_port);

    // Leak check, on every exit path. A ported app that owns megabytes of PSRAM
    // and can be relaunched is exactly where a leak hides: it looks fine once,
    // and kills the device on the fourth or fifth run. Measured at the same
    // point as psram_at_start, so the two are directly comparable.
    {
        size_t psram_at_end = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        if (psram_at_end + 4096 < psram_at_start) {
            ESP_LOGE(TAG, "[mem] LEAKED %u bytes of PSRAM this run (%u -> %u)",
                     (unsigned)(psram_at_start - psram_at_end),
                     (unsigned)psram_at_start, (unsigned)psram_at_end);
        } else {
            ESP_LOGI(TAG, "[mem] clean: psram %u -> %u",
                     (unsigned)psram_at_start, (unsigned)psram_at_end);
        }
    }

    s_running = false;
    // Restores everything speed demon unloaded. Must run on every path out,
    // including the failures above, or the OS is stranded with no launcher.
    app_manager_notify_exited("moy");
    s_task = NULL;
    vTaskDelete(NULL);
}

// ── Module ──────────────────────────────────────────────────────────────────

static int moy_init(void)
{
    if (s_task) return 0;
    s_running = true;

    // 16KB, INTERNAL. Lua's own heap is routed to PSRAM by moy_alloc(), so this
    // stack only carries the C call depth — the interpreter's recursion through
    // pcall plus our verb frames. It must not be a PSRAM stack: exiting writes
    // NVS (pmem_save, and speed demon's own marker), writing NVS disables the
    // flash cache, and a task on a PSRAM stack then faults on its own stack.
    if (xTaskCreatePinnedToCore(moy_task, "moy", 16 * 1024, NULL, 4, &s_task, 1) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        s_running = false;
        return -1;
    }
    return 0;
}

static void moy_deinit(void)
{
    s_running = false;
    for (int i = 0; i < 150 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(20));
}

PURR_MODULE_REGISTER(moy) = {
    // The console owns the panel; nothing else may draw over it. Safe here
    // because Lua is vendored into this component rather than taken from the
    // lua_runtime module speed demon unloads — see moy.h.
    .speed_demon       = 1,
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_APP,
    .load_priority     = PURR_PRIORITY_OPTIONAL,
    .name              = "moy",
    .version           = "0.1.0",
    .kernel_min        = "0.11.1",
    .kernel_max        = "",
    .provided_catcalls = 0,
    .required_catcalls = CATCALL_FLAG_DISPLAY,
    .init              = moy_init,
    .deinit            = moy_deinit,
};
