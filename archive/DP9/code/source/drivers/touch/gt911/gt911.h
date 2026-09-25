#pragma once
#include "esp_err.h"

// Public surface for specialized kernels calling GT911 directly before the
// module loader runs.

// Optional: call before gt911_drv_init() to override pin assignments.
// Pass int_pin=-1 to use polling mode instead of GPIO interrupt.
void gt911_configure(int sda, int scl, int int_pin, int rst_pin, int i2c_port);

int gt911_drv_init(void);   // uses default TDP pins (SDA=18, SCL=8, INT=16)
void gt911_drv_deinit(void);

// Last truly-raw point this driver read off the chip — straight from
// x_low/x_high/y_low/y_high, before any swap/mirror/scale/outlier-rejection.
// For diagnostics only; updated every successful gt911_read_point() call.
void gt911_get_last_true_raw(uint16_t *x, uint16_t *y);
