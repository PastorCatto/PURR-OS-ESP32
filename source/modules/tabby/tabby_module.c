// tabby_module.c — Tabby kernel module entry + render loop.
//
// Same task/lock/tick shape as cupcake_module.c, and for the same reasons —
// see the stack-placement comment below, which is not a stylistic choice.

#include "tabby.h"
#include "../../kernel/core/purr_module.h"
#include "../../kernel/core/purr_kernel.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "tabby";

static TaskHandle_t s_task = NULL;

// Dedicated static INTERNAL-RAM stack. tabby_task is the LVGL render/dispatch
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
#define TABBY_STACK_SIZE 8192
static StackType_t  s_tabby_stack[TABBY_STACK_SIZE];
static StaticTask_t s_tabby_tcb;

static void tabby_task(void *arg)
{
    (void)arg;

    // app_manager's registry must be final before the shell builds its app
    // list from it, or the launcher comes up short a few entries.
    while (!purr_kernel_boot_ready()) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    tabby_shell_init();

    // Explicit self-subscribe rather than the default idle-task watch: the
    // project-wide TWDT is off by default to avoid false positives during long
    // boot/module-init sequences, so tasks opt in individually. This is the
    // task every UI hang ultimately surfaces as, and subscribing gets a real
    // backtrace of whatever it is stuck in instead of only a breadcrumb.
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
        // A single handler call this long means whatever ran inside it stalled
        // rendering for that long — real data to point at instead of guessing,
        // if the UI ever feels sluggish. 50ms ≈ "a dropped frame you'd notice".
        if (handler_us > 50000) {
            ESP_LOGW(TAG, "lv_timer_handler() took %lldms (tick=%lu)",
                     (long long)(handler_us / 1000), (unsigned long)tick);
        }
        if (++tick % 40 == 0) {
            purr_kernel_ui_breadcrumb("shell_tick");
            tabby_shell_tick();   // ~200ms
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

int tabby_init(void)
{
#ifndef CONFIG_PURR_UI_BACKEND_TABBY
    ESP_LOGI(TAG, "Tabby built-in but not selected for this device — skipping");
    return 0;
#endif

    if (purr_kernel_ui()) {
        ESP_LOGW(TAG, "UI catcall already registered — skipping Tabby");
        return 0;
    }

    if (tabby_hal_init() != 0) return -1;

    tabby_win_register();

    // Core 1, grouping with the mesh tasks and away from core 0's app tasks +
    // WiFi/BT — same placement cupcake_task uses.
    s_task = xTaskCreateStaticPinnedToCore(tabby_task, "tabby", TABBY_STACK_SIZE, NULL, 4,
                                            s_tabby_stack, &s_tabby_tcb, 1);
    if (!s_task) {
        ESP_LOGE(TAG, "xTaskCreateStatic failed for tabby task");
        return -1;
    }

    ESP_LOGI(TAG, "Tabby ready (%ux%u)", tabby_hal_width(), tabby_hal_height());
    return 0;
}

void tabby_deinit(void)
{
    // esp_task_wdt_delete() before vTaskDelete(): the task subscribed itself to
    // the TWDT, and deleting it without unsubscribing leaves the watchdog
    // waiting on a handle that no longer exists — it then panics after
    // CONFIG_ESP_TASK_WDT_TIMEOUT_S. Only reachable now that speed demon unloads
    // UI backends at runtime. Harmless if never subscribed.
    if (s_task) {
        #ifdef CONFIG_ESP_TASK_WDT_EN
        esp_task_wdt_delete(s_task);
        #endif
        vTaskDelete(s_task);
        s_task = NULL;
    }
    // lv_deinit() only exists when LV_MEM_CUSTOM is off (or GC is on) — see
    // lv_obj.c's matching guard. Defensive: Tabby is never unloaded at runtime.
#if LV_ENABLE_GC || !LV_MEM_CUSTOM
    lv_deinit();
#endif
}

PURR_MODULE_REGISTER(tabby) = {
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_UI,
    .load_priority     = PURR_PRIORITY_IMPORTANT,
    .name              = "tabby",
    .version           = "0.1.0",
    .kernel_min        = "0.11.1",
    .kernel_max        = "",
    .provided_catcalls = 0,
    .required_catcalls = CATCALL_FLAG_DISPLAY,
    .init              = tabby_init,
    .deinit            = tabby_deinit,
};
