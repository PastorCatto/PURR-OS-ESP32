// cheetah_module.c — Cheetah kernel module entry + render loop.
//
// Same task/lock/tick shape as mochi_module.c/cupcake_module.c, for the same
// reasons — see mochi_hal.c's stack-placement comment, which applies
// unchanged here (this is core UI-kernel dispatch, not "just another app
// task", so the stack is static INTERNAL RAM, not PSRAM).
//
// Deliberately does NOT carry mochi_module.c's frame-time histogram
// (frame_record() and friends) — that instrumentation is explicitly marked
// TEMPORARY/DP8_CHECKLIST-specific in its own comments there, not a pattern
// meant to propagate to every new backend. If Cheetah ever needs to measure
// "light on performance" empirically, the technique is available to borrow,
// but it isn't baked into this file by default.

#include "cheetah.h"
#include "../../kernel/core/purr_module.h"
#include "../../kernel/core/purr_kernel.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "cheetah";

static TaskHandle_t s_task = NULL;

// See mochi_hal.c's identical comment for why this must be static internal
// RAM, not PSRAM: every app widget callback ultimately runs on this task
// (lv_timer_handler -> lv_event_send -> the app's purr_win_cb_t), including
// ones that briefly disable the flash cache — a PSRAM stack is unreachable
// while that's true.
#define CHEETAH_STACK_SIZE 8192
static StackType_t  s_cheetah_stack[CHEETAH_STACK_SIZE];
static StaticTask_t s_cheetah_tcb;

static void cheetah_task(void *arg)
{
    (void)arg;

    // app_manager's registry must be final before Cheetah builds its All Apps
    // pane from it — see mochi_task's identical wait.
    while (!purr_kernel_boot_ready()) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    cheetah_home_init();   // login itself is systemui's job now — see cheetah.h

#ifdef CONFIG_ESP_TASK_WDT_EN
    esp_task_wdt_add(NULL);
#endif

    uint32_t tick = 0;
    while (1) {
        purr_kernel_ui_lock();
        lv_tick_inc(5);
        purr_kernel_ui_breadcrumb("timer_handler");
        int64_t t0 = esp_timer_get_time();
        lv_timer_handler();
        int64_t handler_us = esp_timer_get_time() - t0;
        // Genuine-hang tripwire only — see mochi_module.c's identical guard.
        if (handler_us > 500000) {
            ESP_LOGE(TAG, "lv_timer_handler() STALLED %lldms (tick=%lu)",
                     (long long)(handler_us / 1000), (unsigned long)tick);
        }
        if (++tick % 40 == 0) {
            purr_kernel_ui_breadcrumb("home_tick");
            cheetah_home_tick();   // ~200ms
        }
        purr_kernel_ui_breadcrumb("idle");
        purr_kernel_ui_unlock();
        purr_kernel_ui_heartbeat();
#ifdef CONFIG_ESP_TASK_WDT_EN
        esp_task_wdt_reset();
#endif
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

int cheetah_init(void)
{
#ifndef CONFIG_PURR_UI_BACKEND_CHEETAH
    ESP_LOGI(TAG, "Cheetah built-in but not selected for this device — skipping");
    return 0;
#endif

    if (purr_kernel_ui()) {
        ESP_LOGW(TAG, "UI catcall already registered — skipping Cheetah");
        return 0;
    }

    if (cheetah_hal_init() != 0) return -1;

    cheetah_win_register();

    // Core 1, same placement as mochi_task/cupcake_task — groups with the
    // mesh tasks, away from core 0's app tasks + WiFi/BT.
    s_task = xTaskCreateStaticPinnedToCore(cheetah_task, "cheetah", CHEETAH_STACK_SIZE, NULL, 4,
                                            s_cheetah_stack, &s_cheetah_tcb, 1);
    if (!s_task) {
        ESP_LOGE(TAG, "xTaskCreateStatic failed for cheetah task");
        return -1;
    }

    ESP_LOGI(TAG, "Cheetah ready (%ux%u)", cheetah_hal_width(), cheetah_hal_height());
    return 0;
}

void cheetah_deinit(void)
{
    // Drain any in-flight async flush BEFORE deleting the task, and
    // unsubscribe the watchdog before that — see mochi_deinit()'s identical
    // ordering comment for why both matter (an unbalanced SPI bus
    // acquire/release, and a watchdog panicking over a task that no longer
    // exists).
    cheetah_hal_wait_flush_idle();

    if (s_task) {
#ifdef CONFIG_ESP_TASK_WDT_EN
        esp_task_wdt_delete(s_task);
#endif
        vTaskDelete(s_task);
        s_task = NULL;
    }

    // Release the UI catcall, or this module can never be loaded again — see
    // mochi_deinit()'s identical comment on why this must come after
    // vTaskDelete, not before.
    cheetah_win_unregister();

#if LV_ENABLE_GC || !LV_MEM_CUSTOM
    lv_deinit();
#endif
}

PURR_MODULE_REGISTER(cheetah) = {
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_UI,
    .load_priority     = PURR_PRIORITY_IMPORTANT,
    .name              = "cheetah",
    .version           = "0.1.0",
    .kernel_min        = "1.0.0",
    .kernel_max        = "",
    .provided_catcalls = 0,
    .required_catcalls = CATCALL_FLAG_DISPLAY,
    .init              = cheetah_init,
    .deinit            = cheetah_deinit,
};
