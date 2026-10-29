# PURR Developer Kit: design (2026-10-28)

The kit lets one person build **any part of PURR OS** from a download, on any computer, without this repository or ESP-IDF. The end goal the owner
set: **a 100 % supported T-Deck Plus built through the SDK**: display, touch, keyboard, trackball, battery, GPS, LoRa, the SD card, sound, and the
radios, all as drivers and configuration a person can read, change and rebuild.

Decisions (owner, 2026-10-28; `plan5.md` has the log): prebuilt OS parts that the kit re-packages and re-signs with the developer's keys and settings
now, source builds as a separate later download; a bundled standalone Python; the first kit for this Mac only, with a host-neutral layout; the
emulator is a separate job (a fork of the ESP32 emulator, someone else's work), so the kit only leaves a hook for it.

## What the kit is made of

| Piece | What it is |
|---|---|
| `purr` (CLI) | One command for everything: `doctor`, `new`, `build`, `sign`, `verify`, `inspect`, `keys`, `serve`, `package`, `studio`. Thin over the library. |
| **PURR Studio** | The self-contained app: a Python program with a Metro-style window (a local web UI in a native window, or the browser). Drag a built image onto it: it shows what it is, whether it is signed and by whom, what is inside, and what you can do with it next. Tools for every part of the OS (below). |
| The library (`purrsdk`) | Everything the CLI and Studio do: inspect, verify, scaffold, build, sign, package, serve, edit configs. Plain Python, tested. |
| The build tool (`purrstrap`) | The existing compile / relocate / sign / package pipeline. The kit pins its own copy. |
| Driver classes | `purr_driver_class.h`: typed driver interfaces for GNSS (GPS), LoRa, cellular modem (LTE), Wi-Fi, Bluetooth (HCI), and the existing ones. A driver for any of them is an independent, signed module. |
| Templates | An app, a driver for each class, a board, a flavor. Each builds, signs and verifies with the kit. |
| Reference drivers | The real drivers the T-Deck Plus uses, shipped as readable, rebuildable sources: ST7789, GT911, keyboard, battery, keyboard backlight, BME280, NMEA GPS, SX1262 LoRa. |
| Board descriptions | A board is data (`board.toml`): pins, buses, devices, the driver for each. The T-Deck Plus is the reference. |
| The server | `purr serve`: hosts your builds, drivers and recovery images to a device on the local network (the dev server), and the repository tooling for publishing. |
| Docs | Written in the same friendly voice as the OS. |

## The bundle (one folder per host)

```
purr-sdk-<version>-<host>/
  bin/purr                  launcher (starts the bundled Python)
  python/                   standalone CPython with the few packages the tools need
  toolchain/                the trimmed xtensa-esp-elf: compiler, binutils, libgcc
  include/                  the ABI headers, versioned
  lib/purrstrap/ lib/purrsdk/   the build tool and the library
  studio/                   the app (server, static web UI)
  templates/  boards/  reference/   starters, board files, the real drivers' sources
  images/                   prebuilt OS parts to repackage (for flavors)
  keys/test/                the published test key (insecure by design)
  docs/                     reference and guides
  VERSION, SDK-MANIFEST     the version and every file's hash; `purr doctor` checks the kit against it
```

## Studio: the tools

Home is a panorama of tiles (Metro): the newest at the top, each tile a tool. Friendly voice throughout ("Let's see what you've got.").

1. **Open an image** (drag and drop, or Browse): any `.cat`, `.kitt`, bootloader, boot package, a manifest, or a whole 16 MB flash image. Shows the header, who signed it
   (which key, which role, may that role sign this kind), the payload hash, the version, the board, and for a flash image the partition table, the trust block (mode,
   development flags, keys), the slots and their signatures. Then "what next": sign it, publish it to the dev server, build a release around it.
2. **New project**: app, driver (pick a class), board, flavor. Fills in a working starter.
3. **Build and sign**: builds a project, signs it with a chosen key, verifies, shows the result and its size.
4. **Keys**: make your own, import, see the published test key, who may sign what.
5. **Boards and drivers**: edit a `board.toml` (pins, buses, devices) with checks against reserved pins; pick a driver for each device; see which classes are covered.
6. **Release**: gather components into a recovery release (manifests, hashes, signatures) the way `make_release` does; verify it as the recovery client would.
7. **Server**: start and stop the dev server, see what it hosts and who found it, publish a build to it with one click.
8. **Configure**: edit the files that configure a device: `system.conf`, `apps.conf`, the discovered `board.conf`, the trust block, the boot configuration.
9. **Boot**: the boot package's screens and the logo; the menu key; Fast startup defaults.
10. **UI assets**: pack the images the shell uses; preview them.
11. **Drivers lab**: for each class, a form to try a driver's interface against a fake bus before it touches hardware.
12. **Doctor**: is the kit whole, is each tool found, are the keys sane.

Not in Studio tonight: the emulator (separate job; the "Run" tile is a placeholder that says so kindly).

## Driver classes (the ABI the OS and the SDK share)

- A driver is a freestanding module (`purr_driver_t`) that reaches hardware only through the **bus table**. ABI 6 adds UART (open, read, write, baud, close) to the existing
  SPI, I2C, GPIO, ADC, PWM backlight, delay and clock.
- ABI 6 drivers also declare a **class** and an **ops table** (`class_id`, `ops`). The kernel keeps a class registry: "give me the GNSS driver" returns its ops and
  handle. Drivers built for ABI 4 and 5 keep working unchanged; the kernel never reads the new fields from them.
- A device's pins and parameters travel as a small key/value config (`purr_kv_cfg_t`: `tx=43 rx=44 baud=38400`), so a driver for any bus is configured by data, in the board file
  or the discovered profile, without a new C struct per driver.
- Classes (ops tables are versioned; the first field of each is `uint32_t version`): `gnss`, `lora`, `modem`, `wifi`, `bluetooth` (HCI transport). The OS consumers start
  small (shell commands `gps`, `radio`, `modem`); catcalls for apps and the network stack's use of an external Wi-Fi or modem link follow, and are marked not-yet where they are.
- Everything in a driver is testable on the host: a driver is compiled against a fake bus (a scripted UART or SPI) before a board is ever involved.

## The T-Deck Plus inventory (the end goal; status tonight)

| Part | Chip and bus (LilyGO's published pins, to be checked on the unit) | Driver | State |
|---|---|---|---|
| Display | ST7789 320x240, SPI, CS 12, DC 11, backlight 42 | `st7789` | works on the device |
| Touch | GT911, I2C 0x5D (the board file has the verified wiring) | `gt911` | works on the device |
| Keyboard | its own controller, I2C 0x55 | `kbd-i2c` | works on the device |
| Keyboard backlight | same controller, command `[0x01, level]` | `kbd-backlight` | works on the device (owner confirmed) |
| Battery | ADC GPIO 4 behind a 1:2 divider | `battery-adc` | built; not yet read on the device |
| GPS | u-blox MIA-M10Q, UART, TX 43 / RX 44 | `gnss-nmea` | written tonight; host-tested only |
| LoRa | SX1262, SPI shared with the display, CS 9, RST 17, DIO1 45, BUSY 13 | `lora-sx126x` | written tonight; host-tested only |
| Trackball | five GPIOs (up, down, left, right, click) | not yet | template only |
| SD card | SPI shared, CS 39 | not yet | template only |
| Speaker, microphone | I2S | not yet | template only |
| Wi-Fi, Bluetooth | the ESP32-S3's own radios | Wi-Fi is in the kernel; Bluetooth has no stack | `wifi` and `bluetooth` classes defined, templates written |
| LTE | external modem on a UART (not on the board) | `modem-at` | written tonight; host-tested against a scripted modem |

Where a pin says "to be checked", the board file carries it as data so it is a one-line change, not a code change.

## Branding (added by the owner, last request of the night)

People will make their own flavors, so the look and the names of the system are data, not code. A flavor has a `brand.toml`:

```toml
[brand]
product   = "PURR OS"          # the name in the boot menu, the status text and About
recovery  = "KittenOS"         # the name of the recovery system
codename  = "BlackDeck+"       # the board line under the logo
logo      = "PURR OS!"         # the wordmark on the boot screens (bold italic)
logo_accent_from = 7           # characters from this position on use the accent colour (-1 for none)

[colours]
accent    = "#FFB400"          # boot screens: the logo's accent, the selection border, the default tag
text      = "#FFFFFF"
dim       = "#949694"
ok        = "#52CA7B"
warn      = "#FFB400"

[shell]
accent    = "Cobalt"           # the shell's default accent colour (the Windows Phone palette names)
```

`purrstrap brand build <brand.toml>` checks it (lengths that fit the 8 x 8 font on a 320 px line, colours, names) and writes the headers the boot package, the loader, KittenOS and the shell are built with.
The boot package (the screens the bootloader shows) takes the logo, the colours and the product names; the loader, KittenOS and the shell take the names and the default accent.
The bootloader itself (flash 0x0) has no screen of its own: its log lines are the only text, and it reports the version, not a brand. So rebranding the "bootloader" means rebranding the boot package. Studio has a Brand tool with a
live preview of the splash, the menu and the problem screen, so a flavor's look can be seen before anything is built.

## Order of work (tonight)

1. The driver class ABI, the kernel class registry, bus ABI 6 (UART), the key/value device config, host tests.
2. The reference drivers for the classes and their host tests against fake buses.
3. The library and the CLI (inspect, verify, scaffold, build, sign, package, serve, config) with tests.
4. Studio.
5. Templates, docs, the kit builder and a kit check.

Each step is committed on its own. Nothing is published, flashed or put on the dev server: the 1.0.10 release and the board are untouched.
