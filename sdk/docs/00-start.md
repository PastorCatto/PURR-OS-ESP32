# Start here

*Current as of PURR OS 1.1 (2026-11-08).*

Hello! This kit lets you make anything for PURR OS: an app, a driver for a chip, a description of a whole board, even your own flavor of the system with its own name and colours. Everything you need is in this folder. Nothing else has to be installed.

## The two ways in

- **PURR Studio** is the app. Double-click `PURR-Studio.command` (on Windows, the Studio shortcut), or run `bin/purr`. It opens a window. The big tile at the top is **the last thing you did**, so you can carry on where you stopped; the rest are grouped by what you want to do: make, deliver, trust, look and check, learn.
- **The `purr` command** does the same from a terminal, which is handy for scripts. Try `bin/purr doctor` first: it checks that the kit is whole.

## Try this first

1. Open Studio and **drop an image on it**. Any `.cat`, any `.kitt`, a release folder, even a whole 16 MB flash image. It tells you what it is, whether it is whole, who signed it, and what you can do with it.
2. Make a **new project**: pick "a driver", pick "GPS", give it a name. You get a working starter that builds.
3. Open **build a system**, choose a device, and press build. One road makes the drivers, apps and look you chose, the signed parts, one flash image for the whole device and a folder a device can fetch from. **test it automatically** then boots the image in the emulator and checks it.
4. Want nothing ready-made in it? On the device step choose **compile from the newest source**: Studio pulls the PURR OS source and compiles the firmware, the system modules, the drivers and the apps with your own keys, and you choose whether the image still trusts PURR's keys ([06-keys.md](06-keys.md), [16-device-image.md](16-device-image.md)). That needs ESP-IDF 5.3.5 (Studio can install it) and git; it takes ten minutes or more the first time.
5. Open **boards** and look at the T-Deck Plus, or **brand** and change the name and colours.

## The chapters

| | |
|---|---|
| [01-studio](01-studio.md) | the tour of Studio, screen by screen |
| [02-drivers](02-drivers.md), [03-boards](03-boards.md), [04-t-deck-plus](04-t-deck-plus.md) | drivers, board files, the reference device |
| [05-flavors](05-flavors.md) | your own name and colours for the system |
| [06-keys](06-keys.md) | keys, signing and whose keys an image trusts |
| [07-release-and-server](07-release-and-server.md), [11-repositories](11-repositories.md) | releases, the dev server, repositories of apps |
| [08-configuring](08-configuring.md), [09-flashing](09-flashing.md) | device settings, writing an image to a board |
| [10-cli](10-cli.md) | every `purr` command |
| [12-emulator](12-emulator.md), [13-first-hour](13-first-hour.md) | the emulator, a guided first hour |
| [14-ui-packs-and-themes](14-ui-packs-and-themes.md), [17-making-a-ui](17-making-a-ui.md) | pictures and themes, making a UI |
| [15-windows](15-windows.md) | the Windows build of the kit |
| [16-device-image](16-device-image.md) | one flash image, and compiling everything yourself |

## What is in the kit

| Part | What it is |
|---|---|
| `bin/purr` | the command |
| `sdk/` | the library and Studio, written in plain Python |
| `purrstrap/` | the build tool underneath: compile, relocate, sign, package |
| `toolchain/` | the compiler for apps, drivers and modules (ESP32-S3) |
| `python/` | the kit's own Python, with the signing library inside |
| `images/` | the prebuilt parts of a system, for releases, flavors and ready-made images |
| `emulator/` | the emulator (not in the Windows kit) |
| `Modules/drivers/` | real drivers to learn from |
| `sdk/boards/` | the reference board (T-Deck Plus) as a board file |

Compiling the firmware itself is not in the kit: it needs ESP-IDF 5.3.5, which Studio or `purr source install-compiler` fetches once.

## If something does not work

Run `bin/purr doctor`. It says what is missing in plain words. Studio's **doctor** tile shows the same thing.

## On Windows

There is a Windows build of the kit (without the emulator), written but not yet run on Windows: [15-windows.md](15-windows.md).
