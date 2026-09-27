#pragma once

#include <stdint.h>
#include "esp_err.h"

int bbq20_drv_init(void);

// Writes the keyboard's under-key LED backlight register (REG_BKL, 0x05)
// directly — 0 = off, 255 = full brightness.
esp_err_t bbq20_set_backlight(uint8_t brightness);
