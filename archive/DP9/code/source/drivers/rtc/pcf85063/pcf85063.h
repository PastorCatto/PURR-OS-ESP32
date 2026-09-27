#pragma once
// pcf85063.h — NXP PCF85063 I2C RTC. See pcf85063.c for the real story.
// No public API beyond PURR_MODULE_REGISTER's own init/deinit — this
// driver has no catcall contract (see driver.pcat's own comment), it
// just pushes one purr_kernel_time_set(PURR_TIME_SOURCE_RTC_HW, ...)
// reading at boot.
