// homebase.c — see homebase.h. Small always-on poll task; touches nothing
// but in-RAM state from pairing.h/proximity.h and purr_kernel_notify()'s
// ring buffer, so a plain internal-RAM stack is fine (no PSRAM/NVS hazard
// to worry about here, unlike pairing_module.c's own persist task).

#include "homebase.h"
#include "pairing.h"
#include "../proximity/proximity.h"
#include "../../kernel/core/purr_kernel.h"
#include "../../kernel/core/purr_module.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "homebase";

#define POLL_MS 2000UL

static TaskHandle_t s_task = NULL;
static bool         s_running = false;
static volatile bool s_present = false;   // cached result of the last poll

static bool check_present(void) {
    uint8_t hb_mac[6];
    if (!pairing_get_home_base(hb_mac)) return false;

    int n = proximity_device_count();
    for (int i = 0; i < n; i++) {
        proximity_device_t d;
        if (proximity_device_at(i, &d) && memcmp(d.mac, hb_mac, 6) == 0) return true;
    }
    return false;
}

static void homebase_task(void *arg) {
    (void)arg;
    bool was_present = false;
    while (s_running) {
        bool now_present = check_present();
        s_present = now_present;

        if (now_present != was_present) {
            purr_kernel_notify(now_present ? "Home base connected" : "Home base disconnected",
                                now_present ? "MSN will relay through it now"
                                            : "MSN is back on the local radio",
                                "homebase");
            ESP_LOGI(TAG, "home base %s", now_present ? "connected" : "disconnected");
            was_present = now_present;
        }

        // Interruptible sleep, not vTaskDelay: homebase_deinit() notifies to
        // wake this immediately so an unload does not have to wait out a full
        // poll interval. Normal operation is unchanged — with no notification
        // this simply times out after POLL_MS.
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(POLL_MS));
    }
    // Clear BEFORE deleting: homebase_deinit() waits on this to know the task
    // has actually gone.
    s_task = NULL;
    vTaskDelete(NULL);
}

bool homebase_is_present(void) { return s_present; }

int homebase_init(void) {
    s_running = true;
    BaseType_t ok = xTaskCreatePinnedToCore(homebase_task, "homebase", 2560, NULL, 2, &s_task, 1);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "failed to create homebase task");
        s_running = false;
        return -1;
    }
    ESP_LOGI(TAG, "ready");
    return 0;
}

void homebase_deinit(void) {
    // Ask the task to stop, wake it, and WAIT for it to actually go.
    //
    // This used to set s_running = false and return immediately, on the
    // reasoning that the task polls the flag and self-deletes so no handoff was
    // needed. That is true in isolation and wrong under Speed Demon, which
    // unloads and restores this module in quick succession:
    //
    //   deinit()  sets s_running = false and returns
    //   ~350ms later, restore calls init(), which sets s_running = true again
    //   the ORIGINAL task is still asleep in its 2000ms poll
    //   it wakes, sees true, and loops forever — orphaned, never deleted
    //
    // Every round trip therefore stranded one 2560-byte stack plus its TCB.
    // Measured at exactly 2908 bytes: unloading homebase freed 0 while
    // restoring it cost 2908, the largest single asymmetry in the cycle.
    //
    // The notify makes this cheap rather than slow — the task wakes at once
    // instead of waiting out a poll interval, so the wait below almost always
    // returns on the first iteration. Bounded at ~2.5s (over POLL_MS) anyway,
    // and giving up is safe: worst case is the old behaviour.
    s_running = false;
    if (s_task) {
        xTaskNotifyGive(s_task);
        for (int i = 0; i < 250 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(10));
        if (s_task) {
            ESP_LOGW(TAG, "task did not exit — leaving it running");
            s_task = NULL;
        }
    }
}

// ── Module header ─────────────────────────────────────────────────────────

PURR_MODULE_REGISTER(homebase) = {
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_SYSTEM,
    .load_priority     = PURR_PRIORITY_OPTIONAL,
    .name              = "homebase",
    .version           = "1.0.0",
    .kernel_min        = "0.11.1",
    .kernel_max        = "",
    .provided_catcalls = 0,
    .required_catcalls = 0,
    .init              = homebase_init,
    .deinit            = homebase_deinit,
};
