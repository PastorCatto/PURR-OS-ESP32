// nougat_module.c — Nougat kernel module entry
//
// Ported from cupcake_module.c — same task/lock/tick shape and stack-safety
// rationale (LVGL render/dispatch loop must run on a static internal-RAM
// stack, not a PSRAM-eligible one — see cupcake_module.c's own comment for
// the full crash trail this was found from). Phase 1 (this pass) has no
// chrome yet — nougat_ui_init()/tick() don't exist until Nougat Phase 2, so
// this task loop drives lv_timer_handler() directly with nothing built on
// top of it. purr_kernel_register_ui() (via nougat_win_register()) is
// enough for real apps to open/run windows under Nougat already.

#include "nougat.h"
#include "../../kernel/core/purr_module.h"
#include "../../kernel/core/purr_kernel.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "nougat";

static TaskHandle_t s_task = NULL;

// Same sizing rationale as cupcake_module.c's CUPCAKE_STACK_SIZE: static
// internal-RAM stack, not dynamically allocated, so a widget callback that
// disables the flash cache (NVS/VFS access) never runs on a PSRAM-backed
// stack.
#define NOUGAT_STACK_SIZE 8192
static StackType_t  s_nougat_stack[NOUGAT_STACK_SIZE];
static StaticTask_t s_nougat_tcb;

static void nougat_task(void *arg)
{
    (void)arg;

    while (!purr_kernel_boot_ready()) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    while (1) {
        purr_kernel_ui_lock();
        lv_tick_inc(5);
        int64_t t0 = esp_timer_get_time();
        lv_timer_handler();
        int64_t handler_us = esp_timer_get_time() - t0;
        // Same "dropped frame" diagnostic threshold as cupcake_module.c's
        // identical check.
        if (handler_us > 50000) {
            ESP_LOGW(TAG, "lv_timer_handler() took %lldms", (long long)(handler_us / 1000));
        }
        purr_kernel_ui_unlock();
        purr_kernel_ui_heartbeat();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

int nougat_init(void)
{
#ifndef CONFIG_PURR_UI_BACKEND_NOUGAT
    ESP_LOGI(TAG, "Nougat built-in but not selected for this device — skipping");
    return 0;
#endif

    if (purr_kernel_ui()) {
        ESP_LOGW(TAG, "UI catcall already registered — skipping Nougat");
        return 0;
    }

    if (nougat_hal_init() != 0) return -1;

    nougat_win_register();

    // Pinned to core 1 — same placement rationale as cupcake_module.c's
    // cupcake_task (grouped with mesh/mc tasks, away from core 0's app
    // tasks + WiFi/BT). Tab5 has no mesh radio yet (Phase 2 of the ESP32-P4
    // plan, unrelated to Nougat), but there's no reason to diverge from the
    // established placement for a plain LVGL render task.
    s_task = xTaskCreateStaticPinnedToCore(nougat_task, "nougat", NOUGAT_STACK_SIZE, NULL, 4,
                                            s_nougat_stack, &s_nougat_tcb, 1);
    if (!s_task) {
        ESP_LOGE(TAG, "xTaskCreateStatic failed for nougat task");
        return -1;
    }

    ESP_LOGI(TAG, "Nougat ready (%ux%u)", nougat_hal_width(), nougat_hal_height());
    return 0;
}

void nougat_deinit(void)
{
    if (s_task) { vTaskDelete(s_task); s_task = NULL; }
#if LV_ENABLE_GC || !LV_MEM_CUSTOM
    lv_deinit();
#endif
}

// ── Module header ─────────────────────────────────────────────────────────────

PURR_MODULE_REGISTER(nougat) = {
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_UI,
    .load_priority     = PURR_PRIORITY_IMPORTANT,
    .name              = "nougat",
    .version           = "0.1.0",
    .kernel_min        = "0.11.1",
    .kernel_max        = "",
    .provided_catcalls = 0,
    .required_catcalls = CATCALL_FLAG_DISPLAY,
    .init              = nougat_init,
    .deinit            = nougat_deinit,
};
