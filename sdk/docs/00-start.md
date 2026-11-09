# Start here

*Current as of PURR OS 1.1 (2026-11-08).*

Hello! This kit lets you make anything for PURR OS: an app, a driver for a chip, a description of a whole board, even your own flavor of the system with its own name and colours. Everything you need is in this folder. Nothing else has to be installed.

## The two ways in

- **PURR Studio** is the app. Double-click `PURR-Studio.command`, or run `bin/purr`. It opens a window with a tile for each thing you can do.
- **The `purr` command** does the same from a terminal, which is handy for scripts. Try `bin/purr doctor` first: it checks that the kit is whole.

## Try this first

1. Open Studio and **drop an image on it**. Any `.cat`, any `.kitt`, a release folder, even a whole 16 MB flash image. It tells you what it is, whether it is whole, who signed it, and what you can do with it.
2. Make a **new project**: pick "a driver", pick "GPS", give it a name. You get a working starter that builds.
3. Open **boards** and look at the T-Deck Plus. Every pin it uses is on that page.
4. Open **brand** and change the name and colours. The boot screens on the right change as you type.

## What is in the kit

| Part | What it is |
|---|---|
| `bin/purr` | the command |
| `sdk/` | the library and Studio, written in plain Python |
| `purrstrap/` | the build tool underneath: compile, relocate, sign, package |
| `toolchain/` | the compiler for the ESP32-S3 |
| `python/` | the kit's own Python, with the signing library inside |
| `images/` | the prebuilt parts of a system, so you can make releases and flavors |
| `Modules/drivers/` | real drivers to learn from |
| `sdk/boards/` | the reference board (T-Deck Plus) as a board file |

## If something does not work

Run `bin/purr doctor`. It says what is missing in plain words. Studio's **doctor** tile shows the same thing.

## On Windows

There is a Windows build of the kit (without the emulator), written but not yet run on Windows: [15-windows.md](15-windows.md).
