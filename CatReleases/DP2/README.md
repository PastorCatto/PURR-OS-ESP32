# PURR OS — DP2 (Developer Preview 2)

Full-stack build of **all 10 supported devices** — see the notes below for
how this went from 7/10 (BT/Meshtastic build breaks + a CYD DRAM overflow)
to a clean 10/10, and what changed along the way (see also `CHANGELOG.md`
v1.0.0-dp2).

Every device folder contains **both** forms of the image:

- **Split images** — flash each at its own offset: `bootloader.bin` (0x0 / 0x1000
  depending on chip), `partition-table.bin` (0x8000), `firmware.bin` (per
  partition table), `flash.bin` (SPIFFS, at the `spiffs_offset` listed in
  `manifest.json` for that device).
- **One image** — `PURR_OS_<device>.bin`, pre-merged with `esptool merge_bin`,
  flash it alone at offset `0x0`.

`manifest.json` records chip, kernel_type, UI backend, flash size, SPIFFS
offset, and file sizes for every device in one place.

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

Every device except `heltec` (128×64 OLED, no touch/trackball/keyboard at
all — nothing to drive a taskbar+start-menu with) and `tdeck_plus_test`
(deliberately UI-less hardware input-test kernel) now runs **MiniWin with
the WinCE-style taskbar+start-menu desktop**, forced across the board as of
this release ahead of a planned future switch to the from-scratch "Pounce"
UI backend (keyboard/trackball-first, no LVGL — see `pounce_plan.md` at the
repo root; not built yet).

| Device | Chip | UI backend | Flash |
|---|---|---|---|
| cyd | esp32 | miniwin (WinCE) | 4 MB |
| cyd_s024c | esp32 | miniwin (WinCE) | 4 MB |
| cyd_s028r | esp32 | miniwin (WinCE) | 4 MB |
| heltec | esp32s3 | oled_ui | 8 MB |
| jc3248w535 | esp32s3 | miniwin (WinCE) | 16 MB |
| tdeck | esp32s3 | miniwin (WinCE) | 16 MB |
| tdeck_plus | esp32s3 | miniwin (WinCE) | 16 MB |
| tdeck_plus_arduino | esp32s3 (arduino kernel) | miniwin (WinCE) | 16 MB |
| tdeck_plus_test | esp32s3 (arduino kernel) | (input test mode, no UI) | 16 MB |
| waveshare169 | esp32s3 | miniwin (WinCE) | 4 MB |

`cyd`/`cyd_s024c`/`cyd_s028r`/`waveshare169` were on `kittenui` (LVGL) as of
the previous DP2 pass — switched to `miniwin` in this pass, which is what
actually resolved the CYD boards' DRAM overflow (see below).

## What changed to get here

Three real, previously-undiscovered problems were found and fixed by
attempting full 10-device builds — none of them CYD-specific, all of them
would break any device not actively being hardware-tested this session:

1. **`bt_mgr.c`/`mesh_ble.c` unconditionally `#include`d NimBLE headers**
   with no build guard — every device except `tdeck_plus` failed outright on
   a missing `nimble/nimble_port.h`. Fixed with an `#ifdef
   CONFIG_BT_NIMBLE_ENABLED` guard + stub fallbacks for every public
   function.
2. **Six `device.pcat` files referenced a nonexistent `about` app**
   (`undefined reference to purr_module_about`) — a standalone app folded
   into Settings' own About tab during an earlier rewrite, with the
   references never cleaned up. Removed.
3. **Bluetooth and Meshtastic are now off by default everywhere, gated
   behind opt-in Kconfig toggles rather than deleted** —
   `CONFIG_BT_NIMBLE_ENABLED` (existing) and the new
   `CONFIG_PURR_FEATURE_MESHTASTIC`, both default `n`. `tdeck_plus` (the
   only device that had either on) now has both off. MeshChat needed *zero*
   code changes — its `mesh_manager_*` calls now hit safe stubs (0 nodes,
   sends no-op) instead of the app being removed; flipping either Kconfig
   back on for a future device with the right hardware brings the full
   feature (UI included — Settings' Bluetooth window is gated the same way)
   back with no further changes anywhere.
   **This only shrank the CYD boards' DRAM overflow by ~1.4 KB** (44.8 KB →
   43.4 KB) — a minor contributor, not the fix.
4. **The actual CYD fix: switching their UI backend from KittenUI (LVGL)
   to MiniWin.** LVGL is evidently far heavier on internal DRAM than the
   vendored MiniWin library on this chip — this alone took the 3 no-PSRAM
   CYD boards from a hard 43+ KB link failure to a clean build, with no
   feature-set trimming needed.

## Verification status

All 10 devices completed a full `idf.py build` + `esptool merge_bin`
against ESP-IDF v5.3.5, confirmed via direct build output (not assumed from
a partial run). No hardware-in-the-loop testing has been done for this
batch. `tdeck_plus` in particular carries several changes from earlier this
session (MiniWin WinCE desktop icons, an ST7789 BGR-colour-order fix, a
crash-guard/MW_ASSERT redirect) that are build-clean but **not yet
hardware-verified** — see `CHANGELOG.md` for full context.
