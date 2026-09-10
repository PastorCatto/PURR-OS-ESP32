// pounce_module.c — PURR OS .purr module wrapper for Pounce.
//
// Kernel entry point: registers Pounce's catcall_ui_t and runs its own
// input-polling task. Only activates when CONFIG_PURR_UI_BACKEND_POUNCE=y.
#include "../../kernel/core/purr_module.h"
#include "../../kernel/core/purr_kernel.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef CONFIG_PURR_UI_BACKEND_POUNCE
#include "pounce.h"
#endif

static const char *TAG = "pounce";

#ifdef CONFIG_PURR_UI_BACKEND_POUNCE

static TaskHandle_t s_task = NULL;

static void pounce_task(void *arg) {
    (void)arg;
    pw_status_init();
    pw_launcher_draw();   // nothing else draws an initial screen at boot —
                           // the window stack starts empty, so this is the
                           // only thing that will ever paint the launcher
                           // the very first time (later transitions back to
                           // an empty stack go through pounce_win.c's
                           // repaint_new_top_or_blank()).

    while (1) {
        // Serialize against any app task's purr_win_*() calls (purr_kernel_
        // ui_lock()/unlock()) — the same lesson already learned and fixed
        // for miniwin_task() this session: this task's own input polling
        // and redraw calls must not race an app task touching the shared
        // SPI display driver concurrently.
        purr_kernel_ui_lock();
        pw_focus_input_tick();
        pw_status_tick();
        purr_kernel_ui_unlock();
        purr_kernel_ui_heartbeat();

        vTaskDelay(pdMS_TO_TICKS(16));   // ~60Hz
    }
}

static int pounce_init(void) {
    if (purr_kernel_ui()) {
        ESP_LOGW(TAG, "UI catcall already registered — skipping Pounce");
        return 0;
    }
    if (!purr_kernel_display()) {
        ESP_LOGE(TAG, "no display catcall — pounce cannot start");
        return -1;
    }
    if (!purr_kernel_touch()) {
        ESP_LOGI(TAG, "no touch catcall — fine, Pounce is keyboard/trackball-first");
    }

    purr_kernel_register_ui(pounce_win_register());

    BaseType_t ret = xTaskCreate(pounce_task, "pounce", 8192, NULL, 5, &s_task);
    return (ret == pdPASS) ? 0 : -1;
}

static void pounce_deinit(void) {
    if (s_task) {
        vTaskDelete(s_task);
        s_task = NULL;
    }
}

#else  // !CONFIG_PURR_UI_BACKEND_POUNCE

static int pounce_init(void) {
    ESP_LOGI(TAG, "Pounce built-in but not selected for this device — skipping");
    return 0;
}
static void pounce_deinit(void) {}

#endif

// ── .purr module header ───────────────────────────────────────────────────────

PURR_MODULE_REGISTER(pounce) = {
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_UI,
    .name              = "pounce",
    .version           = "1.0.0",
    .kernel_min        = "0.11.1",
    .kernel_max        = "",
    .provided_catcalls = 0,
    .required_catcalls = CATCALL_FLAG_DISPLAY,   // touch is optional
    .init              = pounce_init,
    .deinit            = pounce_deinit,
};
