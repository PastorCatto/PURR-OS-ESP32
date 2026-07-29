# PURR OS — Overview

## What is PURR OS?

**PURR OS** (Portable Unified Runtime & Radio Operating System) is an embedded operating system for ESP32, ESP32-S3 and ESP32-P4 devices. It runs on small, affordable, readily available hardware — CYD (Cheap Yellow Display) boards, LilyGo T-Deck Plus, JC3248W535, Heltec LoRa nodes, and similar ESP32-based devices with a display, radio, or both.

The kernel is called **KITT** (Kernel Interface Translation Toolkit).

PURR OS is not a bare-metal sketch. It has:
- A proper kernel boot sequence with a hardware abstraction layer
- A plug-and-play driver system — drivers compile to `.purr` blobs, loaded at runtime
- A unified windowed UI layer (11 swappable backends) with a single app-facing API
- An app runtime with five isolation tiers (`.meow`/`.hiss`/`.kitten` Lua, `.paws` userland, `.claw` kernel-access)
- Built-in support for LoRa radio, WiFi, GPS, keyboard HID, and trackball input
- Specialized device kernels for hardware that needs direct driver access at boot
- Two in-house exclusive apps: MagicMac (68k Mac Plus emulator) and MagiDOS (8086 DOS emulator)

---

## Design Philosophy

The guiding inspiration is **QubesOS** — compartmentalization and isolation above all else.

In PURR OS v0.11 and earlier, the kernel had device-specific knowledge baked directly into `CMakeLists.txt`, `lib_miniwin`, and `device_config.cpp`. Adding a new device meant editing kernel files. The IDF two-pass CMake system made conditional driver dependencies fragile. Every build was one monolithic blob.

**v0.12.0 changed this completely.** The kernel spine is intentionally tiny and knows nothing about hardware. Every driver, UI framework, and system service is a precompiled binary module (`.purr`) loaded at runtime. The kernel's only job is to:

1. Mount the flash filesystem
2. Scan for modules
3. Maintain a registry of capability interfaces (catcalls)
4. Idle

Everything else is a module.

**v0.13.0 adds specialized kernels.** For devices with hardware that cannot be reached through the standard IDF driver stack (e.g., IDF 5.3 i2c_master regressions on T-Deck Plus), a specialized kernel can bypass the generic module loader and talk to hardware directly using any available API — including Arduino Wire. The catcall interface stays the same; only the boot path changes.

---

## Versions

| Component | Version | Source of truth |
|-----------|---------|---|
| PURR OS | v1.0.0-dp8 | `purrstrap.py`'s `PURROS_VERSION` **and** `purr_kernel.h`'s `PURR_KERNEL_VERSION` — duplicated, keep in sync |
| `.purr` ABI | 2 | `purr_module.h`'s `PURR_MODULE_ABI_VERSION` |
| Catcall: display / touch / gps | 1 | respective headers |
| Catcall: input | 2 | `catcall_input.h` |
| Catcall: radio | 3 | `catcall_radio.h` |
| Catcall: ui | 7 | `catcall_ui.h` |

Catcalls are versioned **independently** — there is no single "Catcall API"
number. See `02_Catcalls.md`.

---

## Supported Hardware

| Device slug | Chip | Display | Touch | Input | Radio | Notes |
|-------------|------|---------|-------|-------|-------|-------|
| `jc3248w535` | ESP32-S3 | 3.5" AXS15231B 480×320 QSPI | Cap I2C | — | WiFi + BT | 8MB PSRAM, best for MagicMac/MagiDOS |
| `tdeck_plus` | ESP32-S3 | 3.2" ST7789 320×240 SPI | GT911 cap | Trackball + BBQ20 keyboard | WiFi + BT + SX1262 LoRa (`sx1262_rl`) + GPS | Full field device; specialized IDF kernel |
| `tdeck` | ESP32-S3 | 3.2" ST7789 320×240 SPI | — | Trackball | WiFi + BT + SX1262 LoRa | No touch; trackball navigation only |
| `cyd` | ESP32 | 2.8" ILI9341 320×240 SPI | XPT2046 resistive | — | WiFi + BT | Classic Cheap Yellow Display |
| `cyd_s024c` | ESP32 | 2.4" ILI9341 240×320 SPI | CST816S cap | — | WiFi + BT | Backlight GPIO 27 |
| `cyd_s028r` | ESP32 | 2.8" ILI9341 320×240 SPI | XPT2046 resistive | — | WiFi + BT | Portrait-flip MADCTL variant |
| `heltec` | ESP32-S3 | 128×64 SSD1306 OLED I2C | — | — | WiFi + BT + SX1262 LoRa | LoRa-first node; text-mode UI only |
| `waveshare169` | ESP32-S3 | 1.69" ST7789 240×280 SPI | CST816S cap | — | WiFi + BT | Small badge/wearable device |
| `tab5` | ESP32-P4 | ST7123 MIPI-DSI | Integrated cap | tab5_kbd | WiFi + BT | Only ESP32-P4 device; 16MB flash |

### Specialized build targets (dev/debug)

| Target slug | Based on | Purpose |
|-------------|----------|---------|
| `tdeck_plus_arduino` | T-Deck Plus | Production kernel using Arduino Wire for I2C — bypasses IDF 5.3 regression |
| `tdeck_plus_test` | T-Deck Plus | Input test mode — boots to a visualizer showing all touch, trackball, and keyboard events |
| `tdeck_plus_pounce` | T-Deck Plus | Runs the `pounce` raw-framebuffer backend instead of an LVGL one |

---

## Repo Structure at a Glance

```
source/
  kernel/
    catcalls/          catcall headers (display, touch, input, radio, gps, ui, purr_win.h)
    core/              generic kernel (boot.c, purr_kernel.h/.c, purr_module.h)
    kernel_arduino/    shared helpers for Arduino-backed kernels
    kernel_tdeck/      specialized kernel for T-Deck
    kernel_tdeck_plus/ specialized kernel for T-Deck Plus (IDF path)
    kernel_tdeck_plus_arduino/   Arduino Wire kernel for T-Deck Plus (production)
    kernel_tdeck_plus_test/      input test mode kernel
    kernel_tdeck_plus_pounce/    Pounce framebuffer backend target
    kernel_tab5_m5bsp_legacy/    M5Stack Tab5 (BSP path, ESP32-P4)
  drivers/             display/, touch/, input/, radio/, gps/, battery/
  modules/
    driver_manager/    scans + loads .purr driver blobs
    app_manager/       launches .meow/.paws/.claw apps
    <11 UI backends>  kittenui, miniwin, cupcake, cardstack, mochi,
                       tabby, nougat, pounce, blackpurr, oled_ui, lvgldebug
    systemui/          status bar / panels / lock screen (2 styles)
    meshtastic/        mesh networking (+ meshcore alternative)
    proximity*/        ESP-NOW multi-device family
  devices/             device.pcat manifests (12 targets)
  apps/
    system/            settings, terminal, fileman, msn, taskmgr, services, ... (12)
    exclusive/         magicmac, magidos

purrstrap/             final flashable firmware image builder
modulestrap/           .purr module + driver blob compiler, per-device selection
catstrap/              user app builder + SDK

cattobaked/            all build output (firmware, blobs, merged images)
user_drivers/          drop custom/community drivers here — auto-scanned

CoreOS/                IDF project shell (CMake, sdkconfig per device, partition tables)
PURR-OS-0.11/          archived v0.11 codebase
archive/               legacy scripts, old docs, old build artifacts
```

---

## Key Concepts

| Term | Meaning |
|------|---------|
| `.purr` | Precompiled kernel module binary (driver, system service, UI framework) |
| `catcall` | Kernel hardware interface contract — PURR OS's version of a syscall |
| `catcall_ui_t` | Unified widget/windowing catcall — apps never call LVGL or MiniWin directly |
| `purr_win.h` | App-facing dispatch header for `catcall_ui_t` — the only UI header apps need |
| `driver_manager` | System module that loads and validates `.purr` driver blobs from SPIFFS/SD |
| `app_manager` | System module that scans and launches `.meow`/`.paws`/`.claw` apps |
| `kittenui` | One of 11 UI backends — see 03_Modules.md |
| `miniwin` | Non-LVGL windowed backend; only one implementing `canvas_*` |
| `oled_ui` | Shell-tier text-mode UI for 128×64 OLED (registers no `catcall_ui_t`) |
| `.meow` | Lua 5.4 script app — sandboxed VM, `win.*` / `sd.*` / `kitt.*` API |
| `.paws` | Compiled userland app — `purr_win.h` + `sd.*` only |
| `.claw` | Compiled kernel-access app — full `purr_kernel_*` + `purr_win.h` |
| `purrstrap` | Builds the final merged flashable image per device |
| `modulestrap` | Registers `.purr` module + driver blobs as IDF components; selects which ones a device compiles, from its `device.pcat` |
| `catstrap` | Builds user apps and manages the catstrap SDK |
| `cattobaked/` | All build output from all three tools |
| `device.pcat` | Per-device manifest: chip, drivers, radio, apps, pins |
| `purr_device_glue.c` | Auto-generated by purrstrap — pin `#defines` + radio capability flags |
| Specialized kernel | A `kernel_<device>/` directory that replaces the generic core for one device |

---

## See Also

- [01_Architecture.md](01_Architecture.md) — kernel spine, specialized kernels, module loader, .purr ABI, catcall registry
- [02_Catcalls.md](02_Catcalls.md) — all six catcalls with full struct + function docs, driver tables, glue layer
- [03_Modules.md](03_Modules.md) — driver_manager, app_manager, kittenui, miniwin, oled_ui
- [04_Devices.md](04_Devices.md) — device.pcat format, all devices, pin reference, screen classification
- [05_Drivers.md](05_Drivers.md) — driver.pcat format, all drivers, writing a new driver
- [06_Apps.md](06_Apps.md) — app tiers, purr_win.h API reference, system apps guide
- [07_Build_Tools.md](07_Build_Tools.md) — purrstrap, modulestrap, catstrap — full pipeline and command reference
- [08_Exclusives.md](08_Exclusives.md) — MagicMac and MagiDOS in depth
- [10_ModuleLoading.md](10_ModuleLoading.md) — module priority system, SD fallback, panic screen
- [11_KittenUI.md](11_KittenUI.md) — KittenUI LVGL 8 module in depth
- [12_AppAPI.md](12_AppAPI.md) — purr_win.h complete API reference + backend writing guide
- [13_Kernels.md](13_Kernels.md) — specialized kernel system: when to use, how to write, existing kernels
- [14_Driverstrap.md](14_Driverstrap.md) — driverstrap driver template generator: CLI, wizard, generated files

---

*DP8 documentation pass performed by Claude Opus 5 in agentic/auto mode.*
