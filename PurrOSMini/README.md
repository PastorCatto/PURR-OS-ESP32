# PURR OS Mini

The small kernel of a remote display client (RemoteDisplay/SPEC.md, plan1.md "Remote display"). First board: the **CYD 2.4C** (Sunton ESP32-2432S024C: classic ESP32, 4 MB flash, no PSRAM, ILI9341 320 x 240,
CST816S touch). It boots under the **same PURR bootloader** as every other board, so the image is checked the same way; it has **no recovery loader, no KittenOS, no boot package and no file system**.

What it does today: shows a small touch UI of its own (the Click Wheel look: silver bar, white rows, accent selection, silver keys), joins Wi-Fi (list of networks, an on-screen keyboard for the password, the
network is saved in NVS), looks for a PURR host with a UDP broadcast (port 47802), opens the session (TCP 47803) and draws what the host sends (RAW, RLE16 and LZ4 rectangles, fills), sending touches back.
A touch held for three seconds in the top right corner forgets the network and goes back to the Wi-Fi list.

**Not done yet:** pairing and encryption (RemoteDisplay/SPEC.md section 4: until they exist the link is open), the host side (the S3's remote display service), the cache, an update pushed by the host,
a verified orientation and touch mapping (`mini_board.h`: `MINI_LCD_MADCTL`, `MINI_TP_SWAP_XY`, `MINI_TP_FLIP_X`, `MINI_TP_FLIP_Y` are set from the usual CYD values and are to be settled on the real board).
**Run on a real CYD 2.4C (2026-11-25):** the PURR bootloader checks and boots the signed image (`slot @0x20000: ok`), the panel, touch and Wi-Fi start, and the Wi-Fi list is drawn (the owner's photo). Found and fixed on the board: `SPI_TRANS_CS_KEEP_ACTIVE` kept the SPI bus lock so every chunk after the first failed (striped garbage on the screen); the boot was never reported healthy (`boot_fail_count` climbed), now `mini_health.c` clears it. The emulator has no model of the CYD's screen or touch (its CYD board file lists them as planned), so there is no emulator run. **Still to confirm by hand:** the touch mapping (tap a row), the password keyboard, joining a network, and the host search.

## Files

| | |
|---|---|
| `main/main.c` | start-up and the loop: join Wi-Fi, run the link |
| `main/mini_board.h` | the CYD's pins and the orientation settings |
| `main/mini_lcd.c`, `mini_touch.c` | the ILI9341 (SPI, DMA) and CST816S (I2C) drivers |
| `main/mini_gfx.c`, `mini_fonts.c` | drawing and text (Source Sans 3 bitmaps from `tools/mini/gen_font.py`, no LVGL; licence in `UI/fonts/OFL-SourceSans3.txt`) |
| `main/mini_wifi.c`, `mini_ui.c` | Wi-Fi and the few screens |
| `main/mini_link.c` | the client: discovery and the session. Uses `purr_rdisp.c` from `Modules/vdisplay/` (the wire code shared with the host, host-tested) |

## Build, sign, flash

```sh
. ~/esp/esp-idf-v5.3.5/export.sh
# the bootloader (same source as the other boards; the board file puts the table at 0xC000 because the bootloader is too big for 0xA000 on a classic ESP32)
cd bootloader && idf.py -B ../out/boot_cyd_24c -D SDKCONFIG=../out/boot_cyd_24c.sdkconfig -D IDF_TARGET=esp32 -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.board.cyd_24c" build
# the Mini
cd ../PurrOSMini && idf.py -B ../out/mini -D IDF_TARGET=esp32 -D SDKCONFIG=../out/mini.sdkconfig build
# package and sign it as the kernel slot (the passphrase in the environment of the one command)
cd .. && python3 purrstrap/purrstrap.py coreos package-component --component kernel --file out/mini/purrosmini.bin --version 0.1.0 --board cyd_24c --out mini.cat
PURRSTRAP_KEY_PASSWORD=... python3 purrstrap/purrstrap.py keys sign --key boot.key --key-id 3 --image mini.cat
python3 purrstrap/purrstrap.py coreos slot-image --file mini.cat --board cyd_24c --slot kernel --out kernel_slot.bin
# flash (the CYD's USB-UART bridge shows up as /dev/ttyUSB0)
python -m esptool --chip esp32 -p /dev/ttyUSB0 write_flash 0x1000 out/boot_cyd_24c/bootloader/bootloader.bin 0xC000 out/mini/partition_table/partition-table.bin 0x20000 kernel_slot.bin
```
