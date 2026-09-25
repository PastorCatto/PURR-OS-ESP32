# PURR OS bootloader

Custom second-stage bootloader for ESP32 and ESP32-S3, built on ESP-IDF v5.3.5.

It does the minimum: initialise hardware, load the partition table, pick an
app slot (otadata-aware) and jump to it. Everything else belongs in the OS.

## Layout

| Path | Purpose |
|------|---------|
| `bootloader_components/main/` | The bootloader itself (`purr_boot.c`). ESP-IDF builds this in place of its stock bootloader. |
| `main/` | Stub app so the project builds and flashes. Replaced by the OS later. |
| `partitions.csv` | Two OTA slots (`ota_0`, `ota_1`) plus `otadata`, `nvs`, `phy_init`. |
| `sdkconfig.defaults` | Shared defaults for both targets. |

## Build

Use a separate build dir and sdkconfig per target:

```
idf.py -B build_esp32   -D SDKCONFIG=build_esp32/sdkconfig   -D IDF_TARGET=esp32   build
idf.py -B build_esp32s3 -D SDKCONFIG=build_esp32s3/sdkconfig -D IDF_TARGET=esp32s3 build
```

The bootloader binary is `build_<target>/bootloader/bootloader.bin`.

## Status

Builds for both targets. Not yet flashed or tested on hardware.
