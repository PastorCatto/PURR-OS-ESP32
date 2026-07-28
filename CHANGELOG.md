# CHANGELOG

> **Note:** This changelog covers PURR OS v0.12.0 and later — the modular architecture era.
> History up to v0.11.0 is preserved in [archive/CHANGELOG_0.11.md](archive/CHANGELOG_0.11.md).
> v0.11.0 was the final release of the monolithic build system. Everything from here is new.

> **Reconstructed entries (dp3 – dp7).** No changelog was written during the dp3–dp7
> cycle. Those five entries below were reconstructed after the fact from git history and
> the `baked_at` timestamps in `CatReleases/DP*/manifest.json`, because the release
> artifacts were all committed in one bulk commit and so cannot be dated from commit
> order alone. They are accurate about *what changed*; they are **not** a record of what
> was hardware-verified — commit messages do not preserve that, and no claim of on-device
> verification should be read into them unless the commit itself stated one.
> dp2 and earlier, and dp8, were written contemporaneously and carry no such caveat.

---

## v1.0.0-dp8 — 2026-07-24 … 2026-07-27

> Shipped in two passes. The first (2026-07-24) is the mobile UI generation described
> below. The second (2026-07-25 → 27) is a performance and stability pass driven entirely
> by on-device measurement — every figure in it was measured on T-Deck Plus hardware, and
> most of them replaced an earlier figure that turned out to be wrong.

### Pass 2 — performance and stability (2026-07-25 → 27)

**Rendering**
- `-O2` instead of `-Og`, worth ~3.3x on frame time — the single biggest win, and in no
  version of the original plan.
- Fixed the black rectangular blocks: the SPI transfer-length register is 18 bits, so one
  transaction caps at 32,768 bytes and a full 153,600-byte frame was silently rejected,
  leaving stale GRAM. A PSRAM source also forced a failing per-transaction bounce buffer.
- **Asynchronous flush with real double buffering** — `catcall_display` v1 → v3, adding an
  optional `push_pixels_async` / `flush_done_cb` pair (chunked, ping-pong staged, with a
  `stride` so a sub-rectangle of a larger buffer can be sent without flattening it).
  Drivers that skip it keep the blocking path unchanged.
- **Off-screen composition** — bands are assembled into a full-screen mirror and the
  finished frame is sent once, so a redraw no longer paints top-to-bottom over ~150ms.
  The obvious alternative, a full-screen draw buffer, was measured and rejected: 153,600
  bytes cannot sit in a 32KB data cache and mean frame time went 23ms → 44ms.
- **Shadows identified as 40-50% of scroll frame time.** Nothing in the UI sets
  `shadow_width` — every shadow comes from LVGL's default theme, which is why the existing
  effects toggle had never made a measurable difference. It reaches them now.
- **Idle repaint eliminated.** The system UI rewrote the whole status row five times a
  second regardless of change, and rebuilt the notification and task boxes object-by-object
  each time. Measured 75-81ms every 200ms at idle; now ~1 slow frame in 55s.
- Scrolling: **6.2 → 9.7-10.1 fps**, mean frame **128ms → 60ms**.

**Speed Demon** (renamed from "game mode")
- One app takes the whole machine, reclaiming **12.8-15.7KB of internal DRAM**. Apps opt in
  with one line — `.speed_demon = 1` — and `app_manager` owns both entry and restore, so an
  app cannot forget the exit.
- Went from never completing a round trip to **four consecutive cycles with memory flat**.
  Six faults fixed: a UI catcall never released on unload; an SPI bus race on entry (the bus
  lock is per *device*, not per task); a PSRAM stack in the health watchdog; a relaunch that
  was silently inert; 8KB leaked per unload (`xTaskCreateWithCaps` paired with plain
  `vTaskDelete`); and `mesh_persist_task` deleted mid-SD-write.
- Documented in `docs/15_SpeedDemon.md`. Pre-linked `.claw` apps only — Lua tiers cannot use
  it, because the flag lives in a module header they do not have and Speed Demon unloads
  `lua_runtime` itself.

**Contracts and UI**
- **Menu primitive** — `catcall_ui` v8 adds `purr_win_menu_*`, a real grouped-list contract
  with sections, values and headers, so apps stop hand-rolling button grids. Mochi renders
  it as an iOS-style grouped table; backends without a native implementation fall back to
  label + list. Settings' category picker and General page migrated.
- Cupcake and Mochi now **share one display path** (`modules/common/purr_lv_flush.h`). Every
  fix above had landed in Mochi only, and Cupcake — a near-copy of the same file — silently
  had none of them. Bringing it level exposed two of its own bugs: a draw buffer 1.6x the
  data cache, and a second draw buffer allocated unconditionally that could never help while
  its flush was synchronous.
- Half-height notification shade with a grab handle; overpull drops to the lock screen.
  Duplicate Control Center removed. Flat buttons in row layouts. Springboard grid derived
  from panel size instead of a hard-coded 4x2.

**Kernel**
- **Crash strikes reset when the firmware's ELF hash changes.** The counter lives in NVS and
  halts boot at 5; during this pass it reached 13/5 and bricked boot three times, each
  needing a manual `esptool erase_region 0x9000 0x6000`. It also caused a full
  misdiagnosis, where a boot loop after a config change looked like that change's fault.
- Module tasks now unsubscribe from the task watchdog before deletion — four did not, and it
  panicked ~4s into every Speed Demon session.
- Bounded module unloads; NVS settings load before the UI is built.

**Instrumentation** *(worth recording, because it changed conclusions rather than adding logs)*
- The frame metric was wrong twice: first a censored sample that only logged slow frames,
  then one counting no-op loop iterations and overstating fps ~20x.
- Added **BUSY-fps**, measured from the gap between consecutive rendered frames — "how fast
  is it while dragging", which `RENDER-fps` (frames ÷ whole window, idle included) does not
  answer. Caveat: identical builds measured 9.7 and 7.7 depending on what was scrolled;
  compare ranges, not single runs.

### Pass 1 — mobile UI generation (2026-07-24)


### Summary
Mobile UI generation. Two new LVGL 8 backends — **Mochi** (iOS-style squircle
springboard) and **Tabby** (keyboard-first type-to-filter launcher) — plus the extraction
of Cupcake's status bar, quick-settings drawer, nav bar and lock screen into a standalone
**`systemui` module** with two swappable style implementations (Android and iOS) behind a
single host-hook contract. Cupcake becomes a launcher again. The icon pack moves to
Ionicons and becomes system-wide rather than launcher-private, and the wallpaper is
compiled into the firmware as a C array instead of living in SPIFFS. Closes with a full
documentation pass bringing `docs/` back in line with the tree.

### Added
- **`systemui` module** (`source/modules/systemui/`) — status bar, quick-settings drawer,
  nav bar, notification shade and lock screen, extracted from `cupcake_ui.c`. Two style
  implementations selected at build time: `systemui_android.c` (`CONFIG_PURR_SYSTEMUI_STYLE_ANDROID`)
  and `systemui_ios.c` (`CONFIG_PURR_SYSTEMUI_STYLE_IOS`). See `docs/09_SystemUI.md`.
- **`purr_systemui_host_t`** — the host-hook table a UI backend fills in to host System UI.
  No `lv_obj_t*` crosses the boundary: hooks are `width`, `height`, `icon_for_app`,
  `tint_color`, `hide_drawer`, `hide_foreground_windows`, `last_activity_ms`, `wallpaper`
  and `suppress_navbar`. Proven by two independent hosts (Cupcake and Mochi).
- **Mochi UI backend** (`source/modules/mochi/`) — iOS-style springboard: 4-column
  squircle grid matching the 4-slot dock for 1:1 trackball cursor mapping, page swiping,
  a fixed 9-colour saturated icon palette, and a wide short home indicator.
- **Tabby UI backend** (`source/modules/tabby/`) — keyboard-first launcher; type to filter,
  trackball/arrow keys to select, touch supported.
- **Compiled-in wallpaper** — `convert_wallpaper.py` gained `--c-array` / `--symbol`,
  emitting an `LV_IMG_CF_TRUE_COLOR` `lv_img_dsc_t` (`purr_wallpaper`) linked into the
  firmware rather than staged into SPIFFS. Used by both the springboard and the lock screen.
- **Notification privacy** — lock screen can show a bare count ("You have *N* notifications")
  and reveal the list on swipe-up. Backed by `purr_kernel_lock_hide_notifications()`
  (defaults on), toggled from Settings → Customization, honoured by both System UI styles.
- **`purr_kernel_notify_remove(int idx)`** — removes one notification and compacts the ring.
- **`list_set_items_icon`** in `catcall_ui_t` (`CATCALL_UI_VERSION` 6 → 7) — optional; a
  backend that leaves it NULL falls back to the plain `list_set_items` path.
- **`docs/09_SystemUI.md`** — new document covering the module, both styles, the host
  contract and the lock screen / notification model.

### Changed
- **Icons are system-wide.** The icon pipeline (`source/assets/icons/convert_icons.py`) is
  now pack-agnostic (`PACKS` dict, `ACTIVE_PACK`), the active pack is **Ionicons**, and the
  launcher owns the set on behalf of the whole system instead of keeping it private — so
  apps and System UI resolve the same icons.
- **Cupcake is a launcher.** `cupcake_ui.c` was split; everything chrome-related moved to
  `systemui`, and Cupcake now hosts it through `purr_systemui_host_t` like any other backend.
- **`convert_icons.py` runs on Windows** — added an svglib + reportlab render path
  (`_render_svglib()`); cairosvg is not usable on this toolchain.
- **Documentation pass to DP8** — every version number, module, driver, device, app and
  build-tool subcommand across `docs/`, `README.md` and `00_Overview.md` re-verified
  against the tree. Docs had been deliberately allowed to drift through the DP cycle.

### Fixed
- **Trackball key repeat** — repeat is now generated in the driver
  (`source/drivers/input/trackball/trackball.c`) with proper typematic behaviour: a leading
  edge, a 500 ms delay, then a 140 ms interval (40 ms for motion). Previously the repeat
  behaviour came from the wrong layer; this is the bug present since 0.13.
- **Trackball axis tie-break** — `ay >= ax` biased every diagonal towards vertical;
  changed to a strict `>`.
- **`PURR_KERNEL_VERSION` drift** — sat at `1.0.0-dp7` for the whole dp8 cycle, so the
  About page reported a version no baked artifact matched. It is duplicated between
  `purr_kernel.h` and `purrstrap.py`; both are now dp8.
- **Status bar contrast and overlap** on the springboard; notification card background
  height; page swiping, now driven by explicit pointer tracking rather than LVGL scroll.

### Known issues
- `purrstrap clean <device>` targets `source/build_<device>`; the real build directory is
  `CoreOS/build_<device>`. It has silently never cleaned anything. Documented in
  `docs/07_Build_Tools.md`, not fixed here (docs and code commits are kept separate).
- Portable list icons are blocked on a `CONFIG_PURR_UI_LVGL` symbol — apps cannot include
  `lvgl.h` unconditionally, since MiniWin and Pounce builds have no LVGL.
- 16 `module.pcat` files still use the legacy flat schema; migration to the sectioned
  `[module]` form is deferred to its own change.
- **Screen tearing during motion is a hardware limit on T-Deck Plus** — LilyGo does not
  break out the ST7789's TE pin, and scanline readback is impractical on a bus shared with
  the radio. Pushing fewer pixels was measured and does not help: a 40% reduction changed
  nothing, because any asynchronous write crossing the scan produces a seam regardless of
  size. Recorded as a requirement for the next board, not an open task.
- **MagicMac does not build** — it has no `app.pcat`, so it is never scanned into the
  firmware. `docs/08_Exclusives.md` implied otherwise and has been corrected.
- **MagiDOS is the host shell only** — the INT 21h shim, `C:` mapping and bundled DOS app
  are not in this release.
- **Temporary instrumentation is still compiled in** (`[perf]` flush counters, `[frames]`,
  `[mem]` unload/restore accounting). Left in deliberately: it is warn-level logging rather
  than behaviour, and stripping it immediately before a release without re-verifying on
  hardware was the riskier option. First task of the next cycle.
- `homebase`'s task-leak fix is committed but **not hardware-verified** — confirming it
  needs a Speed Demon round trip, which requires launching an app from the touchscreen.
- Milkbar icons and the Tab5 grid remain unverified on hardware.

---

## v1.0.0-dp7 — 2026-07-20 *(reconstructed)*

### Summary
M5Stack Tab5 (ESP32-P4) reaches hardware-verified DP1, and the T-Deck Plus shared-SPI2
hang — radio and display contending for one bus — is closed. Meshtastic RX becomes
interrupt-driven and the full MSN feature set is restored on top of it. The proximity
family grows into multi-device pairing with a remote app manager and a home-base MSN relay.
Longest window of the DP cycle (07-14 → 07-20) and the largest.

### Added
- **M5Stack Tab5 device** (`2034f615`, `fb6809da`) — ESP32-P4 target reaching DP1, booting
  to the desktop and verified on hardware against a pinned IDF 5.3.5.
- **ST7123 MIPI-DSI display + touch drivers and `tab5_kbd`** (`75eab957`, `1a70e1b3`) —
  vendored `esp_lcd` panel and touch drivers replacing the earlier espp dependency.
- **esp32p4 target support in purrstrap** (`e4240ba0`), and build fixes making every
  component compile and link on esp32p4 (`807aa231`, `d998179e`).
- **Multi-device pairing, remote app manager, home-base MSN relay** (`a200e653`).
- **Milk Bottle demo app + big-font label capability** (`0ca7f6e6`), later folded into
  Milkbar's own app list (`a197f98a`).
- **Portable tile-grid Home screen for MSN** (`34d83017`) and a tile-grid category picker
  in Settings, plus NimBLE enabled on `tdeck_plus` (`0c5d7ea6`).
- **3-state panic screen and UI hang breadcrumbs** (`1fdd1826`).

### Changed
- **Cupcake lock screen redesigned** around a minimalist centred layout (`199f276d`).
- **Tab5 UI switched to Cupcake** at the upstream author's request (`a1b3e8af`), after
  `4c61b1d0` unblocked Cupcake on native kernels and wide displays.
- **Meshtastic RX is interrupt-driven** (`7a7dfccc`), replacing polling.

### Fixed
- **Shared-SPI2-bus hang on T-Deck Plus** (`23658e42`) — radio and display on one bus.
- **Module load-order bugs that disabled the UI on boot** (`21f97c9e`).
- **MiniWin wide-display overflow, widget client-area sizing, physical-keyboard Enter**
  (`3917af69`, `8472dd14`).
- **Hang recovery** — `0b1797ee` changed a hang from parking on a possibly-dead panic
  screen to an auto-reboot.
- **`purrstrap`** now regenerates `components_manifest.cmake` on every build (`91a7e32b`).

### Reverted
- `718713c7` reverted `6471460b` (per-node hops/battery data) and `81c434a9`
  (deferred window/list teardown, Cupcake-only MSN icon rows). The MSN feature set was
  restored later in the window by `7a7dfccc`.

---

## v1.0.0-dp6 — 2026-07-14 *(reconstructed)*

### Summary
**Repackage only — no code changes.** DP6 was baked 35 minutes after DP5, and its commit
(`0b946b7a`) states it packaged `CatReleases/DP6` from the existing `cattobaked/` batch.
No feature list is given here because there is none to give.

### Note
`a0f8846e` bumped `PURROS_VERSION` to `v1.0.0-dp7` at 13:30 — *before* DP6 was packaged at
13:44. The version string in the DP6 artifact should be read with that in mind. This is the
first instance of the version-drift pattern that recurs through dp7 and dp8.

---

## v1.0.0-dp5 — 2026-07-14 *(reconstructed)*

### Summary
Mesh goes protocol-agnostic. MeshChat is replaced by **MSN**, which talks to whichever mesh
backend is active, and **MeshCore** joins Meshtastic as a supported backend with a live
switch that no longer requires a reboot. The ESP-NOW **proximity** family arrives. First
ESP32-P4 bring-up work lands (Tab5, Nougat/LVGL v9), then is deliberately reverted to
MiniWin before the bake.

### Added
- **MeshCore backend + mesh backend preference infrastructure** (`6aa8763b`).
- **MSN** (`6ad8ad33`) — protocol-agnostic replacement for MeshChat, with a backend chooser.
- **ESP-NOW device discovery + companion pairing** (`eecdb760`), auto-enabled per
  `device.pcat` along with `msn` and `nearby` (`4578c059`).
- **Runtime module enable / disable / restart**, exposed through Services and Terminal
  (`f77c3bfd`).
- **Memory watchdog + PSRAM offload** (`fb39a3a1`).
- **Nougat backend (LVGL v9)** (`85d64708`) and native MIPI-DSI driver work for Tab5
  (`c6b81897`, `29d8d9cd`) — experimental; Tab5 was reverted to MiniWin for the DP5 bake
  (`6ee85509`) because LVGL v9 was not yet proven stable.
- **Z-ordered WinCE desktop windows and a dedicated lock overlay** (`9dd5db25`).

### Fixed
- **Cupcake UI hangs caused by radio-driver CPU starvation** (`afb1ddd2`).
- **ADC battery calibration** (`038f9f94`) — curve-fitting vs. line-fitting is now selected
  per chip.
- **Stale-pixel redraw bugs** in the WinCE desktop (`9dd5db25`).
- `ec1d6447` held `PURR_KERNEL_VERSION` at dp5 until the next packaged release — an
  early, deliberate attempt to manage the same version-drift problem.

---

## v1.0.0-dp4 — 2026-07-10 *(reconstructed)*

### Summary
Meshtastic messaging works end-to-end, with multi-channel rooms and real PKI direct-message
crypto. Cupcake gains a lock screen and idle screen-off. MeshChat is rebranded toward MSN.

### Added
- **Cupcake lock screen + idle screen-off**, trackball navigation (`ce0e220a`).
- **Multi-channel Meshtastic rooms** and battery voltage reporting (`19c8bbcc`).
- **Real PKI direct-message crypto**, node and room persistence (`ef316b4c`).

### Fixed
- **Meshtastic messaging end-to-end** (`19c8bbcc`) — the headline fix of this release.
- **App-launch render stalls** (`ce0e220a`).

---

## v1.0.0-dp3 — 2026-07-09 *(reconstructed)*

### Summary
T-Deck Plus moves to the MiniWin/WinCE UI and gains crash-loop protection. Meshtastic wire
compatibility becomes real rather than approximate, and plain-ESP32 (non-S3) builds are
unblocked with a stripped-down profile for smaller devices.

### Added
- **MiniWin/WinCE UI on `tdeck_plus`** (`d7a3ba40`), with a crash-loop guard and panic
  screens.
- **Vendored RadioLib** and real Meshtastic wire compatibility (`d0548eac`).
- **Stripped-down build profile for non-T-Deck devices** (`f913ae23`).

### Fixed
- **Close-button freeze** on MiniWin windows (`d7a3ba40`).
- **MiniWin close / layout / keyboard bugs** (`d0548eac`).
- **PSRAM-stack NVS crash** introduced by redirecting `MW_ASSERT` into the crash guard
  (`9e153cad`).
- **Bluetooth on `tdeck_plus`** migrated to lazy-init NimBLE; Lua module registration
  fixed; KITT bumped to 0.11.1 (`2cf30032`).
- **Plain-ESP32 builds unblocked** (`f913ae23`).

---

## v1.0.0-dp2 — 2026-07-07

### Summary
New `.hiss` app tier: a privileged Lua script, launched through the exact same VM/task path as `.meow` (`lua_runtime` + `app_manager.c`'s `launch_meow()`), with three extra Lua namespaces — `kitt.*`, `radio.*`, `gps.*` — registered only for that tier. Trust is extension-only, same as every other tier. An `unsigned` `.hiss` script (no `-- purr-sig:` tag, or an explicitly `unsigned` one) is refused at launch unless Developer Mode is enabled in Settings — a new off-by-default, NVS-persisted toggle; signed scripts always run regardless. Also fixes a three-way documented inconsistency (`README.md`, `app_manager.h`'s tier comment, `catstrap.py`'s `SDK_API`) that incorrectly claimed `.meow` itself had `kitt.*` access — it never did (confirmed by `docs/06_Apps.md` and the real `lua_runtime.c`); `.hiss` is where that access actually lives now. Alongside the new tier: two crash fixes (Terminal/MeshChat/Settings/Fileman not accepting keypresses, `.meow` apps crashing on launch) that were root-caused earlier this session but hadn't been changelogged yet.

### Added
- **`.hiss` app tier** — `APP_TIER_HISS` in `app_manager.h`/`app_manager.c`, dispatched through the same `launch_meow()` path as `.meow`.
- **`radio.*` Lua API** (`.hiss` only) — `send`/`receive`/`available`/`rssi`/`snr`, thin wrappers over `catcall_radio_t`.
- **`gps.*` Lua API** (`.hiss` only) — `fix()`, wraps `catcall_gps_t::get_fix()`.
- **`kitt.*` Lua API** (`.hiss` only) — `modules()`, lists loaded kernel modules (same data `terminal.c`'s `modules` command surfaces).
- **`-- purr-sig:` signature tag** (`.hiss` only) — self-declared, honor-system provenance marker (`unsigned`/`dev-signed`/`trusted-signed`/`dev-approved`). `catstrap validate`/`build` read and print it (build-time, informational). `catstrap/catstrap.py`.
- **Developer Mode toggle** (`settings.c`, new "Developer" section) — off by default, persisted to NVS under `purr_settings`/`dev_mode`, synced to a new kernel-level flag (`purr_kernel_dev_mode_enabled()`/`purr_kernel_set_dev_mode()` in `purr_kernel.h`/`.c`, same getter/setter shape as `sd_available`/`lora_available`). `app_manager.c`'s `launch_meow()` now scans a `.hiss` script's own source for the `purr-sig` tag at launch time (`scan_purr_sig()`) and rejects it (`APP_STATE_ERROR`) when `unsigned` and Developer Mode is off — the only place this tag actually gates anything; `kitt.*`/`radio.*`/`gps.*` registration still depends solely on the `.hiss` extension, unchanged.
- **`sdcard_apps/hisstest.hiss`** — demo script exercising `kitt.modules()`, `radio.*`, `gps.*`.
- Full toolchain support for the new tier: `catstrap.py` (discovery, build, clean, validate, SDK API surface), `catstrap_ui.py` (interactive menu), `purrstrap.py` (SPIFFS staging via `_find_purr_blob()`, and the `pkg app install/remove/upgrade` hot-load subsystem, generalized from `.meow`-only to `.meow`/`.hiss`).

### Fixed
- **Physical keyboard input on Cupcake** — `cupcake_hal.c` never registered an `LV_INDEV_TYPE_KEYPAD` LVGL input device, so BBQ20 keystrokes never reached any widget under the Cupcake UI backend at all. Added a keypad indev + `lv_group_t` bridge (`cupcake_hal.c`/`cupcake_win.c`) polling every registered `catcall_input_t` for `KEY_DOWN` events.
- **On-screen keyboard hidden behind its own window** — `purr_win_keyboard_show()` was called before `purr_win_show()` in `terminal.c`, `settings.c`, `fileman.c`, and `meshchat.c` (×2); `win_show()` raises the window to the foreground, so it painted over the keyboard it had just shown. Reordered at all five call sites.
- **`.meow` apps crash on launch** — `app_manager.c`'s `launch_meow()` ran the script's task on a PSRAM-backed stack, but loading the script (`fopen`/`fread`) briefly disables the flash cache — the same crash class (`esp_task_stack_is_sane_cache_disabled()`) already documented and fixed for `settings`/`fileman`. Fixed by having `launch_meow()` preload the script into a PSRAM *buffer* (not stack) on its own caller's already-safe stack, before `meow_task` exists; `meow_task` itself never touches flash now and runs on an ordinary PSRAM stack.

### Verified this session
- `catstrap list` / `sdk info` / `validate sdcard_apps/hisstest.hiss` (correctly prints `signature: dev-approved`) / a direct `build_app()` exercise of the `.hiss` packaging branch — all correct, no regressions against existing `.meow` apps.
- `purrstrap build tdeck_plus` — full clean firmware build (fresh `CoreOS/build_tdeck_plus/`, ~2679 objects) via a locally-installed ESP-IDF v5.3.5, ending in a merged flashable `cattobaked/tdeck_plus/PURR_OS_tdeck_plus.bin`. Zero compile errors; `app_manager.c`, `lua_runtime.c` (incl. the new `radio.*`/`gps.*`/`kitt.*` bindings), `cupcake_hal.c`, and `cupcake_win.c` all compiled and linked cleanly. One pre-existing, unrelated `CoreOS/build_tdeck_plus/` cache from a prior cross-machine (Linux) session had to be deleted first — not a regression from this change.
- A second incremental `purrstrap build tdeck_plus` after adding the Developer Mode toggle/gate (`purr_kernel.c`/`.h`, `settings.c`, `app_manager.c`'s `scan_purr_sig()`) — 77-step incremental build, zero errors, `purr_os.elf` relinked and remerged cleanly.
- Not verified: on-device flash/run of a `.hiss` script (signed or unsigned, with Developer Mode on/off), or the keypad/meow-crash/MeshChat fixes against real hardware — a clean build proves the code is correct, not that it behaves correctly at runtime on a physical T-Deck Plus.

---

## v1.0.0-dp — 2026-07-03

### Summary
Pre-1.0 UI contract and build-system pass. Adds a flat selectable-list widget to `catcall_ui_t` (bumping the contract to version 2) and implements it on both KittenUI and MiniWin. Fixes MiniWin's row/col layout, which previously stacked labels and buttons on top of each other via two independent dead cursors, plus hardcoded 320×240 window/textarea sizing that broke on non-standard panels like jc3248w535's 480×320. Replaces fileman's Prev/Next/Open button workaround with a real list. Unifies per-device `sdkconfig_<device>` generation: these 10 files were hand-maintained duplicates of values already declared in `device.pcat` (flash size, PSRAM, UI backend) — `purrstrap generate` now derives them automatically, with a small hand-maintained `.overrides` file per device for the handful of quirks (panel mirroring, WinCE shell flag) that have no pcat equivalent.

### Added
- **List widget in the UI contract** (`catcall_ui.h`/`purr_win.h`) — `list_create/set_items/clear/get_selected/set_selected/cb`, plus `PURR_EVENT_SELECTED`/`PURR_EVENT_ACTIVATED` events. `CATCALL_UI_VERSION` 1→2.
- **MiniWin list widget** (`miniwin_win.c`) — wraps the existing `ui_list_box` primitive; trackball selection reuses the existing `miniwin_cursor.c` synthetic-touch path.
- **KittenUI list widget** (`kittenui_win.c`) — wraps LVGL's native `lv_list`.
- **`purrstrap generate [<device>] [--check]`** — regenerates `CoreOS/sdkconfig_<device>` from `device.pcat`; `--check` diffs without writing and exits nonzero on drift (CI-friendly).
- **`device.pcat` `[device] kernel_type = "native" | "arduino"`** — replaces `CoreOS/main/CMakeLists.txt`'s device-name substring matching for Arduino-vs-IDF driver `REQUIRES`.
- **`CoreOS/sdkconfig_<device>.overrides`** (optional, hand-maintained) for `tdeck`, `tdeck_plus_arduino`, `tdeck_plus_test` — carries the handful of hardware quirks (LVGL color-swap, touch-flip, WinCE shell flag, non-derivable UI backend) not representable in `device.pcat`.

### Changed
- **MiniWin backend rewrite** (`miniwin_win.c`): unified row/col layout cursor (was two independent per-widget-type cursors that caused overlapping labels/buttons on every app using `purr_win_row()`); window and textarea size now read the real display resolution instead of hardcoded 320×240/320×200; `label_align` now actually aligns (space-padding, since the underlying control has no reposition/justify API); `textarea_focus` now draws a visible focus border; textarea storage is per-widget heap-allocated instead of a flat static 128×512 array.
- **`fileman.c`** — real `purr_win_list()` replaces the Prev/Next/Open button workaround for browsing SPIFFS/SD.
- **`terminal.c`** — `modules` command now lists real loaded modules via `purr_kernel_module_count()`/`purr_kernel_module_at()` instead of a dead stub.
- **`settings.c` / `kittenui_module.c`** — theme is now written/read only via the `"purr_settings"` NVS namespace; removed a hardcoded dual-write to a `"kittenui"` namespace that was wrong on any non-KittenUI device.
- **`tdeck_plus`** — UI backend switched from `blackpurr` (shell tier) to `miniwin` (windowed tier), so system apps render on this device; a deliberate divergence from the shell-tier default documented in `docs/01_Architecture.md`, not a bug fix.
- **`CoreOS/sdkconfig_<device>`** (all 10) — now auto-generated from `device.pcat`; header marks them `DO NOT EDIT BY HAND`.
- **`CoreOS/main/CMakeLists.txt`** — Arduino-vs-native `REQUIRES` now branches on `PURR_KERNEL_TYPE` (set by purrstrap from `device.pcat`) instead of guessing from the device name string.

### Fixed
- Four `app.pcat` files (`calculator`, `settings`, `about`, `terminal`) had an unterminated `idf_requires` string.

### Verified this session
- `purrstrap generate --check` — zero drift across all 10 devices after migration.
- Full `idf.py build` + merge succeeded for `cyd` (KittenUI, native) and `tdeck_plus` (MiniWin, native, touch+trackball+keyboard) via a locally-installed ESP-IDF v5.3.5.
- Not build-tested this session: `cyd_s024c`, `cyd_s028r`, `heltec`, `jc3248w535`, `tdeck`, `tdeck_plus_arduino`, `tdeck_plus_test`, `waveshare169` — no hardware-in-the-loop pass either; visual/interaction correctness (list widget usability, MiniWin layout on real panels) still needs a device in hand.

---

## v0.13.1 — 2026-06-15

### Summary
Replaces MiniWin as the T-Deck Plus UI with **BlackPURR** — a zero-LVGL text-mode shell built on direct `fill_rect`/`push_pixels` catcall primitives and a 6×8 bitmap font. Eliminates the MiniWin touch calibration dependency entirely: GT911 outputs screen-native coordinates (0–319, 0–239) which are used directly for hit-testing. Fixes a critical Wire I2C race condition between the BBQ20 poll task and GT911 reads. Foundation for the planned LVGL-on-demand app layer.

### Added
- **BlackPURR text-mode shell** (`source/modules/blackpurr/`) — rewritten from scratch, no LVGL dependency
  - 4×3 app grid drawn with bitmap font and `fill_rect`; selection highlight in orange
  - Status bar: brand, uptime clock, page indicator
  - Bottom hint bar: selected app name + key hint
  - BBQ20 keyboard: Enter to launch, letter keys to type-jump to app by first character
  - Trackball: delta accumulation with threshold → grid navigation
  - Touch: direct GT911 screen-native coordinate hit-test, no calibration required
  - Task loop at ~60 Hz; 1 s status tick; 8 KB stack (vs 32 KB for LVGL path)
- **Wire mutex** in `kernel_tdeck_plus_arduino` — `s_wire_mutex` (FreeRTOS `SemaphoreHandle_t`) guards all Wire transactions; eliminates race between `bbq20_poll_task` and GT911 reads that caused touch to fail after first read

### Changed
- `tdeck_plus_arduino` device: `ui = "blackpurr"` (was `miniwin`), flash priority updated
- `sdkconfig_tdeck_plus_arduino`: removed `CONFIG_PURR_UI_BACKEND_MINIWIN`, `CONFIG_LV_COLOR_16_SWAP`, `CONFIG_PURR_TOUCH_FLIP_X`; added `CONFIG_PURR_UI_BACKEND_BLACKPURR`
- `blackpurr/CMakeLists.txt`: removed LVGL and icon asset dependencies; requires only `esp_common driver freertos nvs_flash esp_timer`

### Removed
- `blackpurr_hal.c` — LVGL display/touch bridge (no longer needed)
- `blackpurr_win.c` — LVGL catcall_ui registration (no longer needed)

---

## v0.13.0 — 2026-06-15

### Summary
Architecture stabilization release. Introduces specialized kernels — per-device `kernel_<device>/` directories that replace the generic core for hardware that cannot be reached through the standard IDF driver stack. T-Deck Plus is fully functional: display, touch, trackball, and keyboard all confirmed working. Adds an input test mode kernel for hardware bring-up. Resolves IDF 5.3 i2c_master regression for GT911 via Arduino Wire. Extensive documentation update covering all new concepts.

### Added

#### Specialized kernel system
- **`kernel_<device>/` selection in CMake** — `CoreOS/main/CMakeLists.txt` now checks for `source/kernel/kernel_${PURR_DEVICE}/` and globs `*.c` + `*.cpp`; falls back to generic `core/` if not found
- **`kernel_arduino/kernel_arduino.h`** — shared static-inline helpers for all Arduino-backed kernels (`arduino_kernel_nvs_init`, `arduino_kernel_spiffs_init`)
- **`kernel_tdeck_plus_arduino/kernel_atdp_boot.cpp`** — production kernel for T-Deck Plus using Arduino Wire for all I2C; bypasses IDF 5.3 i2c_master regression completely
  - GT911 touch found at 0x5D via Wire probe
  - GT911 status register 0x814E always cleared after buffer-ready reads
  - GT911 power-save keepalive: write `0x00` to 0x8040 every 2 s
  - BBQ20 keyboard polled via plain `Wire.requestFrom(0x55, 1)` — no write preamble
  - `Wire.setTimeOut(50)` prevents BBQ20 NACK from hanging the shared I2C bus
  - Serial console via `Serial.begin(115200)` — avoids `uart_driver_install` conflict
- **`kernel_tdeck_plus_test/kernel_tdp_test.cpp`** — input test mode kernel for T-Deck Plus
  - Boots to 3-panel visualizer: touch coordinates / trackball events / keyboard keypresses
  - Built-in 6×8 bitmap font — no external font dependency
  - All events also printed over serial at 115200 baud
  - GT911 keepalive every 2 s, `Wire.setTimeOut(50)` on shared bus

#### New device targets
- **`tdeck_plus_arduino`** — `source/devices/tdeck_plus_arduino/device.pcat` + `CoreOS/sdkconfig_tdeck_plus_arduino`
- **`tdeck_plus_test`** — `source/devices/tdeck_plus_test/device.pcat` + `CoreOS/sdkconfig_tdeck_plus_test`
- Both sdkconfigs include `CONFIG_FREERTOS_HZ=1000` and all Arduino component settings

#### Documentation
- **`docs/13_Kernels.md`** — new: complete specialized kernel reference (when to use, how CMake selects, Arduino requirements, all existing kernels with boot sequence and code examples)
- **`docs/00_Overview.md`** — updated: v0.13.0 version table, all 8 devices + 2 dev targets, specialized kernel concept, new key concepts table
- **`docs/01_Architecture.md`** — updated: two-kernel-path diagram, specialized kernel selection logic, static vs dynamic modules, IDF 5.3 known issue, source file map with all kernel dirs
- **`docs/04_Devices.md`** — updated: full T-Deck Plus detail (BOARD_POWERON sequence, GT911 protocol, BBQ20 protocol), all 8 production devices, dev/debug target section, complete T-Deck Plus GPIO table
- **`docs/05_Drivers.md`** — updated: GT911 IDF 5.3 regression documented with workaround, BBQ20 bus interaction note, ssd1306 and bbq20 driver entries added, compat matrix updated
- **`docs/07_Build_Tools.md`** — merged with former `09_BuildTools.md`; added `--erase` flag docs, `monitor` command, two-layer CLI/UI architecture, `.catt` tier, full `cattobaked/` layout
- **`README.md`** — updated: v0.13.0, specialized kernel column in device table, all 10 build targets, updated docs index (13 entries, no duplicate 09)
- **Deleted `docs/09_BuildTools.md`** — content merged into `07_Build_Tools.md`

### Fixed

#### T-Deck Plus touch (GT911)
- **Root cause:** IDF 5.3 `i2c_master_probe()` returns `ESP_ERR_INVALID_STATE` instead of NACK — breaks GT911 discovery on `kernel_tdeck_plus`
- **Fix:** `kernel_tdeck_plus_arduino` uses Arduino Wire; GT911 found at 0x5D immediately
- **GT911 sleep lockout** — fixed with periodic keepalive write to 0x8040
- **GT911 status register** — always write `0x00` to 0x814E when buffer-ready bit set (was missing on some code paths)

#### T-Deck Plus keyboard (BBQ20)
- **Root cause:** `Wire.beginTransmission(0x55)` + `Wire.endTransmission(false)` before `requestFrom` confuses the RP2040 bridge
- **Fix:** Changed to plain `Wire.requestFrom(0x55, 1)` — returns key byte directly, 0x00 when idle
- **Bus hang** — fixed with `Wire.setTimeOut(50)` preventing BBQ20 NACK from blocking GT911 read timing

#### Build system
- **CMake kernel selection** — now globs `.c` AND `.cpp` from specialized kernel dirs (previously `.c` only, breaking C++ Arduino kernels)
- **Arduino IDF component name** — `espressif__arduino-esp32` (not `arduino`) in CMake REQUIRES

### Versions
- PURR OS: v0.13.0
- KITT: v0.9.2

---

## v0.12.1 — 2026-06-13

### Summary
Hotfix release: `CoreOS/` IDF project was absent from the repo and all driver/module source
files had a cascade of compiler errors preventing any firmware from being built. This patch
adds the full CoreOS project shell, fixes every compiler error across all drivers and modules,
and produces the first complete full-device-set bake (8 targets, all flashable merged images).

### Fixed

#### Build system
- **CoreOS/ missing** — created `CoreOS/CMakeLists.txt` and `CoreOS/main/CMakeLists.txt`
  wrapping `source/kernel/core/` as the IDF main component; pulls in all registered
  modules/drivers/apps via `cattobaked/components_manifest.cmake`
- **Per-device partition tables** — `partitions_4mb.csv`, `partitions_8mb.csv`,
  `partitions_16mb.csv`; correct SPIFFS offsets (0x290000 / 0x690000 / 0xD90000)
- **Per-device sdkconfig** — `sdkconfig.defaults` (base) + 8 device override files;
  purrstrap now chains `sdkconfig.defaults;sdkconfig_<device>` via `SDKCONFIG_DEFAULTS`
- **spiffs_offset in device.pcat** — heltec (0x690000), tdeck/tdeck_plus/jc3248w535
  (0xD90000) now correctly declared; 4 MB devices continue using the 0x290000 default
- **purrstrap firmware binary name** — `CoreOS.bin` → `purr_os.bin` (IDF project name)

#### MiniWin module
- **CMakeLists** — added all 75 MiniWin source files and include directories (previously
  only `miniwin_module.c` was listed, causing every MiniWin header to fail to resolve)
- **miniwin_win.c** — complete rewrite using the actual MiniWin API (`mw_handle_t`,
  `mw_add_window`, `mw_ui_label_add_new`, `mw_ui_button_add_new`, `mw_ui_text_box_add_new`,
  `mw_set_window_visible`, `mw_remove_window/control`); replaced a hallucinated `MwAdd*`/
  `MwCreate*` API layer that had never existed in the library
- **miniwin_config.h** — added missing colour constants (`MW_TITLE_BAR_COLOUR_*`,
  `MW_CONTROL_UP/DOWN/DISABLED_COLOUR`) from the MiniWin template; MiniWin core was
  referencing these at compile time
- **hal_timer.h** — added `#include <stdint.h>` (`uint32_t` was undeclared)
- **hal_touch.c** — cast `int16_t*` → `uint16_t*` for `catcall_touch_t.read_point` call
- **miniwin_module.c** — added missing `hal_touch.h` and `hal_init.h` includes

#### KittenUI module
- **kittenui_win.c** — fixed LVGL 8 API: `lv_win_create` requires a second `header_height`
  argument; removed non-existent `lv_layout_t` type (replaced with `LV_LAYOUT_FLEX` direct);
  added `#include "esp_heap_caps.h"` for `heap_caps_malloc` / `MALLOC_CAP_DEFAULT`
- **kittenui_hal.c** — cast `int16_t*` → `uint16_t*` for `catcall_touch_t.read_point`

#### Display drivers
- **ili9341, st7789, ssd1306** — moved kernel/catcall `#include` statements to the top of
  each file; they were placed after first use of `display_config_t` / `catcall_display_t`,
  causing "unknown type name" errors
- **axs15231b** — added `#include "esp_check.h"` for `ESP_RETURN_ON_ERROR`; removed
  duplicate `spi_clock_hz` field (renamed to `pclk_hz` in IDF v5); added
  `purr_kernel_register_display` call that was missing from `drv_init`; added `esp_lcd` to
  CMakeLists REQUIRES

#### Touch drivers (gt911, xpt2046, cst816s)
- Added `#include "esp_check.h"` for `ESP_RETURN_ON_ERROR`
- Added forward declaration `static const catcall_touch_t s_catcall` before the init
  function (definition was after first use)
- Replaced stack-allocated compound literal `&(catcall_touch_t){...}` passed to
  `purr_kernel_register_touch` with `&s_catcall` (compound literal goes out of scope
  immediately — would have been a dangling pointer at runtime)

#### Other drivers
- **generic_nmea** — added forward declaration to fix `s_catcall` ordering
- **trackball, sx1262, sx1276 CMakeLists** — added `esp_timer` to REQUIRES
  (`esp_timer.h` was not resolvable without it)

### Versions
- PURR OS: v0.12.1
- KITT: v0.9.1

---

## v0.12.0 — 2026-06-13

### Summary
Complete ground-up architecture redesign and the first full release of the PURR OS modular era.
Every driver, UI framework, and app is an isolated precompiled module. The kernel spine has zero
hardware knowledge — it just loads modules and hands off. KITT v0.9.0 ships alongside with the
same catcall-based kernel interface translation toolkit.

This release also introduces the **Unified UI API** (`purr_win.h`), a base set of five system apps
that work identically on all UI backends, and per-device `[radio]` and `[apps]` configuration so
builds are explicitly declared rather than assembled at runtime.

### New in v0.12.0

#### catcall_ui_t — Unified UI catcall
- New sixth catcall: `catcall_ui_t` registered via `purr_kernel_register_ui()`, accessed via `purr_kernel_ui()`
- Covers windows, labels, buttons, textareas, layout containers, on-screen keyboard
- `source/kernel/catcalls/catcall_ui.h` — full struct definition
- `source/kernel/catcalls/purr_win.h` — thin inline dispatch header for apps; all `purr_win_*()` helpers null-check before calling through the registered backend

#### KittenUI LVGL backend
- `source/modules/kittenui/kittenui_win.c` — implements `catcall_ui_t` using LVGL 8.x
- Handle pool: `s_wins[16]` / `s_wids[128]` → `lv_obj_t*`
- Button/textarea callbacks via `cb_ctx_t` trampoline structs
- LVGL keyboard attached to textarea on `kb_show`
- Called from `kittenui_module.c` init via `kittenui_win_register()`

#### MiniWin backend
- `source/modules/miniwin/miniwin_win.c` — implements `catcall_ui_t` using MiniWin WM
- Handle pool: `win_slot_t s_wins[16]` / `wid_slot_t s_wids[128]`
- Vertical cursor stacking for label/button layout
- `kb_show` is a no-op — physical keyboard handled via `catcall_input` automatically
- Called from `miniwin_module.c` init via `miniwin_win_register()`

#### Five built-in system apps
All apps use `purr_win.h` exclusively — zero direct LVGL or MiniWin calls.
They run identically on KittenUI and MiniWin without any source changes.

| App | Tier | Description |
|-----|------|-------------|
| `settings` | `.claw` | Theme switcher (WCE/Luna/Dark, persisted to NVS "kittenui" namespace), display brightness via `catcall_display->set_brightness`, SD card status, system reboot |
| `about` | `.claw` | PURR OS + KITT version, chip model/revision/cores, flash size, free RAM, uptime (live-updating every 5 s via background FreeRTOS task), all active catcall driver names |
| `terminal` | `.claw` | Interactive shell: `ls [path]`, `cat <path>`, `echo <text>`, `modules`, `mem`, `uptime`, `clear`, `reboot`, `help`; 2 KB output scroll buffer |
| `fileman` | `.claw` | Browse SPIFFS + SD card; Prev/Next cursor selection; Open enters directories or previews text files; binary-safe (non-printable bytes replaced with `.`) |
| `calculator` | `.paws` | Basic arithmetic: `+`, `-`, `*`, `/`, decimal point, ERR:DIV0 guard; 5-row button layout via `purr_win_row()` |

#### Device configs — [radio] and [apps] sections
Every `device.pcat` now has:
- `[radio]` — declares `wifi`, `bt`, `lora` capabilities; purrstrap emits `CONFIG_PURR_WIFI`, `CONFIG_PURR_BT`, `CONFIG_PURR_LORA`, `CONFIG_PURR_LORA_DRIVER` into the glue layer
- `[apps]` — per-app `true/false` flags; controls which apps are baked into the SPIFFS flash image
- Medium/large-screen devices (cyd*, tdeck*, jc3248w535) get all five apps bundled by default
- Small-screen devices (heltec, waveshare169) ship without GUI apps

#### purrstrap — full pipeline wired
- `_generate_glue()` now emits radio flags and `/flash/apps` + `/sdcard/apps` path constants
- `build_flash_image()` now calls catstrap automatically (was manual step before)
- `_find_purr_blob()` handles `apps/<name>` slugs: resolves `.claw`/`.paws`/`.meow` output files and `.meta.json` registrations
- App blob staging into `spiffs_staging/apps/` alongside modules and drivers

#### New GPS driver
- `source/drivers/gps/generic_nmea/generic_nmea.c` — full UART NMEA 0183 parser
- Background FreeRTOS task reads UART line by line; parses `$GPRMC` (lat/lon/speed/validity) and `$GPGGA` (satellites/hdop/altitude)
- Mutex-protected `gps_fix_t`; thread-safe `get_fix()` reads
- PURR_PRIORITY_OPTIONAL — kernel does not panic if GPS is absent

#### New OLED UI module
- `source/modules/oled_ui/oled_ui_module.c` — text-mode UI for SSD1306 128x64
- Embedded 6x8 bitmap font (95 printable ASCII chars)
- Ring-buffer log display (5 visible lines); `oled_ui_log(const char *)` public API for other modules
- Row 0 = title bar, row 1 = status, row 2 = separator, rows 3-7 = scrolling log
- Redraws every 500 ms via FreeRTOS task

#### app_manager — fully wired
- `launch_meow()` — looks up `lua_runtime` module via `purr_kernel_get_module()`, stores pending path, spawns `meow_task()`
- `launch_native()` — looks up app name via `purr_kernel_get_module()`; reports "not pre-linked: recompile firmware" if absent; spawns `native_task()` (16 KB stack for .claw, 8 KB for .paws)
- `app_manager_stop()` — calls `mod->deinit()`, waits up to 2 s via semaphore, force-deletes task via `vTaskDelete()` if still alive
- `app_manager_open_launcher()` — logs app list with name/tier/state
- `app_task_ctx_t` struct tracks task handle + done semaphore per app slot

#### catstrap — IDF component model
- `build_app()` now generates IDF component `CMakeLists.txt` fragments with correct include paths to `source/kernel/` headers
- Writes `.meta.json` with status "registered" (was "placeholder" before)
- .claw apps get `source/kernel/core/` + `source/kernel/catcalls/` in their include dirs

#### Documentation refresh (docs/)
- `02_Catcalls.md` — added `catcall_ui_t` section with full struct, functions, backend table, app usage guide; added Glue Layer section documenting `CONFIG_PURR_WIFI`/`BT`/`LORA` defines; added per-catcall driver tables
- `04_Devices.md` — added all 8 devices (was 4); added `[radio]`, `[apps]`, `[flash]` format; added screen size classification table; expanded pin reference with LoRa busy pin, MADCTL override, SD keys
- `06_Apps.md` — rewrote with `purr_win.h` API reference, full function signatures for all widget types; added built-in system apps table; updated .meow kitt.* section with `radio_rssi()`, GPS fix fields
- `07_Build_Tools.md` — complete rewrite; documents full purrstrap pipeline (7-step sequence), modulestrap IDF component model, catstrap SDK headers including `catcall_ui.h` and `purr_win.h`
- `12_AppAPI.md` — added settings/about/fileman to built-in apps table; added file manager layout diagram

### Bug fixes
- `oled_ui_module.c` — removed duplicate kernel header includes (double `../../kernel/` and `../../../source/kernel/` paths would fail compilation)
- `modulestrap` — removed broken per-module IDF mini-project invocation (ESP-IDF cannot build isolated component mini-projects); replaced with component registration model
- `cyd_s024c` — backlight pin was GPIO 21 in old config; corrected to GPIO 27 (verified)
- `cyd_s028r` — missing MADCTL=0x40 portrait flip; added `display_madctl` pin key

### Breaking changes from v0.11.0
- Old `CoreOS/`, `drivers/`, `ui/`, `devices/`, `SDK/`, `CattoHID/`, `Userland/`, `sdcard_apps/` are archived to `PURR-OS-0.11/` — not built by default
- `purrstrap.py` (monolithic CLI) replaced by three separate tools: `purrstrap/`, `modulestrap/`, `catstrap/`
- Build output moves from `baked/<device>/` to `cattobaked/<device|modules|drivers|apps>/`

### Versions
- PURR OS: v0.12.0
- KITT: v0.9.0
- `.purr` ABI version: 1
- Catcall API version: 1 (+ catcall_ui added as slot 6)

---

*v0.11.0 and earlier: see [archive/CHANGELOG_0.11.md](archive/CHANGELOG_0.11.md)*
