#pragma once
// epd1in54.h — SSD1681-family 200x200 1.54" e-paper driver.
// See epd1in54.c for the real story (hardware command sequence, full-
// refresh-only tradeoff). No public API beyond PURR_MODULE_REGISTER's own
// init/deinit — same shape st7789.h/ssd1306.h keep (pins come from
// CONFIG_DRV_DISPLAY_*_PIN glue macros, not a runtime configure() call,
// since this is the first and only device that uses this panel so far).
