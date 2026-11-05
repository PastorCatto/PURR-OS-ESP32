/* purr_netsettle.c - see purr_netsettle.h. */
#include "purr_netsettle.h"

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "purr_board.h"
#include "purr_driver.h"
#include "purr_kernel.h"
#include "purr_net.h"

#define SETTLE_MAX_MS   40000       /* no address by then (no saved network, or none in range): stop waiting */
#define SETTLE_AFTER_MS 1500        /* once it has an address, let Wi-Fi's own start-up finish before the radio is brought up */

static void settle_task(void *arg)
{
    (void)arg;
    int64_t t0 = esp_timer_get_time();
    for (;;) {
        purr_net_status_t st;
        purr_net_status(&st);
        if (st.connected || (esp_timer_get_time() - t0) / 1000 > SETTLE_MAX_MS) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    vTaskDelay(pdMS_TO_TICKS(SETTLE_AFTER_MS));
    purr_drivers_net_settled(purr_board());
    vTaskDelete(NULL);
}

void purr_net_settle_start(void)
{
    if (xTaskCreate(settle_task, "net-settle", 3072, NULL, 3, NULL) != pdPASS) {
        purr_drivers_net_settled(purr_board());             /* no task: do not leave the devices waiting for ever */
    }
}
