# 07. Drivers, modules and the driver bus

## Short version

- A **driver** is a small signed file (`.cat`, image type module, subtype driver) in `/boot/drivers`. It has no libc and reaches hardware only through the **driver bus table**, `purr_driver_bus_table_t`, whose layout is the ABI. The ABI is version **7**; the oldest still served is 4.
- A board file names each device and the driver it needs (`compatible`). The kernel's registry matches them, claims the pins and calls `probe()`.
- Five drivers are baked into the firmware and **planted again on every start**: display, touch, keyboard, battery, keyboard backlight. They are never set aside.
- Other drivers come from the driver index (`drivers.manifest`, published next to the module index in JumpingJaguar2) and are fetched one at a time by the Device Manager.
- To change the bus table: **append only**, bump the version, then rebuild, re-sign and re-embed every baked-in driver.

## What it is for

Drivers keep hardware code out of the kernel and let a board be described by data. One driver file serves different boards with different wiring, because the pins come from the board profile, not from the driver. A user or vendor can add a driver without a new firmware, under the same signing rules as everything else.

## The files

| File | What it is |
|---|---|
| [PurrOS/components/kernel/include/purr_driver.h](../PurrOS/components/kernel/include/purr_driver.h) | `purr_driver_t`, device states, `register`, `bind_all` |
| [PurrOS/components/kernel/include/purr_driver_bus.h](../PurrOS/components/kernel/include/purr_driver_bus.h) | The bus table and the ABI version numbers |
| [PurrOS/components/kernel/include/purr_driver_class.h](../PurrOS/components/kernel/include/purr_driver_class.h) | Driver classes (GNSS, LoRa, modem, Wi-Fi, HCI, block) and their ops tables |
| [PurrOS/components/kernel/include/purr_board.h](../PurrOS/components/kernel/include/purr_board.h) | The board profile structs |
| [PurrOS/components/kernel/boards/tdeck_plus.c](../PurrOS/components/kernel/boards/tdeck_plus.c) | The T-Deck Plus device list (also `tdeck.c`, `waveshare154.c`) |
| [PurrOS/components/kernel/src/purr_driver.c](../PurrOS/components/kernel/src/purr_driver.c) | Matching and pin claims |
| [PurrOS/main/commands.c](../PurrOS/main/commands.c) | The loader (`load_one_driver_file`), the baked-in blobs, planting, quarantine, deferred loading |
| [PurrOS/main/purr_hwsetup.c](../PurrOS/main/purr_hwsetup.c) | Find hardware, driver install and update |
| [PurrOS/main/purr_drvcmds.c](../PurrOS/main/purr_drvcmds.c) | Shell commands for the driver classes: `gps`, `radio`, `modem`, `sd` |
| [PurrOS/components/coreos/src/purr_hwprofile.c](../PurrOS/components/coreos/src/purr_hwprofile.c) | The discovered profile `/etc/board.conf` (read as untrusted input) |
| [Modules/drivers/](../Modules/drivers/) | Source of the 14 drivers |
| [Modules/coreos/](../Modules/coreos/), [Modules/appmanager/](../Modules/appmanager/), [Modules/netinstall/](../Modules/netinstall/), [Modules/network/](../Modules/network/), [Modules/test/](../Modules/test/) | Source of the command modules and test modules |
| [Drivers/SPEC.md](../Drivers/SPEC.md) | Packaging, versions, the bus table rules (the most useful spec here) |
| [Modules/SPEC.md](../Modules/SPEC.md) | The module container, relocation, loading and quarantine |
| [purrstrap/scripts/modules.py](../purrstrap/scripts/modules.py) | The module builder |
| [tools/release/make_drivers_index.py](../tools/release/make_drivers_index.py) | Writes `drivers.manifest` |
| [PurrOS/components/coreos/test/host/fakebus.h](../PurrOS/components/coreos/test/host/fakebus.h) | A fake bus for testing a real driver on a PC |

## The drivers that exist

| Driver | Class | Bus | Notes |
|---|---|---|---|
| `st7789` | display | SPI | The T-Deck panel. Baked in |
| `gt911` | touch | I2C | Baked in |
| `kbd-i2c` | keyboard | I2C | Baked in |
| `battery-adc` | battery | ADC | Percent, millivolts, a "cell connected" flag. Baked in |
| `kbd-backlight` | backlight | I2C | The control channel is `read()`. Baked in |
| `kbd-raw` | keyboard | I2C | Raw key bitmap for games. Loads before sign-in |
| `trackball-gpio` | none | GPIO | `read()` returns `[dx, dy, click, 0]`. Loads before sign-in |
| `gnss-nmea` | GNSS | UART | GGA, RMC, GSV |
| `lora-sx126x` | LoRa | SPI | SX1261/62/68. Deferred until Wi-Fi settles |
| `sd-spi` | block | SPI | SD, SDHC, SDXC in SPI mode; mounted as `/sd` through FatFs (FAT only, no exFAT) |
| `modem-at`, `wifi-esp-at`, `hci-h4` | modem, Wi-Fi, HCI | UART | Reference drivers; no board has the hardware yet |
| `bme280` | sensor | I2C | Example sensor |

## How a driver is loaded

1. **Plant.** At start, `purr_modules_setup()` writes the five baked-in blobs into `/boot/drivers`, replacing any file that differs. These files come only from the firmware.
2. **Pick what to load now.** On the full system only the input drivers needed for sign-in (`kbd-raw`, `trackball-gpio`) and the baked-in ones load at start. Every other file loads once somebody has signed in (`purr_drivers_load_deferred()`, called from `purr_session_started()`), on a 12 KB internal-stack task. A driver that stops the device then can no longer lock the user out.
3. **Verify.** The loader reads the file, runs `purr_image_verify()` (chip, role, hash, signature), and checks `bus_abi` is between `PURR_DRIVER_BUS_ABI_MIN` (4) and the current version (7). A mismatch is refused by name. An unsigned driver loads only in developer mode with "allow unsigned apps and drivers" on, only from `/boot/drivers`, and still needs its hash to match.
4. **Relocate.** A copy goes to a 64 KB-aligned buffer in PSRAM, the data pass relocates against the writable alias, the code pass against the executable alias, and the cache is flushed in the order the spike proved ([Modules/SPEC.md](../Modules/SPEC.md) section 7).
5. **Entry.** The loader calls `entry(bus_table)`. The driver returns a `purr_driver_t` (name, `compatible`, `probe`, `remove`, optional `read`, `bus_abi`, optional `class_id` and `ops`) and the registry registers it.
6. **Bind.** The registry probes every device whose `compatible` matches and whose pins were free. A device that fails is marked, and nothing else is affected.

### The crash guard

Before each `probe()` the kernel calls `purr_driver_guard()`, and the OS writes the driver's file name to `/boot/drivers.trying`; after the probe it removes the file. If the device dies inside a probe, the next start finds the file and moves that driver to `/boot/drivers.crashed` **before loading anything**. The boot log says which, and Device Manager lists it with a way to put it back. The baked-in drivers are exempt: without the display there is nothing to show. The deferred loader also writes `/boot/drivers.settling` while it loads.

### Putting set-aside drivers back (1.0.34)

A driver that stops the device is set aside in `/boot/drivers.crashed`. When the start before this one never settled (the failure count is 2 or more), *every* downloaded driver is set aside as a precaution, which also hit devices after an update. Since 1.0.34 a task puts those back **one at a time, after sign-in, without a restart**: it waits until the 30 s settle window has closed, posts "The system is still setting itself up in the background.", then for each driver writes its name to `/boot/drivers.retrying`, moves the file back, loads it, binds the device it is for, and waits 20 s before the next. If the device dies meanwhile, the next start finds the marker, sets that driver aside again and lists it in `/boot/drivers.bad`; a driver on that list, or one caught by the probe guard above, is never put back by itself (Device Manager > Drivers still offers it). The failure count is also cleared before the restart that follows an update. A driver that fails more than 20 s after it was put back is not blamed. Not run on a device.

### The bus table

Everything a driver may do is a function pointer in the table, in this order of introduction:

| ABI | Added (always at the end) |
|---|---|
| 4 | SPI add/transfer, I2C add/write/read/write-read/remove, GPIO set/read/write, `delay_ms`, `now_us`, `dma_alloc`, backlight PWM |
| 5 | `adc_read_mv` |
| 6 | UART open, write, read, set baud, close (ports 1 and 2; port 0 is the console) |
| 7 | `spi_lock`, `spi_unlock` (recursive; the display, radio and SD card share one SPI bus) |

Why the rule is strict: when a function was once inserted in the middle of the table, a touch driver built earlier called `now_us()` and landed on `delay_ms()` with a leftover register as the argument. That is a 12.8-day sleep inside LVGL's input timer, and the UI froze on the first white frame. Nothing checked the version then; now the loader does.

### Classes

A driver built for ABI 6 or later may name a **class** and an ops table whose first field is a version. The kernel keeps a registry by class (`purr_class_find`). Always read the class with `purr_driver_class_id()`, never the field, because a driver built for an older ABI does not have the field.

## Board data and discovered devices

Devices in the board file are fixed. For hardware the board file does not know, Settings > Updates > **Find hardware** scans I2C, and the owner confirms what was found. The result goes in `/etc/board.conf` (name, driver, bus, pins, address). The file is read as untrusted input: a reserved pin or a bad address drops the entry. Discovered devices are bound after the boot-time driver load, in a second board object, on their own I2C controller, so the real board profile is never edited and the keyboard's bus can never be handed to them by mistake. A device that is not I2C carries its settings as up to twelve named integer pairs (`purr_kv_cfg_t`).

## Command modules

The same container and loader also serve **command modules**: `about`, `apps`, `wifi`, `netinstall` (loaded from `/system`) and `sysinfo`, `fs`, `login`, `appmgr`, `debug` (from `/kernelmods`). They are planted from blobs in the firmware if missing, and a signed set is published in JumpingJaguar2 ([chapter 08](08-updates-and-release-channels.md)). At each start every file in those folders is verified and loaded. A file that fails is moved into `/system/.rejected` and never retried; one bad file never stops the others.

## How to build and test a driver

1. Build: `python3 purrstrap/purrstrap.py modules build --board tdeck_plus --kind driver --source Modules/drivers/<name>_module.c --entry <name>_entry --name <name> --version <n> --out <name>.cat`. `--source` takes a comma-separated list.
2. Sign with the developer key (key id 2) as in [chapter 03](03-trust-and-keys.md).
3. Publish to the driver list: `python3 tools/release/make_drivers_index.py --out <dir> --driver <file>:<compatible>:<buses>:<catcall>[:<abi_min>] ...`. **The script writes the manifest from only the `--driver` arguments, so list every driver each time.** A driver that calls `adc_read_mv` needs `abi_min` 5 or more; one that calls `uart_open` needs 6; one that calls `spi_lock` needs 7.
4. Copy the files and `drivers.manifest` to the dev server's `recovery/` folder, or into a release.
5. Test on a PC: run the real driver against a Python bus (`sdk/tests/test_drivers_host.py`) or against `fakebus.h` in the C host tests (`test_drv_gnss`). `test_driver` covers the registry.
6. After any change to the bus table: raise the version, **rebuild, re-sign and re-embed the baked-in blobs** in `commands.c` (`s_kbd_i2c_driver_module`, `s_gt911_driver_module`, `s_st7789_driver_module`, `s_battery_driver_module`, `s_kbd_backlight_driver_module`).

## What goes wrong and how to fix it

| Symptom | Cause | Fix |
|---|---|---|
| A driver is refused with a `bus_abi` message | Built against a version outside 4 to 7 | Rebuild against the current header |
| UI frozen on the first frame after a driver change | Table layout changed in the middle | Append only; rebuild every driver |
| A driver app dies at its first `switch` | Jump table treated as data | Rebuild with `modules.py` from 2026-11-01 or later ([plan9.md](../plan9.md) recorded the `modem-at` driver as still to republish that day; not rechecked) |
| Device stops during start, then boots without a driver | The crash guard set it aside | Device Manager > Drivers; fix the driver; "put it back" |
| Stack error loading a driver | The loading task had a PSRAM stack; a driver hands stack buffers to DMA | Load drivers only on internal-stack tasks ([plan9.md](../plan9.md), 2026-11-05) |
| Manifest lists only some drivers | `make_drivers_index.py` rewrites from its arguments | List all drivers every run |
| Files in `/boot/drivers` keep reverting | They are baked-in blobs planted every start | Change the blob in the firmware, not the file |

## What is not finished

- **Not verified on a device:** the crash guard, one-at-a-time driver install, and deferred loading (built 2026-11-05, run in the VM at most). The LoRa driver shares the SPI bus with the display and SD card and was the main suspect for the 2026-11-05 crash; install it last ([plan9.md](../plan9.md)).
- **No audio driver class.** There is no I2S output driver and no `audio` catcall ([plan9.md](../plan9.md)).
- **Driver signing is meant to be restricted** after the key rotation, so the developer key signs only in developer mode (owner decision, [plan3.md](../plan3.md)). It is not in `purr_role_may_sign()` yet ([chapter 03](03-trust-and-keys.md)).
- **The pack idea** (a signed list that sets up a whole board with per-board pins) in [Drivers/SPEC.md](../Drivers/SPEC.md) section 2 is design; the real mechanism is the board file plus the per-driver index. Drivers/SPEC.md section 4 also still says code drivers are built into the kernel "until the spike passes"; that is out of date, drivers load from files now.
- **Open:** dependencies between drivers, two drivers matching one device, and rolling a bad driver back ([Drivers/SPEC.md](../Drivers/SPEC.md) section 6, [plan8.md](../plan8.md) "Driver foundation").

Plan references: [plan2.md](../plan2.md) (display and driver bus), [plan8.md](../plan8.md) (driver index, Find hardware), [plan9.md](../plan9.md) (crashes and deferral).
