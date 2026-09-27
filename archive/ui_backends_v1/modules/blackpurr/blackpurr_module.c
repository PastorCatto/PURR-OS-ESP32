// blackpurr_module.c — BlackPURR text-mode shell module entry

#include "blackpurr.h"
#include "../../kernel/core/purr_module.h"
#include "../../kernel/core/purr_kernel.h"
#include "../../kernel/catcalls/catcall_touch.h"
#include "../../kernel/catcalls/catcall_input.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "blackpurr";
static TaskHandle_t s_task = NULL;

// Trackball click debounce: synthesize on_click only on the first press
static bool s_click_held = false;

static void blackpurr_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "task started");

    blackpurr_shell_init();

    uint32_t uptime_s  = 0;
    uint32_t last_tick = 0;

    while (1) {
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

        // ── Touch ─────────────────────────────────────────────────────────
        const catcall_touch_t *touch = purr_kernel_touch();
        if (touch) {
            uint16_t tx = 0, ty = 0;
            if (touch->read_point(&tx, &ty))
                blackpurr_shell_on_touch(tx, ty);
        }

        // ── All registered inputs (trackball + BBQ20) ─────────────────────
        int n = purr_kernel_input_count();
        for (int i = 0; i < n; i++) {
            const catcall_input_t *inp = purr_kernel_input_at(i);
            if (!inp || !inp->poll_event) continue;
            input_event_t ev;
            while (inp->poll_event(&ev)) {
                switch (ev.type) {
                    case INPUT_EVENT_POINTER:
                        blackpurr_shell_on_pointer(ev.delta_x, ev.delta_y);
                        break;
                    case INPUT_EVENT_KEY_DOWN:
                        if (ev.keycode == 0x0028) {   // trackball click
                            if (!s_click_held) {
                                s_click_held = true;
                                blackpurr_shell_on_click();
                            }
                        } else {
                            blackpurr_shell_on_key((uint8_t)ev.keycode);
                        }
                        break;
                    case INPUT_EVENT_KEY_UP:
                        if (ev.keycode == 0x0028)
                            s_click_held = false;
                        break;
                    default: break;
                }
            }
        }

        // ── 1-second tick for status bar ──────────────────────────────────
        if (now_ms - last_tick >= 1000) {
            last_tick = now_ms;
            uptime_s++;
            blackpurr_shell_tick(uptime_s);
        }

        vTaskDelay(pdMS_TO_TICKS(16));   // ~60 Hz poll
    }
}

int blackpurr_init(void)
{
#ifndef CONFIG_PURR_UI_BACKEND_BLACKPURR
    return 0;
#endif

    if (purr_kernel_ui()) {
        ESP_LOGW(TAG, "UI catcall already registered — skipping BlackPURR");
        return 0;
    }

    BaseType_t ret = xTaskCreatePinnedToCore(
        blackpurr_task, "blackpurr", 8192, NULL, 4, &s_task, 1
    );
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "failed to create task");
        return -1;
    }

    ESP_LOGI(TAG, "BlackPURR text shell ready (touch=%s inputs=%d)",
             purr_kernel_touch()        ? "yes" : "no",
             purr_kernel_input_count());
    return 0;
}

void blackpurr_deinit(void)
{
    if (s_task) { vTaskDelete(s_task); s_task = NULL; }
}

PURR_MODULE_REGISTER(blackpurr) = {
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_UI,
    .load_priority     = PURR_PRIORITY_IMPORTANT,
    .name              = "blackpurr",
    .version           = "1.0.0",
    .kernel_min        = "0.11.1",
    .kernel_max        = "",
    .provided_catcalls = 0,
    .required_catcalls = CATCALL_FLAG_DISPLAY,
    .init              = blackpurr_init,
    .deinit            = blackpurr_deinit,
};
