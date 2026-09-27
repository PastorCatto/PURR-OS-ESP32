// lvgldebug_module.c — LVGL Debug kernel module entry
//
// Not a real desktop shell — no catcall_ui_t, no apps. Purely a diagnostic
// screen for verifying touch mapping on a new hardware/kernel combo before
// building a real UI on top of it.

#include "lvgldebug.h"
#include "../../kernel/core/purr_module.h"
#include "../../kernel/core/purr_kernel.h"
#include "lvgl.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "lvgldebug";

static TaskHandle_t s_task = NULL;

static void lvgldebug_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "task started");
    lvgldebug_screen_init();
    ESP_LOGI(TAG, "screen init done");
    while (1) {
        // lv_tick_inc() is driven by lvgldebug_hal.c's own tick task.
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

int lvgldebug_init(void)
{
#ifndef CONFIG_PURR_UI_BACKEND_LVGLDEBUG
    ESP_LOGI(TAG, "LVGL Debug built-in but not selected for this device — skipping");
    return 0;
#endif

    if (lvgldebug_hal_init() != 0) return -1;
    ESP_LOGI(TAG, "HAL done, creating task");

    BaseType_t ret = xTaskCreate(lvgldebug_task, "lvgldebug", 8192, NULL, 4, &s_task);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "failed to create lvgldebug task");
        return -1;
    }

    ESP_LOGI(TAG, "LVGL Debug ready (%ux%u)", lvgldebug_hal_width(), lvgldebug_hal_height());
    return 0;
}

void lvgldebug_deinit(void)
{
    if (s_task) { vTaskDelete(s_task); s_task = NULL; }
    // lv_deinit() only exists when LV_MEM_CUSTOM is off (or GC is on) — see
    // lv_obj.c's matching guard.
#if LV_ENABLE_GC || !LV_MEM_CUSTOM
    lv_deinit();
#endif
}

// ── Module header ─────────────────────────────────────────────────────────────

PURR_MODULE_REGISTER(lvgldebug) = {
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_UI,
    .load_priority     = PURR_PRIORITY_IMPORTANT,
    .name              = "lvgldebug",
    .version           = "1.0.0",
    .kernel_min        = "0.11.1",
    .kernel_max        = "",
    .provided_catcalls = 0,
    .required_catcalls = CATCALL_FLAG_DISPLAY,
    .init              = lvgldebug_init,
    .deinit            = lvgldebug_deinit,
};
