# 19. Boards

*Current as of PURR OS 1.1 (2026-11-08).*

## Short version

- A **board** is data: which chip, which buses, which pins, which devices. In the firmware it is one C file in `PurrOS/components/kernel/boards/`, picked by a `CONFIG_PURR_BOARD_*` option.
- Boards with a kernel board file today: **T-Deck**, **T-Deck Plus** (the primary, the one this OS is tested on) and **Waveshare ESP32-S3-ePaper-1.54**. The **CYD 2.4C** is named in the build tool and the bootloader but has no kernel board file.
- In the SDK a board is a `.toml` file (`sdk/boards/tdeck_plus.toml`). `purr board check` and `purr board build` turn it into the board file and the device profile `/etc/board.conf`.
- Devices that "Find hardware" discovers are saved to `/etc/board.conf` and bound to drivers at boot.
- Specs: [Boards/SPEC.md](../Boards/SPEC.md) (draft 0.1) and [sdk/docs/03-boards.md](../sdk/docs/03-boards.md).

## What it is for

The rule is plug and play: adding a board should mean adding data, not changing other code. A driver never hard-codes a pin; it reads its pins from the board table ([PurrOS/components/kernel/SPEC.md](../PurrOS/components/kernel/SPEC.md) section 2, the origin of the rule). The owner's goal for the T-Deck Plus is "100 percent supported": every part with a driver a person can read and rebuild ([sdk/DESIGN.md](../sdk/DESIGN.md)).

## The files

| File | What it is |
|---|---|
| [Boards/SPEC.md](../Boards/SPEC.md) | Which boards are supported, e-paper decisions, how a board is added |
| [purr_board.h](../PurrOS/components/kernel/include/purr_board.h) | The typed table: buses, display, keyboard, touch, devices |
| [tdeck_plus.c](../PurrOS/components/kernel/boards/tdeck_plus.c) | T-Deck Plus board file |
| [tdeck.c](../PurrOS/components/kernel/boards/tdeck.c) | T-Deck board file |
| [waveshare154.c](../PurrOS/components/kernel/boards/waveshare154.c) | Waveshare e-paper board file |
| [sdkconfig.board.tdeck_plus](../PurrOS/sdkconfig.board.tdeck_plus), [tdeck](../PurrOS/sdkconfig.board.tdeck), [waveshare154](../PurrOS/sdkconfig.board.waveshare154) | Per-board build options: flash size, PSRAM, partition table, fonts |
| [PurrOS/partitions/](../PurrOS/partitions/tdeck_plus.csv) | One partition table per board |
| [bootloader/](../bootloader/SPEC.md) | `sdkconfig.board.<board>` files; the bootloader is built once per board |
| [purr_hwprofile.h](../PurrOS/components/coreos/include/purr_hwprofile.h) | The discovered profile (`/etc/board.conf`) format and its checks |
| [purr_hwsetup.c](../PurrOS/main/purr_hwsetup.c) | Boot binding, "Find hardware", installing drivers |
| [purr_hwscan_plat.c](../PurrOS/main/purr_hwscan_plat.c) | The scan on a real board |
| [sdk/boards/tdeck_plus.toml](../sdk/boards/tdeck_plus.toml) | The SDK's reference board file |
| [sdk/purrsdk/boards.py](../sdk/purrsdk/boards.py) | `check`, `pin_map`, `coverage`, `to_c`, `to_profile` |
| [sdk/docs/03-boards.md](../sdk/docs/03-boards.md), [04-t-deck-plus.md](../sdk/docs/04-t-deck-plus.md) | Guides |
| [purrstrap/scripts/coreos.py](../purrstrap/scripts/coreos.py) | The `BOARDS` table: board to chip |
| [Install/SPEC.md](../Install/SPEC.md) | How a board is found and installed |
| [Drivers/SPEC.md](../Drivers/SPEC.md) | Driver classes and the bus table |

## How it works

### The supported boards

| Board | Chip | Flash and PSRAM | Kernel board file | State |
|---|---|---|---|---|
| T-Deck Plus | ESP32-S3 | 16 MB, 8 MB | yes | Primary. Display, touch, keyboard and keyboard backlight "work on the device" per the SDK table; GPS, LoRa, trackball, SD are written and tested against fakes |
| T-Deck | ESP32-S3 | see `sdkconfig.board.tdeck` | yes | Same family as the Plus; I did not compare the two board files |
| Waveshare ESP32-S3-ePaper-1.54 | ESP32-S3 | 8 MB, 8 MB | yes | A board file and a partition table in the flash map v2 shape exist (2026-11-08); the e-paper path is still compiled into the kernel (`epd1in54.c`). The minimal and recovery builds compile; the full profile does not link yet (no UI stack for this board) |
| CYD 2.4C | ESP32 | 4 MB, none | **no** | In `BOARDS` in `coreos.py`, a `bootloader/sdkconfig.board.cyd_24c`, and the spec; nothing in the kernel builds for it |

The spec also names the Heltec, JC3248W535, the CYD 2.8-inch variants, the Waveshare 1.69-inch and the Tab5 as "not yet planned". The Display and Boards notes agree that LVGL builds only for the two T-Deck boards ([UI/DISPLAY_NOTES.md](../UI/DISPLAY_NOTES.md)).

### Tiers

Boards come in tiers ([PurrOS/SPEC.md](../PurrOS/SPEC.md) section 1.1). **Modular** boards (T-Deck Plus, the Waveshare) have separate slots for the kernel, KittenOS and the loader. **Monolithic** 4 MB boards (the CYD) cannot hold a KittenOS with a network stack; their updates would come from an SD card ([chapter 13](13-networking.md)).

### The partition tables

The T-Deck Plus uses a 16 MB table with these data and app slots: `nvs`, `purrcfg`, `phy_init`, `bootpkg` (56 KB), `kittenos` (1.75 MB), `kernel` (2.125 MB), `loader` (1.75 MB), `netrec` and `root` (the file system, 0x5C1000 to the end of the flash, about 10.2 MiB). Each slot is 256 KB larger than up to 1.0.28. That is flash map v2 of release 1.0.29: the old 5 MB `ui_assets` partition is gone and the picture packs are files in `root` (not yet run on a device; see [chapter 2](02-flash-map-and-partitions.md)). The Waveshare table is smaller (kittenos 1.125 MB, kernel 1.25 MB, loader 1.125 MB) and keeps its own layout. The SDK carries the T-Deck Plus table in `sdk/boards/tdeck_plus.toml`. The slot sizes are also in `SLOT_BYTES` in `purrstrap/scripts/dev.py` and checked at build time; the full map is in [chapter 2](02-flash-map-and-partitions.md).

### One bootloader per board

purrstrap builds the same bootloader code once per board with the board's **codename** inside. A name compiled in cannot go missing the way a recorded value can, and it tells the installer which driver pack to fetch ([Boards/SPEC.md](../Boards/SPEC.md) section 3, [Install/SPEC.md](../Install/SPEC.md)).

### The board table and the device profile

Two sources describe the devices:

| Source | Where | When it is used |
|---|---|---|
| Board file | Compiled into the kernel from `boards/<id>.c` | Always; names the display, keyboard, touch, power pin, and the known extra devices |
| Device profile | `/etc/board.conf` | At boot, `purr_hwsetup_boot` binds each listed device to a loaded driver |

The profile is plain `key=value` stanzas separated by blank lines (`device=`, `compatible=`, `label=`, `bus=`, pin keys, `addr=`). It is read as **untrusted input**: a pin outside what a scan may touch, or an I2C address outside the legal range, drops that entry, so a hand-edited or corrupted file cannot make the system drive a flash or USB line. A device may list up to 12 settings, with keys up to 7 letters. At most 8 devices (`PURR_HWP_MAX`).

### Find hardware

Designed by the owner on 2026-10-28 ([plan8.md](../plan8.md)): one button that scans without asking questions where it can (passive first; nothing drives an output on a reserved pin), matches what it finds against a database, downloads the drivers, writes the pins to `/etc/board.conf` once the owner confirms, and loads them. What it cannot find (a part wired straight to a pin) is added by hand or with a hand-written profile. Device Manager ([chapter 16](16-apps.md)) is the front end.

Related behaviours: built-in devices have **planted** driver files in `/boot/drivers` and are exempt from quarantine; drivers load after the first sign-in except the ones sign-in needs; a driver that stops the device during start is set aside ([plan9.md](../plan9.md)).

### Devices marked "defer net"

A board device flagged `PURR_DEFER_NET` (the LoRa radio) is not given its pins while Wi-Fi starts, because starting both together was too much for the fast RAM. `purr_netsettle` lets them bind once Wi-Fi has an address.

### A board in the SDK

`sdk/boards/tdeck_plus.toml` has sections `[board]` (id, name, chip, flash and PSRAM, the pin that switches peripheral power, chip selects held high), `[spi]`, `[display]`, `[keyboard]`, `[touch]`, `[buttons]`, and `[[device]]` blocks. Each device has a `status`: `works`, `built`, `written` or `template`. The checker rejects reserved pins (flash, PSRAM, USB), two parts on one pin (shared bus lines and a shared reset line excepted), a setting name over 7 letters, more than 12 settings, a missing `compatible`, a bad `status`, and an id that is not a C name. Strapping pins get a gentle note.

**The pin numbers are not repeated here.** Read them from the board file, [sdk/docs/04-t-deck-plus.md](../sdk/docs/04-t-deck-plus.md) and the table in the board's `.toml`. Pins marked "LilyGO" there come from the vendor's headers and are not checked on this unit.

## How to build and test it

1. Check a board file: `bin/purr board check mine.toml`.
2. Write the board file and the profile: `bin/purr board build mine.toml --out build/`. This produces `<id>.c` and `board.conf`.
3. To use the board file in a source build, copy the `.c` into `PurrOS/components/kernel/boards/` (and add the option and source line, as `tdeck`, `tdeck_plus` and `waveshare154` have in the kernel's CMake file).
4. To try a profile on a device, copy `board.conf` to `/etc/board.conf`. The device reads it at boot.
5. A firmware build for a board passes `sdkconfig.board.<board>` in `SDKCONFIG_DEFAULTS` (see the runbook). `purrstrap coreos build --board tdeck_plus --profile full` builds one, in the background.
6. Tests: `python3 -m unittest discover -s sdk/tests -t sdk/tests -p "test_boards_templates.py"`; host tests `run.py hwprofile hwscan` in `PurrOS/components/coreos/test/host`.

## What goes wrong

| Symptom | Cause | Fix |
|---|---|---|
| A new board builds but nothing draws | Missing `sdkconfig.board.<board>`, partition table or Kconfig option | Add the three files; purrstrap lists the board folders in `BOARDS` |
| A device in the profile is ignored | A pin or address outside the allowed range | Read the log; fix `board.conf` |
| Reset line shared (touch and LoRa) | The board's wiring | The `.toml` leaves LoRa reset unset until confirmed; sharing resets the touch controller too |
| Driver does not bind after install | The device is `defer net` or drivers load after sign-in | Wait for Wi-Fi or sign in |
| Baked-in drivers get quarantined after a crash | Built-in drivers are planted every start | Deferral and quarantine exempt them ([plan9.md](../plan9.md)) |

## What is not finished

- **CYD:** named but not built. No kernel board file, no `sdkconfig.board`, no partition table in `PurrOS/partitions/` ([Boards/SPEC.md](../Boards/SPEC.md)).
- **E-paper:** a different refresh model; the `display` catcall's optional `refresh_now` and `set_refresh_mode` are in the spec. The power button (tap Back, long hold reboot, timed in the kernel) is a decision, not code I verified. Audio (ES8311) needs a driver.
- **Folder per board:** proposed, "not yet confirmed" in the spec. Today a board is spread over a board file, a sdkconfig overlay, a partition table and a bootloader overlay.
- Open in the spec: exact pin maps checked against vendor sources, the refresh policy, the tier of other boards.
- **Not verified on a device:** LoRa, GPS, trackball and SD drivers; the Waveshare image; Find hardware on a bare board.
- The SDK's T-Deck Plus table lists the keyboard backlight and battery states; the battery was "built, not yet compared with a multimeter".

Next: [20 Tools and scripts](20-tools-and-scripts.md).
