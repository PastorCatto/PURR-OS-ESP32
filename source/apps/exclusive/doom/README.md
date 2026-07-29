# DOOM on PURR OS

DOOM, running under [Speed Demon](../../../../docs/15_SpeedDemon.md) — the app
takes the whole machine, and the OS is restored when it exits.

## Provenance

| Directory | Origin | Licence |
|---|---|---|
| `prboom/` | PrBoom 2.5.0, as vendored by Espressif in [esp32-doom](https://github.com/espressif/esp32-doom) | GPL-2.0-or-later |
| `wad_tables/` | Precomputed sine/tangent/gamma tables, same source | GPL-2.0-or-later |
| `compat/` | Espressif's `prboom-esp32-compat`, **substantially rewritten** | GPL-2.0-or-later / Apache-2.0 |
| `doom_app.c` | Written for PURR OS | GPL-3.0 (this repo) |

DOOM's source was released by id Software in 1999 under the GPL. The PrBoom
files here carry the standard header: *"either version 2 of the License, or (at
your option) any later version"* — GPL-2.0-**or-later**, which is what makes
them compatible with this repository's GPL-3.0. Espressif's own ESP32
modifications are Apache-2.0, also GPL-3.0 compatible.

**No WAD is included and none should be committed.** The game data is
copyrighted separately from the engine and is not redistributable here. See
below.

## Getting a WAD onto the device

Put any DOOM WAD in `/sdcard/doom/` on the SD card. The filename does not
matter — the directory is scanned for `*.wad`.

- **Shareware `DOOM1.WAD`** is freely redistributable and is the usual choice.
- Espressif's `doom1-cut.wad` (a trimmed shareware WAD, ~3 MB) works too.
- A commercial `DOOM.WAD` / `DOOM2.WAD` works if you own it.

If several are present an **IWAD is preferred over a PWAD**, because a PWAD is a
patch and cannot start the game on its own.

If no WAD is found, the app says so on screen and waits for a keypress before
exiting cleanly — it does not leave you on a black screen. That path matters
more than it sounds: under Speed Demon the launcher and system UI are already
unloaded, so an app that merely logged an error and returned would strand the
device until a power cycle.

## Controls

Keyboard only; the trackball is not used.

| Key | Action |
|---|---|
| `W` / `S` | Forward / back |
| `A` / `D` | Turn left / right |
| `Q` / `E` | Strafe left / right |
| `F` | Fire |
| `Space` | Use / open door |
| `1`–`7` | Select weapon |
| `Tab` | Automap |
| `Enter` | Menu select |
| `Backspace` | Escape — opens the menu (the bbq20 has no Esc key) |

## Two things worth knowing about the port

**The WAD lives in PSRAM, not flash.** PrBoom reads lumps as *pointers* — even
`I_Read` is implemented on top of `I_Mmap` — and never copies them into the zone
heap. That zero-copy design is a large part of why it fits on this class of
device. Espressif got it by memory-mapping a dedicated flash partition; FAT on
SD cannot be mapped, so instead the WAD is read into PSRAM once at startup and
`I_Mmap` becomes pointer arithmetic. The design is preserved, the bytes just
live somewhere else. It also takes the SD card off the shared SPI bus for the
whole of gameplay.

**Key releases are synthesised.** The bbq20 driver only ever emits
`INPUT_EVENT_KEY_DOWN` — there is no key-up event anywhere in it. DOOM needs
releases, or the player walks forward forever after one tap. So a key counts as
held while events keep arriving and is released 120 ms after they stop. See the
long comment above `I_StartTic` in `compat/i_video.c` for how this behaves under
both possible keyboard firmware behaviours.

## Re-vendoring

`prboom/` and `wad_tables/` are unmodified third-party drops and should stay
that way — `CMakeLists.txt` globs them, and suppresses warnings on those files
only, so our own code stays under the project's normal `-Werror` level. To
update, replace the directories wholesale. All PURR OS changes live in `compat/`
and `doom_app.c`.
