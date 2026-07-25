# PURR OS — Build Tools

Three separate tools handle three distinct build concerns. All output lands in `cattobaked/` at the repo root.

```
purrstrap/       final flashable firmware image per device
modulestrap/     .purr kernel module + driver blobs
catstrap/        user apps (.meow/.paws/.claw) + SDK
```

A master launcher (`purr.py` / `purr.sh` / `purr.ps1`) ties all three together with an interactive menu or CLI pass-through.

---

## Quick Start

```bash
# Interactive launcher — pick your tool from a menu
./purr.sh                         # bash (Linux/macOS)
.\purr.ps1                        # PowerShell (Windows)
python3 purr.py                   # Python directly

# Jump to a specific tool's interactive UI
./purr.sh purrstrap
./purr.sh modulestrap
./purr.sh catstrap

# CLI pass-through (no UI) — fully scriptable, CI-friendly
python3 purrstrap/purrstrap.py build tdeck_plus_arduino
python3 modulestrap/modulestrap.py list
python3 catstrap/catstrap.py sdk info

# Build + flash a device
python3 purrstrap/purrstrap.py flash tdeck_plus_arduino -p /dev/ttyACM0 --erase
```

---

## Two-Layer Architecture

Every tool comes in two files:

| File | Role |
|------|------|
| `<tool>.py` | Pure CLI — argparse, all logic, no interactive prompts |
| `<tool>_ui.py` | Interactive wrapper — numbered menus → calls `<tool>.py` |

The UI layer never reimplements logic. It collects choices, assembles CLI arguments, and delegates via `subprocess`. This keeps the CLI always scriptable and the UI always optional.

`purr.py` is the master launcher:
- No args → tool picker menu
- Tool name only → opens that tool's interactive UI
- Tool name + args → passes args straight to the underlying CLI

---

## purrstrap — Firmware Image Builder

**File:** `purrstrap/purrstrap.py`
**Output:** `cattobaked/<device>/`

purrstrap is the top-level image builder. It:
1. Reads `source/devices/<device>/device.pcat`
2. Calls modulestrap to build all `.purr` module + driver blobs
3. Calls catstrap to register all app blobs
4. Generates `purr_device_glue.c` with pin `#defines` and radio capability flags
5. Generates/refreshes `CoreOS/sdkconfig_<device>` from `device.pcat` (chained with an optional hand-maintained `sdkconfig_<device>.overrides`)
6. Stages blobs into a SPIFFS image (`flash.bin`)
7. Invokes IDF to compile the kernel spine
8. Merges everything into `PURR_OS_<device>.bin`

### Commands

```
purrstrap build <device>               build firmware
purrstrap flash <device> [-p PORT]     build + flash to connected device
purrstrap flash <device> --erase       erase entire chip before flashing (recommended)
purrstrap clean <device>               remove build artifacts for device   (see caveat)
purrstrap monitor <device> [-p PORT]   open serial monitor after flash
purrstrap generate [device] [--check]  regenerate sdkconfig_<device> from device.pcat (omit device = all)
purrstrap bake [--dp]                  build ALL devices → releases/v<version>/
purrstrap list                         list all supported devices + radio capabilities
purrstrap status                       show .purrstrap workspace config
purrstrap doctor                       check IDF + environment health

purrstrap pkg list <device>            show what's selected for a device
purrstrap pkg add <device> <pkg>       select a driver/module/app (edits device.pcat)
purrstrap pkg remove <device> <pkg>    unselect a static package
purrstrap pkg upgrade <device> [pkg]   re-check version of selected static packages
purrstrap pkg verify <device>          check installed.json against device.pcat + source/

purrstrap pkg app list <device>        list apps installed via pkg app
purrstrap pkg app install <device> <f> stage a .meow/.hiss script into the flash image
purrstrap pkg app remove <device> <f>  remove a staged script
purrstrap pkg app upgrade <device> [f] re-stage installed scripts
```

> ⚠️ **`purrstrap clean` does not currently work.** `cmd_clean` looks for
> `source/build_<device>`, but `cmd_build` builds in `CoreOS/build_<device>`. It
> reports "nothing to clean" while the real build tree sits untouched. Until
> that's fixed, delete `CoreOS/build_<device>/` by hand for a clean build. Both
> paths are gitignored, so removing it is safe.

### `purrstrap pkg` — package management

Two distinct things share this namespace, and the difference matters:

- **`pkg add`/`remove`/`upgrade`/`verify`** operate on *static* packages —
  drivers, modules and apps compiled into the firmware. They edit `device.pcat`
  and require a rebuild + reflash to take effect.
- **`pkg app install`/`remove`/`upgrade`** operate on `.meow`/`.hiss` Lua
  scripts, which are staged into the SPIFFS image rather than compiled. This is
  the only genuine file-drop path in the system.

`pkg verify` compares `installed.json` against what `device.pcat` and `source/`
actually contain, and reports drift — useful after hand-editing a manifest.

### `purrstrap bake`

Builds every device in `source/devices/` and copies the results to
`releases/v<PURROS_VERSION>/`, with a `manifest.json` recording per-device
status.

`--dp` additionally packages a full developer-preview bundle into
`CatReleases/DP<N>/` — split *and* merged images per device, a README with
flashing instructions, and a top-level zip. The `N` is parsed from
`PURROS_VERSION`'s `-dpN` suffix, so **bumping that one constant is what
determines the next DP number**; the two numbering schemes cannot drift apart.

> `PURROS_VERSION` in `purrstrap.py` is duplicated by `PURR_KERNEL_VERSION` in
> `source/kernel/core/purr_kernel.h`, which the About app reports. These are
> hand-maintained separately and have drifted a full release before. Bump both.

### `purrstrap generate`

`CoreOS/sdkconfig_<device>` is generated from `device.pcat` (flash size, PSRAM, UI backend, Arduino kernel config) — it's regenerated automatically on every `purrstrap build`, but `generate` lets you refresh it (or check it) without a full build:

```bash
python3 purrstrap/purrstrap.py generate tdeck_plus     # regenerate one device
python3 purrstrap/purrstrap.py generate                # regenerate all devices
python3 purrstrap/purrstrap.py generate --check         # CI-style drift check, exits nonzero, writes nothing
```

Hardware quirks with no equivalent `device.pcat` field (panel color-swap, touch-flip, the WinCE shell flag, ...) live in a hand-maintained `CoreOS/sdkconfig_<device>.overrides`, chained in after the generated file. Most devices don't need one.

### `--erase` flag

Always use `--erase` when flashing after a kernel or partition table change. It performs a full chip erase before writing, preventing stale SPIFFS data from causing boot issues:

```bash
python3 purrstrap/purrstrap.py flash tdeck_plus_arduino -p /dev/ttyACM0 --erase
```

### `purrstrap list` example output

```
device                  chip        wifi  bt    lora      sd
cyd                     esp32       yes   yes   -         yes
cyd_s024c               esp32       yes   yes   -         yes
cyd_s028r               esp32       yes   yes   -         yes
tdeck                   esp32s3     yes   yes   sx1262    yes
tdeck_plus              esp32s3     yes   yes   sx1262_rl yes
tdeck_plus_arduino      esp32s3     yes   yes   sx1276    yes
jc3248w535              esp32s3     yes   yes   -         no
heltec                  esp32s3     yes   yes   sx1262    no
waveshare169            esp32s3     yes   yes   -         no
```

### `purrstrap doctor`

Checks: `idf.py` on PATH, `python3`, `git`, `IDF_PATH` set and valid, `source/kernel/core/boot.c` exists, `spiffsgen.py` available.

### `purrstrap build <device>` — full sequence

1. Reads `device.pcat`
2. Writes `.purrstrap` workspace JSON
3. Calls `modulestrap build all` → produces `.purr` blobs
4. Calls `catstrap build all` → registers app blobs
5. Generates `cattobaked/<device>/glue/purr_device_glue.c`
6. Generates `CoreOS/sdkconfig_<device>` from `device.pcat` (chained with an optional `sdkconfig_<device>.overrides` at build time)
7. Stages into `cattobaked/<device>/spiffs_staging/`:
   - `modules/` — system modules (`.purr`)
   - `drivers/<type>/` — driver blobs (`.purr`)
   - `apps/` — app blobs (`.claw`/`.paws`/`.meow`) + `.meta.json`
8. Runs `spiffsgen.py` → `cattobaked/<device>/flash.bin`
9. Runs `idf.py set-target` + `idf.py build` on `CoreOS/` → `firmware.bin`
10. Runs `esptool.py merge_bin` → `PURR_OS_<device>.bin`

### Output: `cattobaked/<device>/`

```
PURR_OS_<device>.bin        merged flash image (bootloader + kernel + SPIFFS)
firmware.bin                kernel spine only
bootloader.bin
partition-table.bin
flash.bin                   SPIFFS filesystem image (modules + drivers + apps)
glue/purr_device_glue.c     generated pin + radio #defines
spiffs_staging/             staged files before spiffsgen runs
build.json                  build metadata (device, versions, timestamps)
```

(`CoreOS/sdkconfig_<device>` is also regenerated on every build, but it's a repo-root artifact under `CoreOS/`, not `cattobaked/<device>/`.)

### Building without IDF

If `IDF_PATH` is not set, purrstrap skips the kernel spine build and merge. `flash.bin` is still produced and can be flashed manually to the SPIFFS partition:
```bash
esptool.py write_flash 0xD90000 cattobaked/tdeck_plus_arduino/flash.bin
```

---

## modulestrap — Module + Driver Blob Compiler

**File:** `modulestrap/modulestrap.py`
**Output:** `cattobaked/modules/`, `cattobaked/drivers/`

modulestrap compiles all `.purr` kernel module and driver blobs. It scans `source/modules/`, `source/drivers/`, and `user_drivers/` (community drop zone), generates IDF component `CMakeLists.txt` fragments, and writes `cattobaked/components_manifest.cmake` so CoreOS includes everything in one IDF build.

### Commands

```
modulestrap build all               register all modules and drivers
modulestrap build modules           system modules only
modulestrap build drivers           drivers only
modulestrap build <name>            one target (e.g. "kittenui", "display/ili9341")
modulestrap list                    list all buildable targets
modulestrap list --drivers PATH     also list from an external driver directory
modulestrap clean [name|all]        remove .purr blobs from cattobaked/
```

### What "register" means

modulestrap does not invoke IDF directly — that is purrstrap's job. Instead it:
1. Finds all `.c`/`.cpp` source files in the module directory
2. Generates `CMakeLists.txt` as an IDF component fragment (include dirs point to `source/kernel/`)
3. Writes `cattobaked/components_manifest.cmake` listing all `EXTRA_COMPONENT_DIRS`

When purrstrap runs `idf.py build` on `CoreOS/`, it includes `components_manifest.cmake` which pulls every registered module in as an IDF component.

### Output structure

```
cattobaked/
  modules/
    driver_manager.purr
    kittenui.purr
    miniwin.purr
    oled_ui.purr
    app_manager.purr
  drivers/
    display/  ili9341.purr  st7789.purr  axs15231b.purr  ssd1306.purr
    touch/    xpt2046.purr  cst816s.purr  gt911.purr
    input/    trackball.purr  bbq20.purr
    radio/    sx1262.purr  sx1262_rl.purr  sx1276.purr
    gps/      generic_nmea.purr
  components_manifest.cmake
```

### Custom / community drivers

Drop a directory containing `driver.pcat` and C source into `user_drivers/`:
```
user_drivers/
  my_sensor/
    driver.pcat
    my_sensor.c
    CMakeLists.txt
```
modulestrap auto-scans `user_drivers/`. Or point to an external path:
```bash
python3 modulestrap/modulestrap.py build all --drivers /path/to/community_drivers
```

---

## catstrap — App Builder + SDK

**File:** `catstrap/catstrap.py`
**Output:** `cattobaked/apps/`

catstrap builds user apps and the in-house system exclusives. It also manages the catstrap SDK headers that app developers build against.

### Commands

```
catstrap build all              build/register all user apps + exclusives
catstrap build <name>           build one app by name
catstrap build magicmac         build MagicMac (.catt exclusive)
catstrap build magidos          build MagiDOS (.catt exclusive)
catstrap validate <file.meow>   syntax-check a Lua script without running it
catstrap sdk info               show SDK version + API surface
catstrap sdk install            copy SDK headers to catstrap/sdk/include/
catstrap list                   list all buildable apps
catstrap clean [name|all]       remove app output from cattobaked/apps/
```

### App tiers

| Extension | Tier | API access |
|-----------|------|-----------|
| `.meow` | Lua sandbox | `win.*`, `sd.*`, `kitt.*`, `purr.info()` |
| `.paws` | Compiled userland | `purr_win.h`, `sd.*` |
| `.claw` | Compiled kernel-access | `purr_win.h`, `sd.*`, `purr_kernel_*`, `purr_module_*` |
| `.catt` | In-house exclusive | Same as `.claw` — precompiled by the PURR OS team |

### SDK

`catstrap sdk install` populates `catstrap/sdk/include/`:

```
catstrap/sdk/include/
  purr_sdk.h           tier gate — picks the right sub-header at compile time
  purr_sdk_paws.h      catcall_display_t, catcall_touch_t, purr_win.h
  purr_sdk_claw.h      all above + purr_kernel.h + purr_module.h
  catcall_display.h    copied from source/kernel/catcalls/
  catcall_touch.h
  catcall_input.h
  catcall_radio.h
  catcall_gps.h
  catcall_ui.h
  catcalls.h
  purr_win.h
  purr_kernel.h        (.claw only)
  purr_module.h        (.claw only)
```

Compile your app with `-DPURR_TIER_PAWS` or `-DPURR_TIER_CLAW` to get the right API surface.

### Output structure

```
cattobaked/apps/
  settings.claw             settings.claw.meta.json
  about.claw                about.claw.meta.json
  terminal.claw             terminal.claw.meta.json
  fileman.claw              fileman.claw.meta.json
  calculator.paws           calculator.paws.meta.json
  magicmac.catt             magicmac.catt.meta.json
  magidos.catt              magidos.catt.meta.json
```

---

## Full Build Pipeline

```bash
# One-time SDK setup
python3 catstrap/catstrap.py sdk install

# Full build for a device
python3 purrstrap/purrstrap.py build tdeck_plus_arduino

# Build + flash with chip erase
python3 purrstrap/purrstrap.py flash tdeck_plus_arduino -p /dev/ttyACM0 --erase

# Open serial monitor
python3 purrstrap/purrstrap.py monitor tdeck_plus_arduino -p /dev/ttyACM0
```

purrstrap calls modulestrap and catstrap automatically as part of `purrstrap build`. Run them separately for incremental work:
```bash
python3 modulestrap/modulestrap.py build display/st7789   # rebuild just one driver
python3 catstrap/catstrap.py build settings               # rebuild just one app
python3 purrstrap/purrstrap.py build tdeck_plus_arduino   # re-assemble image
```

---

## Complete `cattobaked/` Layout

```
cattobaked/
├── <device>/                ← purrstrap output (one dir per device built)
│   ├── PURR_OS_<device>.bin
│   ├── firmware.bin
│   ├── bootloader.bin
│   ├── partition-table.bin
│   ├── flash.bin
│   ├── glue/purr_device_glue.c
│   ├── spiffs_staging/
│   └── build.json
├── modules/                 ← modulestrap: system module blobs
│   ├── driver_manager.purr
│   ├── kittenui.purr
│   ├── miniwin.purr
│   ├── oled_ui.purr
│   └── app_manager.purr
├── drivers/                 ← modulestrap: hardware driver blobs
│   ├── display/
│   ├── touch/
│   ├── input/
│   ├── radio/
│   └── gps/
├── apps/                    ← catstrap: app packages
│   ├── settings.claw
│   ├── terminal.claw
│   └── …
└── components_manifest.cmake  ← modulestrap: included by CoreOS CMake
```

---

## Workspace Config — `.purrstrap`

purrstrap saves the last-used device and build metadata to `.purrstrap` (JSON) at the repo root. The interactive UI reads this to pre-fill the device picker. It is gitignored.

---

## Master Launcher — `purr.py` / `purr.sh` / `purr.ps1`

| File | Platform |
|------|---------|
| `purr.py` | Python directly — all platforms |
| `purr.sh` | bash — Linux / macOS |
| `purr.ps1` | PowerShell — Windows |

`purr.sh` and `purr.ps1` both `cd` to the repo root before launching so they work from any directory.

---

## Environment Requirements

Run `python3 purrstrap/purrstrap.py doctor` to check your environment.

| Requirement | Minimum | Notes |
|-------------|---------|-------|
| Python | 3.8+ | System install |
| ESP-IDF | 5.3.x | `IDF_PATH` must be set; virtualenv activated |
| `idf.py` | — | Must be on PATH or resolvable from `IDF_PATH` |
| `esptool.py` | — | Included with IDF |
| `spiffsgen.py` | — | Included with IDF (`$IDF_PATH/components/spiffs/spiffsgen.py`) |
| `git` | — | Used for version tagging |
| `pyserial` | — | Needed for flash/monitor; installed in IDF venv |

---

*DP8 documentation pass performed by Claude Opus 5 in agentic/auto mode. The
command lists above were generated from each tool's `add_parser()` calls.*
