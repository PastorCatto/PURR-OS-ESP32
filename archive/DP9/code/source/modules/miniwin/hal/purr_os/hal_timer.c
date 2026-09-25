// hal_timer.c — MiniWin HAL timer backend for PURR OS
//
// MiniWin needs a 20ms tick (MW_TICKS_PER_SECOND = 50).
// We use a FreeRTOS software timer identical to the old DevKitC implementation,
// but without the DEVKITC guard so it always compiles on ESP32/S3.

#include "hal/hal_timer.h"
#include "miniwin_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"

volatile uint32_t mw_tick_counter = 0;

static TimerHandle_t s_mw_timer = NULL;

static void timer_cb(TimerHandle_t xTimer)
{
    (void)xTimer;
    mw_tick_counter++;
}

void mw_hal_timer_init(void)
{
    s_mw_timer = xTimerCreate("mw_tick",
                              (TickType_t)(configTICK_RATE_HZ / MW_TICKS_PER_SECOND),
                              pdTRUE, NULL, timer_cb);
    if (s_mw_timer) xTimerStart(s_mw_timer, 0);
}

void mw_hal_timer_fired(void) {}
