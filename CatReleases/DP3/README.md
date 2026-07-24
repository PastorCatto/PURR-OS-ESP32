# PURR OS — DP3 (Developer Preview 3)

Full-stack build of **all 11 supported devices**. Same package shape as
DP2: every device folder has both split images and one pre-merged image,
plus a `manifest.json` recording chip/kernel_type/UI backend/flash size/file
sizes for all of them in one place.

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
  <spiffs_offset from manifest.json>  flash.bin
```

## Devices in this release

| Device | Chip | UI backend | Flash | App set |
|---|---|---|---|---|
| cyd | esp32 | miniwin (WinCE) | 4 MB | stripped |
| cyd_s024c | esp32 | miniwin (WinCE) | 4 MB | stripped |
| cyd_s028r | esp32 | miniwin (WinCE) | 4 MB | stripped |
| heltec | esp32s3 | oled_ui | 8 MB | none (OLED, no touch) |
| jc3248w535 | esp32s3 | miniwin (WinCE) | 16 MB | stripped |
| tdeck | esp32s3 | miniwin (WinCE) | 16 MB | full |
| tdeck_plus | esp32s3 | miniwin (WinCE) | 16 MB | full |
| tdeck_plus_arduino | esp32s3 (arduino kernel) | miniwin (WinCE) | 16 MB | full |
| tdeck_plus_pounce | esp32s3 | pounce (WIP) | 16 MB | pounce-native |
| tdeck_plus_test | esp32s3 (arduino kernel) | (input test mode, no UI) | 16 MB | none |
| waveshare169 | esp32s3 | miniwin (WinCE) | 4 MB | stripped |

**"stripped" app set** — Settings, Driver Manager, Services only. WiFi
stays on; Bluetooth and Meshtastic are off. This is the new default for
every non-T-Deck device as of this release (see below). **"full"** — the
T-Deck family keeps everything: Terminal, File Manager, Task Manager,
MeshChat, hwtest, Meshtastic, Bluetooth, GPS.

## What changed since DP2

### Meshtastic actually talks to real nodes now

Three independent bugs, found in sequence by finally getting real RF
traffic flowing, each fully masking the next until fixed:

1. **Radio never actually heard anything** — the hand-rolled SX1262 SPI
   driver never enabled the module's TCXO (confirmed missing against
   Meshtastic's own T-Deck `variant.h`). Fixed by vendoring RadioLib
   (`source/lib/lib_radiolib`) and writing a new `sx1262_rl` driver against
   it — the same chip bring-up code real Meshtastic firmware uses,
   replacing the SPI-command driver device by device.
2. **Wrong cipher** — the default channel is AES-**128** with the real
   16-byte PSK; the code was doubling it into a fake 32-byte key and
   running AES-256.
3. **Wrong wire format** — real Meshtastic sends a raw 16-byte
   `PacketHeader` struct directly as the LoRa payload, not a
   protobuf-encoded outer `MeshPacket`. Confirmed against live captured
   packets from real nearby nodes and two-way exchange (RX, TX, relay) —
   node discovery, MeshChat messaging, and flood relay all confirmed
   working live against real Meshtastic hardware.

### MiniWin: the close-button hang is actually fixed, not routed around

Earlier sessions worked around a hang on window close by having the
title-bar X minimize instead of actually closing — the real cause was never
found. This time it was:

- **List-box selection was always -1** — MiniWin clears its own internal
  "pressed" flag before posting the selection message; reading it back was
  always stale. Broke Task Manager's Kill button and MeshChat's buddy list
  identically.
- **A genuine infinite loop** — three loops compared a `uint8_t` counter
  against `MW_MESSAGE_QUEUE_SIZE` (256), which a `uint8_t` can never reach.
  Every window/control close with a real control to remove hung forever.
  This was the actual root cause of the long-standing close hang; the
  minimize-instead-of-close workaround just never exercised it.
- **Stale taskbar entries** on X-icon close (the unregister call only ran
  on the app-initiated close path).
- **No repaint after closing a window** — left stale content on screen that
  looked like a stuck overlay.
- **Textarea placement was hardcoded** to `(4, 28)`, ignoring the layout
  system every other widget uses — a second textarea, or one placed beside
  a list, rendered on top of whatever was already there.
- **Physical keyboard input never reached any textarea** — the message
  MiniWin's input pump posts for a key-down had no handler at all.

With all of the above fixed, the title-bar X now does a real close again.

### Stripped-down default app set for non-T-Deck devices

Adding Task Manager to every device in the last pass pushed the
PSRAM-less CYD boards (and nearly jc3248w535/waveshare169) back into a DRAM
budget overflow — a different one than DP2's LVGL-vs-MiniWin overflow, same
underlying constraint. Rather than trim one app's memory use, every device
except the T-Deck family now ships a deliberately small baseline: Settings,
Driver Manager, Services, WiFi, and core `.meow`/`.hiss`/`.kitten` Lua
script support. Terminal, File Manager, Task Manager, Calculator, Bluetooth,
and Meshtastic are all off by default on these devices — same
opt-in-via-Kconfig/device.pcat pattern as DP2's Bluetooth/Meshtastic gating,
nothing deleted, easy to re-enable per device if a specific board has the
RAM budget for more.

## Verification status

All 11 devices completed a full `idf.py build` + `esptool merge_bin`
against ESP-IDF v5.3.5. `tdeck_plus` specifically was flash-verified on
real hardware throughout this session — Meshtastic RX/TX/relay against
real nodes, MiniWin close/kill/layout/keyboard fixes all confirmed live,
not just build-clean. The other 10 devices are build-clean only; no
hardware-in-the-loop testing has been done for this batch.
