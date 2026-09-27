# PURR OS — Repo Briefing (for a new Opus 5 session)

**Snapshot date:** 2026-07-24. Verify anything time-sensitive below (git log,
`PURROS_VERSION`) rather than trusting this document if it's more than a
day or two old.

## What this is

PURR OS is a modular embedded OS for ESP32-family boards (T-Deck Plus,
Tab5/ESP32-P4, CYD variants, Heltec, Waveshare, jc3248w535, etc.), built on
ESP-IDF. Everything is componentized: a per-device kernel, a `catcall_*`
driver-contract layer (display/input/radio/etc. behind a stable API so
drivers are swappable), and multiple swappable UI backends:

- `cupcake` — LVGL-based, touch-first
- `miniwin` — Win95/WinCE-style windowed desktop, mouse+keyboard-first
- `kittenui` — another LVGL-based backend
- `blackpurr` — minimal shell tier

Apps come in three trust tiers: `.meow` (normal Lua apps), `.hiss`
(privileged Lua, extra `kitt.*`/`radio.*`/`gps.*` namespaces, gated by a
Developer Mode toggle + honor-system signature tag), and native C apps.

## Toolchain

- Python-driven build tools: `purrstrap` (build/flash/bake/package
  management), `catstrap` (app-tier packaging/SDK), `modulestrap`. Entry
  point wrapper: `purr.ps1` / `purr.py` / `purr.sh`.
- `device.pcat` (per-device) and `module.pcat`/`app.pcat` (per-component)
  are the declarative config files driving what gets statically compiled in.
- `CoreOS/sdkconfig_<device>` files are **auto-generated** from `device.pcat`
  via `purrstrap generate` — don't hand-edit them; use the paired
  `.overrides` file for the handful of quirks that have no pcat equivalent.
- ESP-IDF v5.3.5 is installed locally at `C:\esp\v5.3.5\esp-idf`, with its
  own Python venv at `C:\Espressif\tools\python\v5.3.5\venv`. The bare
  `python` command on PATH resolves to a Windows Store stub — you must
  prepend `C:\Espressif\tools\python` to PATH *before* sourcing
  `C:\esp\v5.3.5\esp-idf\export.ps1`, in the **same PowerShell invocation**
  (shell state doesn't persist between separate tool calls in this
  environment).

## Versioning / release model

- `PURROS_VERSION` lives in `purrstrap/purrstrap.py` (currently
  `"1.0.0-dp8"` — just bumped from dp7, **not yet baked**).
- `purrstrap bake` builds all devices → `releases/v<version>/` (lightweight:
  flash.bin/build.json/firmware.bin per device + manifest.json).
- `purrstrap bake --dp` additionally packages a full flashable developer-
  preview bundle into `CatReleases/DP<N>/` (split + merged images, README,
  manifest) plus a top-level `CatReleases/DP<N>.zip`. The `N` is parsed
  straight out of `PURROS_VERSION`'s `-dpN` suffix, so the two numbering
  schemes can't drift apart — **bumping the version string is the only
  thing that determines the next DP number.**
- DP1 through DP7 have already been baked, released, and committed to git
  (both `CatReleases/DP1..DP7/` and `releases/v1.0.0-dp2` through
  `v1.0.0-dp7` are tracked — this is intentional repo convention, not
  bloat; confirmed by matching prior commit history).
- `CHANGELOG.md` covers v0.12.0+ (the modular-architecture era); older
  history is archived. **Note: the changelog has a gap — it stops at
  `v1.0.0-dp2` even though dp3 through dp7 were baked and released.** No
  dp8 entry exists yet either (an attempt to add one was made and then
  explicitly reverted by the user mid-session — see "Current state" below).

## Current state (as of this session)

- Last commit: `9ef61b97` — "feat(miniwin,cupcake): 4-window WinCE desktop
  rewrite, lock overlay, boot splash". This bundled all the accumulated
  work sitting in the tree since dp7 was baked:
  - **MiniWin WinCE desktop rewrite**: split the single full-screen
    `desktop_paint()` into four real z-ordered windows (wallpaper/icons/
    taskbar+Start Menu/lock overlay). Fixes stale-pixel regressions from an
    earlier attempt at the same split (see that commit's own comments
    referencing prior commit `fb39a3a1`, "regressions") — every window's
    paint callback now fills its own background first, and all repaints
    route through `mw_paint_all()` (targeted repaints skip occlusion checks
    on focused windows and could paint over a just-opened app).
  - New `miniwin_lock` module — dedicated idle-timeout lock overlay.
  - New `boot_splash` module — raw-framebuffer splash before any UI
    backend starts, wired into `kernel_tdp_boot.c`.
  - `tdeck_plus` UI backend flipped back to `miniwin` (was `cupcake`) for
    this pass.
  - Cupcake nav bar Home/Back fix (foreground window stack, was only
    hiding one window handle).
  - NimBLE buffer trims for tdeck_plus's tight internal-DRAM budget.
  - Also swept in: CatReleases DP2-DP7 artifacts and `releases/v1.0.0-dp7/`
    that were sitting untracked, plus a pre-Windows-reinstall session
    export (`export/`).
- **Uncommitted right now:** only `purrstrap/purrstrap.py`'s
  `PURROS_VERSION` bump (`1.0.0-dp7` -> `1.0.0-dp8`). No bake has run yet -
  **no dp8 firmware artifacts exist.** No CHANGELOG entry for dp8 exists
  yet either.
- Untracked/deliberately-excluded scratch files sitting in the tree:
  `bake_msn.log` (a build log) and `wallpaper.webp` (unreferenced asset) -
  left out of the last commit on purpose, still sitting untracked.
- **Known unverified item carried over from before this session:** the
  4-window desktop split's stale-pixel fix has been written and committed
  but **never rebuilt or reflashed to real hardware** - a
  `purrstrap build tdeck_plus` + on-device check is still outstanding.

## What's actually being planned right now

The user bumped the version string toward DP8 but explicitly said **"no
baking yet"** and **"dont do anything else while i plan the next moves"**
- i.e., this session paused mid-planning, before deciding what should
actually go into the DP8 cycle (on-hardware verification of the desktop
rewrite? more features first? just ship what's on main?). Nothing has been
baked, and the CHANGELOG has no dp8 entry. Whatever comes next should
start from: decide DP8's actual scope, verify the outstanding hardware
item above, *then* bake + changelog together.
