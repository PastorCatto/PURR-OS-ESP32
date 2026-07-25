# PURR OS — Architecture

## Big Picture

```
┌─────────────────────────────────────────────────────────────────────┐
│                         PURR OS Boot                                │
│                                                                     │
│  app_main()  ← from generic core/ OR specialized kernel_<device>/  │
│    │                                                                │
│    ├── NVS init                                                     │
│    ├── SPIFFS mount (/flash)                                        │
│    │                                                                │
│    ├── [Specialized kernel only]                                    │
│    │     ├── BOARD_POWERON GPIO HIGH (gates all peripherals)        │
│    │     ├── Display driver init (direct SPI)                       │
│    │     ├── Touch driver init  (Wire / IDF I2C)                    │
│    │     ├── Input driver init  (GPIO ISR / Wire)                   │
│    │     └── purr_kernel_register_display/touch/input(...)          │
│    │                                                                │
│    ├── purr_kernel_scan_modules("/flash/modules")                   │
│    │     ├── load driver_manager.purr → loads .purr driver blobs   │
│    │     │     display ──► catcall_display_t slot                   │
│    │     │     touch   ──► catcall_touch_t slot                     │
│    │     │     input   ──► catcall_input_t slot                     │
│    │     │     radio   ──► catcall_radio_t slot                     │
│    │     │     gps     ──► catcall_gps_t slot                       │
│    │     ├── register UI backend (mochi/cupcake/miniwin/...) → UI task     │
│    │     └── load app_manager.purr → scans + launches apps          │
│    │                                                                │
│    └── idle (vTaskDelay forever)                                    │
│                                                                     │
│  All work runs in FreeRTOS tasks created by modules.               │
└─────────────────────────────────────────────────────────────────────┘
```

---

## Two Kernel Paths

### Generic Core Kernel (`source/kernel/core/`)

Used for most devices. The kernel spine knows nothing about hardware. It mounts SPIFFS, scans for `.purr` module blobs, loads them in order, and idles. All hardware access happens through drivers that register catcalls.

### Specialized Kernels (`source/kernel/kernel_<device>/`)

Used when a device cannot be reached through the standard IDF driver stack, or when direct hardware access at boot is more reliable than the module loader path. The specialized kernel replaces `core/boot.c` for that device — it still initialises NVS and SPIFFS, but then drives hardware directly before handing off to the module system.

CMake selects the kernel automatically:
```
if source/kernel/kernel_${PURR_DEVICE}/ exists → use it
else → fall back to source/kernel/core/
```

All files matching `*.c` and `*.cpp` in the specialized kernel directory are compiled as the IDF main component. The specialized kernel still calls `purr_kernel_register_*()` so the rest of the OS sees a standard catcall interface regardless of how the driver was initialized.

See [13_Kernels.md](13_Kernels.md) for the full specialized kernel reference.

### Rule: Specialized Kernels Consume Drivers, They Don't Reimplement Them

A specialized kernel is a different **loading path** for a driver, not a
different **implementation** of it. The driver's register-level protocol logic
lives in exactly one place — `source/drivers/<type>/<name>/<name>.c` — and a
specialized kernel is only allowed to consume it by calling that driver's public
functions (`<name>_drv_init()`, `<name>_configure()`, `<name>_drv_deinit()`)
directly, the same way `driver_manager` calls them indirectly through the
`.purr` module ABI. A specialized kernel must never hand-roll a driver's
register reads/writes, keepalive timers, or status-clear logic from scratch.

`kernel_tdeck_plus` (`source/kernel/kernel_tdeck_plus/kernel_tdp_boot.c`)
is the canonical example — it `#include`s `source/drivers/touch/gt911/gt911.h`
and calls `gt911_configure(...)` / `gt911_drv_init()` directly instead of
talking to the GT911 registers itself. `purrstrap` already knows to skip
loading a driver as a `.purr` module when a specialized kernel has baked it in
(see `purrstrap.py`'s static-module glue generation), so there is no double-init
risk in following this rule.

The one exception in the tree today, `kernel_tdeck_plus_arduino`, predates this
rule — it was written as a stopgap when the IDF `i2c_master` stack had a GT911
regression, before a driver-level fix landed. See `docs/13_Kernels.md` and
`PURR_OS_1.0_CHECKLIST.md` §1 for its consolidation status.

---

## Kernel Spine (`source/kernel/core/`)

The generic kernel spine has four responsibilities and nothing else.

### 1. Flash VFS mount

```c
esp_vfs_spiffs_conf_t conf = {
    .base_path       = "/flash",
    .partition_label = "spiffs",
    .max_files       = 16,
    .format_if_mount_failed = false,
};
esp_vfs_spiffs_register(&conf);
```

All flash-resident content is accessible under `/flash/`. Module blobs live at `/flash/modules/`, drivers at `/flash/drivers/`, apps at `/flash/apps/`.

### 2. NVS init

NVS is initialized before any module runs. If the partition is corrupt it is erased and re-initialized. Modules call `nvs_open()` directly — no re-initialization needed.

### 3. Module scanner + loader

`purr_kernel_scan_modules(dir)` opens a directory, finds every `.purr` file, and calls `purr_kernel_load_module(path)` on each.

`purr_kernel_load_module(path)` does:
1. `fread` the first `sizeof(purr_module_header_t)` bytes
2. Validate `magic == 0x50555252` (`'PURR'`)
3. Validate `abi_version == PURR_MODULE_ABI_VERSION`
4. Check `kernel_min` — if current KITT < kernel_min, log `[SKIP]` and stop
5. Check `kernel_max` — if set and KITT > kernel_max, run compat check:
   - Walk `required_catcalls` bitmask
   - All present → proceed with `[COMPAT]` badge
   - Any missing → log `[FAIL]`, stop
6. Call `hdr.init()`. Non-zero return → log `[FAIL]`
7. Store a copy of the header in the module registry (32 slots)

### 4. Catcall registry

Six static slots — one per catcall type:

```c
static const catcall_display_t *s_display = NULL;
static const catcall_touch_t   *s_touch   = NULL;
static const catcall_input_t   *s_input   = NULL;
static const catcall_radio_t   *s_radio   = NULL;
static const catcall_gps_t     *s_gps     = NULL;
static const catcall_ui_t      *s_ui      = NULL;
```

Drivers call `purr_kernel_register_display(ptr)` etc. to register themselves.
For display, touch, radio, gps and ui, **last registered wins** — registration
is a plain assignment, which allows hot replacement. Everything else calls
`purr_kernel_display()` etc. to get the current pointer. If no driver is
registered these return `NULL`, so all callers must null-check.

**`input` is the exception: it is a list, not a single slot** — a device can
have a trackball and a keyboard registered simultaneously, as the T-Deck Plus
does. Enumerate with `purr_kernel_input_count()`/`purr_kernel_input_at(i)`;
`purr_kernel_input()` returns only the first-registered and is kept for legacy
callers. See `02_Catcalls.md` for the capability test used to tell them apart.

---

## The `.purr` Module ABI

**Source:** `source/kernel/core/purr_module.h`

Every `.purr` binary exports one symbol: `purr_module` of type `purr_module_header_t`.

```c
typedef struct {
    uint32_t magic;             // 0x50555252 ('PURR') — must match
    uint8_t  abi_version;       // must match PURR_MODULE_ABI_VERSION (2)
    uint8_t  module_type;       // PURR_MOD_DRIVER / SYSTEM / UI / APP
    char     name[32];          // human-readable name
    char     version[12];       // semver string "0.1.0"
    char     kernel_min[12];    // minimum KITT version required
    char     kernel_max[12];    // maximum KITT version supported ("" = no ceiling)
    uint32_t provided_catcalls; // bitmask of CATCALL_FLAG_* this module registers
    uint32_t required_catcalls; // bitmask of CATCALL_FLAG_* this module needs at load time
    int    (*init)(void);       // called at load — return 0 for success
    void   (*deinit)(void);     // called if module is unloaded
} purr_module_header_t;
```

### Module types

| Constant | Value | Meaning |
|----------|-------|---------|
| `PURR_MOD_DRIVER` | 0x01 | Hardware driver — registers one or more catcalls |
| `PURR_MOD_SYSTEM` | 0x02 | System service — driver_manager, app_manager |
| `PURR_MOD_UI` | 0x03 | UI framework — 11 backends; see 03_Modules.md |
| `PURR_MOD_APP` | 0x04 | App — reserved for future use |

### Catcall bitmask flags

| Flag | Value | Catcall |
|------|-------|---------|
| `CATCALL_FLAG_DISPLAY` | `1<<0` | Display pixel output |
| `CATCALL_FLAG_TOUCH` | `1<<1` | Touch point reading |
| `CATCALL_FLAG_INPUT` | `1<<2` | HID events (keyboard, trackball) |
| `CATCALL_FLAG_RADIO` | `1<<3` | LoRa / sub-GHz radio |
| `CATCALL_FLAG_GPS` | `1<<4` | NMEA GPS fix |
| `CATCALL_FLAG_UI` | `1<<5` | Widget/window layer |

### Version compat logic

```
kernel_min = "0.9.0"   → module needs at least KITT 0.9.0
kernel_max = ""        → no ceiling, always load
kernel_max = "0.12.0"  → if KITT > 0.12.0, run compat check
```

Compat check: walk `required_catcalls` bitmask. All present → `[COMPAT]`. Any missing → `[FAIL]`.

### Runtime status badges

| Badge | Meaning |
|-------|---------|
| `[OK]` | Loaded cleanly, within version range |
| `[COMPAT]` | Beyond `kernel_max`, but all required catcalls present |
| `[FAIL]` | Required catcall missing or `init()` returned non-zero |
| `[SKIP]` | Kernel too old (below `kernel_min`) |

---

## Catcall System

Catcalls are PURR OS's hardware interface contracts — the kernel's version of syscalls. They are plain C structs of function pointers, each defining a capability. Drivers implement these structs and register them with the kernel. Everything else calls through them.

**The kernel never calls a driver directly.** It stores a pointer and that's it.

```c
// Driver side (inside a .purr module or specialized kernel):
purr_kernel_register_display(&my_display_catcall);

// Module or app side:
purr_kernel_display()->fill_rect(0, 0, 320, 240, 0x0000);
purr_kernel_display()->push_pixels(x, y, w, h, data);

// UI layer — app never calls LVGL or MiniWin directly:
purr_kernel_ui()->create_window("My App", 320, 240);
```

See [02_Catcalls.md](02_Catcalls.md) for the full interface spec for all six catcalls.

---

## Two UI Tiers

`catcall_ui_t` only covers window/widget/keyboard dispatch — it has no touch
slots. This is intentional: PURR OS has two distinct kinds of UI module, and
only one of them needs `catcall_ui_t` at all.

| Tier | Registers `catcall_ui_t`? | Modules | Use case |
|------|---------------------------|---------|----------|
| **Windowed** | Yes | `kittenui`, `miniwin`, `cupcake`, `cardstack`, `mochi`, `tabby`, `nougat` | Full widget/window apps — buttons, labels, text areas, layout. Reads `catcall_touch_t` internally to drive its own hit-testing; apps never touch the touch catcall directly. All but `miniwin` are LVGL-based (`nougat` on LVGL 9, the rest on LVGL 8). |
| **Framebuffer** | Yes | `pounce` | Implements the same widget contract with no LVGL and no vendored toolkit — draws direct to the panel via `catcall_display_t`, with its own widget model and keyboard/trackball focus navigation. Apps run unmodified. |
| **Shell** | No | `blackpurr`, `oled_ui`, `lvgldebug` | Lightweight text-mode or grid shells that draw directly via `catcall_display_t.fill_rect`/`push_pixels` and read `catcall_touch_t`/`catcall_input_t` themselves. No window manager, no widget tree. |

A device picks exactly one UI module via `device.pcat`'s `ui = "..."` field.
This table makes explicit which contract that module is expected to honor: a
shell-tier module registering `catcall_ui_t` (or a windowed-tier module skipping
it) would be a bug.

**Apps written against `purr_win.h` run on the Windowed and Framebuffer tiers
only.** Shell-tier backends register no UI catcall, so `purr_win_*()` calls
degrade to no-ops there — those devices run their own built-in shell instead.

Several windowed backends additionally **host the `systemui` module** for the
status bar, panels, app switcher and lock screen — see `09_SystemUI.md`. That is
a separate concern from the UI catcall: a backend can implement the widget
contract without hosting any chrome.

### MiniWin desktop chrome: icon-grid vs. WinCE (experimental)

Within the windowed tier, `miniwin` itself has two interchangeable desktop
chrome implementations, chosen per-device at build time via
`CONFIG_PURR_MINIWIN_DESKTOP_WINCE` (default off):

- **Icon-grid (default)** — `miniwin_module.c`'s own status bar + app-icon
  grid, tapping an icon launches via `app_manager_launch_idx()`.
- **WinCE-style (`CONFIG_PURR_MINIWIN_DESKTOP_WINCE=y`, EXPERIMENTAL)** —
  `miniwin_wince_desktop.c`: a taskbar with a Start button, a dynamic
  Programs list built from the real `app_manager` registry, and automatic
  taskbar buttons for every `purr_win_create()` window. Opt in per device
  via a `sdkconfig_<device>.overrides` line (see `tdeck_plus`'s, the first
  device dogfooding it) — not a proven default yet, and not mutually
  compatible with `CONFIG_PURR_UI_WINCE_SHELL` (that flag is for kernels
  that bake their own shell in directly instead of using this module).

Because `app_manager` launches each app in its own FreeRTOS task, **any**
MiniWin desktop task loop that touches the display/window manager directly
(not through an app's `purr_win_*()` calls) must wrap that work in
`purr_kernel_ui_lock()`/`purr_kernel_ui_unlock()`, matching `kittenui_task`'s
existing pattern — skipping this causes two tasks to reach the display SPI
driver concurrently and crash (`assert failed: spi_device_transmit ...`).

---

## Module Load Order

Modules are loaded in filesystem order within `/flash/modules/`. The intended boot order is:

1. `driver_manager.purr` — scans `/flash/drivers` and `/sdcard/drivers`, loads display + touch + input drivers, registers catcalls
2. The selected UI backend — requires display catcall; spawns UI task
3. `app_manager.purr` — requires UI catcall; scans apps, populates registry, shows launcher

For specialized kernels (e.g., `kernel_tdeck_plus_arduino`), display + touch + input catcalls are registered at boot before the module scanner runs. This means `driver_manager` finds those catcall slots already filled and skips loading display/touch/input blobs — only radio and GPS drivers are loaded from SPIFFS.

---

## Static vs. Dynamic Modules

| Type | Where registered | Loaded by |
|------|-----------------|-----------|
| Static module | Compiled into the IDF image via `purr_register_static_modules()` | Kernel at boot, before SPIFFS scan |
| Dynamic module | `.purr` blob in SPIFFS `/flash/modules/` or SD `/sdcard/modules/` | `purr_kernel_scan_modules()` |

Static modules are used when the device has no SD card and SPIFFS space is tight, or when a module must be available before the filesystem is fully scanned. Dynamic modules are hot-swappable — dropping a newer `.purr` blob on the SD card replaces the SPIFFS version on next boot.

---

## Source File Map

```
source/kernel/
  catcalls/
    catcall_display.h      display (init, push_pixels, fill_rect, set_brightness, info, deinit)
    catcall_touch.h        touch  (init, read_point, is_pressed, deinit)
    catcall_input.h        input  (init, poll_event, deinit)
    catcall_radio.h        radio  (init, send, receive, rssi, snr, set_freq, set_power, deinit)
    catcall_gps.h          GPS    (init, get_fix, deinit)
    catcall_ui.h           UI     (create_window, add_label, add_button, add_input, show, destroy)
    catcalls.h             master include for all six
    purr_win.h             app-facing dispatch header — the only UI header apps need
  core/
    boot.c                 generic app_main — NVS, VFS, scan modules, idle
    purr_kernel.h          public kernel API
    purr_kernel.c          catcall registry + module loader
    purr_module.h          .purr binary ABI struct
    README.md              kernel spine responsibilities
  kernel_arduino/
    kernel_arduino.h       shared helpers for all Arduino-backed kernels
                           (arduino_kernel_nvs_init, arduino_kernel_spiffs_init)
  kernel_tdeck/
    kernel_td_boot.c       T-Deck specialized kernel (no touch, trackball only)
  kernel_tdeck_plus/
    kernel_tdp_boot.c      T-Deck Plus IDF kernel (GT911 over IDF i2c_master — see §Known Issues)
  kernel_tdeck_plus_arduino/
    kernel_atdp_boot.cpp   T-Deck Plus Arduino kernel — Wire I2C, GT911, BBQ20, trackball
  kernel_tdeck_plus_test/
    kernel_tdp_test.cpp    Input test mode — boots to touch/trackball/keyboard visualizer
```

---

## Known Issues

### GT911 touch on IDF 5.3 (`kernel_tdeck_plus`)

ESP-IDF 5.3 introduced a regression in `i2c_master_probe()` and `i2c_master_transmit()` where NACK responses return `ESP_ERR_INVALID_STATE` instead of `ESP_ERR_NOT_FOUND`. On T-Deck Plus, this causes the GT911 probe to fail completely even though the hardware is present.

**Workaround:** Use `kernel_tdeck_plus_arduino` — it uses Arduino Wire for I2C, which bypasses the IDF i2c_master stack entirely and finds GT911 at 0x5D reliably.

This issue is tracked for resolution in v0.14.0 (either patch the IDF path or deprecate the IDF kernel in favour of the Arduino kernel).
