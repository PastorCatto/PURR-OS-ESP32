# Describing a board

A board is **data**. You write down the pins, the buses and the parts, and the kit turns that into the two things a board needs:

- the **board file**, a C source the kernel is built with (`PurrOS/components/kernel/boards/<id>.c`), and
- the **device profile**, `/etc/board.conf`, which a running device reads at boot to find the extra parts (GPS, LoRa, ...).

## The file

`sdk/boards/tdeck_plus.toml` is the example. It has these sections:

| Section | What it says |
|---|---|
| `[board]` | the id, the name, the chip, flash and PSRAM, the pin that switches the peripheral power, which chip selects to hold high |
| `[spi]` | the main SPI bus: host, mosi, miso, sclk |
| `[display]` | the screen: its driver, pins, size, orientation and colour order |
| `[keyboard]`, `[touch]` | the I2C parts: pins, address, speed |
| `[buttons]` | boot and power buttons, if any |
| `[[device]]` | everything else, one block each |

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

Strapping pins (0, 3, 45, 46) get a gentle note: they are fine as inputs once the chip has started, but keep them unpulled at reset.

## Make your own

In Studio: **new project, a board**. It starts as a copy of the T-Deck Plus. Change the pins and devices, watch the checks, then **build**. From a terminal:

```
bin/purr board check mine.toml
bin/purr board build mine.toml --out build/
```

To use the board file in a system you build from source, copy the `.c` into `PurrOS/components/kernel/boards/`. To try a profile on a running device, copy `board.conf` to `/etc/board.conf` (the device reads it at boot and binds drivers to what it lists).
