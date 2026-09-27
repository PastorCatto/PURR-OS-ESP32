# lib_st7123 — vendored ST7123 esp_lcd panel + touch drivers

Sitronix ST7123 combined display+touch IC, as used by the M5Stack Tab5
(post-Oct-2025 hardware revision). All files Apache-2.0, Espressif Systems
(SPDX headers intact):

- `esp_lcd_st7123.c/.h` — MIPI-DSI panel driver. From the local component in
  the working Tab5 LVGL reference project (not on the ESP Component Registry).
- `esp_lcd_touch_st7123.c/.h` — I2C touch driver, registry component
  `espressif/esp_lcd_touch_st7123` v1.0.2.
- `esp_lcd_touch.c/.h` — the generic esp_lcd_touch base API it needs, registry
  component `espressif/esp_lcd_touch` v1.2.1.

Vendored (same pattern as lib_radiolib / lib_lua) instead of pulled from the
registry so non-P4 devices' dependency lock files don't churn — modulestrap
only scans driver/module dirs, so these are compiled privately into
`source/drivers/display/st7123`'s component. Requires ESP-IDF >= 5.5
(esp_lcd_dpi / MIPI-DSI APIs exist only for esp32p4).
