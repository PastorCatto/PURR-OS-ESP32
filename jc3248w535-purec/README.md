# PURR OS — JC3248W535 (pure ESP-IDF / C port)

A pure-C ESP-IDF port of PURR OS's boot kernel (KITT) for the **Guition
JC3248W535** (ESP32-S3-WROOM-1 N16R8, AXS15231B 320×480 QSPI display, AXS15231B
I²C capacitive touch).

No Arduino, no C++ — builds and runs on plain ESP-IDF (tested on **v5.5.2**).

## Why this port

The upstream `jc3248w535` target was WIP and assumed an ST7796 SPI panel driven
through TFT_eSPI/arduino-esp32. Real JC3248W535 boards use an **AXS15231B QSPI**
panel. This port replaces the display/touch stack with the native `esp_lcd`
AXS15231B driver (correct pinout + vendor init sequence) and reimplements the
KITT boot path in C, removing the arduino-esp32 dependency entirely.

## Hardware

| Function | Pins |
|---|---|
| Display (QSPI) | CS=45, PCLK=47, D0=21, D1=48, D2=40, D3=39, RST=-1, BL=1 |
| Touch (I²C)    | SDA=4, SCL=8 @ 400 kHz |

## Build & flash

```bash
. ~/esp/v5.5.2/esp-idf/export.sh      # ESP-IDF 5.3+ (5.5.x ok)
idf.py build
idf.py -p /dev/ttyACM0 flash monitor  # Ctrl+] to exit
```

Target (`esp32s3`), 16 MB flash, OPI PSRAM and the partition table are all set
in `sdkconfig.defaults`, so no manual `set-target` is needed.

## Layout

```
CMakeLists.txt                 project
partitions_jc3248w535.csv      factory + dual OTA + spiffs (16 MB)
sdkconfig.defaults             S3 / PSRAM / flash / partition config
spiffs_image/                  flashed to the spiffs partition
  system/kernel/device.json    runtime device config (parsed with cJSON)
main/
  main.c                       app_main: NVS + SPIFFS + KITT
  kitt.c/.h                    boot sequence / log / splash (C port of KITT)
  device_config.c/.h           device.json loader (cJSON)
  display_axs.c/.h             AXS15231B QSPI driver: framebuffer + 8x8 font
  font8x8_basic.c              public-domain 8x8 font
  touch_axs.c/.h               AXS15231B I2C touch
  wifi_mgr.c/.h                esp_wifi STA bring-up
  power_mgr.c/.h               CPU/clock reporting
managed_components/            esp_lcd_axs15231b + esp_lcd_touch (+ cmake_utilities)
```

## Status

Boots to the PURR OS splash + verbose KITT boot log on the display and serial
(115200). WiFi brings up the STA interface; touch is wired. App/firmware/LoRa/BT
subsystems from the full C++ kernel are not part of this minimal C boot port yet.
