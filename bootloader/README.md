# PURR OS bootloader

Custom second-stage bootloader for ESP32 and ESP32-S3, built on ESP-IDF v5.3.5.

It does the minimum: initialise hardware, load the partition table, pick an
app slot and jump to it. Everything else belongs in the OS.

## Layout

| Path | Purpose |
|------|---------|
| `bootloader_components/main/` | The bootloader itself (`purr_boot.c`, `purr_bootpkg.c`). ESP-IDF builds this in place of its stock bootloader. |
| `main/` | Stub app so the project builds and flashes. Replaced by the OS later. |
| `partitions.csv` | The real, current T-Deck Plus layout (kept identical to `PurrOS/partitions/tdeck_plus.csv`): `kernel` (single slot, kept on the `ota_0` subtype so the stock ESP-IDF loader can still jump to it -- not an OTA A/B pair anymore), `kittenos`, `loader` (rescue), `bootpkg`, `purrcfg`, `root`, plus `nvs`/`otadata`/`phy_init`. |
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

Builds for both chips. **Flashed and running on a T-Deck Plus** (ESP32-S3, 16 MB) against the
real, current partition table -- not just the stub app. Not yet tested on the CYD 2.4C.

Loads and verifies the **boot package** (`bootpkg` partition, SPEC.md section 9) by its real
PURR signature (not hash-only anymore -- signed with the boot-role key), which shows the real
boot menu on the real display and returns a chosen slot.

**The `kernel` -> `kittenos` -> recovery-loader fallback chain (SPEC.md section 6) is real and
proven on hardware, 2026-09-28:** with no menu interaction, it boots `kernel` by default; with
`kernel`'s first word corrupted, it correctly falls back to `kittenos` and boots that instead.
The third level (both dead -> the recovery loader) is written the same way but untested on
this device, since the recovery loader has never actually been flashed onto it. That fallback
still only checks that a slot *looks* like a real app image (a magic byte), not a real PURR
signature -- extending it to real per-image signature verification is tracked, real future
work (SPEC.md section 6, steps 4-5), not yet built.
