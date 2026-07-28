# PURR OS — Apps

## App Tiers

PURR OS has five app tiers, each with a distinct file extension and capability level.

| Extension | Name | API access | Typical use |
|-----------|------|-----------|-------------|
| `.meow` | Lua script, sandboxed | `win.*`, `sd.*`, `system.*` | Scripted tools, dashboards, simple games |
| `.hiss` | Lua script, privileged | `win.*`, `sd.*`, `system.*`, `kitt.*`, `radio.*`, `gps.*` | Scripted hardware tools (LoRa, GPS) without a full compile |
| `.kitten` | Lua script, privileged **+ autorun** | same as `.hiss` | A script that should run at boot without being launched |
| `.paws` | Compiled userland | `win.*`, `sd.*` | Native apps with no direct kernel calls |
| `.claw` | Compiled kernel-access | Full `purr_kernel_*` + `win.*` + `sd.*` | System tools, emulators, advanced shells |

**`.kitten` is `.hiss` that autoruns.** Same VM, same launch path, same
`kitt.*`/`radio.*`/`gps.*` bindings, and the same `purr-sig`/Developer-Mode
consent gate — the only difference is that the first `.kitten` found on SD is
started automatically at boot by `app_manager_init()`, with no user action.

Treat that as a meaningful trust decision, not a convenience: dropping a
`.kitten` on a card means it runs on the next power-on. The consent gate is what
stops an unsigned one doing so silently.

Enum values are `APP_TIER_MEOW`/`PAWS`/`CLAW`/`HISS`/`KITTEN` in
`source/modules/app_manager/app_manager.h`. Note the enum order (`PAWS` = 1,
`CLAW` = 2, `HISS` = 3, `KITTEN` = 4) does not match the trust ordering in the
table above — it is historical.

All five tiers access UI through the same `purr_win.h` dispatch layer (or `win.*` Lua bindings) — apps are not tied to a specific UI framework.

`.meow` scripts are executed by the `lua_runtime` module (`source/modules/lua_runtime/`), which vendors a real Lua 5.4 VM (`source/lib/lib_lua/`) — previously dead code (`app_manager.c` looked up a `"lua_runtime"` module that never existed), now a working system module.

---

## Unified UI API — purr_win.h

Added in v0.12.0. All compiled apps (.paws and .claw) use `purr_win.h` instead of calling LVGL or MiniWin directly. The active UI module registers a `catcall_ui_t` at boot and `purr_win.h` dispatches through it.

```c
#include "purr_win.h"   // all you need — no LVGL, no MiniWin headers

static purr_win_t  s_win;
static purr_wid_t  s_lbl;

static void on_tap(purr_wid_t w, purr_event_t e, void *u) {
    (void)w; (void)e; (void)u;
    purr_win_label_set(s_lbl, "Tapped!");
}

int my_app_init(void) {
    s_win = purr_win_create("My App");
    s_lbl = purr_win_label(s_win, "Hello, PURR OS!");
    purr_win_button(s_win, "Tap me", on_tap, NULL);
    purr_win_show(s_win);
    return 0;
}
```

This app runs identically on every windowed and framebuffer backend without changes — see `01_Architecture.md`'s tier table.

### Window management

```c
purr_win_t purr_win_create (const char *title);
void       purr_win_show   (purr_win_t win);
void       purr_win_hide   (purr_win_t win);
void       purr_win_clear  (purr_win_t win);   // remove all child widgets
void       purr_win_destroy(purr_win_t win);
```

### Labels

```c
purr_wid_t purr_win_label      (purr_win_t win, const char *text);  // create + add
void       purr_win_label_set  (purr_wid_t wid, const char *text);  // update text
void       purr_win_label_align(purr_wid_t wid, purr_align_t align);
// align: PURR_ALIGN_LEFT | PURR_ALIGN_CENTER | PURR_ALIGN_RIGHT
```

### Buttons

```c
purr_wid_t purr_win_button       (purr_win_t win, const char *label,
                                   purr_win_cb_t cb, void *user_data);
void       purr_win_button_enable(purr_wid_t wid, bool enabled);
```

Callback signature:
```c
typedef void (*purr_win_cb_t)(purr_wid_t wid, purr_event_t event, void *user);
// event: CLICKED | CHANGED | FOCUSED | DEFOCUS | SELECTED | ACTIVATED
```

### Textarea

```c
purr_wid_t  purr_win_textarea          (purr_win_t win, uint16_t w_pct, uint16_t h_pct);
void        purr_win_textarea_append   (purr_wid_t wid, const char *text);
void        purr_win_textarea_set      (purr_wid_t wid, const char *text);
void        purr_win_textarea_clear    (purr_wid_t wid);
const char *purr_win_textarea_get      (purr_wid_t wid);   // backend-owned, copy if needed
void        purr_win_textarea_focus    (purr_wid_t wid);   // shows keyboard / cursor
void        purr_win_textarea_on_change(purr_wid_t wid, purr_win_cb_t cb, void *user);
```

`w_pct` and `h_pct` are percentages of the window content area (0-100).

### Layout containers

```c
purr_wid_t purr_win_row       (purr_win_t win, uint8_t padding);  // horizontal row
purr_wid_t purr_win_col       (purr_win_t win, uint8_t padding);  // vertical column
purr_wid_t purr_win_row_grow  (purr_win_t win, uint8_t padding);  // row that fills remaining space
purr_wid_t purr_win_col_grow  (purr_win_t win, uint8_t padding);  // column that fills remaining space
void       purr_win_layout_end(purr_wid_t container);
```

Widgets created between `purr_win_row()` / `purr_win_col()` and `purr_win_layout_end()` are placed inside that container. On LVGL backends this uses flex layout; on MiniWin it uses simple stacking.

The plain `_row`/`_col` variants hug their own content (right for a row of buttons). Use the `_grow` variant when the container holds **percentage-sized** children — a list, a textarea, a split view — since a content-sized container can't resolve a percentage-sized child (it collapses to 0 size). `fileman.c`'s file-list/preview split is the reference example.

### On-screen keyboard

```c
void purr_win_keyboard_show(purr_win_t win, purr_wid_t target_textarea);
void purr_win_keyboard_hide(purr_win_t win);
```

On LVGL backends: shows LVGL's built-in keyboard. On MiniWin: no-op — a physical keyboard is handled via `catcall_input` automatically. Backends targeting keyboard devices (Cupcake/Tabby/Mochi) suppress the on-screen keyboard entirely when one is present.

---

## .meow — Lua Scripts

`.meow` files are Lua 5.4 scripts run by `lua_runtime` (`source/modules/lua_runtime/`), a `PURR_MOD_SYSTEM` module that vendors the real Lua 5.4 VM (`source/lib/lib_lua/`, ported from the PURR-OS-0.11 archive) and binds it to the current codebase's plain-C `purr_win.h`/SD APIs — not KITT's old C++ singleton. One global Lua state runs at a time, matching the single-.meow-VM assumption `app_manager.c` already makes elsewhere.

The VM exposes three namespaces: `system.*`, `sd.*`, `win.*` — no `kitt.*` (that was the 0.11-era C++ API this doesn't use). A `.hiss` script (see below) runs through this exact same VM and launch path, with three additional namespaces registered — `kitt.*`, `radio.*`, `gps.*` — for scripted access to hardware. Trust is extension-only: a `.hiss` file gets those extra namespaces whether it's on flash or SD, same as every other tier here.

### `win.*` — Window API

A thin, 1:1 Lua wrapper over `purr_win.h`'s own C API — same handles, same call shape:

```lua
local win = win.create("My App")
local lbl = win.label(win, "Hello!")
win.label_set(lbl, "Updated text")

local wid = win.button(win, "Tap me", function()
    win.label_set(lbl, "Tapped!")
end)

local ta = win.textarea(win, 100, 60)   -- w_pct, h_pct
win.textarea_set(ta, "some text")
local text = win.textarea_get(ta)

-- Layout containers — wrap a line of widgets so they sit side-by-side
-- instead of each stacking in its own row (mirrors purr_win_row/col in C).
local row = win.row(win, 4)             -- pad
win.button(win, "1", function() end)
win.button(win, "2", function() end)
win.layout_end(row)

win.label_align(lbl, 2)   -- 0=left, 1=center, 2=right

win.show(win)
win.destroy(win)
```

`win.row`/`win.col` hug their own content; `win.row_grow`/`win.col_grow` expand to fill the remaining space in their parent — use the `_grow` variant when the container holds percentage-sized children (a list, a textarea), same rule as the C API's `_grow` variants.

Button callbacks are plain Lua closures — the VM keeps a registry reference and trampolines back into Lua on click. **Threading note**: a script's main body runs synchronously inside its launch task, which exits right after the script returns (same as every native app's `init()`) — write UI-building scripts that build the window and return, then respond to taps via callbacks, rather than scripts that loop forever themselves.

### `sd.*` — SD Card API

```lua
local contents, err = sd.read("/sdcard/myapp/data.txt")
if contents then
    local ok = sd.write("/sdcard/myapp/out.txt", "hello world\n")
end
```

Plain path-based read/write (no file-handle object) — use full `/flash/...` or `/sdcard/...` paths like any other PURR OS app.

### `system.*` — System API

```lua
system.print("logged via ESP_LOGI")
system.delay(100)              -- milliseconds
local now = system.time_ms()
```

### Example .meow app

```lua
-- hello.meow
local win_h = win.create("Hello PURR")
local lbl   = win.label(win_h, "Tapped: 0")
local count = 0

win.button(win_h, "Tap me!", function()
    count = count + 1
    win.label_set(lbl, "Tapped: " .. count)
end)

win.show(win_h)
```

---

## .hiss — Privileged Lua Scripts

`.hiss` files run through the exact same Lua VM and launch path as `.meow` (same `lua_runtime` module, same `app_manager.c` `launch_meow()`) — it is not a separate interpreter or a separate launch mechanism. The only difference: three extra namespaces are registered into the script's Lua state before it runs.

Trust is **extension-only** — a `.hiss` script gets `kitt.*`/`radio.*`/`gps.*` whether it's baked into `/flash/apps` or dropped on `/sdcard/apps`, same as `.meow`/`.paws`/`.claw` are all trusted by extension alone today. This is a deliberate simplicity choice, not an oversight — see the signature tag below for the lightweight, non-gating provenance marker that exists alongside it.

### `radio.*` — LoRa Radio API

Thin wrappers over `catcall_radio_t` (`source/kernel/catcalls/catcall_radio.h`):

```lua
if radio.available() then
    local data = radio.receive(256)
    if data then
        system.print("got " .. #data .. " bytes, rssi=" .. radio.rssi())
    end
end

local ok = radio.send("hello mesh")
```

- `radio.send(data) -> ok` — `data` is a Lua string, sent byte-for-byte (binary-safe).
- `radio.receive(max_len) -> data_or_nil` — `max_len` defaults to 256, capped at 1024.
- `radio.available() -> bool`
- `radio.rssi() -> int`, `radio.snr() -> number`

All five degrade gracefully (return `false`/`nil`/`0`, never error or crash) when no radio catcall is registered on the current device.

### `gps.*` — GPS API

```lua
local fix = gps.fix()
if fix and fix.valid then
    system.print(string.format("%.5f, %.5f (%d sats)", fix.latitude, fix.longitude, fix.satellites))
end
```

`gps.fix() -> table_or_nil` wraps `catcall_gps_t::get_fix()` — returns `nil` if no GPS catcall is registered, otherwise a table with `latitude`, `longitude`, `altitude_m`, `speed_mps`, `hdop`, `satellites`, `valid`.

### `kitt.*` — Kernel Introspection

```lua
for _, m in ipairs(kitt.modules()) do
    system.print(m.name .. " [" .. m.type .. "] v" .. m.version)
end
```

`kitt.modules() -> array of {name, type, version}` — the same loaded-module registry `terminal.c`'s `modules` command surfaces, via `purr_kernel_module_count()`/`purr_kernel_module_at()`.

### Signature tag + Developer Mode

A `.hiss` file may carry a one-line comment near the top marking its provenance:

```lua
-- purr-sig: dev-signed
```

One of `unsigned` (the default — no tag present reads as this), `dev-signed`, `trusted-signed`, `dev-approved`. This is a **self-declared, honor-system tag, not a cryptographic signature** — anyone editing the file can change it, the same trust level as the extension-only decision above. `kitt.*`/`radio.*`/`gps.*` availability is still decided by the `.hiss` extension alone, not this tag — that split hasn't changed.

What the tag *does* gate is whether the script is allowed to run at all when it's `unsigned`: Settings has a **Developer Mode** toggle (off by default, persisted to NVS), and `app_manager.c`'s `launch_meow()` rejects launching an `unsigned` `.hiss` script — `APP_STATE_ERROR`, "unsigned .hiss — enable Developer Mode in Settings" — unless Developer Mode is on. A `dev-signed`/`trusted-signed`/`dev-approved` script always launches regardless of the toggle; only the `unsigned` (or no-tag) case is affected. This check happens on-device, at launch time, against the script's own source — it's independent of `catstrap validate`/`build`'s build-time print of the same tag (still informational, still dev-machine-side).

### Building

Same as `.meow` — no compilation, just packaged and copied:

```bash
catstrap build my_tool
catstrap validate my_tool.hiss
# Output: cattobaked/apps/my_tool.hiss
```

---

## .paws — Compiled Userland Apps

`.paws` apps are compiled native binaries. They have access to `purr_win.h` and SD file APIs, but **no** `purr_kernel_*` calls.

### app.pcat

```ini
name        = "my_app"
version     = "0.1.0"
tier        = "paws"
author      = "Your Name"
description = "What this app does."

idf_requires = "esp_common driver freertos"
```

### Building

```bash
catstrap build my_app
# Output: cattobaked/apps/my_app.paws
# Also writes CMakeLists.txt into source/apps/*/my_app/ for IDF component inclusion
```

### Minimal .paws app

```c
#include "purr_win.h"
#include "purr_module.h"

static purr_win_t s_win;

static int my_init(void) {
    s_win = purr_win_create("My App");
    purr_win_label(s_win, "Hello!");
    purr_win_show(s_win);
    return 0;
}

static void my_deinit(void) {
    purr_win_destroy(s_win);
}

purr_module_header_t purr_module = {
    .magic         = PURR_MODULE_MAGIC,
    .abi_version   = PURR_MODULE_ABI_VERSION,
    .module_type   = PURR_MOD_APP,
    .load_priority = PURR_PRIORITY_OPTIONAL,
    .name          = "my_app",
    .version       = "0.1.0",
    .init          = my_init,
    .deinit        = my_deinit,
};
```

---

## .claw — Kernel-Access Apps

`.claw` apps have the same structure as `.paws` but add full `purr_kernel_*` access: catcall accessors, system info, reboot, and SD availability checks.

### Speed Demon — taking the whole machine

A pre-linked `.claw` can ask for the device to itself: the launcher, system UI,
mesh stack and radios are unloaded before it starts and restored when it exits,
freeing 12.8-15.7 KB of internal DRAM and the SPI bus. It is one line in the
app's module registration:

```c
PURR_MODULE_REGISTER(mygame) = {
    ...
    .speed_demon = 1,
};
```

`app_manager` drives both halves; the app calls neither enter nor exit. It does
have to beat `purr_speed_demon_heartbeat()` every 5 s and call
`app_manager_notify_exited("mygame")` before its task ends.

**Pre-linked `.claw` only.** Lua tiers cannot use this — the flag is read from
the module header they do not have, and Speed Demon unloads `lua_runtime`
itself. See [15_SpeedDemon.md](15_SpeedDemon.md) for the full contract.

```c
#include "purr_win.h"
#include "purr_kernel.h"    // adds purr_kernel_display(), free_ram(), etc.
#include "purr_module.h"

// Access display info directly:
const catcall_display_t *disp = purr_kernel_display();
if (disp) {
    display_info_t info;
    disp->get_info(&info);
    // info.width, info.height, info.name
}

// Access radio:
const catcall_radio_t *radio = purr_kernel_radio();
if (radio) radio->send((uint8_t*)"ping", 4);

// System info:
uint32_t free_ram = purr_kernel_free_ram();
uint64_t uptime   = purr_kernel_uptime_ms();
bool     has_sd   = purr_kernel_sd_available();
```

Set `tier = "claw"` in app.pcat.

---

## Built-in System Apps

These are baked into the SPIFFS flash image for medium/large-screen devices:

| App | Tier | Description |
|-----|------|-------------|
| `settings` | `.claw` | Theme, brightness, keyboard backlight, WiFi (scan/connect), Bluetooth (BLE scan/pair), wallpaper, **lock-screen notification privacy**, mesh-backend switch, Developer Mode, SD status, About (OS/KITT version, chip, RAM, uptime, drivers), reboot |
| `terminal` | `.claw` | Shell: `ls`, `cat`, `echo`, `modules`, `mem`, `uptime`, `reboot` |
| `fileman` | `.claw` | Browse SPIFFS + SD; New Folder/Rename/Delete; text file preview |
| `hwtest` | `.claw` | Hardware diagnostics — live trackball motion/click and keyboard keypress log |
| `drivermgr` | `.claw` | Lists scanned drivers (`driver_manager` module) with OK/COMPAT/FAIL/SKIP status |
| `msn` | `.claw` | Mesh Social Network — tile-grid home (Nodes/Messages/Channels/Manage), private 1:1 chat and multi-channel group chat, over whichever mesh backend is active. Renamed from `meshchat`; talks to `msn_backend.h`'s vtable rather than `meshtastic.h` directly, so it works on Meshtastic **and** MeshCore |
| `meshdiag` | `.claw` | Meshtastic hardware/debug diagnostics — kernel log tail, radio + node stats, test-send |
| `taskmgr` | `.claw` | Lists running apps; the one deliberate place to kill one |
| `services` | `.claw` | Live status of core background services + memory pressure, from the kernel health registry |
| `nearby` | `.claw` | Read-only list of other PURR OS devices seen via ESP-NOW proximity beacons |
| `milkbar` | `.claw` | Manage apps on a *paired* PURR OS device remotely (via `app_manager_remote` over `proximity_rpc`) |

Exclusive apps (`source/apps/exclusive/`) — `magicmac`, `magidos` — are covered
in `08_Exclusives.md`.

Calculator is no longer a built-in — it's now `calculator.meow` (`sdcard_apps/calculator.meow` in this repo), an SD-loaded `.meow` script. See "Built-in vs. SD Demo Apps" below.

## SD Demo Apps

`sdcard_apps/` at the repo root holds the canonical source for `.meow` scripts meant to be copied onto a device's SD card at `/sdcard/apps/` — they are never baked into the flash image. Exception: `jc3248w535` has no SD card slot (`sd_enabled = false`), so a `.meow` file could never reach it — that device keeps `calculator` as its original native `.paws` app.

| Script | Demonstrates |
|--------|--------------|
| `calculator.meow` | Full keypad UI via `win.row`/`win.button`/`win.layout_end` — replaces the old native `calculator.paws` |
| `clock.meow` | A callback-free loop updating a label via `system.time_ms()`/`system.delay()` |
| `notepad.meow` | `sd.read()`/`sd.write()` wired to Save/Load buttons over a textarea |

Requires the `lua_runtime` module to be flashed on the target device (see its device.pcat's `[flash]` section) — without it, `.meow` files are discovered but fail to launch.

`settings` is a staple system feature — always present on any medium/large screen device (it absorbed the old standalone `about` app). The rest follow the same `purr_win.h` API and can be excluded on flash-constrained builds. There is no standalone `about` app anymore.

---

## App Scan Order

`app_manager` scans at boot in this priority order:

```
/flash/apps      baked-in system apps (highest priority)
/sdcard/apps     user-installed apps from SD card
```

SD apps with the same name as a flash app shadow the flash version — useful for testing app updates without reflashing.

---

## Writing Your First App

### Option A — .meow (fastest)

1. Create `myapp.meow` on the SD card at `/sdcard/apps/myapp.meow`
2. Write Lua using `win.*`, `sd.*`, `system.*`
3. Power on — appears in the Cat Apps launcher
4. Tap to launch

### Option B — .paws (native, no kernel)

1. `mkdir source/apps/user/myapp/`
2. Write `app.pcat` (tier = "paws") and `myapp.c`
3. `catstrap build myapp`
4. Copy `cattobaked/apps/myapp.paws` to `/sdcard/apps/`

### Option C — .claw (native, full kernel)

Same as .paws but set `tier = "claw"` in app.pcat and include `purr_kernel.h`.

---

## App Directory Layout

```
source/apps/
  system/             built-in system apps
    settings/         theme, brightness, keyboard backlight, WiFi, Bluetooth, wallpaper, About, SD, reboot
    terminal/         built-in shell
    fileman/          file manager
    calculator/       calculator (.paws) — only still referenced by jc3248w535, the one device with no SD card slot; every other device now uses sdcard_apps/calculator.meow instead
    hwtest/           hardware diagnostics
    drivermgr/        driver status list (UI over the driver_manager module)
    meshchat/         MSN-style buddy list + private chat over Meshtastic
  exclusive/          in-house exclusives (rewrite in progress)
    magicmac/         Mac OS inspired shell
    magidos/          DOS inspired shell
```

Note: the `drivermgr` app directory is named differently from the `driver_manager` backend module (`source/modules/driver_manager/`) on purpose — ESP-IDF component names are derived from the directory name, and giving the app the same directory name as the module caused a silent component-name collision (one component's object files displaced the other's in the final link, producing "undefined reference" errors for functions that were actually compiled — just discarded).

User apps live on the SD card at `/sdcard/apps/` — they are never in the repo.

---

*DP8 documentation pass performed by Claude Opus 5 in agentic/auto mode. The
app list and tier table were verified against `source/apps/` and
`app_manager.h`.*
