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

Each board has a settings file, `sdkconfig.board.<board>`, that carries what differs per board
(for now the flash size). Use a separate build dir and sdkconfig per board:

```
idf.py -B build_tdeck_plus -D SDKCONFIG=build_tdeck_plus/sdkconfig -D IDF_TARGET=esp32s3        -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.board.tdeck_plus" build

idf.py -B build_cyd_24c -D SDKCONFIG=build_cyd_24c/sdkconfig -D IDF_TARGET=esp32        -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.board.cyd_24c" build
```

The bootloader binary is `build_<board>/bootloader/bootloader.bin`. Flash with the `flash_args`
file the build writes: `python -m esptool --chip esp32s3 --port COM5 write_flash "@flash_args"`
from inside the build directory.

## Status

Builds for both chips. **Flashed and running on a T-Deck Plus** (ESP32-S3, 16 MB): it logs
`PURR OS bootloader`, reads the partition table, boots slot 0, and hands off to the stub app.
Not yet tested on the CYD 2.4C. No verification yet: it only picks a slot.
