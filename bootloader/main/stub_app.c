// Placeholder app so the bootloader project builds and can be flashed.
// The real OS will replace this.
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void app_main(void)
{
    for (;;) {
        printf("PURR OS stub app: bootloader handed off successfully\n");
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
