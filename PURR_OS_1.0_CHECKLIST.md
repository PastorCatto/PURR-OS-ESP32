# PURR OS v1.0 — Release Preparation Checklist

> **Audited at v1.0.0-dp8 (2026-07-24).** This checklist was written during the v0.13.0
> era and had never been maintained — all 140 items were unticked. Every item below has
> now been walked against the tree.

### Legend

| Mark | Meaning |
|---|---|
| `[x]` | Done and verifiable |
| `[~]` | Partially done, or done for some targets only — see the note |
| `[ ]` | Not done |
| `[–]` | No longer applicable — struck through, with the reason |

**A `[x]` on a hardware item means the hardware was actually exercised.** Anything that
could only be confirmed by reading source is marked as such in its note; a clean build is
not evidence of runtime behaviour. Where no one has put the device on a bench, the box
stays empty — the point of this audit is to make the untested surface visible, not to
make the list look finished.

---

## 1. Hardware / Device Validation

All catcalls must be confirmed working on every supported device before 1.0.

### T-Deck Plus (`tdeck_plus` kernel — IDF path, canonical as of the consolidation pass below)
- [x] Display — ST7789 320×240, correct colors, no inversion
- [x] Touch — GT911 responds and stays awake (power-save keepalive confirmed)
- [x] Trackball — all 4 directions + click register correctly *(repeat/typematic behaviour fixed at the driver layer in dp8; the long-standing 0.13 bug)*
- [x] Keyboard — BBQ20 all keys respond, no bus lockout
- [~] LoRa — **SX1262 via `sx1262_rl`, not SX1276.** The item was written against a
  misconfiguration: the board carries an SX1262 and failed with
  `sx1276: unexpected version 0x00` until the driver was swapped (see `meshplan.md`).
  Init, retune to LONG_FAST and a NODEINFO broadcast are confirmed in a real boot log;
  RSSI/SNR were never bench-measured, and a two-device text exchange needs a second board.
- [ ] GPS — NMEA fix acquired and surfaced through `catcall_gps_t`
- [x] SD card — mount, read, write confirmed
- [–] ~~Cardstack — responds to touch/trackball/keyboard correctly, hosts app windows cleanly~~
  → `tdeck_plus` no longer selects Cardstack. It ships **Cupcake** with the Android
  systemui style as of the dp8 performance pass (it shipped Mochi earlier in dp8);
  Mochi and Tabby are the other supported choices, and both now share one display
  path (`modules/common/purr_lv_flush.h`). Cardstack remains a backend but is not
  this device's.

### T-Deck Plus IDF-kernel migration gate (must pass before `kernel_tdeck_plus_arduino` is archived)

`kernel_tdeck_plus` (IDF i2c_master path) received a GT911 fix in commit `322159c6`
that makes it functionally equivalent to `kernel_tdeck_plus_arduino` (the Wire-based
workaround kernel, now deprecated — see `docs/13_Kernels.md`). Before deleting the
Arduino kernel, confirm on real hardware:

- [x] Flash `tdeck_plus` (IDF kernel) to a physical unit via `purrstrap flash tdeck_plus -p <port> --erase`
- [x] No `ESP_ERR_INVALID_STATE` (the original IDF 5.3 regression symptom) appears in the serial log during GT911 init
- [x] Tap all 4 screen corners + center — coordinates are correct and non-mirrored (validates the byte-offset/signed-coordinate fix)
- [ ] Power-cycle 10+ times consecutively — GT911 I2C address (0x5D) latches reliably every time, not just on a fresh boot
- [~] Trackball, BBQ20 keyboard, SD, LoRa, and GPS all match current `tdeck_plus_arduino` behavior
  *(trackball, keyboard and SD confirmed; GPS not comparatively tested. LoRa **cannot** be
  compared like-for-like — `tdeck_plus_arduino` still selects `sx1276`, which does not match
  the physical board. That target's radio is misconfigured, not a baseline.)*
- [ ] Extended soak test (hours, not minutes) — no I2C bus lockups or touch-driver hangs under sustained use
- [ ] WiFi STA and battery-status reporting confirmed working on the IDF kernel, or explicitly scoped out if not yet ported (the IDF kernel does not currently bring these up the way the Arduino kernel does — see `docs/13_Kernels.md`)
- [ ] Independent sign-off — someone other than the original author flashes and confirms separately

**Gate status: not passed.** Four of eight items remain — the soak test and the independent
sign-off are the substantive ones. `kernel_tdeck_plus_arduino` stays in tree.
Documentation and the README now describe the IDF kernel as canonical and the Arduino
kernel as deprecated, which is true of *intent*; the archive step is still blocked here.

### T-Deck (`tdeck` kernel)
- [ ] Display, touch, trackball, keyboard
- [ ] LoRa SX1262 send/receive
- [ ] SD card

### CYD variants (`cyd`, `cyd_s024c`, `cyd_s028r`)
- [ ] Display (ILI9341) correct orientation per variant
- [ ] Touch (XPT2046 resistive) calibration
- [ ] SD card

### JC3248W535 (`jc3248w535`)
- [ ] Display (AXS15231B 480×320 QSPI)
- [ ] Touch

### Heltec (`heltec`)
- [~] OLED SSD1306 + oled_ui module *(built and flashed to a Heltec V3 during dp8; boot confirmed, UI surface not systematically exercised)*
- [ ] LoRa SX1262

### Waveshare 1.69" (`waveshare169`)
- [ ] Display (ST7789 240×280)
- [ ] Touch (if wired)

### M5Stack Tab5 (`tab5`) — *added after this checklist was written*
- [x] Display (ST7123 MIPI-DSI) — commit `2034f615` reached DP1 booting to the desktop, verified on hardware
- [x] Touch (integrated capacitive) — same
- [ ] `tab5_kbd` keyboard driver — driver exists and is hardware-adopted (`fb6809da`); full key coverage not confirmed
- [ ] WiFi / BT on ESP32-P4

---

## 2. Kernel

- [~] Generic core kernel (`source/kernel/core/`) boots cleanly on all non-specialized devices *(Heltec confirmed; CYD variants, JC3248W535 and Waveshare not exercised)*
- [x] Specialized kernel selection in CMake works correctly for all `kernel_<device>` directories *(source-verified; six kernel directories, all reachable from a device target)*
- [~] `kernel_tdeck_plus_arduino` — all baked-in drivers fully functional (see §1) *(was the production kernel through dp7; now deprecated in favour of the IDF path)*
- [x] `kernel_tdeck_plus_test` — test mode kernel boots and shows all input events (confirm keyboard works after BBQ20 fix)
- [x] Arduino kernels (`_arduino`, `_test`) — `Wire.setTimeOut(50)` confirmed preventing bus hangs
- [x] GT911 status register 0x814E always cleared after every read with buffer-ready bit set
- [~] GT911 power-save keepalive (write 0x00 to 0x8040 every 2 s) confirmed stable over long sessions *(implemented and stable in normal use; no long-session soak — same gap as §1)*
- [ ] Serial console "Writing to serial is timing out" bug resolved in non-Arduino kernels
- [x] `purr_kernel_panic()` triggers clean reboot with log message on all kernels *(dp5's `0b1797ee` replaced parking on a possibly-dead panic screen with auto-reboot; dp7's `1fdd1826` added the 3-state panic screen and hang breadcrumbs)*
- [x] BOARD_POWERON (GPIO 10 on T-Deck Plus) boot sequence documented and reliable

---

## 3. Catcall API

- [ ] All six catcalls have stable struct layouts — no field additions planned before 1.0
  **Not true today.** `catcall_ui_t` reached version **7** in dp8 (`list_set_items_icon`),
  having been 4 when this checklist was written. It has moved every DP cycle. Freezing it
  is a real 1.0 gate and has not happened.
- [x] `CATCALL_*_VERSION` constants are correct in all headers *(display/touch/gps 1, input 2, radio 3, ui 7 — cross-checked against the headers during the dp8 docs pass)*
- [~] `catcall_display_t` — `push_pixels`, `fill_rect`, `set_brightness` tested on all display drivers *(ST7789, SSD1306 and ST7123 exercised; ILI9341 and AXS15231B not)*
- [~] `catcall_touch_t` — `read_point`, `is_pressed` work correctly through all touch drivers *(GT911 and the Tab5 controller exercised; XPT2046 and CST816S not)*
- [x] `catcall_input_t` — `poll_event` works for BBQ20 and trackball
- [~] `catcall_radio_t` — `send`, `receive`, `rssi`, `snr` tested on SX1262 *(send path proven on `tdeck_plus` via the Meshtastic NODEINFO broadcast; receive proven by interrupt-driven RX since dp7. `rssi`/`snr` surface values but have never been checked against a known reference. SX1276 is selected by no real hardware.)*
- [ ] `catcall_gps_t` — `get_fix` returns valid struct on T-Deck Plus
- [~] `catcall_ui_t` — `create_window`, `add_label`, `add_button`, `show`, `destroy` work on both backends
  → **"both" is now eleven.** Exercised on Cupcake, Mochi, MiniWin and OLED. KittenUI is
  selected by no device. Pounce and Nougat have never been hardware-verified.
- [x] Kernel accessors (`purr_kernel_display()`, etc.) return NULL cleanly when driver not registered *(source-verified; note `input` is a list, not a slot — `purr_kernel_input_count()`/`_at()`)*
- [x] No catcall header has any IDF-specific types in its public interface (must be portable) *(source-verified during the dp8 pass on `02_Catcalls.md`)*

---

## 4. Driver Layer

### Display
- [x] `st7789` — correct on T-Deck/T-Deck Plus/Waveshare *(T-Deck Plus confirmed; Waveshare not)*
- [ ] `ili9341` — correct on all CYD variants
- [ ] `axs15231b` — correct on JC3248W535 (QSPI)
- [x] `ssd1306` — correct on Heltec OLED
- [x] `st7123` — MIPI-DSI on Tab5 *(added post-checklist; hardware-verified, `fb6809da`)*
- [x] All display drivers call hardware init from `drv_init()` (regression fixed in v0.12.1 — confirm still correct) *(source-verified)*

### Touch
- [–] ~~`gt911` — IDF i2c_master path: document as known-broken on IDF 5.3, workaround is Arduino kernel~~
  → **Superseded.** `322159c6` fixed the IDF path; it is now the canonical one for
  `tdeck_plus` and works on hardware. The item as written records a state that no longer
  exists. What remains open is the *migration gate* in §1, not the fix.
- [x] `gt911` — Wire path (Arduino kernels): fully working
- [ ] `xpt2046` — resistive touch on CYD devices
- [ ] `cst816s` — if used on any device, confirm *(used by `cyd_s024c` and `waveshare169`; neither exercised)*

### Input
- [x] `trackball` — GPIO ISR, all directions *(and typematic repeat, fixed at the driver layer in dp8)*
- [x] `bbq20` — raw Wire read (no write preamble), clean on bus
- [ ] `tab5_kbd` — Tab5 keyboard *(added post-checklist)*

### Radio
- [~] `sx1262_rl` — LoRa send/receive/RSSI on T-Deck Plus *(RadioLib-backed; the driver actually in production. Exercised end-to-end through Meshtastic/MSN; not bench-measured)*
- [ ] `sx1262` — plain SPI driver, on T-Deck *(never exercised — `tdeck` has not been on a bench)*
- [ ] `sx1276` — **selected by no real hardware.** Only `tdeck_plus_arduino` points at it, and that is a misconfiguration. Either fix that target's `device.pcat` or retire the driver.

### GPS
- [ ] `generic_nmea` — UART parse, valid fix struct returned

### Driver ABI
- [x] All driver `CMakeLists.txt` list correct REQUIRES (no missing IDF components) *(implied by clean builds across all 12 targets)*
- [x] All `.purr` driver blobs produce a valid `catcall_*_t` and call `purr_kernel_register_*()` *(source-verified)*
- [ ] `user_drivers/` auto-scan path works end-to-end (drop a driver in, bake, it loads)
- [~] Every specialized kernel consumes drivers by calling `<name>_drv_init()`/`<name>_configure()` from `source/drivers/` — no specialized kernel reimplements register-level driver logic from scratch. `kernel_tdeck_plus_arduino` is the sole grandfathered exception pending §1's migration gate.
  *(Holds. The exception is still present because the gate has not passed.)*

---

## 5. Module System

- [x] `driver_manager` — scans SPIFFS and SD, loads `.purr` blobs in correct order
- [x] `app_manager` — scans SPIFFS and SD, launches `.meow`/`.hiss`/`.kitten`/`.paws`/`.claw` apps *(five tiers now, not three)*
- [ ] `kittenui` — LVGL 8 backend: window create, label, button, text box, show, destroy
  *(implemented, but **no device selects it** — it is untested by construction. Either give
  it a target or retire it before 1.0.)*
- [x] `miniwin` — MiniWin backend: same surface, touch dispatch *(the `hal_touch.c` fix landed; MiniWin is the WinCE desktop shell on `tdeck_plus`, exercised heavily through dp5–dp8)*
- [x] `oled_ui` — text-mode UI for 128×64 OLED (Heltec)
- [x] Static module registration (`purr_register_static_modules`) works without SD present *(this is now the **only** registration path — the `.purr`-scanning description elsewhere in this checklist predates the change)*
- [x] `.purr` ABI version check — mismatched ABI version is rejected cleanly with log message
- [ ] **New:** 16 of 27 `module.pcat` files still use the legacy flat schema; the sectioned
  `[module]` form is current. Migration is a tracked, separate change.
- [ ] **New:** `systemui`, `mochi`, `tabby`, `nougat`, `pounce`, `cardstack`, `lvgldebug`,
  `meshcore`, and the `proximity*` family did not exist when this list was written. They
  are documented (`docs/03_Modules.md`, `docs/09_SystemUI.md`) but only Cupcake, Mochi,
  MiniWin, OLED and systemui have been on hardware.

---

## 6. App Layer

### System apps (all medium/large-screen devices)
- [x] `settings` — theme, brightness, SD status, reboot all functional *(plus the dp8 Customization panel and notification-privacy toggle)*
- [–] ~~`about` — OS version, KITT version, chip info, free RAM, uptime correct~~
  → **There is no `about` app.** Device and OS info live inside `settings`. The app was
  folded in; README and `docs/06_Apps.md` were corrected in the dp8 pass.
- [x] `terminal` — `ls`, `cat`, `echo`, `modules`, `reboot`, `clear` all work *(`mem`/`uptime` are not commands; the item listed them in error)*
- [x] `fileman` — SPIFFS + SD browse, text preview
- [x] `calculator` — basic arithmetic, decimal support
- [~] **Not in the original list** — `taskmgr`, `services`, `drivermgr`, `hwtest`, `msn`,
  `meshdiag`, `nearby`, `milkbar`. Twelve system apps ship today, not five. All build and
  are documented; `msn`, `hwtest`, `services` and `settings` are the ones actually
  exercised on hardware.

### Exclusives
- [ ] `magicmac` — current status documented; either working or explicitly deferred to post-1.0 *(still mid-rewrite; the honest call is to defer it)*
- [ ] `magidos` — same

### `purr_win.h` unified API
- [~] All system apps compile and run against both KittenUI and MiniWin backends without changes
  → Restate as: against **all eleven** backends. They compile against every backend and
  run correctly on Cupcake, Mochi, MiniWin and OLED. Pounce omits three optional contract
  members (`label_set_big`, `tile_grid_create`, `tile_grid_set_items`) and falls back
  rather than failing — see `pounce_plan.md`.
- [x] `purr_win_create`, `purr_win_label`, `purr_win_button`, `purr_win_textarea`, `purr_win_show`, `purr_win_destroy` all work
  *(the original item said `purr_win_input`, which has never existed — the text entry
  widget is `purr_win_textarea`, with `_append`/`_set`/`_clear`/`_get`/`_focus`/`_on_change`)*
- [ ] **New gate:** `purr_win_list_set_items_icon()` cannot be used portably — apps would
  need `lvgl.h` unconditionally, which MiniWin and Pounce builds do not have. Blocked on a
  `CONFIG_PURR_UI_LVGL` symbol. Tracked as a separate change.

---

## 7. Build Tools

### purrstrap
- [~] `build`, `flash`, `clean`, `list`, `status`, `doctor` all work
  → **`clean` does not work.** `cmd_clean` targets `source/build_<device>`; `cmd_build`
  uses `CoreOS/build_<device>`. It has silently never cleaned anything. Documented in
  `docs/07_Build_Tools.md`; the fix belongs in a code commit, not the docs pass.
- [x] `--erase` flag works correctly (full chip erase before flash)
- [x] Per-device sdkconfig chaining works for all device targets *(12 `sdkconfig_<device>` files for 12 device directories — but note the trap: `CoreOS/build_<device>/sdkconfig` must be deleted by hand when `ui =` changes, because `clean` is broken)*
- [ ] `purrstrap_ui.py` — GUI launcher functional if shipped with 1.0
- [x] Build output paths are consistent (`cattobaked/<device>/`)
- [ ] **Not in the original list:** `generate`, `verify`, `bake`, `monitor`, `add`, and the
  whole `pkg app install/remove/upgrade` family. Sixteen subcommands exist; six were
  listed. All sixteen are now documented in `docs/07_Build_Tools.md`.

### modulestrap
- [x] `build all`, `build <name>`, `list`, `clean` work
- [~] All 5 system modules build without warnings → **27 modules**, all building
- [x] All driver blobs build without warnings

### catstrap
- [x] `build all`, `build <name>`, `validate`, `sdk install`, `sdk info`, `list`, `clean` work
- [x] SDK headers (`catstrap/sdk/include/`) are generated correctly
- [x] `.meow` Lua scripts validate cleanly

### purr.py / purr.sh / purr.ps1
- [ ] Cross-platform launcher scripts are consistent and documented
- [ ] `purr.py` is the canonical entrypoint for the unified toolchain CLI

---

## 8. Documentation

**This entire section was closed by the dp8 documentation pass.** Every version number,
module, driver, device, app and build-tool subcommand across `docs/` and the root files
was re-verified against the tree.

### README.md
- [x] Version table updated *(v1.0.0-dp8; catcall versions listed individually — there is no single "Catcall API" number)*
- [x] Architecture diagram still accurate (updated for specialized kernels and the UI-backend fan-out)
- [x] Supported devices table accurate *(adds `tab5`; dev targets separated, `tdeck_plus_arduino` marked deprecated, `tdeck_plus_pounce` added)*
- [x] Build tool command reference matches actual purrstrap/modulestrap/catstrap behavior
- [x] Repo layout section reflects current directory structure
- [x] Versions table reflects actual PURR OS / KITT / ABI versions

### docs/ files
- [x] `00_Overview.md` — device list current, concepts current
- [x] `01_Architecture.md` — all 11 UI backends and the windowed / shell / framebuffer tier table *(listed 4)*
- [x] `02_Catcalls.md` — all 6 catcalls at current struct fields *(was pinned at `CATCALL_UI_VERSION 4`, and admitted its own listing was stale)*
- [x] `03_Modules.md` — all **27** modules *(documented 13)*, `module.pcat` schema in both live forms
- [x] `04_Devices.md` — all 12 devices; pin tables complete
- [x] `05_Drivers.md` — all drivers documented; GT911/IDF 5.3 history noted with its resolution
- [x] `06_Apps.md` — 12 system apps + exclusives; all five tiers
- [x] `07_Build_Tools.md` — all 16 purrstrap subcommands including the `pkg` family; `cmd_clean` bug recorded
- [x] `08_Exclusives.md` — verified, no stale references
- [x] **`09_SystemUI.md` — new.** The System UI module: two styles, `purr_systemui_host_t`, lock screen, notification privacy, how a backend hosts it
- [x] `10_ModuleLoading.md` — verified against `purr_register_static_modules()` and purrstrap's `_generate_glue()`
- [x] `11_KittenUI.md` — verified; now states plainly that no device selects it
- [x] `12_AppAPI.md` — `purr_win.h` reference complete, including optional members and the NULL-fallback rule
- [x] `13_Kernels.md` — all six specialized kernels; when to use, how to write
- [x] `14_Driverstrap.md` — verified, no stale references

### CHANGELOG.md
- [–] ~~v0.13.0 entry written~~ / ~~v0.14.0 entry written before 1.0 tag~~
  → Superseded. The project went v0.13 → v1.0.0-dp; there was no v0.14.0.
- [x] dp3–dp7 backfilled, explicitly labelled **reconstructed**, with the caveat that
  hardware-verification status is not recoverable from commit messages
- [x] dp8 entry written contemporaneously
- [ ] 1.0.0 entry written at tag time

### PURR_OS_0.13.0.md
- [ ] Architectural decisions captured; mark which items are done vs deferred *(not touched by the dp8 pass — it is a historical design record, and rewriting it would destroy that)*

---

## 9. Code Quality

- [x] No `TODO` / `FIXME` / `HACK` comments left in any first-party file
  *117 occurrences remain across 27 files, but **all** are in vendored third-party code
  (`source/lib/lib_radiolib`, `lib_nanopb`, `lib_mesh_pb`, `lib_st7123`,
  `modules/meshcore/vendor/`) or the exclusives (`magicmac`, `magidos`). Zero in the
  kernel, drivers, modules or system apps. Vendored code should not be edited.*
- [ ] No dead kernel directories (e.g. stale test variants not in any device.pcat)
  *`kernel_tab5_m5bsp_legacy` is named "legacy"; `kernel_tdeck_plus_arduino` is deprecated
  but still referenced by a live target. Both need a decision before 1.0.*
- [x] `CoreOS/sdkconfig.old` — delete or gitignore *(absent, and gitignored)*
- [ ] `archive/` — confirm nothing in there is needed by the active build
- [ ] `PURR-OS-0.11/` — confirm it is reference-only and excluded from builds
- [ ] All `.bin` files in `releases/` and `archive/releases_legacy/` are correct and not accidentally modified
  *`CatReleases/` is now the release-image folder. The older `releases/` tree is pending
  archival or removal — a decision, not a check.*
- [ ] `purr.ps1` / `purr.py` / `purr.sh` — consistent feature parity across platforms

---

## 10. Repo Hygiene

- [x] `.gitignore` covers all build output (`CoreOS/build_*/`, `cattobaked/`, `*.bin` in wrong places)
- [x] No secrets, keys, or credentials anywhere in tree
- [~] All submodule or managed_component pins are explicit versions (no `@latest`)
  *`lvgl/lvgl` is bounded (`>=8.3.0,<9.0.0`) — acceptable. `espressif/arduino-esp32` is
  `>=3.0.0` with no upper bound, which is the next item and is still open.*
- [ ] `idf_component.yml` — `espressif/arduino-esp32 >= 3.0.0` — confirm this resolves deterministically
  *Unbounded. Pin it or add an upper bound before 1.0. Note the manifest already documents
  why esp32p4 is excluded from this dependency.*
- [ ] Branch is clean — no accidental staged files
- [ ] All commits on `main` have coherent messages (squash any WIP commits before 1.0 tag)
- [ ] GitHub releases accurate and drafted before the 1.0 tag

---

## 11. Known Issues to Resolve Before 1.0

| Issue | Status | Target |
|-------|--------|--------|
| MiniWin does not respond to touch | **Resolved.** The stale `>>6` digitiser-scale hack in `hal_touch.c` was removed; MiniWin has since served as the WinCE desktop shell on `tdeck_plus` through dp5–dp8 | done |
| GT911 over IDF i2c_master (`ESP_ERR_INVALID_STATE`) | **Resolved** by `322159c6`. The IDF kernel is now canonical for `tdeck_plus` | done |
| BBQ20 keyboard in test kernel | **Resolved** — confirmed on hardware | done |
| Serial console timeout on non-Arduino kernels | Open | 1.0 |
| `purrstrap clean` cleans nothing (wrong path) | Open — documented, not yet fixed | 1.0 |
| `catcall_ui_t` has changed every DP cycle (now **v8** — dp8 added the menu primitive) | Open — the contract must be frozen before 1.0 | 1.0 |
| `catcall_display_t` moved v1 → **v3** in dp8 (async push, then a `stride` parameter) | Open — same freeze applies; both additions are optional members with NULL fallback, so no driver was forced to change | 1.0 |
| Screen tearing on `tdeck_plus` | **Won't fix — hardware.** No TE pin is broken out (verified against LilyGo's own `utilities.h`), and pushing 40% fewer pixels was measured to change nothing. Recorded as a requirement for the next board | — |
| Crash-guard strikes could permanently brick boot | **Resolved** in dp8 — strikes now clear when the firmware's ELF hash changes. Previously five crashes during development halted boot until someone knew to erase NVS at `0x9000` | done |
| Temporary render/memory instrumentation compiled into releases | Open — `[perf]`, `[frames]`, `[mem]` log lines still ship in dp8 | 1.0 |
| KittenUI is selected by no device, so is untested | Open — give it a target or retire it | 1.0 |
| Portable list icons blocked on `CONFIG_PURR_UI_LVGL` | Open — tracked as a separate change | 1.0 |
| 16 legacy-schema `module.pcat` files | Open — tracked as a separate change | 1.0 |
| `arduino-esp32` dependency has no upper bound | Open | 1.0 |
| `tdeck_plus_arduino` selects `sx1276`, but the board carries an SX1262 | Open — found during this audit; fix the `device.pcat` or retire the target | 1.0 |
| Pounce and Nougat have never been hardware-verified | Open | 1.0 or scope out |
| KittenUI XP desktop shell — touch dispatch | Superseded — the XP shell is not a current target | — |
| MagicMac / MagiDOS rewrite | In progress | post-1.0 |

---

## 12. 1.0 Tag Criteria (all must be ✅)

- [ ] Every device in the supported table boots to a working UI
  *4 of 12 targets confirmed on hardware (`tdeck_plus`, `tdeck_plus_test`, `heltec`,
  `tab5`). **This is the single largest gap between here and 1.0**, and no amount of
  further code work substitutes for putting the other eight on a bench.*
- [ ] All six catcalls functional on all devices that have the hardware *(radio and GPS are the weakest — neither has been bench-measured on any device)*
- [~] `purr_win.h` API works on all UI backends *(4 of 11 hardware-verified)*
- [~] All system apps functional on all medium/large-screen devices *(functional on `tdeck_plus`; unverified elsewhere)*
- [ ] `purrstrap build/flash/clean` works for all production targets *(`clean` is broken)*
- [x] README and all docs/ files are accurate and complete *(closed by the dp8 pass)*
- [~] CHANGELOG has entries for every release *(dp3–dp8 present; the 1.0.0 entry is written at tag time)*
- [ ] Repo is clean, no WIP commits, no accidental binaries staged
- [ ] GitHub release created with merged firmware for all production devices

---

## Summary of this audit

Rough shape of where 1.0 actually stands:

| Area | State |
|---|---|
| Documentation (§8) | **Done.** Closed entirely by the dp8 pass |
| Build tooling (§7) | Nearly done — one real bug (`clean`) and the GUI launcher |
| Kernel + module system (§2, §5) | Solid on the two exercised platforms |
| Repo hygiene (§9, §10) | Mostly mechanical; a few decisions (archive `releases/`, retire dead kernels, pin `arduino-esp32`) |
| **Hardware validation (§1)** | **The bottleneck.** 4 of 12 targets exercised; radio and GPS untested everywhere |
| **API freeze (§3)** | **Not started.** `catcall_ui_t` moved in every DP cycle including dp8 |

The two items that gate 1.0 in a way nothing else does are the hardware sweep and the
catcall freeze. Everything else on this list is tractable work with a known shape.

---

*Last audited: v1.0.0-dp8 / 2026-07-24. Audit performed by Claude Opus 5 in agentic/auto mode.*
