# 17. The SDK (the PURR Developer Kit)

*Current as of PURR OS 1.1 and SDK 2.0 (2026-11-09).*

## Short version

- The kit is one folder (about 360 MB with the emulator) that lets one person build and sign apps, drivers, boards and flavors without this repository or ESP-IDF. Its version is **2.0.0** (`sdk/purrsdk/__init__.py`, owner decision of 2026-11-09).
- Two ways in: **PURR Studio** (a Windows-Phone-styled local web app) and the **`purr` command**. `purr doctor` checks the kit; `purr run <app>` builds, signs, serves and runs an app in the emulator.
- Sources are in `sdk/`; the kit is made by `python3 sdk/build-kit.py`. The user guides are the files in [sdk/docs/](../sdk/docs/00-start.md). **This chapter links them and does not repeat them.**
- Two roads to a system image. **Ready-made:** the kit re-packages and re-signs prebuilt OS parts. **From source:** Studio's *build a system* (or `purr system build`) pulls the PURR OS source, compiles the firmware, system modules, drivers and apps with ESP-IDF 5.3.5 (installed once, not in the kit), signs them with the person's own keys, and writes the key list the image trusts: PURR's keys kept (default) or only theirs ([16-device-image.md](../sdk/docs/16-device-image.md), [06-keys.md](../sdk/docs/06-keys.md)).
- Plan: [plan5.md](../plan5.md) (inventory of 2026-11-06) and Part A of [showtime1.md](../showtime1.md).

## What it is for

Anyone should be able to write an app or a driver, or describe a new board, from a download. The owner's end goal (2026-10-28, [sdk/DESIGN.md](../sdk/DESIGN.md)) is "a 100 percent supported T-Deck Plus built through the SDK": every part as drivers and configuration a person can read, change and rebuild.

## The files

| File | What it is |
|---|---|
| [sdk/DESIGN.md](../sdk/DESIGN.md) | Design of the kit (2026-10-28) |
| [sdk/purr.py](../sdk/purr.py) | The `purr` entry point |
| [sdk/purrsdk/cli.py](../sdk/purrsdk/cli.py) | The command parser; every sub-command |
| [sdk/purrsdk/](../sdk/purrsdk/__init__.py) | The library: inspect, keys, templates, build, release, devserver, repo, runner, emulator, flash |
| [sdk/purrsdk/emulator.py](../sdk/purrsdk/emulator.py), [runner.py](../sdk/purrsdk/runner.py) | `purr emulator` and `purr run` |
| [sdk/purrsdk/repo.py](../sdk/purrsdk/repo.py) | `purr repo` |
| [sdk/purrsdk/templates.py](../sdk/purrsdk/templates.py) | Starters for app, driver, board, flavor |
| [sdk/studio/server.py](../sdk/studio/server.py), [sdk/studio/web/](../sdk/studio/web/index.html) | Studio: a local server and its pages |
| [sdk/build-kit.py](../sdk/build-kit.py) | Builds the kit folder |
| [sdk/boards/tdeck_plus.toml](../sdk/boards/tdeck_plus.toml) | The reference board file |
| [sdk/tests/](../sdk/tests/test_core.py) | The kit's tests |
| [sdk/NOTICES-emulator.md](../sdk/NOTICES-emulator.md) | Licence notices for the bundled emulator |
| [AppSDK/SPEC.md](../AppSDK/SPEC.md), [AppSDK/templates/](../AppSDK/templates/unprivileged) | The app SDK spec and starters |

## The guides

| Guide | Topic |
|---|---|
| [00-start.md](../sdk/docs/00-start.md) | Start here, what is in the kit |
| [13-first-hour.md](../sdk/docs/13-first-hour.md) | Your first hour: make, build, run, publish |
| [14-ui-packs-and-themes.md](../sdk/docs/14-ui-packs-and-themes.md) | UI packs and themes: `purr ui pack`, `purr ui check`, `purr new theme` |
| [15-windows.md](../sdk/docs/15-windows.md) | The kit on Windows (written, not yet run on Windows); full guide in [chapter 22](22-building-on-windows.md) |
| [01-studio.md](../sdk/docs/01-studio.md) | The tour of Studio |
| [02-drivers.md](../sdk/docs/02-drivers.md) | Writing a driver |
| [03-boards.md](../sdk/docs/03-boards.md) | Describing a board |
| [04-t-deck-plus.md](../sdk/docs/04-t-deck-plus.md) | The T-Deck Plus board |
| [05-flavors.md](../sdk/docs/05-flavors.md) | Making your own flavor |
| [06-keys.md](../sdk/docs/06-keys.md) | Keys and signing |
| [07-release-and-server.md](../sdk/docs/07-release-and-server.md) | Releases and the dev server |
| [08-configuring.md](../sdk/docs/08-configuring.md) | Configuring a device |
| [09-flashing.md](../sdk/docs/09-flashing.md) | Flashing an image |
| [10-cli.md](../sdk/docs/10-cli.md) | The `purr` command |
| [11-repositories.md](../sdk/docs/11-repositories.md) | Running a repository |
| [12-emulator.md](../sdk/docs/12-emulator.md) | The emulator |
| [16-device-image.md](../sdk/docs/16-device-image.md) | One flash image for a device, and compiling everything yourself |
| [17-making-a-ui.md](../sdk/docs/17-making-a-ui.md) | Making a UI from one small file or a SquareLine export |
| [TEST-PLAN-2.0.md](../sdk/TEST-PLAN-2.0.md), [NOTES-compile-from-source.md](../sdk/NOTES-compile-from-source.md) | The 19 tests with the bench result of each, and the log of what happened building the source road |

## How it works

### The kit's parts

| Part | Role |
|---|---|
| `bin/purr`, `PURR-Studio.command` | Launchers that use the bundled Python |
| `python/` | A standalone CPython 3.12 with the `cryptography` module |
| `toolchain/` | The xtensa-esp-elf compiler, trimmed to the ESP32-S3 and C |
| `sdk/`, `purrstrap/` | The library, Studio and the build tool underneath |
| `PurrOS/components/...`, `AppSDK/`, `Modules/`, `Drivers/`, `bootpkg/` | ABI headers, starters, reference driver sources and specs |
| `images/` | Prebuilt parts of a system (kernel, KittenOS, loader, boot package, bootloader) |
| `emulator/` | The window, the headless runner, the patched QEMU and board files ([chapter 18](18-emulator-and-vm.md)) |
| `VERSION`, `SDK-MANIFEST.json` | Version and file hashes; the kit checks itself |

### The commands

The complete table is in [10-cli.md](../sdk/docs/10-cli.md). The ones that matter first:

| Command | Does |
|---|---|
| `purr doctor` | Checks the kit is whole |
| `purr new app|driver|board|flavor NAME` | Starts a project from a template |
| `purr build PROJECT [--key K --key-id N]` | Builds and signs |
| `purr keys new --role R --key-id N` | Makes your own key |
| `purr system new\|set\|show\|build\|test NAME` | A device with its parts, drivers, apps and keys: build it from the signed parts or from source, then boot the image in the emulator and check it |
| `purr source check\|pull\|compiler\|install-compiler` | The source a system is compiled from |
| `purr serve`, `purr publish FILE` | The dev server and putting a build in its folder |
| `purr run TARGET` | Builds, signs, serves, boots the VM and installs through the Developer Portal |
| `purr repo init|add|build|verify|endorse` | Makes and runs a repository ([chapter 14](14-store-and-repositories.md)) |
| `purr flash IMAGE --port P` | Writes an image to a board over USB |

Projects, keys and builds live in `~/PURR-Projects` (change with `--workspace` or `PURR_WORKSPACE`). A key password comes from a prompt, or from `PURRSTRAP_KEY_PASSWORD` for scripts, for that one command only.

### Ready-made parts and compiling from source

The owner's decision (2026-10-28, [plan5.md](../plan5.md)) was prebuilt OS parts that the kit re-packages and re-signs with the developer's keys, with a source build as a later, separate step. A flavor (your own name and colours) therefore changes the boot package, and `PURR_BRAND_*` strings in the boot package, but not the kernel's own wording.

On 2026-11-09 the second road was added for a person who does not want to trust our binaries: pull the source, compile everything, sign with their own keys, choose whose keys the bootloader, kernel and boot package compile in. What it compiles: the kernel, recovery loader, KittenOS, boot package and bootloader; the eight system modules; the drivers and apps; and the 14 signed binaries that `PurrOS/main/commands.c` carries as byte arrays (the loader's display, touch and keyboard drivers among them) plus the Store. Details, and the list of mistakes made on the way, are in [sdk/NOTES-compile-from-source.md](../sdk/NOTES-compile-from-source.md).

### Studio

Studio is a Python program with a local web page (`sdk/studio/server.py` serves `sdk/studio/web/`). It is styled like a Windows Phone start screen on purpose; the big tile is the last thing you did, the others are tools grouped by purpose, and the circle with the arrow takes you home. Anything Studio does, the `purr` command also does. The tour is [01-studio.md](../sdk/docs/01-studio.md).

| Tile | Use |
|---|---|
| Open an image | Drop a `.cat`, `.kitt`, a release folder, a manifest, a key or a whole flash image. Studio says what it is, whether the hash matches, who signed it and whether that role may sign this kind of file. For a flash image it draws a coloured partition map (green signed and checked, red needs a look, grey erased) |
| New project, my projects | Start an app, a driver (pick a class: GPS, LoRa, modem, Wi-Fi, Bluetooth or a plain I2C sensor), a board or a flavor; edit and build with a key |
| Boards | Check a board file as you type and see the pin map and which parts have drivers ([chapter 19](19-boards.md)) |
| Brand | Change a flavor's names and colours and watch the boot screens change |
| Build a system | One road: device, parts, build, deliver. Drivers, apps, a UI and signing, one flash image, a delivery folder, a flash by cable, serving over the air, and a test in the emulator. Parts ready-made or compiled from source |
| Build a UI, make a UI | Draw a screen layout on the device's real screen size, or describe a theme in one file; SquareLine exports and icon packs are accepted |
| Release, server, configure, flash, doctor, docs | The rest of the commands, with checks |

The drag-and-drop gesture itself was not exercised in a headless browser; the upload route behind it is tested ([plan5.md](../plan5.md)).

### Projects and templates

`purr new app NAME` copies a starter that builds. `purr new driver NAME` gives a complete module with the right interface for its class and TODO notes where your code goes. Templates are generated by `sdk/purrsdk/templates.py`; the app starter comes from [AppSDK/templates/](../AppSDK/templates/unprivileged). The build step is `purrstrap modules build`: compile, relocate (a `switch` jump table must be treated as code), and pack into a `.cat`. A build without a key is unsigned; a device only runs an app signed by a key it trusts, or an unsigned one in developer mode ([chapter 14](14-store-and-repositories.md), [06-keys.md](../sdk/docs/06-keys.md)).

### Drivers and boards in the kit

The kit ships readable sources for the real drivers the T-Deck Plus uses (display, touch, keyboard, battery, keyboard backlight, BME280, GPS, LoRa, SD) and typed driver classes for GPS, LoRa, modem, Wi-Fi and Bluetooth. Reference drivers are tested against a Python model of the bus (`sdk/tests/driverhost.py`), not on a board. [02-drivers.md](../sdk/docs/02-drivers.md) is the guide; the board side is [chapter 19](19-boards.md).

### Flavors

A flavor is a `brand.toml`: product and recovery names, a codename, a wordmark, boot-screen colours and the shell's first accent. `purr brand check` verifies it and `purr brand build` writes the header the system is built with. [05-flavors.md](../sdk/docs/05-flavors.md) has an example.

### Keys

A device trusts only the keys in its key bag. The kit never contains the project's signing keys. The unlock for a locked device is not weakened by any SDK step. The published **test key** idea (an insecure key for development images) is in [plan5.md](../plan5.md); a ready-made VM image that trusts it was **not delivered**, because the trust block must be signed with the boot key by the owner ([showtime1.md](../showtime1.md), VM image design).

## How to build and test it

1. Build the kit (needs network the first time, for the Python build and wheels, cached in `out/sdk-cache`):

   ```
   python3 sdk/build-kit.py --out ~/Desktop/PURR-SDK
   ```

   Options: `--images DIR`, `--no-python`, `--no-toolchain`, `--no-emulator`, `--emulator-src DIR`, `--keep-cache`. The emulator is built from a read-only checkout of the emulator project with the compiler on this computer; the kit's user needs no compiler ([chapter 18](18-emulator-and-vm.md)).
2. Check it: `bin/purr doctor`, then `bin/purr version` prints `PURR Developer Kit 2.0.0`.
3. SDK tests: `python3 -m unittest discover -s sdk/tests -t sdk/tests`. About 370 pass on 2026-11-09 (the number grows; some are skipped on a machine without git, the emulator or the signing library). The driver tests are the 41+ in `sdk/tests/test_drivers_host.py`, run against a Python model of the bus.
4. `cd purrstrap && python3 -m unittest discover -s tests`: 158 passed with the kit's Python on 2026-11-09 (with the system Python the two key tests fail to import). `test_keys` and `test_trustblock` need the `cryptography` module, which the system Python may lack; use the IDF Python or the kit's own.
5. Walk [13-first-hour.md](../sdk/docs/13-first-hour.md) as a stranger would. The owner's "done when" is that the kit alone takes someone from "new project" to "running in the emulator" and on to "installed on a T-Deck Plus through the dev server".

## What goes wrong

| Symptom | Cause | Fix |
|---|---|---|
| `purr run` finds no VM image | No image ships in the kit yet | Supply an image you built; the planned `purr vm` command and prompt are step (1) of the VM image plan in [showtime1.md](../showtime1.md) |
| `purr run` stops at the Developer Portal's server page | An emulator panic after the Find servers tap | See [chapter 18](18-emulator-and-vm.md) and [plan7.md](../plan7.md) |
| Tests fail with a missing `cryptography` module | System Python lacks it | Use the kit's Python or the IDF Python |
| `purrstrap apps build` fails | It needs Python 3.11 (`tomllib`) | Use `modules build --kind app` |
| Emulator refuses to open after download | macOS quarantine | `xattr -dr com.apple.quarantine <the kit folder>` |

## What is not finished

- **Part A of showtime1** (the SDK up to spec) is waiting for the owner's keyword. The inventory of 2026-11-06 in [plan5.md](../plan5.md) says: the kit builds end to end; `purr run` works up to the Portal's server page; find, trust, install and launch were done by hand, not scripted end to end.
- **Plan and code:** plan5's status table (dated 2026-10-29) and the inventory (2026-11-06) both live in the plan; the table is the older. The kit version in the older table is 0.1.0; the code says 2.0.0.
- **Not run on a device:** flavors, drivers on a board, Studio drag-and-drop (the upload route behind it is tested; the gesture itself was not).
- **Phase D of plan6** (the app lifecycle in the SDK) is not started.
- **A shell choice** (Windows 10 Mobile, Android) is built for source builds (the kernel is compiled with one of them, `--shell`); a UI-select button for an already-built image is not ([chapter 11](11-ui-stack-and-shells.md)).
- **Emulator licence:** GitHub shows no licence file for the emulator repository though the notice says MIT OR Apache-2.0 (owner decision 7, 2026-11-06). The ROM images are loaded unchanged and no separate licence text came with them (decision 8).
- **Platform:** the emulator in the kit runs on an Intel Mac with macOS 26 or newer today. The owner wants a lower target (decision 9); the emulator's session was asked to rebuild with one. Result not recorded.
- Docs still to write there: a command and catcall reference, annotated Photos, Clock and Files examples ([showtime1.md](../showtime1.md), step 7).

Next: [18 The emulator and the VM](18-emulator-and-vm.md).
