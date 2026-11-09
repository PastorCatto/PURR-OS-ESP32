# The emulator

*Current as of PURR OS 1.1 (2026-11-08).*

**Short version.** The kit carries an emulator in `emulator/`: a window that runs a PURR OS flash image the way a T-Deck Plus would, with a screen, touch, keyboard and SD card, and no device on your desk. It emulates the ESP32-S3 (and the plain ESP32 enough to boot). The VM board has wired Ethernet, so the guest can reach your computer at `10.0.2.2`; it has no Wi-Fi radio. Nothing has to be installed. `emulator/run-emulator IMAGE BOARD` starts it, or `bin/purr emulator` and `bin/purr run` do it for you.

## Start it

```
emulator/run-emulator my-flash.bin t-deck-plus-vm            # the window
emulator/run-emulator my-flash.bin t-deck-plus-vm card.img   # with an SD card image
bin/purr emulator                                            # the same, on the kit's VM image
```

`my-flash.bin` is a full flash image (bootloader at 0x0, partition table at 0xA000 for PURR OS, system after). The emulator works on a copy in `emulator/run/`; the file you give is never changed. Booting takes about a minute of real time: the emulated chip is slower than the real one. `emulator/boards/` lists the boards you can name; `t-deck-plus-vm` is the one to use for PURR OS.

## Without a window

For scripts and tests: no window, pictures and the serial log are written to files.

```
HEADLESS_SHOTS=55 emulator/run-emulator --headless my-flash.bin t-deck-plus-vm 90 shots/run.png
```

That boots for 90 seconds and writes `shots/run-55.png` (a picture at second 55), plus the board's serial log on the terminal. To drive it while it runs, give it a command file and append one command per line:

```
HEADLESS_CMD=cmd.txt emulator/run-emulator --headless my-flash.bin t-deck-plus-vm 300 shots/run.png &
echo "tap 160 120"   >> cmd.txt        # press the screen (320 x 240)
echo "type hello"     >> cmd.txt        # keys on the keyboard (write \n for Enter)
echo "shot home"     >> cmd.txt        # writes shots/run-home.png
echo "quit"          >> cmd.txt
```

The other commands are `drag X1 Y1 X2 Y2`, `serial TEXT` (to the first serial port) and `dump` (writes `shots/run-serial.txt`). The program does not wait between commands: pace them yourself. `purr run` does this for you.

## What it models

| | |
|---|---|
| Chip | ESP32-S3 (two cores, 8 MB octal PSRAM, flash from your image). The plain ESP32 boots to a serial console (the CYD boards' screens are not modelled yet) |
| Screen | the T-Deck Plus's 320 x 240 ST7789 panel, drawn in the window; also read back as pictures |
| Touch | the GT911 touch panel (mouse clicks and drags) |
| Keyboard | the T-Deck's keyboard (your keys) |
| SD card | a card on the SPI bus, backed by an image file you pass in |
| Ethernet (VM board only) | a W5500 chip on its own SPI bus, so the OS's real Ethernet driver runs. Frames go to QEMU's user-mode network: DHCP and DNS work, the guest is `10.0.2.15` and **your computer is `10.0.2.2`** |
| VM info | a small device that tells PURR OS it is running in a VM (the VM flavor of the OS looks for it) |

Reaching your computer: a dev server on your computer at port 47801 is `http://10.0.2.2:47801` to the guest. The network only goes out: your computer cannot dial into the guest.

## What it does not do

- **No Wi-Fi, Bluetooth or LoRa.** The radio is not emulated (it is closed firmware). The VM board has Ethernet instead; the real `t-deck-plus` board is declared but its radio parts are reported as "not modelled".
- **No GPS, trackball, microphone, speaker or USB.** The board file names them; nothing answers.
- **Other chips.** The S3 is the one the kit targets. Other ESP32 variants (C3, S2) are not supported.
- **Speed.** Everything runs slower than a real T-Deck, and timing-sensitive code (audio, radio) cannot be judged here.
- **It is not a security test.** A locked device's rules are checked on the device; the VM image is signed with developer keys.

## What is in the folder

`esp32-emulator` (the window), `headless_tap` (no window), `run-emulator` (the launcher), `vendor/qemu/` (the patched QEMU with its libraries and ROM images), `boards/`, `licenses/`, `source/` (the QEMU source offer), `NOTICES-emulator.md`. It runs on a Mac with an Intel processor and macOS 26 or newer. If macOS refuses to open it after a download: `xattr -dr com.apple.quarantine <the kit folder>`.

## Testing a whole image

`purr system test NAME` (or *test it automatically* on the deliver page of *build a system*) boots a flash image you built in the emulator without a window and checks the serial log and the screen: the bootloader starts, the boot package verifies (and which key signed it), the kernel slot verifies, no crash, the screen is drawn, and how far it got (home screen, setup wizard, or the recovery loader a blank device stops at). It runs on a copy, so the image is never changed. It does not tap the UI, and it does not run on the Windows kit (no emulator). See [16-device-image.md](16-device-image.md).
