# PURR OS — Session Handoff (Windows → Linux)

**Written:** 2026-09-03
**Repo:** PastorCatto/PURR-OS-ESP32 (`origin` = GitHub)
**Branch:** `feature/rtc-ota-users-oobe`
**Primary target device:** `tdeck_plus` (ESP32-S3, T-Deck Plus hardware, 3.2" ST7789 + GT911 touch + trackball + LoRa SX1262 + GPS, real PSRAM)
**Secondary device touched this session:** `heltec` (ESP32-S3, no PSRAM, 128x64 OLED, LoRa SX1262)
**Toolchain:** ESP-IDF 5.3.5

This doc exists because the user is moving development to Linux. It captures
real state — not just narrative — so nothing needs re-discovering. Read the
**"Before you leave Windows"** section first; it's the one thing here that's
time-sensitive.

---

## 0. Before you leave Windows — unpushed work

As of this writing:

```
## feature/rtc-ota-users-oobe...origin/feature/rtc-ota-users-oobe [ahead 2]
```

Two full commits (`fe35db51` shelve-Lumia, `06b14184` the big checkpoint —
mesh backend work, MiniWin/WinCE rewrite, Mochi fixes, DRAM budget recovery,
macOS login screen, hardware-probe kernel) **exist only in this local working
tree and have never been pushed to GitHub.**

On top of that, there is real **uncommitted** work sitting dirty right now:

```
 M CoreOS/sdkconfig_heltec
 M CoreOS/sdkconfig_tdeck_plus
 M CoreOS/sdkconfig_tdeck_plus_pounce
 M purrstrap/purrstrap.py                    (+92/-… : --profile support for `bake`)
 M source/apps/system/milkbar/milkbar_app.c  (stale-comment fix only)
 M source/devices/heltec/device.pcat         (mesh -> reticulum-only)
 M source/devices/tdeck_plus/device.pcat     (stray CRLF/whitespace touch)
 M source/modules/app_manager/app_manager.h  (stale-comment fix only)
 M source/modules/mochi/CMakeLists.txt       (app-download deps)
 M source/modules/mochi/mochi_springboard.c  (+169: app-download dialog)
 M source/modules/systemui/systemui_ios.c    (+56: logout row in Control Center)
```

**If you clone this repo fresh on Linux without pushing first, all of the
above is gone** — both unpushed commits and the uncommitted diff. Either:

- have this session commit + push everything before you switch, or
- `git bundle`/copy the working tree over some other way, or
- accept the loss and redo the app-download/logout/--profile/heltec-mesh work
  (all described in detail below, so it's redoable from this doc alone if it
  comes to that).

Untracked, not source, safe to leave behind or `.gitignore`: `doom/` (a
stray `.wad` test asset), `CatReleases/ota_sdcard_test/`, `bake_msn.log`
(a leftover log from Jul 14), `wallpaper.webp`. Untracked, IS real release
output worth keeping if you want it: `CatReleases/DP9/`, `CatReleases/DP9.zip`,
`releases/v1.0.0-dp9/`.

---

## 1. What PURR OS is

A custom OS for ESP32/ESP32-S3/ESP32-P4 boards (T-Deck Plus, Heltec LoRa32,
CYD variants, Tab5, Waveshare, JC3248, etc.), written in C against ESP-IDF,
with an LVGL-based `purr_win`-abstracted UI layer, a Lua app runtime, a
static module-registry kernel, and its own `.claw`/`.paws` app-packaging
format built by `purrstrap.py`/`catstrap`. `device.pcat` files (one per
board under `source/devices/<slug>/`) declare drivers/modules/apps/pins/
flash-staging per device; `purrstrap.py` turns that into generated glue
code + sdkconfig + a merged flashable image.

## 2. Environment setup — needs redoing on Linux

The Windows activation recipe (memory: `esp-idf-activation-recipe`) will
**not** transfer as-is — it exists because this specific Windows install is
non-standard (missing constraints file, wrong default python, wrong venv
path). On Linux the standard ESP-IDF flow is usually just:

```bash
. $HOME/esp/esp-idf/export.sh    # or wherever v5.3.5 lands
```

but **verify it actually compiles**, don't trust exit code 0 alone —
`purrstrap.py build <device>` degrades gracefully when IDF tooling is
missing: it still builds the SPIFFS `flash.bin`, prints a warning, and
**exits 0 anyway**. The tell is `idf.py build failed` / "kernel spine not
built" in the output, or just check `cattobaked/<device>/build.json` for
`"firmware_bin": "not built"` after any build you care about. This bit me
hard once already on Windows (see `esp-idf-activation-recipe` memory) — same
risk class exists on a fresh Linux setup with any tool-path assumption.

Also carry over: **stale `sdkconfig` trap**. `CoreOS/build_<device>/sdkconfig`
only gets regenerated from `device.pcat`-derived Kconfig values (drivers,
mesh backend, radio flags, etc.) the *first* time it's created.
`SDKCONFIG_DEFAULTS` seeds it; it never re-applies. Editing a device.pcat
value that maps to a `CONFIG_*` symbol and rebuilding without first deleting
`CoreOS/build_<device>/sdkconfig` silently keeps the old value — confirmed
multiple times this session (most recently: heltec's mesh backend switch
needed this exact deletion).

## 3. Full arc of this session's work (chronological themes)

Everything below happened across one very long continuous session on this
branch. Grouped by theme, not strictly ordered:

**Remote pairing / login / server stack** (earliest work, all committed
before the checkpoint): X25519 ECDH shared secret in the pairing handshake;
remote-login primitives in `user_mgr` (salt/hash/verify, `REMOTE` account
type); pairing Phase B/C RPC (password gate, key registration,
challenge-response reconnect); Remote OOBE push + Heltec auto-login/approval
screens; Server Manager wire protocol + client app (`server_mgr`/
`server_manager`); `app_manager` remote-mode desktop with local/remote/
hybrid app placement; `milkbar`'s Connection → Dashboard → remote-desktop
flow (replacing a bespoke remote UI); Settings' Users tab (add/remove
accounts, per-account offline-access toggle); `systemui` remote-account
identity handling + user picker + real unlock-gate wiring; `proximity`
channel-hop discovery + capability beacon bits; Cheetah remote-desktop
bypass and window fixes; an `admin` flag on `user_mgr` driving milkbar's
dashboard-vs-desktop routing decision.

**Mochi springboard boot-order bug**: `mochi_springboard_init()` was
calling `recompute_pages()`/`render_all()` **before**
`purr_systemui_init(&s_systemui_host)` — but it's `purr_systemui_init()`
that transitively fires `purr_systemui_boot_login_check()` →
`app_manager_notify_unlocked()` on the no-password auto-login path.
App Manager reported an empty registry ("0 apps") until that ordering was
fixed. Real bug, real fix, done.

**DRAM/PSRAM budget crisis**: a live link-time overflow + a memory-watchdog
boot-loop led to a full pass moving passive/row-buffer/registry static
arrays into PSRAM via `EXT_RAM_BSS_ATTR` (from `esp_attr.h`) across
`settings.c`, `fileman.c`, `diagnostics.c`, `msn_relay.c`,
`driver_manager.c`, `magidos_app.c`, `mochi_win.c`, `purr_kernel.c`,
`server_manager_app.c`. Explicitly **not** applied to real task stacks
(settings/fileman/milkbar in `app_manager.c`, mochi's own render task,
`lua_runtime.c`'s I/O worker, `purr_crash_guard.c`'s worker) — those must
stay internal DRAM because PSRAM access breaks while flash cache is
disabled during NVS/flash writes. Also: `msn.c`'s chat-log buffers went
from a static `[MAX_CHATS][CHAT_LOG_LEN]` array to lazily
`heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`'d per-chat pointers.
Two sdkconfig knobs were tested on real hardware:
`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` (lowered 16384→512, safe) and
`CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP` (tested alone, **definitively
reproduces task-creation failures** — pairing/mesh/homebase/proximity —
reverted, documented in `sdkconfig_tdeck_plus.overrides`'s own comments).

**Meshtastic/MSN/meshcore removal from `tdeck_plus`**: per an explicit "I
don't want Mischtastic" decision — Reticulum gets its own chat app
(`reticulum_app`), MSN/meshtastic/meshcore are not tied to it.
`tdeck_plus/device.pcat`: `mesh = ""` (was `"meshtastic"`), `msn_relay = ""`,
`[apps] msn = false` (explicit `false`, not deletion — `apply_radio_
companion_defaults()`'s `setdefault()` would silently re-add it otherwise
since `mochi` is in `PURR_WIN_UI_BACKENDS`). `meshtastic_module.c` itself
**stays compiled in** everywhere — it's an unavoidable transitive
`REQUIRES` of systemui/lua_runtime (lock-screen node-count line, Lua's
`kitt.*`/`radio.*` bindings) — but with `mesh=""` it links as its documented
`#else` no-op-stub branch: every `mesh_manager_*()` call becomes a real,
linkable no-op. This is a deliberate, established pattern in this codebase
— "linkable no-op stub, never a fake feature."

**A Windows-Phone/"Lumia" springboard rewrite** was built in full (tile
grid, apps list, quick-settings panel, new lock screen — replicating
fbiego's Lumia-ESP32 UI language at 320×240) and verified working on real
hardware, then **explicitly rolled back** per user decision ("the lumia
shell needs to be shelved"). The code wasn't deleted — it's preserved at
`source/modules/mochi/mochi_springboard_lumia.c.shelved` (non-`.c`
extension so it's excluded from `CMakeLists.txt` SRCS), with a header
explaining how to restore it. `mochi_springboard.c` itself was reverted via
`git checkout` back to the original iOS-style squircle grid. If picked back
up, the full design plan still exists at
`C:\Users\PastorCatto\.claude\plans\enumerated-sauteeing-hamming.md`
(not Linux-portable — copy its contents over if this matters later).

**App-download + logout feature** (most recent code work, still
uncommitted): `app_manager_remote_download()` and its install-dialog UI
existed only in the now-dead `cheetah_home.c` (no device ships `ui=cheetah`
anymore). Ported into `mochi_springboard.c` nearly verbatim under a
`MOCHI_HAS_APP_DOWNLOAD` guard (off for `CONFIG_IDF_TARGET_ESP32P4`, radio-
less targets) — `s_install_win`/`personal_app_exists()`/
`install_download_task()`/`open_install_dialog()`/etc., plus a gating check
at the top of `launch_app()`. `CMakeLists.txt` picked up
`app_manager_remote proximity user_mgr claw_loader` in
`INCLUDE_DIRS`/`REQUIRES` to support it. Separately, `systemui_ios.c`'s
Control Center panel (`refresh_ctrl()`) got an always-present account row
(username + red "Log Out" button) reusing `systemui_xp.c`'s exact logout
recipe (`app_manager_clear_remote(); user_mgr_logout();
purr_systemui_return_home(); purr_systemui_show_login(s_host);`).
**Compiles/links/boots clean on real hardware; the actual interactive
UI behavior (does tapping the button really work) was never confirmed** —
no touch/keyboard injection capability exists in this environment. Worth an
actual hands-on check.

**Developer Preview 9 packaging + `purrstrap.py` release tooling work**
(this and the immediately preceding session): added `--device` (repeatable)
and `--profile` filters to the `bake` subcommand, threaded through
`cmd_bake()`'s synthetic `_Args` class into per-device `cmd_build()` calls.
Fixed three Windows-only UTF-8 mojibake bugs (`open(path, "w")` without
`encoding="utf-8"` defaults to the locale codepage — corrupted em-dashes in
generated READMEs/manifests). Made both the `releases/v.../` copy step and
the `--dp` → `CatReleases/DP<N>/` copy step profile-aware (merged image is
named `PURR_OS_<device>_<profile>.bin` under a profile, and the old code
looked for the un-suffixed name).

## 4. Real bugs found this session, NOT yet fixed

**`cmd_bake()`'s status reporting is unreliable.** Per-device `"status":
"ok"` is derived only from `"flash.bin" in copied` — the SPIFFS image,
which builds *before* the kernel/firmware link step. A device whose kernel
build fails to link still shows `"ok"` (confirmed live: heltec and
`tdeck_plus_pounce` both failed to link under a `--profile minimal` bake of
all 14 devices, both still reported `"status": "ok"` in
`releases/v1.0.0-dp9/manifest.json`). Compounding it: `cmd_build()` only
clears the stale **merged** binary before building
(`PURR_OS_<out_name>.bin`), never stale `firmware.bin`/`bootloader.bin`/
`partition-table.bin` — so a failed rebuild's copy step picks up **old
leftover files from a previous unrelated build** and ships them looking
like fresh output. Real fix needed in `cmd_build()`/`cmd_bake()`: clear all
stale kernel-spine artifacts up front, and derive bake status from whether
the actual merged/firmware binary exists, not just SPIFFS staging.

**`tdeck_plus_pounce` is currently in that broken, misrepresented state.**
Its `kernel_tdp_boot.c` unconditionally launches straight into the
Diagnostics app's Mesh section as its whole boot behavior (it's a
Meshtastic hardware-debug board) — but `--profile minimal` blanket-clears
every `apps.*` key including `apps.diagnostics`, so the symbol it calls
(`diagnostics_open_mesh`) isn't linkable. `CatReleases/DP9/
tdeck_plus_pounce/` and `releases/v1.0.0-dp9/tdeck_plus_pounce/` both
currently contain **stale Aug-26 binaries**, not real minimal-profile
output — this was surfaced to the user and, at last check, intentionally
left unfixed/unaddressed (scope was narrowed to "just heltec" instead).
**Do not ship DP9 as-is if `tdeck_plus_pounce` needs to be in it.**

**`heltec`'s minimal-profile glue-generation bug** (same bake run): the
battery-override codegen in `_generate_glue()` (`purrstrap.py` around line
985) gates on `cfg.get("pins.battery_adc_channel", "")` instead of
`cfg.get("drivers.battery", "")`. `--profile minimal` clears
`drivers.battery` (driver component not compiled in) but never touches the
pins key, so the glue layer still emitted a call into a driver that no
longer exists → link failure. **`heltec` was subsequently rebuilt with the
*default* (non-minimal) profile instead and that build succeeded cleanly**
— but the underlying glue-generator bug is still present in
`purrstrap.py` and will bite any other battery-equipped device that tries
`--profile minimal`.

## 5. Currently active / most recent change: heltec → reticulum-only

Last concrete action this session: `heltec/device.pcat` `[modules] mesh`
changed from `"meshtastic"` to `""`, mirroring the exact treatment
`tdeck_plus` already got — reticulum becomes the only active mesh backend,
`meshtastic_module.c` still compiles as its documented no-op stub, the
orphaned `[flash] meshtastic = 2` staging line was removed. Stale
`CoreOS/build_heltec/sdkconfig` was deleted and heltec was rebuilt clean
with the **default** profile (real `PURR_OS_heltec.bin`, `firmware_bin` no
longer `"not built"`). `releases/v1.0.0-dp9/heltec/` and
`CatReleases/DP9/heltec/` were both updated with the fresh output
(replacing the stale Aug-31 files that were there), both `manifest.json`
copies were hand-corrected to match, and `CatReleases/DP9.zip` was
regenerated and spot-checked (zip contains the real 5.9MB merged image).

**This has not been flashed/boot-verified on real heltec hardware** — only
build/link success was confirmed, same "compiles clean ≠ verified working"
caveat noted throughout this session.

## 6. Pending / open tasks

- Decide whether to fix `tdeck_plus_pounce` (either exclude it from
  `--profile minimal` bakes entirely, or make its boot code tolerate a
  missing diagnostics app) before DP9 is considered done.
- Fix `cmd_bake()`'s stale-artifact-copy and false-"ok"-status bugs
  (section 4) — affects the trustworthiness of every future bake, not just
  this release.
- Fix the glue-generator battery-gating bug (section 4) so
  `--profile minimal` is actually safe on battery-equipped devices.
- Hands-on verification (real touch input) of the app-download dialog and
  the new Control Center logout button — both compile/boot clean but were
  never interactively exercised.
- The Lumia UI plan is shelved, not abandoned — full design doc at
  `enumerated-sauteeing-hamming.md` if it resurfaces (see section 3).

## 7. Pinned idea: PURR OS's own Linux/*nix rewrite

Separate from the user's own move to Linux as a dev machine — the user
floated rewriting **PURR OS itself** to behave more like Linux/*nix
(process model, permissions, packaging). Explicitly asked for pushback,
got it, then said to pin it and move on — not started, no scope decided.

Pushback on record (full detail in the Claude memory file
`purr-os-nix-rewrite-idea.md`, worth re-reading if this comes back):
ESP32-S3 has no MMU, so real hardware-enforced process isolation is not
achievable at all, not just hard; internal DRAM is already the tightest
resource on this hardware (this session's own overflow/boot-loop history is
the receipt); a ground-up rewrite risks re-discovering a long list of
hard-won hardware-specific correctness fixes already baked into the current
code (SPI bus lock ordering, watchdog unsubscribe-before-delete sequencing,
which allocator calls are safe from which task context, why certain
buffers can't move to PSRAM). The open question, never answered: what
specifically about Linux/*nix behavior is the actual itch — sandboxing,
shell/dev workflow, permissions model, app packaging? That determines
whether this is a small incremental project (the codebase already has
POSIX-ish paths, a real terminal, and `.claw` packaging that already rhymes
with a package manager) or something structurally unreachable here.

## 8. Where things are on disk right now

- `releases/v1.0.0-dp9/` — lightweight per-device build record (flash.bin/
  build.json/firmware.bin/merged bin), `manifest.json` at its root.
- `CatReleases/DP9/` + `CatReleases/DP9.zip` — the full hand-a-user-a-zip
  developer preview package (split images + merged image per device,
  README.md, manifest.json).
- `cattobaked/<device>/` — the actual build output directory purrstrap
  writes to; `build.json` per device is the ground truth for "did this
  really build" (`firmware_bin` field — `"not built"` means it didn't).
- `CoreOS/build_<device>/` — raw ESP-IDF CMake build directories, one per
  device that's been built at least once. Delete `sdkconfig` inside one of
  these, not the whole directory, when a device.pcat Kconfig-affecting
  value needs to actually take effect (section 2).
