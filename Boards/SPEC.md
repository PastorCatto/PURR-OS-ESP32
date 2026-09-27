# Boards spec (draft 0.1)

Which boards are supported, what is special about each, and how a board gets added. The tiers are
in `../PurrOS/SPEC.md` section 1.1, and the first two boards' details are in section 8 there. How a
board is found and installed is in `../Install/SPEC.md`.

## 1. The boards

| Board | Chip | Tier | Notes |
|-------|------|------|-------|
| **T-Deck Plus** | ESP32-S3, 16 MB flash, 8 MB PSRAM | modular, primary | 3.2" ST7789, keyboard, touch, trackball, LoRa, GPS |
| **CYD 2.4C** | ESP32, 4 MB flash, no PSRAM | monolithic | 2.4" ILI9341-compatible, CST816S touch, no keyboard |
| **Waveshare ESP32-S3-ePaper-1.54** | ESP32-S3, 8 MB flash, 8 MB PSRAM | modular, third | 200 by 200 e-paper, no touch, no keyboard, audio codec, clock, battery gauge |

Also in the old archive, not yet planned: Heltec (128 by 64 OLED and LoRa), JC3248W535 (480 by 320),
the T-Deck (non-Plus), the CYD 2.8-inch variants, the Waveshare 1.69-inch, and the Tab5.

## 2. Waveshare ESP32-S3-ePaper-1.54

Chosen as the third board because it is a very different display, and tests whether the `display`
and `ui` catcalls really hide the hardware. Facts from the old archive profile
(`archive/DP9/code/source/devices/waveshare154/device.pcat`): 200 by 200 SSD1681-family e-paper, a
PCF85063 clock, a battery gauge on an ADC divider, an SHTC3 temperature and humidity sensor, an
ES8311 audio codec with microphone and speaker, an SD slot, and only BOOT and PWR buttons.

**Decisions**

- **The driver manages refreshing.** It batches draws, uses fast partial refreshes for small changes
  and a full refresh now and then to clear ghosting. Apps draw as usual and never know it is
  e-paper. The display catcall gets two **optional** functions for apps that care, `refresh_now` and
  `set_refresh_mode`, marked by feature bits (`../PurrOS/components/kernel/SPEC.md` section 8).
- **Input is the USB serial console plus the two buttons.** The shell runs on the console. The
  buttons are ordinary key events through the `input` catcall, so a UI app can be navigated with
  just them.
- **The power button:** a **tap is Back.** A **long hold force-reboots** the device. The hold is
  handled **by the kernel itself,** timed in a hardware timer below apps and CoreOS, so it works
  even when every task is stuck. The BOOT button as select or next is proposed.
- **Audio** needs a code driver: the ES8311 is controlled over I2C and streams over I2S, which is
  two buses (`../Catcalls/SPEC.md` section 10).
- **The clock** is a time source for the `system` catcall and for the certificate time floor
  (`../Keys/SPEC.md` section 5).

## 3. Adding a board

The proposal is **one folder per board,** holding everything specific to it: the hardware
description (pins, buses, devices), the partition layout, the build settings, the boot package's
board quirks, and the drivers it needs. Adding a board means adding a folder and changing no other
code, which follows the plug-and-play rule (`../PurrOS/components/kernel/SPEC.md` section 2). The
build tool lists the folders and lets you pick one. This was proposed and not yet confirmed.

**The codename.** For a board that has a driver pack, the bootloader is built with the board's
codename inside. purrstrap builds the same bootloader code once per board with that name. A built-in
name cannot go missing the way a recorded value can, and it tells the installer which driver pack to
fetch (`../Install/SPEC.md`).

## 4. Open questions

- The exact pin maps, checked against the vendor's own sources as the archive did.
- A monochrome **e-paper backend** for `ui`: theme, no animations, and how refresh hints reach it
  (`../UI/SPEC.md`).
- The refresh policy: how often a full refresh, and what triggers a partial one.
- The tier and flash size of the other boards, and the order they come in.
- Whether the BOOT button is select or next.
- Whether the folder-per-board layout is right.
