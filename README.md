# PURR OS — v1.0.0-DeveloperPreview8

> **Docs current as of Developer Preview 8.** Documentation was deliberately left to drift
> through the Developer Preview cycle while the focus was stability; it has now been brought
> back in line with the code ahead of a 1.0 RC. Every version number, module,
> driver, device and command listed across `docs/` was verified against the
> tree. Reach out with any issues or concerns.

**P.U.R.R.** = Portable Unified Runtime & Radio Operating System
**K.I.T.T** = Kernel Interface Translation Toolkit

A fully modular, plug-and-play embedded OS for ESP32/ESP32-S3 devices, built on ESP-IDF.
Inspired by QubesOS — every driver, UI framework, and app is an isolated module.
The kernel spine knows nothing about hardware; it loads modules and hands off catcall interfaces.
For devices where the standard IDF driver stack has issues, a **specialized kernel** takes over boot directly.

---

## Architecture

```
┌──────────────────────────────────────────────────────────────┐
│  app_main  ← generic core/  OR  specialized kernel_<device>/ │
│    │                                                          │
│    ├── [Specialized kernel: direct hw init → catcall reg]    │
│    │                                                          │
│    ├── purr_kernel_scan_modules("/flash/modules")             │
│    │     driver_manager  →  loads .purr driver blobs         │
│    │                        registers catcalls                │
│    │     UI backend (mochi/cupcake/miniwin/...) -> catcall_ui_t│
│    │     app_manager  →  launches .meow / .paws / .claw      │
│    │                                                          │
│    └── idle forever                                           │
└──────────────────────────────────────────────────────────────┘
```

### Extension types

| Extension | What it is |
|-----------|-----------|
| `.purr` | Precompiled kernel module (driver, system service, UI framework) |
| `.meow` | Lua 5.4 script — sandboxed VM, `win.*` / `sd.*` / `system.*` API |
| `.hiss` | Lua 5.4 script — same VM as `.meow`, plus `kitt.*` / `radio.*` / `gps.*` |
| `.kitten` | As `.hiss`, but the first one found on SD **autoruns at boot** |
| `.paws` | Compiled userland app — `purr_win.h` + `sd.*` only |
| `.claw` | Compiled kernel-access app — full `purr_kernel_*` + `purr_win.h` |
| `.catt` | In-house exclusive (MagicMac, MagiDOS) — same as `.claw`, team-built |

### Catcalls

The kernel's hardware abstraction layer — named "catcalls" (PURR OS's version of syscalls).
Drivers register implementations; everything else calls through the kernel accessor.

| Catcall | Purpose | Accessor |
|---------|---------|---------|
| `display` | Pixel output (push_pixels, fill_rect, brightness) | `purr_kernel_display()` |
| `touch` | Touch point reading | `purr_kernel_touch()` |
| `input` | Keyboard/trackball HID events | `purr_kernel_input()` |
| `radio` | LoRa SPI radio (send/receive/RSSI/SNR) | `purr_kernel_radio()` |
| `gps` | NMEA UART GPS fix | `purr_kernel_gps()` |
| `ui` | Widget/window layer | `purr_kernel_ui()` |

### Unified UI API

Apps never call LVGL or MiniWin directly. All UI goes through `purr_win.h`:

```c
#include "purr_win.h"

purr_win_t win = purr_win_create("My App");
purr_wid_t lbl = purr_win_label(win, "Hello PURR OS!");
purr_win_button(win, "Tap", on_tap, NULL);
purr_win_show(win);
```

This compiles once and runs on every windowed and framebuffer backend — eight of them today.

---

## Repo Layout

```
source/
  kernel/
    catcalls/           catcall headers + purr_win.h
    core/               generic kernel (boot, registry, module loader)
    kernel_arduino/     shared helpers for Arduino-backed kernels
    kernel_tdeck/       T-Deck specialized kernel
    kernel_tdeck_plus/  T-Deck Plus IDF kernel (canonical)
    kernel_tdeck_plus_arduino/   T-Deck Plus Arduino Wire kernel (deprecated)
    kernel_tdeck_plus_test/      Input test mode kernel (dev/debug)
    kernel_tdeck_plus_pounce/    Pounce framebuffer target
    kernel_tab5_m5bsp_legacy/    M5Stack Tab5 (ESP32-P4)
  drivers/              display/, touch/, input/, radio/, gps/
  modules/
    driver_manager/     loads .purr driver blobs
    app_manager/        launches .meow/.hiss/.paws/.claw apps
    <11 UI backends>    kittenui, miniwin, cupcake, cardstack, mochi, tabby,
                        nougat, pounce, blackpurr, oled_ui, lvgldebug
    systemui/           status bar / panels / lock screen (Android + iOS styles)
    meshtastic/         mesh networking (+ meshcore as an alternative backend)
    proximity*/         ESP-NOW multi-device family (pairing, RPC, homebase)
  devices/              device.pcat manifests (12 targets)
  apps/
    system/             settings, terminal, fileman, msn, taskmgr, services,
                        drivermgr, hwtest, meshdiag, milkbar, nearby, calculator
    exclusive/          magicmac, magidos (rewrite in progress)

CoreOS/                 IDF project shell (CMake, sdkconfig per device, partitions)
purrstrap/              builds final flashable firmware image
modulestrap/            compiles .purr module + driver blobs
catstrap/               user app builder + SDK

cattobaked/             all build output (firmware, blobs, merged images)
user_drivers/           drop custom/community drivers here — auto-scanned

PURR-OS-0.11/           archived v0.11 codebase
archive/                legacy scripts, old docs
```

---

## Supported Devices

| Device | Chip | Screen | Input | Radio | SD | Kernel |
|--------|------|--------|-------|-------|----|--------|
| `jc3248w535` | ESP32-S3 | 3.5" AXS15231B 480×320 QSPI | touch | WiFi + BT | no | generic |
| `tdeck_plus` | ESP32-S3 | 3.2" ST7789 320×240 | touch + trackball + keyboard | WiFi + BT + SX1262 LoRa (`sx1262_rl`) + GPS | yes | specialized (IDF) |
| `tdeck` | ESP32-S3 | 3.2" ST7789 320×240 | trackball | WiFi + BT + SX1262 LoRa | yes | specialized |
| `cyd` | ESP32 | 2.8" ILI9341 320×240 | resistive touch | WiFi + BT | yes | generic |
| `cyd_s024c` | ESP32 | 2.4" ILI9341 240×320 | cap touch | WiFi + BT | yes | generic |
| `cyd_s028r` | ESP32 | 2.8" ILI9341 320×240 | resistive touch | WiFi + BT | yes | generic |
| `heltec` | ESP32-S3 | 128×64 SSD1306 OLED | — | WiFi + BT + SX1262 LoRa (`sx1262_rl`) | no | generic |
| `waveshare169` | ESP32-S3 | 1.69" ST7789 240×280 | cap touch | WiFi + BT | no | generic |
| `tab5` | **ESP32-P4** | ST7123 MIPI-DSI | cap touch + tab5_kbd | WiFi + BT | — | specialized |

### Dev / debug targets

| Target | Purpose |
|--------|---------|
| `tdeck_plus_arduino` | Arduino-Wire kernel for T-Deck Plus — **deprecated**, `tdeck_plus` (IDF) is now canonical |
| `tdeck_plus_test` | Input visualizer — confirms touch, trackball, keyboard hardware |
| `tdeck_plus_pounce` | T-Deck Plus running the `pounce` raw-framebuffer backend |

---

## Build Tools

All commands run from the repo root.

### purrstrap — firmware image builder

```bash
python3 purrstrap/purrstrap.py build <device>
python3 purrstrap/purrstrap.py flash <device> -p /dev/ttyACM0 --erase
python3 purrstrap/purrstrap.py monitor <device> -p /dev/ttyACM0
python3 purrstrap/purrstrap.py clean <device>
python3 purrstrap/purrstrap.py list
python3 purrstrap/purrstrap.py doctor
```

`purrstrap build` automatically calls modulestrap and catstrap — you do not need to run them separately.

### modulestrap — .purr module + driver compiler

```bash
python3 modulestrap/modulestrap.py build all
python3 modulestrap/modulestrap.py build <name>    # e.g. "kittenui" or "display/st7789"
python3 modulestrap/modulestrap.py list
python3 modulestrap/modulestrap.py clean [all]
```

It also decides which components a given device compiles, from that device's
`device.pcat`, and can change that:

```bash
python3 modulestrap/modulestrap.py modules tdeck_plus          # what is on / off
python3 modulestrap/modulestrap.py disable meshtastic tdeck_plus
python3 modulestrap/modulestrap.py enable  nearby     tdeck_plus
```

### catstrap — user app builder + SDK

```bash
python3 catstrap/catstrap.py build all
python3 catstrap/catstrap.py build <name>
python3 catstrap/catstrap.py validate <file.meow>
python3 catstrap/catstrap.py sdk install
python3 catstrap/catstrap.py sdk info
python3 catstrap/catstrap.py list
python3 catstrap/catstrap.py clean [all]
```

### Interactive launcher

```bash
./purr.sh        # Linux/macOS — interactive menu
.\purr.ps1       # Windows PowerShell
python3 purr.py  # any platform
```

---

## System Apps

Bundled on all medium and large-screen devices:

| App | Tier | Description |
|-----|------|-------------|
| `settings` | `.claw` | Theme, brightness, SD card, device info, customization panel |
| `terminal` | `.claw` | Shell: help, ls, cat, echo, modules, reboot, clear |
| `fileman` | `.claw` | Browse SPIFFS + SD card; text file preview |
| `taskmgr` | `.claw` | Running apps; the one deliberate place to kill one |
| `services` | `.claw` | Live status of core background services + memory pressure |
| `drivermgr` | `.claw` | Scanned drivers and their OK/COMPAT/FAIL/SKIP status |
| `hwtest` | `.claw` | Live trackball motion/click and keyboard keypress log |
| `msn` | `.claw` | Mesh Social Network — buddy list + rooms over the active mesh backend |
| `meshdiag` | `.claw` | Mesh diagnostics: kernel log tail, radio + node stats, test-send |
| `nearby` | `.claw` | Read-only list of PURR OS devices seen via ESP-NOW proximity beacons |
| `milkbar` | `.claw` | Manage apps on a paired PURR OS device remotely |
| `calculator` | `.paws` | Basic arithmetic with decimal support |

There is no separate `about` app — device/OS info lives inside `settings`.

---

## Versions

| Component | Version |
|-----------|---------|
| PURR OS | v1.0.0-dp8 |
| `.purr` ABI | 2 |
| Catcall: display / touch / gps | 1 |
| Catcall: input | 2 |
| Catcall: radio | 3 |
| Catcall: ui | 7 |

Catcalls are versioned independently — there is no single "Catcall API" number.
See [docs/02_Catcalls.md](docs/02_Catcalls.md).

---

## Documentation

| Doc | Contents |
|-----|---------|
| [docs/00_Overview.md](docs/00_Overview.md) | What PURR OS is, supported hardware, key concepts |
| [docs/01_Architecture.md](docs/01_Architecture.md) | Kernel spine, specialized kernels, module loader, .purr ABI |
| [docs/02_Catcalls.md](docs/02_Catcalls.md) | All six catcalls — full struct + function docs, driver tables |
| [docs/03_Modules.md](docs/03_Modules.md) | Complete index of all 27 modules; `module.pcat` schema (two forms) |
| [docs/04_Devices.md](docs/04_Devices.md) | device.pcat format, all devices, T-Deck Plus detail, pin reference |
| [docs/05_Drivers.md](docs/05_Drivers.md) | driver.pcat format, all drivers, GT911/IDF 5.3 known issue, writing a driver |
| [docs/06_Apps.md](docs/06_Apps.md) | Tiers, purr_win.h API reference, .meow/.hiss/.paws/.claw guide |
| [docs/07_Build_Tools.md](docs/07_Build_Tools.md) | purrstrap, modulestrap, catstrap — full pipeline and commands |
| [docs/08_Exclusives.md](docs/08_Exclusives.md) | MagicMac and MagiDOS — architecture, build, current status |
| [docs/09_SystemUI.md](docs/09_SystemUI.md) | System UI module — two styles, host contract, lock screen, notifications |
| [docs/10_ModuleLoading.md](docs/10_ModuleLoading.md) | Static registration, module priority, SD fallback, panic screen |
| [docs/11_KittenUI.md](docs/11_KittenUI.md) | KittenUI LVGL 8 module in depth (no device currently selects it) |
| [docs/12_AppAPI.md](docs/12_AppAPI.md) | purr_win.h complete API reference + backend writing guide |
| [docs/13_Kernels.md](docs/13_Kernels.md) | Specialized kernel system — when to use, how to write, all existing kernels |
| [docs/14_Driverstrap.md](docs/14_Driverstrap.md) | driverstrap — driver template generator CLI + wizard reference |
| [docs/15_SpeedDemon.md](docs/15_SpeedDemon.md) | Speed Demon — give one app the whole machine with `.speed_demon = 1` |

---

## Prior Versions

The v0.11.0 codebase is preserved in `PURR-OS-0.11/`.
Legacy docs, changelogs, and build artifacts are in `archive/`.

---

## License

MIT. MiniWin: MIT (John Blaiklock). Lua 5.4: MIT. See subproject READMEs for full attribution.

---

*Documentation reviewed and updated for v1.0.0-dp8 by Claude Opus 5 in agentic/auto mode.*
