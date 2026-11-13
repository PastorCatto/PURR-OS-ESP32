# Drivers spec (draft 0.1)

How drivers are packaged, distributed and pinned. How drivers work inside the kernel (matching, the pin
registry, the catcalls) is in `../PurrOS/components/kernel/SPEC.md`. Finding hardware and pulling drivers
for an unknown board is in `../Install/SPEC.md`. Keys are in `../Keys/SPEC.md`.

## 1. Decisions so far

- **Two forms of driver distribution.**
  - An **individual driver** is a signed file that can be pulled on its own and updated without touching
    anything else. Discovery uses these for add-ons and unknown boards.
  - A **driver pack** is for a pre-supported device, and sets up a whole board.
- **A pack is a signed list, not one big file.** It holds the **board description** and, for each driver,
  its name, its **pinned version and hash,** and its **pin assignments.** It is signed with a release key.
- **The pinout adapts to the board.** Drivers never hard-code pins. The pack gives each driver the pins it
  uses on that board, so the same driver file serves different boards with different wiring.
- **Core drivers stay locked.** Because the signed list pins each driver's exact hash, a swapped or
  modified driver is caught.
- **Drivers are files in `/boot/drivers`.** A driver several boards share is stored once and can be updated
  on its own.
- **Data-description drivers and code drivers.** A driver is a data description when it is one device on one
  bus plus its own pins, and needs real code otherwise (`../PurrOS/components/kernel/SPEC.md` section 14).
- **Users can add drivers** from an SD card, through the individual path, under the same rules.

## 2. The pack

A signed manifest, made by purrstrap. It contains:

- the board's codename and its hardware description (buses, devices, which driver serves each)
- for each driver: name, version, **SHA-256 of the driver file,** and its **pins** for this board
- the minimum kernel version it needs

The device downloads each driver file separately, checks it against the pinned hash, and keeps it in
`/boot/drivers`. If the manifest's signature is wrong, or a file's hash does not match, the driver is not
used.

## 3. Individual drivers

- A signed file with its own header: name, version, the `compatible` strings it serves, the buses it needs,
  and the catcall it provides.
- **Who signs it** decides where it may be used: the system role for core drivers, a vendor certificate for a
  vendor's driver (for example a mesh radio), the developer role for a user's code driver, and no signature
  needed for a data description that only uses pins no locked driver owns (`../Keys/SPEC.md`).
- An index in the repo lists each driver with its version, hash, signer, size and requirements, so the
  device can look one up when discovery finds a matching device.

## 4. How it fits the kernel

- The kernel matches each device in the board description to a driver by its `compatible` string and hands
  it the pack's pins. A device with no driver ends in the `no_driver` state, and the installer fetches one.
- The pin registry still refuses any conflicting claim, including one from a pack entry
  (`../PurrOS/components/kernel/SPEC.md` section 6).
- **Code drivers need loadable native modules,** which depend on the module loading spike
  (`../ModuleSpike/SPEC.md`). Until it passes, code drivers are built into the kernel and only data-description
  drivers are separate files.

### 4.1 The bus table and its ABI (rule added 2026-10-25)

A loadable driver is freestanding code with no libc and no ESP-IDF link. Everything it reaches
(SPI, I2C, GPIO, delays, DMA memory, the backlight) it reaches through a table of function
pointers the kernel hands to its entry function: `purr_driver_bus_table_t`
(`PurrOS/components/kernel/include/purr_driver_bus.h`). The module calls a slot by position, so
**the table layout is the ABI**.

- **Every loadable driver stamps `bus_abi`.** `purr_driver_t.bus_abi` is set to
  `PURR_DRIVER_BUS_ABI_VERSION`, taken from the same header the module was compiled with. The
  loader (`load_one_driver_file()` in `commands.c`) refuses a driver whose value differs from the
  kernel's, with an error naming the module.
- **Why this exists.** When `gpio_write` was inserted in the middle of the table (ABI 3 to 4), a
  touch module built before it called `now_us()` and landed on `delay_ms()` with a leftover
  register as the argument: a 12.8-day sleep inside LVGL's input timer, so the UI froze on its
  first white frame. Nothing checked the version, so the failure looked like a display bug.
- **Change the table by appending.** A new function goes at the END of the table, never in the
  middle, so an old module's slots keep their meaning. Bump `PURR_DRIVER_BUS_ABI_VERSION` on every
  layout change regardless.
- **After a bump, every driver blob must be rebuilt, re-signed and re-embedded.** The firmware
  carries baked-in copies in `commands.c` (`s_kbd_i2c_driver_module`, `s_gt911_driver_module`,
  `s_st7789_driver_module`, `s_hello_driver_module`). Build with `purrstrap modules build --kind
  driver`, sign with the developer key (key id 2), and replace the byte arrays.
- **Planting replaces a differing file.** Driver files in `/boot/drivers` come only from the
  firmware, so on boot a file whose contents differ from the baked-in blob is replaced. (Files in
  `/system` and `/kernelmods` keep the older rule: planted only if missing.)

### 4.2 Driver versions, the ABI range, the discovered profile (added 2026-10-28)

- **A driver's version is independent of the OS's.** The bus table only grows at its end, so a driver built against bus ABI N works on every
  kernel that serves N or later. The loader accepts a driver whose `bus_abi` is between `PURR_DRIVER_BUS_ABI_MIN` and
  `PURR_DRIVER_BUS_ABI_VERSION` (purr_driver_bus.h) and refuses anything outside it by name. Raise MIN only if a slot is ever moved.
- **The driver index** is a manifest of `type=driver` stanzas (purr_manifest.h) published next to the module index (`drivers.manifest`), with
  `compatible`, `buses`, `catcall`, `abi_min`/`abi_max` (the kernel ABIs it works with; 0 = no bound) and `blocked=yes` for a version found
  broken on a newer OS: it stays listed, and is never offered. `tools/release/make_drivers_index.py` writes it; `make_release.py --driver`
  puts it in the release.
- **Unsigned drivers** load only in developer mode with "Allow unsigned apps and drivers" on, only from `/boot/drivers`, with the payload hash
  still checked. A device whose signed trust block says DEV_LOCKED never has developer mode, so never loads one.
- **The discovered profile** (`/etc/board.conf`, purr_hwprofile.h) lists devices that Settings > Updates > Find hardware found and the owner
  confirmed: name, driver, bus, pins, address. It is read as untrusted input (a reserved pin or a bad address drops the entry). A device in it is
  bound to its driver after the boot-time driver load (`purr_hwsetup_boot`), in a second board object, so the real board profile is never edited.
  A driver for such a device reads `dev->cfg` as `purr_i2c_dev_cfg_t` (`buses=i2c`).
- **Two I2C buses at most:** the kernel's bus lookup is by pin pair, so a driver for a discovered device gets its own controller (port 1) and
  can never be handed the keyboard's bus by mistake.

## 5. Testing

- Pack checks: a wrong signature, a driver file whose hash does not match, a swapped driver, and a missing
  driver.
- Adaptable pinout: the same driver file on two boards with different pin maps.
- A driver shared by two packs is stored once and updated once.
- Pulling an individual driver on demand, and using the cached one when offline.
- A pack entry that asks for a pin a locked driver owns is refused.

## 6. Open questions

- **The file format for code drivers,** and how they are built per chip. The old system had a driver build
  tool, and purrstrap would get a subscript for it.
- **Where the drivers live** in the repo, and whether they have their own repo.
- **The pack's encoding.**
- **Dependencies between drivers,** and what happens when two drivers match one device.
- **Rolling back a bad driver,** using the same `.bak` swap as other system files.
- **Whether a user can override the pins** in a pack on their own board.


### 4.3 Bus ABI 5: `adc_read_mv`, and two drivers that became board devices (2026-10-28)
- The bus table grew at the end by one function, `adc_read_mv(pin)`: the calibrated, averaged millivolts at an ADC-capable GPIO (-1 otherwise). `PURR_DRIVER_BUS_ABI_VERSION` is 5;
  `PURR_DRIVER_BUS_ABI_MIN` stays 4, so drivers built against 4 (bme280, the display/touch/keyboard modules) load unchanged. A driver that calls `adc_read_mv` lists `abi_min=5` in
  drivers.manifest (`make_drivers_index.py --driver FILE:COMPATIBLE:BUSES:CATCALL:ABI_MIN`).
- `battery-adc` (Modules/drivers/battery_module.c): cfg `purr_adc_dev_cfg_t {pin, mul, div}`; `read()` returns `[percent, mV lo, mV hi, flags]` (flags bit 0 = a cell is connected). T-Deck Plus: GPIO 4,
  divider 2:1. Percent is a single-cell Li-ion curve estimate; the OS (`purr_battery.c`) smooths it and caches it for 20 s.
- `kbd-backlight`: now a board device on the T-Deck Plus (0x55 on the keyboard's wires). `read()` is the control channel: `[0xA5, level]` sets the nearest of the four steps, `[0xA6, x]` reports it.
  The level is the setting `keyboard.backlight` (0 off, 1 low, 2 medium, 3 high), switched off with the screen.
- Both are baked into the firmware like the keyboard driver (`s_temp_drivers`), so they work with no network, and are also published in drivers.manifest.

### 4.4 Bus ABI 6, driver classes and named-value config (2026-10-28)
- **UART in the bus table.** `uart_open(port, tx, rx, baud, rx_buffer)` (ports 1 and 2; a handle is the port plus one), `uart_write`, `uart_read(handle, buf, cap, timeout_ms)`, `uart_set_baud`, `uart_close`.
  Port 0 is the console and is not offered. `PURR_DRIVER_BUS_ABI_VERSION` is 6; `PURR_DRIVER_BUS_ABI_MIN` stays 4.
- **Classes.** A driver of ABI 6 may declare `class_id` and `ops` (purr_driver_class.h). The kernel keeps a registry of bound drivers by class: `purr_class_find()` / `purr_class_ops()`. A driver built for ABI 4 or 5 has no such
  fields and `purr_driver_class_id()` never reads them. Classes: `GNSS` (1), `LORA` (2), `MODEM` (3), `WIFI` (4), `HCI` (5), `BLOCK` (6, ABI 7). Every ops table starts with `uint32_t version`; a field is only ever added at the end.
- **Named-value config.** `purr_kv_cfg_t` is up to 12 `{key[8], int32}` pairs. A board device or a line of `/etc/board.conf` that is not I2C carries its pins and settings this way (`bus=uart`, `tx=43`, `baud=38400`), and the driver reads
  them with `purr_kv(cfg, "key", fallback)`. Keys that name a pin (tx rx cs rst busy dio1 dio2 pwr pps int en irq mosi miso sclk bck ws din dout mclk) are checked against the chip's reserved pins when the file is read.

### 4.5 Bus ABI 7, the shared SPI bus lock, and the BLOCK class (2026-10-29)
- **`spi_lock()` / `spi_unlock()`** (appended to the table; `PURR_DRIVER_BUS_ABI_VERSION` is 7, `_MIN` stays 4). The T-Deck Plus's display, radio and SD card are one SPI bus. `spi_transfer()` already takes the bus for a single call; a driver
  whose conversation is several calls (an SD card: command, answer, data, with its chip select held low in between) takes the lock around the whole of it, so a screen update cannot land in the middle. The lock is recursive.
- **Chip select by hand.** A driver that must hold the line low across calls makes its SPI devices with `cs = PURR_PIN_NONE` and drives the pin itself with `gpio_set_output` / `gpio_write`. The board file lists the pin in `idle_high_pins`
  so it is high from the start.
- **Class `BLOCK` (6)**: `purr_block_ops_t {version, info, read, write, sync}` over 512-byte sectors. `info` fills `purr_block_info_t {sectors, sector_size, removable, writable, card, reserved}` and fails when there is no card.
  A removable device is started lazily by its first call and found again after it was taken out.
- **What sits on top** (main/purr_sd.c): FatFs, mounted as `/sd` through the C library's file calls, over the `sd0` device's block ops. Nothing formats a card. FAT12/16/32 only: ESP-IDF's FatFs has no exFAT, so a card of 64 GB
  or more has to be formatted FAT32 on a computer first.

### 4.6 Bus ABI 8, a bus of the device's own, and the ETHERNET class (2026-11-11)
- **`spi_open_bus(host, sclk, mosi, miso, max_transfer)`** (appended; `PURR_DRIVER_BUS_ABI_VERSION` is 8, `_MIN` stays 4). Starts an SPI bus for a chip that is not on the board's shared one. The kernel refuses the flash host (0) and the board's
  own host, and a second call with the same host and pins is fine. After it, `spi_add_device(host, ...)` adds the chip.
- **Class `ETHERNET` (7)**, `purr_eth_ops_t`: `start(mac)`, `stop`, `link` (bits `PURR_ETH_LINK_UP`, `_SPEED_100`, `_FULL`), `send_frame`, `recv_frame`. The driver moves raw Ethernet frames; the kernel's TCP/IP stack sits on top (`purr_net_link.c` makes an esp_netif of it).
- **`purr_device_add()`** declares a device the board table cannot list (only the machine running the OS knows it is there). The kernel uses it for a virtual device's Ethernet controller (`eth0`, compatible `w5500`, from the VM-info device), and a
  module registered later finds it like any board device.
- **The first driver of the class** is `Modules/drivers/w5500_module.c` (a W5500 in MACRAW mode, same register sequence as ESP-IDF's driver). Host test: `test_drv_eth.c`, which plays the chip.

### 4.7 Bus ABI 9, a quiet probe, and the GAMEPAD class (2026-11-12)
- **`i2c_probe(handle, timeout_ms)`** (appended; `PURR_DRIVER_BUS_ABI_VERSION` is 9, `_MIN` stays 4). Asks whether the I2C device behind a handle answers its address, with no data and no error in the log (the IDF's `i2c_master_probe`). A driver for a module that may be unplugged looks for it with this: a failed write is an ERROR line every time.
- **Class `GAMEPAD` (8)**, `purr_gamepad_ops_t`: `buttons(h)` (a bit for each of the nine buttons, `PURR_PAD_*`), `present(h)`, `set_leds(h, mask)` (a negative mask hands the lights back to the driver). It is for a shell that needs more than the keys the driver sends through `read()`: FUNC held, FUNC plus a face button, and the lights as hints.
- **The reference driver** is `Modules/drivers/gamepad_mcp23017_module.c` (the Playtime board's MCP23017 module): the "keyboard" device, arrow keys, Enter, Esc, Menu, Home. An absent module is probed after 0.5 s, then 1, 2, 4 and up to 8 s. While no button is down it looks once every 24 ms with a single read (both rows are driven low, so any press pulls a column down), and only a press starts the row-by-row scan (every 8 ms while something is held) and puts the rows back afterwards: this keeps the traffic to about 40 bus reads a second at rest, where it was once about 500 transfers a second (an emulated I2C controller crashed the guest under that load, 2026-11-13). Transfers wait up to 60 ms. A change is believed only when two scans in a row agree (8 ms apart), so a bounce or a bad read on the bus is not a key press. Host test: `test_drv_gamepad.c`, which plays the chip and its diode matrix.

## 6a. Reference drivers by class (2026-10-28)
| Driver | Class | Bus | Notes |
|---|---|---|---|
| `gnss-nmea` | GNSS | UART | GGA, RMC, GSV; checksum checked; fixed point, no libc |
| `lora-sx126x` | LORA | SPI | SX1261/62/68: image calibration by band, TCXO on DIO3, RF switch on DIO2, LDRO, sync word; probe checks GetStatus |
| `modem-at` | MODEM | UART | 3GPP AT commands: CEREG/CREG, CSQ, COPS, CGSN, CGDCONT/CGAUTH/CGACT, CGPADDR; sockets are not offered |
| `wifi-esp-at` | WIFI | UART | Espressif AT firmware: scan, join, TCP sockets in passive receive mode |
| `hci-h4` | HCI | UART | H4 framing, HCI_Reset; no flow control (not in the bus table) |
| `trackball-gpio` | none | GPIO | `read()` returns `[dx, dy, click, 0]` |
| `sd-spi` | BLOCK | SPI | SD, SDHC and SDXC in SPI mode: 74+ clocks, CMD0/8, ACMD41, CMD58, CMD16, CSD for the size; CMD17/18/12 reads, CMD24/25 writes; started at 400 kHz, then `hz` (20 MHz); chip select by hand, bus lock held per conversation |

All are freestanding. A module has no libc, so a driver that copies structs or initialises arrays defines its own `memcpy` and `memset` (and `modules.py` compiles with `-fno-tree-loop-distribute-patterns` so those loops stay loops).
They are tested by running the real driver against a Python bus (`sdk/tests/driverhost.py`, `test_drivers_host.py`).
