# PURR OS � DP8 (Developer Preview 8)

Full-stack build of all 12 supported devices. Every device folder
has both split images and one pre-merged image, plus a `manifest.json`
recording chip/name/copied-files for all of them in one place.

## What changed in DP8

See **[RELEASE_NOTES.md](RELEASE_NOTES.md)** — measured performance results,
Speed Demon, the contract changes, and the known issues (including screen
tearing, which is a hardware limit on T-Deck Plus and will not be fixed there).

## Flashing the merged image (recommended)

```bash
esptool.py -p <PORT> write_flash 0x0 PURR_OS_<device>.bin
```

## Flashing split images

```bash
esptool.py -p <PORT> write_flash \
  0x0     bootloader.bin \
  0x8000  partition-table.bin \
  0x10000 firmware.bin \
  <spiffs_offset from device.pcat>  flash.bin
```

## Devices in this release

| Device | Chip | Status |
|---|---|---|
| cyd | esp32 | ok |
| cyd_s024c | esp32 | ok |
| cyd_s028r | esp32 | ok |
| heltec | esp32s3 | ok |
| jc3248w535 | esp32s3 | ok |
| tab5 | esp32p4 | ok |
| tdeck | esp32s3 | ok |
| tdeck_plus | esp32s3 | ok |
| tdeck_plus_arduino | esp32s3 | ok |
| tdeck_plus_pounce | esp32s3 | ok |
| tdeck_plus_test | esp32s3 | ok |
| waveshare169 | esp32s3 | ok |
