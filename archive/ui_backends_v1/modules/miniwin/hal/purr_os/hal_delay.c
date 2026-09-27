// hal_delay.c — MiniWin HAL delay backend for PURR OS (FreeRTOS + esp_rom)

#include "hal/hal_delay.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_rom_sys.h"

void mw_hal_delay_init(void) {}

void mw_hal_delay_ms(uint16_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

void mw_hal_delay_us(uint32_t us)
{
    esp_rom_delay_us(us);
}
