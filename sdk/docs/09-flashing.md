# Flashing an image

Flashing writes an image to a real board over USB. It replaces what is on the board, so make sure you mean it.

## Put the board in download mode

On the T-Deck Plus, hold the **boot** button (the trackball click), tap **reset**, then let go. The device shows up as a USB serial port, such as `/dev/cu.usbmodem1101`.

## From Studio

Open **flash**, pick the port and the image, and press **flash it**. The whole log is shown.

## From a terminal

```
bin/purr flash image.bin --port /dev/cu.usbmodem1101
```

A **full flash image** (16 MB) goes to offset `0x0`. A single signed part goes to its slot: kittenos at `0x20000`, kernel at `0x1A0000`, loader at `0x380000`. Studio's inspector shows each slot's offset.

## Tips

- If the port does not show up, try another cable. Some cables only carry power.
- Opening the serial port can reset the board. That is normal.
- A board that boots into a blank screen after flashing the wrong thing is not broken: put it in download mode and flash again.
