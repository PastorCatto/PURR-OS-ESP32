# Describing a board

*Current as of PURR OS 1.1 (2026-11-08).*

A board is **data**. You write down the pins, the buses and the parts, and the kit turns that into the two things a board needs:

- the **board file**, a C source the kernel is built with (`PurrOS/components/kernel/boards/<id>.c`), and
- the **device profile**, `/etc/board.conf`, which a running device reads at boot to find the extra parts (GPS, LoRa, ...).

## The file

`sdk/boards/tdeck_plus.toml` is the example (the reference board; the other boards in the repository, `tdeck` and `waveshare154`, are kernel board files without an SDK `.toml` yet). It has these sections:

| Section | What it says |
|---|---|
| `[board]` | the id, the name, the chip, flash and PSRAM, the pin that switches the peripheral power, which chip selects to hold high |
| `[spi]` | the main SPI bus: host, mosi, miso, sclk |
| `[display]` | the screen: its driver, pins, size, orientation and colour order |
| `[keyboard]`, `[touch]` | the I2C parts: pins, address, speed |
| `[buttons]` | boot and power buttons, if any |
| `[[device]]` | everything else, one block each |
| `[[partition]]` | the flash layout, one block per partition (optional; see below) |

A `[[device]]` has a `name`, a `compatible` (which driver serves it), a `bus` (`adc`, `i2c`, `uart`, `spi`, `gpio`, ...) and its settings. It also has a `status` that says how far it has got on a real unit:

- `works`: used every day
- `built`: the code runs, but nobody has read real values yet
- `written`: the driver exists, tested only against a scripted fake
- `template`: no driver yet

## What the checker looks for

- a pin the chip keeps for its flash, PSRAM or USB (GPIO 19, 20, 22 to 37, 49 and up on the ESP32-S3);
- two parts on one pin (the bus lines a bus shares, and a reset line two parts share on purpose, are fine);
- a setting name longer than 7 letters, or more than 12 settings on one device;
- a missing `compatible`, a bad `status`, an id that is not a C name.

A board that carries a flash layout is checked for it too: every partition has a unique name of at most 15 letters, offsets and sizes are multiples of 4 KB, app partitions start on a 64 KB boundary, nothing overlaps another or the bootloader and partition table (0x0 to 0xB000), nothing runs past `flash_mb`, and `root` is the last partition and runs to the end of the flash. The old raw `ui_assets` partition is refused.

Strapping pins (0, 3, 45, 46) get a gentle note: they are fine as inputs once the chip has started, but keep them unpulled at reset.

## The flash layout

A `[[partition]]` block has a `name`, a `type` (`app` or `data`), a `subtype` (a name such as `factory`, `ota_0`, `test`, `nvs`, `phy`, or a number such as `"0x83"`), an `offset` and a `size`. `sdk/boards/tdeck_plus.toml` holds the T-Deck Plus table of **flash map v2 (release 1.0.29)**: `nvs`, `purrcfg`, `phy_init`, `bootpkg`, the three slots (`kittenos` 0x20000, `kernel` 0x1E0000, `loader` 0x400000; each 256 KB larger than up to 1.0.28), `netrec` at 0x5C0000, and `root` (the LittleFS file system, 0x5C1000 for 0xA3F000 bytes, to the end of the 16 MB). There is no picture partition any more: the picture packs, themes, apps and drivers are files in `root`. The build's own copy is `PurrOS/partitions/tdeck_plus.csv`; a test keeps the two equal. This layout is written but has **not yet run on a device**.

The layout is also what `purr inspect` compares a whole flash image against, and what `purr flash` checks before it writes a whole image (see [09-flashing.md](09-flashing.md)).

## Make your own

In Studio: **new project, a board**. It starts as a copy of the T-Deck Plus. Change the pins and devices, watch the checks, then **build**. From a terminal:

```
bin/purr board check mine.toml
bin/purr board build mine.toml --out build/
```

To use the board file in a system you build from source, copy the `.c` into `PurrOS/components/kernel/boards/`. To try a profile on a running device, copy `board.conf` to `/etc/board.conf` (the device reads it at boot and binds drivers to what it lists).
