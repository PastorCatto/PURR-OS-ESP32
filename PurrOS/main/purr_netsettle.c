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

/* What the late bind runs on. A driver's start-up (the LoRa radio's resets, SPI transfers and log lines) needs a real stack: the waiting task below has 3 KB, which is
 * enough for polling the network status and nothing more. Installing a driver from Device Manager runs it on a 12 KB job task and works; after a restart the same start-up
 * ran here, on 3 KB, and the device stopped (2026-11-05, found by reading: not yet seen on a device). The stack must be internal RAM: a start-up may hand a stack buffer to DMA. */
static void bind_task(void *arg)
{
    (void)arg;
    purr_drivers_net_settled(purr_board());
    vTaskDelete(NULL);
}

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
    if (xTaskCreate(bind_task, "net-bind", 12288, NULL, 3, NULL) != pdPASS) {
        purr_drivers_net_settled(purr_board());             /* no room for a proper stack: bind here rather than leave the devices waiting for ever */
    }
    vTaskDelete(NULL);
}

void purr_net_settle_start(void)
{
    if (xTaskCreate(settle_task, "net-settle", 3072, NULL, 3, NULL) != pdPASS) {
        purr_drivers_net_settled(purr_board());             /* no task: do not leave the devices waiting for ever */
    }
}
