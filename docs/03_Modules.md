# PURR OS — System Modules

> **Accurate as of v1.0.0-dp8.** The index below is complete — all 27 modules
> in `source/modules/` are listed. Sections further down cover a subset in
> depth; anything without a section is documented by its own `module.pcat` and
> source header.

System modules are `PURR_MOD_SYSTEM` or `PURR_MOD_UI` components. They are
either compiled in and statically registered (the normal case — see
`10_ModuleLoading.md`) or loaded as `.purr` binaries from `/flash/modules/`.

---

## Module index (all 27)

Selected per-device via `device.pcat`'s `[modules]` section. `type` is as
declared in each `module.pcat`.

### UI backends (8 register a `catcall_ui_t`; 3 are shell-tier and do not)

| Module | Tier | Notes |
|---|---|---|
| `kittenui` | Windowed | LVGL 8, swappable themes. Implements no optional UI members. |
| `miniwin` | Windowed | Vendored MiniWin WM, non-LVGL. Only `canvas_*` implementation. |
| `cupcake` | Windowed | LVGL 8, Android 1.5-style launcher. Hosts `systemui`. |
| `cardstack` | Windowed | LVGL 8, Rabbit R1-style snap-scroll cards. |
| `mochi` | Windowed | LVGL 8, iOS-style springboard. Hosts `systemui` (iOS style). |
| `tabby` | Windowed | LVGL 8, keyboard-first type-to-filter launcher. Hosts `systemui`. |
| `nougat` | Windowed | LVGL 9. **Experimental, parked** — see `02_Catcalls.md`. |
| `pounce` | Framebuffer | No LVGL, no MiniWin. Own widget model + focus navigation. |
| `blackpurr` | Shell | Minimal text-mode Palm-style launcher. No `catcall_ui_t`. |
| `oled_ui` | Shell | 128x64 text-mode (Heltec). No `catcall_ui_t`. |
| `lvgldebug` | Diagnostic | Touch/HAL diagnostic screen, not a real shell. |

Apps written against `purr_win.h` run on the **Windowed** and **Framebuffer**
tiers only — shell-tier backends don't register the UI catcall.

### System services

| Module | Purpose |
|---|---|
| `app_manager` | App registry, launching, tiers (`.meow`/`.hiss`/`.claw`). See below. |
| `driver_manager` | Scans and loads `.purr` drivers, tracks OK/COMPAT/FAIL/SKIP. See below. |
| `lua_runtime` | Lua VM for `.meow`/`.hiss` scripts. See below. |
| `systemui` | Status bar, panels, nav bar/home indicator, Recents, lock screen. Two swappable styles. **See `09_SystemUI.md`.** |
| `boot_splash` | Raw-framebuffer splash drawn via `catcall_display_t` before any UI backend starts. |
| `usb_msc` | USB mass-storage. **Dormant** — no `module.pcat`; see its `DISABLED.md`. |

### Connectivity and mesh

| Module | Purpose |
|---|---|
| `wifi_mgr` | WiFi lifecycle. See below. |
| `bt_mgr` | NimBLE lifecycle. See below. |
| `meshtastic` | Meshtastic-compatible mesh. Gated by `CONFIG_PURR_FEATURE_MESHTASTIC`. |
| `meshcore` | MeshCore mesh backend. Mutually exclusive with `meshtastic` at runtime (one radio). |
| `msn_relay` | Relay service backing the MSN app. |

### Multi-device (ESP-NOW proximity family)

| Module | Purpose |
|---|---|
| `proximity` | ESP-NOW beacons; discovers other PURR OS devices. |
| `proximity_rpc` | RPC transport layered on `proximity`. |
| `pairing` | Pair/trust management between devices. |
| `app_manager_remote` | Remote app list/control on a paired device (backs Milkbar). |
| `homebase` | Home-base role for a paired device set. |

Added automatically to any device with `[radio] wifi = true` by purrstrap's
`apply_radio_companion_defaults()` — they usually do not appear in `device.pcat`
explicitly.

---

## `module.pcat` schema — two forms are live

**Use the sectioned form for anything new.** Both parse, and 16 of 27 modules
still use the older flat form; migrating them is tracked work, not a rewrite you
should do incidentally.

**Current (sectioned):**
```ini
[module]
name        = "systemui"
version     = "0.1.0"
type        = "module"            # or "ui"
description = "one line, shown in tooling"
author      = "PURR OS"

[requires]
catcalls    = "display"
kernel_min  = "1.0.0"

[depends]                          # other PURR packages, by directory slug
app_manager = ">=0.1.0"

[build]
sources     = "a.c b.c"
requires    = "lvgl app_manager"   # IDF component names
```

**Legacy (flat keys)** — e.g. `app_manager`, `miniwin`, `meshtastic`:
```ini
name              = "app_manager"
version           = "0.1.0"
module_type       = "PURR_MOD_SYSTEM"
kernel_min        = "0.9.0"
provides          = ["app_launch", "app_registry"]
required_catcalls = ["display", "touch"]
```

Note `type` vs `module_type`, and that the legacy form has no `description`,
`[depends]` or `[build]` section.

---

## driver_manager

**Source:** `source/modules/driver_manager/`  
**Type:** `PURR_MOD_SYSTEM`  
**Version:** 0.1.0  
**Required catcalls:** none (it creates them)

The driver manager is the first module the kernel should load. Its job is to find and load every driver blob, validate it, call its `init()`, and let it register its catcall. After `driver_manager_init()` returns, all hardware drivers are up and all catcalls are populated.

### Scan paths

```c
static const char *s_scan_paths[] = {
    "/flash/drivers",    // built-in drivers baked into firmware image
    "/sdcard/drivers",   // hot-loaded drivers from SD card
    NULL
};
```

Paths are scanned in order. First match wins (flash takes precedence over SD).

### Load sequence for each `.purr` driver found

1. `fread` header, validate magic
2. Check `kernel_min` — if KITT too old, mark `[SKIP]`
3. Check `kernel_max` — if beyond ceiling, run `compat_check()`:
   - Walk each `CATCALL_FLAG_*` in `required_catcalls`
   - Check `purr_kernel_display()` etc. — `NULL` means missing
   - All present → `[COMPAT]`. Any missing → `[FAIL]`
4. Call `hdr.init()` — driver registers its catcall inside here
5. Store in `drv_entry_t` registry with final status badge

### Status badges

| Badge | Meaning |
|-------|---------|
| `[OK]` | Loaded cleanly |
| `[COMPAT]` | Beyond kernel_max, all required catcalls still present |
| `[FAIL]` | Required catcall missing, or `init()` returned error |
| `[SKIP]` | Kernel too old for this driver |

### Public API (for UI / driver status screen)

```c
int              driver_manager_get_count(void);
const drv_entry_t *driver_manager_get_entry(int idx);
const char       *drv_status_badge(drv_status_t s);
```

`drv_entry_t` holds name, version, type, status, and a fail_reason string for `[FAIL]` entries. The `drivermgr` system app (`source/apps/system/drivermgr/`) is that dedicated drivers screen — a thin `purr_win_list` UI over this exact API, no backend changes needed. (Its app directory is deliberately named `drivermgr`, not `driver_manager` — giving it the same directory name as this module caused an ESP-IDF component-name collision that silently dropped this module's object files from the link.)

### Module header

```c
purr_module_header_t purr_module = {
    .module_type       = PURR_MOD_SYSTEM,
    .name              = "driver_manager",
    .required_catcalls = 0,    // no catcalls needed — we create them
    .provided_catcalls = 0,    // drivers provide catcalls, not the manager itself
    .init              = driver_manager_init,
    .deinit            = driver_manager_deinit,
};
```

---

## app_manager

**Source:** `source/modules/app_manager/`  
**Type:** `PURR_MOD_SYSTEM`  
**Version:** 0.1.0  
**Required catcalls:** none at load time (display/touch accessed at runtime when launcher opens)

The app manager scans for user apps, maintains the registry, and provides the Cat Apps launcher UI. It is the bridge between the filesystem and the user.

### App scan paths

```c
static const char *s_scan_paths[] = {
    "/flash/apps",
    "/sdcard/apps",
    NULL
};
```

### App tiers

| Extension | Tier | Launch method |
|-----------|------|--------------|
| `.meow` | Lua script | Lua VM (engine module must be loaded) |
| `.paws` | Compiled userland | Dynamic loader (planned) |
| `.claw` | Compiled kernel-access | Dynamic loader (planned) |

The app manager detects the tier from the file extension. Display names are derived by stripping the path prefix and extension from the filename.

### Registry API

```c
int              app_manager_count(void);
const app_entry_t *app_manager_get(int idx);
int              app_manager_launch_idx(int idx);
int              app_manager_launch_path(const char *path);
void             app_manager_stop(int idx);
void             app_manager_open_launcher(void);
int              app_manager_scan(void);   // hot-reload from SD
```

### app_entry_t

```c
typedef struct {
    char        name[48];       // display name
    char        path[128];      // full filesystem path
    app_tier_t  tier;           // MEOW / PAWS / CLAW
    app_state_t state;          // IDLE / RUNNING / STOPPED / ERROR
    char        error[64];      // populated on ERROR
} app_entry_t;
```

### Cat Apps launcher

`app_manager_open_launcher()` is the hook that opens the Cat Apps grid UI. It is called by MiniWin (or any module with display access) to present the app launcher. The actual UI rendering is done through the display catcall — the app_manager itself contains no rendering code.

### Launch status

`.meow` launch now works — see the `lua_runtime` module below. `.paws` and `.claw` return "dynamic loader not yet implemented" — in the precompiled model these apps are statically linked into the firmware, so the dynamic path is for future hot-loading from SD.

---

## lua_runtime

**Source:** `source/modules/lua_runtime/`
**Type:** `PURR_MOD_SYSTEM`
**Version:** 0.1.0
**Required catcalls:** none (UI access goes through `purr_win.h` at script-run time)

Runs `.meow` scripts — previously dead code (`app_manager.c`'s `launch_meow()` looked up a `"lua_runtime"` module that never existed anywhere in the repo). Vendors a real Lua 5.4 VM (`source/lib/lib_lua/`, ~30 files, ported from the PURR-OS-0.11 archive) privately into this component, same vendoring pattern the `meshtastic` module already uses for nanopb.

One global Lua state runs at a time. `init()` is called twice in the normal course of things: once at boot like any other system module (a no-op — nothing is pending yet, checked before allocating a VM so the boot-time call doesn't leak one), and again each time `app_manager.c`'s `launch_meow()` actually launches a `.meow` file (it reads the pending path via the new `app_manager_get_pending_meow_path()` accessor).

Exposes three Lua namespaces — `win.*` (1:1 wrapper over `purr_win.h`: create/label/label_set/button/textarea/show/destroy — button callbacks are real Lua closures via a registry-ref trampoline), `sd.*` (path-based read/write), `system.*` (print/delay/time_ms). See `docs/06_Apps.md` for the Lua-facing API and an example script.

**Threading note:** a script's main body runs synchronously inside its launch task, which exits right after the script returns (same as every native app's `init()`) — write scripts that build a window and return, then respond to taps via callbacks, not scripts that loop forever themselves; there's no locking between the script's own execution and a button-callback trampoline firing from the UI task.

---

## wifi_mgr

**Source:** `source/modules/wifi_mgr/`
**Type:** `PURR_MOD_SYSTEM`
**Version:** 0.1.0

Drives WiFi station mode — `esp_wifi_init()`/`esp_netif_init()` themselves run once at boot (`kernel_tdp_boot.c`, before any module loads), this module calls `esp_wifi_start()` and handles scan/connect/disconnect. Registers `WIFI_EVENT`/`IP_EVENT` handlers that call the existing (previously never-driven-on-this-path) `purr_kernel_set_wifi_connected()`, so the status-bar WiFi icon reflects real state for the first time.

```c
int  wifi_mgr_scan(void);                          // blocking, a few hundred ms
int  wifi_mgr_scan_count(void);
bool wifi_mgr_scan_at(int idx, wifi_scan_result_t *out);
void wifi_mgr_connect(const char *ssid, const char *password);
void wifi_mgr_disconnect(void);
wifi_mgr_status_t wifi_mgr_status(void);           // IDLE / CONNECTING / CONNECTED / FAILED
const char *wifi_mgr_ip_str(void);
```

Last-connected SSID/password persist to NVS (namespace `"wifi_sta"`); `wifi_mgr_init()` auto-reconnects if credentials are present. Driven from Settings' WiFi section.

---

## bt_mgr

**Source:** `source/modules/bt_mgr/`
**Type:** `PURR_MOD_SYSTEM`
**Version:** 0.1.0

Bluetooth — **BLE only**. T-Deck Plus's ESP32-S3 has no classic Bluetooth (BR/EDR) hardware at all (`SOC_BT_CLASSIC_SUPPORTED` isn't even defined for this chip in ESP-IDF's `soc_caps.h`, only `SOC_BLE_SUPPORTED` is) — Bluedroid is still the host stack (vs. NimBLE), it just only ever runs in BLE mode on this board. Also note: this ESP-IDF version defaults to BLE 5.0 extended-advertising APIs, which compile out the legacy `esp_ble_gap_start_advertising`/`set_scan_params`/`start_scanning` functions this module uses (link-time "undefined reference", not a compile error) — `sdkconfig_tdeck_plus.overrides` explicitly selects `CONFIG_BT_BLE_42_FEATURES_SUPPORTED=y` / `CONFIG_BT_BLE_50_FEATURES_SUPPORTED=n` (the two are mutually exclusive) to get them back, since this module's simple accessory-pairing use case doesn't need 5.0's extended/coded-PHY features.

```c
void bt_mgr_set_enabled(bool on);      // gates scanning/advertising — the Bluedroid host itself
bool bt_mgr_is_enabled(void);          // stays initialized+enabled for the module's whole lifetime
int  bt_mgr_scan(uint32_t duration_sec);
int  bt_mgr_scan_count(void);
bool bt_mgr_scan_at(int idx, bt_scan_result_t *out);
esp_err_t bt_mgr_pair(const uint8_t addr[6]);   // "Just Works" bonding — no PIN entry UI exists yet
```

Driven from Settings' Bluetooth section. See `meshtastic`'s BLE companion service below for the other consumer of this module's "Bluedroid is up" precondition.

---

## meshtastic — BLE companion service addendum

**Source:** `source/modules/meshtastic/mesh_ble.c`

Not a new module, but a significant addition to the existing `meshtastic` module: implements Meshtastic's published phone-API BLE GATT service (service UUID `6ba1b218-...`, `toradio`/`fromradio`/`fromnum` characteristics) so the official Meshtastic phone app can connect to this device exactly as it would to a real Meshtastic node. **The UUIDs are reproduced from memory of the public spec, not fetched from a live copy of the Meshtastic firmware source during this work — verify against current `github.com/meshtastic/firmware` before relying on this for real phone interop.**

Depends on `bt_mgr` having already brought up Bluedroid — relies on `device.pcat`'s `[flash]` load-priority ordering (`bt_mgr` at priority 2, `meshtastic` at priority 3) rather than an explicit dependency check. `mesh_ble_init()` is called from `mesh_manager_init()`; advertising is a separate step (`mesh_ble_set_advertising()`) gated on the user enabling Bluetooth in Settings.

Also extended in this round: `mesh_router.c`'s node table now tracks a display name (parsed from NodeInfo packets, previously decoded and discarded) and `mesh_manager_send_text()` gained a `to` parameter for addressed direct messages (previously broadcast-only) — both needed by the new `meshchat` app (`docs/06_Apps.md`). The single RX-callback slot (`mesh_manager_set_rx_callback`) became a small multi-subscriber array (`mesh_manager_add_rx_callback`/`_remove_rx_callback`, up to `MESH_MAX_RX_CB`) since both `meshchat` and this BLE service need independent RX visibility.

---

## miniwin

**Source:** `source/modules/miniwin/`  
**Type:** `PURR_MOD_UI`  
**Version:** 0.1.0  
**Required catcalls:** `CATCALL_FLAG_DISPLAY` (display is mandatory; touch is optional — graceful degradation)

MiniWin is an MIT-licensed embedded window manager by John Blaiklock. PURR OS wraps it as a `.purr` UI module. The upstream source is vendored in `MiniWin/` and is untouched. All PURR OS integration lives exclusively in `hal/purr_os/`.

### Directory layout

```
source/modules/miniwin/
  MiniWin/                  upstream source (MIT, John Blaiklock — do not modify)
    miniwin.c/.h            core window manager
    miniwin_touch.c         touch event processing
    miniwin_settings.c      calibration + settings persistence
    gl/                     graphics library (fonts, bitmaps, drawing primitives)
    ui/                     UI controls (buttons, labels, scroll bars, tabs, etc.)
    hal/                    upstream HAL implementations (DevKitC, Linux, STM32, etc.)
  hal/purr_os/              PURR OS HAL (the only files we own/modify)
    hal_lcd.c               routes mw_hal_lcd_* through catcall_display_t
    hal_touch.c             routes mw_hal_touch_* through catcall_touch_t
    hal_delay.c             vTaskDelay + esp_rom_delay_us
    hal_timer.c             FreeRTOS software timer (20ms tick for mw_tick_counter)
    hal_non_vol.c           NVS namespace "miniwin" for calibration data
  miniwin_config.h          unified config — runtime dimensions via catcall
  miniwin_module.c          .purr entry point + miniwin_task
  module.pcat               manifest
```

### How the HAL works

MiniWin was designed with a clean HAL separation. All platform-specific code lives in `hal/`. The old DevKitC HAL hardcoded SPI pins and drove ILI9341 directly. The new `purr_os` HAL routes everything through the kernel's catcall accessors:

```
mw_hal_lcd_filled_rectangle(x,y,w,h,color)
    → purr_kernel_display()->fill_rect(x,y,w,h, rgb888_to_rgb565(color))

mw_hal_touch_get_state()
    → purr_kernel_touch()->is_pressed() ? DOWN : UP

mw_hal_touch_get_point(&x,&y)
    → purr_kernel_touch()->read_point(&tx,&ty)
```

This means miniwin works with any display and any touch driver that has registered a catcall. No per-device configuration or compile-time flags needed.

### miniwin_config.h — unified config

The old architecture had per-device `miniwin_config.h` files because `MW_ROOT_WIDTH` and `MW_ROOT_HEIGHT` were compile-time constants. In PURR OS v0.12, these are runtime calls:

```c
#define MW_ROOT_WIDTH   mw_hal_lcd_get_display_width()
#define MW_ROOT_HEIGHT  mw_hal_lcd_get_display_height()
```

`mw_hal_lcd_get_display_width()` calls `purr_kernel_display()->get_info()` which returns the actual registered display dimensions. One config file works for all devices.

### miniwin_task

The module spawns a FreeRTOS task with 8KB stack:

```c
static void miniwin_task(void *arg) {
    mw_hal_non_vol_init();
    mw_hal_timer_init();      // starts 20ms FreeRTOS timer for mw_tick_counter
    mw_hal_lcd_init();        // reads display dimensions from catcall
    mw_hal_touch_init();      // no-op — touch already initialised by driver
    mw_init();                // MiniWin window manager init
    while (1) {
        mw_process_message(); // MiniWin message pump
        taskYIELD();
    }
}
```

### Touch without a touch driver

If no touch catcall is registered, `mw_hal_touch_get_state()` always returns `MW_HAL_TOUCH_STATE_UP` and `mw_hal_touch_get_point()` returns `false`. MiniWin starts and renders normally — you just can't interact by touch. Keyboard/trackball input via `catcall_input_t` is unaffected.

### Updating MiniWin

To update the upstream source:
1. Replace `MiniWin/` with the new version
2. Do **not** touch `hal/purr_os/` — that's our patch set
3. Do **not** touch `miniwin_config.h` or `miniwin_module.c`
4. Rebuild

---

## Module Registration Summary

| Module | Type | Provided catcalls | Required catcalls |
|--------|------|------------------|------------------|
| `driver_manager` | SYSTEM | — | — |
| `app_manager` | SYSTEM | — | — (runtime) |
| `lua_runtime` | SYSTEM | — | — |
| `wifi_mgr` | SYSTEM | — | — |
| `bt_mgr` | SYSTEM | — | — |
| `meshtastic` | SYSTEM | — | RADIO (runtime) |
| `miniwin` | UI | — | DISPLAY (TOUCH optional) |

This table is incomplete for UI backends beyond `miniwin` (`cupcake`, `cardstack`, `kittenui`, etc. aren't listed) — out of scope for this update, listed here for the modules actually touched.

---

*DP8 documentation pass performed by Claude Opus 5 in agentic/auto mode. The
module index above was generated from `source/modules/*/module.pcat` and
verified complete against the tree.*
