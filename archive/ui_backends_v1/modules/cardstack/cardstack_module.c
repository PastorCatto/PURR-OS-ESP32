// cardstack_module.c — Cardstack kernel module entry

#include "cardstack.h"
#include "../../kernel/core/purr_module.h"
#include "../../kernel/core/purr_kernel.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "cardstack";

static TaskHandle_t s_task = NULL;

static void cardstack_task(void *arg)
{
    (void)arg;

    // This task is higher-priority than app_main's boot task and can start
    // running the instant xTaskCreate() returns — before app_manager's
    // registry is final (it's only rebuilt once boot.c finishes loading
    // every static module/app). Wait for that signal before building any
    // app cards, or the stack would permanently bake in a 0-app list.
    while (!purr_kernel_boot_ready()) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    cardstack_ui_init();

    uint32_t tick = 0;
    while (1) {
        // Held for the whole LVGL-touching slice of the loop, not just
        // lv_timer_handler() — purr_win.h's dispatch macros take the same
        // lock from whatever task an app updates its UI from, so this is
        // what actually prevents that update from landing mid-frame here.
        purr_kernel_ui_lock();
        lv_tick_inc(5);
        lv_timer_handler();
        cardstack_ui_poll_trackball();             // every tick — see cardstack_ui.c
        if (++tick % 40 == 0) cardstack_ui_tick();  // ~200ms
        purr_kernel_ui_unlock();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

int cardstack_init(void)
{
#ifndef CONFIG_PURR_UI_BACKEND_CARDSTACK
    ESP_LOGI(TAG, "Cardstack built-in but not selected for this device — skipping");
    return 0;
#endif

    if (purr_kernel_ui()) {
        ESP_LOGW(TAG, "UI catcall already registered — skipping Cardstack");
        return 0;
    }

    if (cardstack_hal_init() != 0) return -1;

    extern void cardstack_win_register(void);
    cardstack_win_register();

    BaseType_t ret = xTaskCreate(cardstack_task, "cardstack", 12288, NULL, 4, &s_task);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "failed to create cardstack task");
        return -1;
    }

    ESP_LOGI(TAG, "Cardstack ready (%ux%u)", cardstack_hal_width(), cardstack_hal_height());
    return 0;
}

void cardstack_deinit(void)
{
    if (s_task) { vTaskDelete(s_task); s_task = NULL; }
    // lv_deinit() only exists when LV_MEM_CUSTOM is off (or GC is on) — see
    // lv_obj.c's matching guard. Cardstack never actually gets unloaded at
    // runtime today, so this is defensive rather than load-bearing.
#if LV_ENABLE_GC || !LV_MEM_CUSTOM
    lv_deinit();
#endif
}

// ── Module header ─────────────────────────────────────────────────────────────

PURR_MODULE_REGISTER(cardstack) = {
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_UI,
    .load_priority     = PURR_PRIORITY_IMPORTANT,
    .name              = "cardstack",
    .version           = "1.0.0",
    .kernel_min        = "0.11.1",
    .kernel_max        = "",
    .provided_catcalls = 0,
    .required_catcalls = CATCALL_FLAG_DISPLAY,
    .init              = cardstack_init,
    .deinit            = cardstack_deinit,
};
