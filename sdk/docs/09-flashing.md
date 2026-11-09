# Flashing an image

*Current as of PURR OS 1.1 (2026-11-08).*

Flashing writes an image to a real board over USB. It replaces what is on the board, so make sure you mean it.

## Put the board in download mode

On the T-Deck Plus, hold the **boot** button (the trackball click), tap **reset**, then let go. The device shows up as a USB serial port, such as `/dev/cu.usbmodem1101`.

## From Studio

Open **flash**, pick the port and the image, and press **flash it**. The whole log is shown.

## From a terminal

```
bin/purr flash image.bin --port /dev/cu.usbmodem1101
```

A **full flash image** (16 MB) goes to offset `0x0`. A single signed part goes to its slot: kittenos at `0x20000`, kernel at `0x1E0000`, loader at `0x400000`. Studio's inspector shows each slot's offset.

## The flash layout

The T-Deck Plus uses **flash map v2** from release 1.0.29: three slots, a few small system areas, and `root` (the file system) from `0x5C1000` to the end of the 16 MB. There is no picture partition; the picture packs are files in `root` ([04-t-deck-plus.md](04-t-deck-plus.md)). The partition table is written only by a whole-image flash, so a device that has the older table (a 5 MB `ui_assets` partition and a smaller `root`) must be flashed with a whole 16 MB flash-map-v2 image. That writes `root` too, so what was in the old file system is gone. Single slots written to an old table would leave the new kernel looking for `root` in the wrong place.

`purr inspect image.bin` checks a whole image's partition table against the board's layout and says "Layout" with `ok` when it is flash map v2, or names what differs (it flags the older `ui_assets` layout in so many words). `purr flash image.bin` prints the same note, before it writes, when you flash a whole image whose table is not the board's. This layout is new in 1.0.29 and **has run on a T-Deck Plus (the owner's hardware pass on release 1.1, 2026-11-08)**.

## Tips

- If the port does not show up, try another cable. Some cables only carry power.
- Opening the serial port can reset the board. That is normal.
- A board that boots into a blank screen after flashing the wrong thing is not broken: put it in download mode and flash again.
