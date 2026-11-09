# plan5: the PURR developer kit (SDK), a self-contained download

**Source of truth for this work.** Re-read after any context compaction. Update the Status table and
the Session log as work happens. Started 2026-10-26.

## Goal

One download that lets someone write, build, sign, run and ship things for PURR OS without this
repository and without ESP-IDF installed:

- **Apps**: pre-compiled, signed `.cat` images (`AppSDK/SPEC.md`).
- **Drivers**: loadable driver modules built against the driver bus ABI (`Drivers/SPEC.md`).
- **Flavors**: your own build of PURR OS, with your own keys, board, recovery manifest and network backend
  (`SECURITY.md` section 14).

And a way to **run all three without hardware**: the emulator, with the VM-flavor image, so a developer
sees their app work (and their driver, and their flavor boot) before touching a board.

## Owner decisions (2026-10-26)

- Scope: the **whole developer kit**: apps, drivers and flavor tooling behind one command.
- Delivery: a **self-contained, versioned download**. No ESP-IDF, no repo checkout, no Python install needed.

## What exists today (so we do not rebuild it)

| Piece | State |
|---|---|
| `AppSDK/SPEC.md`, `purrstrap apps new/build`, one template (`AppSDK/templates/unprivileged`) | partly built; the app ABI is minimal (`purr_app_abi.h`: `on_create`, `on_destroy`, one `console_print`) |
| `purrstrap modules build` (compile, relocate, pack for kernel modules, drivers, apps) | works, uses the ESP-IDF xtensa toolchain found on disk |
| `purrstrap keys` (generate, sign, verify, trust-block) | works; needs the `cryptography` package |
| `purrstrap coreos build --flavor vm`, `coreos package-component`, `coreos slot-image`, `bootpkg build` | work (plan4) |
| Emulator: Rust shell + patched QEMU, W5500 VM board, secure VM image under `Testrun/vm-secure/` | works (plan4) |
| ABI headers: `purr_abi.h`, `purr_app_abi.h`, `purr_module_abi.h`, `purr_driver_bus.h` (driver bus ABI v4) | exist, owned by coreos and kernel |

## What does not exist, and the problems that come with it

1. **The apps tool does not run here.** It needs Python 3.11 (`tomllib`); the machine has 3.9 (the IDF
   environment's). A self-contained kit ships its own Python, or vendors a TOML reader.
2. **The toolchain is 711 MB** (`xtensa-esp-elf`, the whole ESP-IDF one). The kit needs only the compiler,
   assembler, linker, `objcopy`, `nm`, `readelf` and `libgcc` for Xtensa. That trims to a fraction.
3. **The emulator is not relocatable.** The QEMU binary links MacPorts libraries by absolute path
   (`~/macports/lib/...`). A download needs them bundled and re-pointed, or QEMU built statically.
4. **The app ABI is a stub.** There is no catcall dispatcher, no mini-library, no event loop, so there is
   little for an app to do besides print. The SDK should document that honestly and grow with the runtime.
5. **No `run` path.** Nothing installs a `.cat` into the VM and starts it. Needs a way to put an app on the
   VM's `root` filesystem (or the app store partition) and run it from the shell.
6. **Keys.** A developer needs signing keys that the VM image trusts. The kit ships a published, clearly
   labelled **test key** (developer role) and a VM image built to trust it in development mode; production
   keys are the developer's own (`SECURITY.md` section 14).
7. **Host platforms.** macOS Intel is the only machine here. The layout should not assume it, but a first
   download is built for the host we have; others follow.

## Design

### The `purr` command

One entry point (a small launcher that runs the bundled Python), thin over purrstrap:

| Command | Does |
|---|---|
| `purr doctor` | check the kit is whole and the host can run it (toolchain, emulator, python, keys) |
| `purr new app <name>` / `driver <name>` / `flavor <name>` | scaffold a project from a template |
| `purr build` | build the project in the current folder (reads its manifest) |
| `purr sign` | sign with the project's configured key (the test key by default, in development) |
| `purr verify` / `purr inspect` | check a signature, show a header |
| `purr run` | build, sign, put the result in the VM, boot it in the emulator window and open the shell |
| `purr emulator` | open the emulator on a chosen image (default: the kit's VM image) |
| `purr keys new` / `keys test` | generate your own keys; show the published test key |
| `purr flavor build` | build your flavor's OS (needs the full OS source; see open question 1) |
| `purr package` | produce the release bundle for an app or driver |

### The bundle (one folder per host)

```
purr-sdk-<version>-<host>/
  bin/purr                   launcher
  python/                    bundled Python (3.11+) with cryptography
  toolchain/                 trimmed xtensa-esp-elf (compiler, binutils, libgcc, newlib headers if needed)
  include/                   the ABI headers, versioned: purr_abi.h, purr_app_abi.h, purr_module_abi.h, purr_driver_bus.h
  lib/purrstrap/             the existing build tool, pinned to this SDK version
  emulator/                  the emulator app, patched QEMU and its libraries, board files
  images/                    the VM image(s): enforce-mode, trusting the test key in development; blank eFuse file
  keys/test/                 the published developer-preview test key (insecure by design)
  templates/                 app, driver, flavor starters
  docs/                      reference and guides
  VERSION, SDK-MANIFEST      version, file list and hashes (the kit verifies itself)
```

### Versioning

The kit has one version. It records the ABI versions it contains (`PURR_APP_ABI_VERSION`, driver bus ABI,
module ABI) and a project pins the kit version it builds with, so an old project can rebuild with an old kit
(`AppSDK/SPEC.md` section 7). A built app or driver records the kit version in its manifest.

### Testing the kit

- Every template builds with the kit, passes the import check, and is signed and verified.
- A smoke test runs inside the emulator: install the built app into the VM, run it, check its console output.
- `purr doctor` runs in CI on each host the kit ships for.

## Phases

| # | Phase | Result |
|---|---|---|
| 1 | Make the existing tools run self-contained: fix the 3.9 / `tomllib` problem, pick the trimmed toolchain set, write `purr doctor` and a pinned-layout builder script (`sdk/build-kit.py`) | `purr new app` and `purr build` work from a bundle folder on this machine |
| 2 | Keys and signing: published test key, `purr sign`, development-mode trust in a kit VM image | a built app is signed and verifies |
| 3 | The emulator in the kit: relocate QEMU, bundle the emulator app and the VM image, `purr emulator` | window opens from the bundle with nothing else installed |
| 4 | `purr run` for apps: put a `.cat` on the VM and run it | `new -> build -> sign -> run` shows the app's output |
| 5 | Drivers: driver template, `purr build` for drivers, run in the emulator against a board file with the device modelled | a sample driver loads in the VM |
| 6 | Flavors: `purr flavor` (keys, board, recovery manifest, network backend), build and boot in the emulator | a custom flavor boots in the VM |
| 7 | Docs, versioning, the release script, `purr package`, checks, and the other host platforms | a downloadable, versioned kit |

## Open questions (need an owner decision)

1. **Flavors need the OS source.** Building your own flavor of the whole OS needs the kernel, coreos and
   bootloader code, which is much larger than the kit and needs ESP-IDF. Options: (a) the kit ships prebuilt
   OS components plus the tools to repackage and re-sign them with your keys and config (no ESP-IDF); (b) the
   kit ships the OS source as a separate download and `purr flavor build` drives ESP-IDF; (c) both. This
   decides how big phase 6 is. Recommendation: (a) first, (b) later.
2. **Licence and publishing.** What licence the kit ships under, where it is hosted, and how the published
   test key is announced.
3. **Python.** Bundle a standalone Python (about 30-40 MB per host), or make the tool pure Python 3.9+
   compatible and require a system Python? Recommendation: bundle.
4. **Host platforms for the first release**: macOS Intel only, or also macOS arm64, Linux and Windows.
5. **App ABI.** The SDK can only offer what the runtime has (`console_print`). Do we ship now on the minimal
   ABI and version it, or wait for the catcall dispatcher and mini-library?

## Status

| # | Item | State (2026-10-29, after the overnight build) |
|---|---|---|
| 0 | Plan written, scope and delivery decided | done |
| 1 | Driver framework: bus ABI 6 (UART), driver classes, named-value config, class registry | done, host-tested (all C host tests pass, 2026-10-29) |
| 2 | Reference drivers: GPS (NMEA), LoRa (SX126x), LTE modem (AT), Wi-Fi (ESP-AT), Bluetooth (HCI H4), trackball | written; **41+ host tests pass** against a Python bus (`sdk/tests/test_drivers_host.py`); built as modules (`out/drivers/*.cat`, unsigned); **not run on a board** |
| 3 | `purrsdk` library + `purr` CLI: inspect (pure-Python signature check), keys, new, build, sign, release, serve, publish, flash, brand, board, config, doctor | done, 72 tests |
| 4 | PURR Studio (Metro web app, local server): drop-an-image inspector, projects, keys, boards (pin map), brand (live boot-screen preview), release, server, configure (settings, apps, trust block), flash, doctor, docs | done; looked at in headless Chrome; the drag-and-drop gesture itself was not exercised (no way to drag in a headless browser), the upload route behind it is tested |
| 5 | Flavors: `brand.toml` -> boot package (names, wordmark, colours) and the system's visible names/first accent | boot package rebuilt with a flavor and checked in the binary; the shell, loader and KittenOS use `PURR_BRAND_*` (compile-checked in all three profiles; **not run**, default values are the stock strings) |
| 6 | Kit builder (`sdk/build-kit.py`): bundled Python 3.12 + cryptography + esptool, trimmed xtensa toolchain, headers, purrstrap, prebuilt parts, manifest | done (0.1.0 was the first kit; the current kit version is 1.1.0, see the 2026-11-06 inventory below); first kit at `~/Desktop/PURR-SDK/purr-sdk-0.1.0-macos-x86_64` (about 330 MB) works with the system Python; bundled Python 3.12 verified on this Mac after the stall cleared; the 74 SDK tests pass under it |
| 7 | Firmware consumers: `gps`, `radio`, `modem` shell commands; the T-Deck Plus board lists its GPS and radio | built in all three profiles; **not flashed** |
| 8 | Emulator in the kit (Showtime Part A, 2026-11-06) | the kit builder bundles a relocated QEMU, the window, the headless runner and board files (`emulator/`, +39 MB, kit 364 MB); `purr emulator` and `purr run` drive it; the kit built end to end and `purr doctor` finds it. **Run end to end only as far as the Developer Portal's server page**: the Find servers tap hit an emulator panic (see showtime1.md). No VM image ships in the kit yet |
| 9 | Phase D of plan6 (app lifecycle in the SDK) | not started |

## Session log

- 2026-10-26: scope set (whole developer kit, self-contained download); surveyed what exists (above);
  plan written. Nothing built yet.
- 2026-10-28/29 (overnight, owner away): built items 1-8 above. Commits: driver classes and bus ABI 6; GPS and LoRa drivers; `sdk/` (library, CLI, Studio, templates, docs, kit builder); branding;
  AT modem, ESP-AT Wi-Fi and HCI drivers; trackball driver; firmware commands. Things worth knowing:
  - **macOS stopped starting any newly built program** from about 22:48 (processes sat in the "U" state, even tiny C programs). Cause, found next morning: a "debug other apps" authorization prompt was waiting for
    an answer (probably from force-killing lldb/debugserver earlier). Answering it cleared everything. Meanwhile the drivers were tested by loading them as shared libraries from Python
    (`sdk/tests/driverhost.py`), which found real problems: a two-digit GPS year read as 2094, a LoRa probe that could not tell an absent chip from a present one, modules that needed memcpy.
    The C host tests then ran too; two wrong expectations in `test_drv_gnss.c` (my arithmetic) were fixed.
  - `purrstrap`'s module build now passes `-fno-tree-loop-distribute-patterns` (new modules only; nothing already signed changed).
  - The T-Deck Plus GPS is on GPIO 43/44, which are also UART0's default console pins. UART1 moved onto them takes over the pins; the USB serial port keeps carrying the logs (it is the console's second output).
  - The radio's reset is left unset: LilyGO wires it to GPIO 17, the touch controller's reset line.
  - Not decided by anyone: the kit licence (`LICENSE-TODO`), and whether the kit's bundled Python should be verified on a second Mac before it is handed out.

## Absorbs plan6 phase D (owner, 2026-10-28)
Plan6 phase D (SDK side of the app runtime) is a phase of this kit now. It includes the lifecycle rule: unload apps that are not
displayed, freeze those that stay open. See plan6 for the runtime design.

### Background cap (owner, 2026-10-28)
At most 5 apps stay resident in the background at once: 3 user apps (frozen when not shown, see above) and 2 service apps. A service is the
small daemon half of an app (the Meshtastic radio loop is the model); it has no screen, stays running, and on real hardware runs on a
30-60 s alarm that wakes the device out of light sleep (plan10: `wake-alarm` permission, 5 s floor, wake window, three-overrun cut-off).
Over the cap, the oldest frozen user app is unloaded; a new service is refused with a message when both service slots are taken.
Open: whether a service is a second entry in the same package (manifest `[service]`) or its own package; the cap numbers as Kconfig values.

### Decided (owner, 2026-10-28)
- A service is a second entry in the same package (manifest `[service]` section): one install, one signature, one update.
- The background cap is computed at boot from the PSRAM size, not fixed. 8 MB: 3 user apps + 2 services. 4 MB: 2 user + 1 service.
  2 MB: 1 user + 1 service. Other sizes interpolate (a table in firmware, one row per size class); a Kconfig override can lower it.

### App lifecycle, decided (owner, 2026-10-28)
- **Leaving the screen:** the UI half is unloaded. An app that is mid file transfer, holds an open connection, or is talking to hardware does not stay
  frozen as a whole: its UI is closed and a small **companion service** (the `[service]` entry in the same package) carries on, finishes its turn
  and closes cleanly. Frozen apps (anything that must keep its memory) still count against the user slots (3 / 2 / 1 by PSRAM).
- **Services:** no UI, about 64 KB, CPU wake window only, no screen wake lock. A service may post a **notification** (messaging apps such as Meshtastic);
  the notification wakes the screen. That goes through the permissions manager (`notify` permission). Permissions are asked at first launch; "just
  once" and "don't allow" ask again every time (current behaviour, keep). Three wake-window overruns in a row switch the service off and tell the user.
- **Over the cap:** the oldest frozen/unloaded-candidate app is closed and the shell shows a short notice ("Closed Notes to free memory"). If the app has
  a companion service still running, the notice says so ("Closed Meshtastic; its service is still running"). Before closing, the app gets a
  `PURR_APPEV_SAVE` event (save now); apps may also autosave.

## Boot menu key mapping (owner, 2026-10-28)
The bootloader's menu opens when Space is held at power-on. The key is meant to be a setting the owner can map to any key, and the SDK should expose that mapping (so a
keyboard layout or a custom board can choose its own). Stored with the boot configuration; the bootloader reads it before the boot package loads. Not built: the quieter
bootloader redesign (menu only when needed, Fast startup, 20 s fall-through, "PURR OS!" logo) is waiting on the owner's approval of the preview.

## Owner answers before the overnight start (2026-10-28)
- **Flavors:** both, source later. The first kit repackages and re-signs prebuilt OS parts with the developer's keys and settings (no ESP-IDF); building from source is a separate, later download.
- **Python:** bundle a standalone Python (about 30-40 MB), so the kit needs nothing installed.
- **Hosts:** this Mac only for the first kit. The folder layout stays host-neutral (`purr-sdk-<version>-<host>/`), so Linux and Windows kits can follow.
- **Emulator:** I may run it while the owner is away, for the kit's smoke tests only: one short run at a time, stopped when done (see memory no-emulator-runs: the owner does not want it run otherwise).
- Defaults I chose (change on request): kit files carry a placeholder licence notice (`LICENSE-TODO`) until the owner picks one; the kit is built into `~/Desktop/PURR-SDK/` and the repo keeps only its sources under `sdk/`.

## Overnight build (owner away, 2026-10-28): scope as the owner set it
- Independent drivers for **LTE, Wi-Fi, Bluetooth, GPS, LoRa** (driver classes, templates, reference drivers, host tests).
- The server part: `purr serve` (dev server and repository tooling) inside the kit.
- A self-contained **Python UI app (PURR Studio)**, Metro design language, friendly voice, drag-and-drop of a pre-built image to verify it and see what to do next, and many tools
  for creating and configuring every part of the OS.
- The emulator is NOT part of this job (the owner works on it separately as a fork of the ESP32 emulator): leave a hook only.
- The end goal: a 100 % supported T-Deck Plus built through the SDK, GPS and LoRa included. Inventory and status: `sdk/DESIGN.md`.
- Design details and order of work: `sdk/DESIGN.md`. Nothing is released, flashed or put on the dev server tonight.

- **Branding (owner, last request):** a flavor's names, wordmark and colours are data (`brand.toml`), applied to the boot package, loader, KittenOS and shell; Studio gets a Brand tool with a live preview. Design in `sdk/DESIGN.md`.

- 2026-10-29: `purr repo` (init, add, build, release, endorse, verify) and the repository tool in Studio; Repositories/SPEC.md. See plan8.


## App lifecycle: frozen apps, eviction, service slots (2026-10-31, built; host-tested, compile-checked, NOT run on a device)
The rule from "App lifecycle, decided" above, in code:
- **Frozen.** An app that leaves the screen and may not run in the background (not background-ok, or over the service cap) goes SUSPENDED and its task is frozen. The runtime
  never suspends a task from outside (it could be inside littlefs or waiting on the UI task, holding their locks). `host_freeze` queues a "freeze" message behind whatever the app is
  doing; the task parks itself on a semaphore when it reaches it. `deliver()` thaws first, so RESUME, DESTROY and events always reach a running task; teardown thaws too.
  Freezing drops its screen and CPU wake locks and cancels its alarms (`on_frozen` in `purr_catrt_power.c`).
- **Caps.** `cfg.max_frozen` / `cfg.max_services`, set at boot from the PSRAM size by `purr_rt_caps_for_psram` (8 MB class 3 + 2, 4 MB 2 + 1, smaller 1 + 1). `/etc/apps.conf`
  `general:max_frozen` and `general:max_services` can lower them, never raise them. Over `max_frozen`, the app unused longest is closed (persistent and pinned apps are skipped, the app just
  brought to the front never is) after an `evicted>>id,name` event; it saved its state when it left the screen. A new daemon over `max_services` is refused (`PURR_RT_E_SERVICES`,
  `refused>>name,service-slots`); a background-ok app that cannot get a slot is frozen instead.
- **Notice.** The runtime's event sink keeps a one-line notice ("Closed Notes to free memory", "X cannot run in the background: no free slot"); the shell's clock timer takes it and
  shows it on the status strip (`purr_catrt_take_notice`). The runtime task never waits on the UI.
- Tests: `test_runtime.c` (273 checks): freeze before no delivery, thaw before RESUME and DESTROY, eviction order, protected apps, service cap and fallback, PSRAM table, config clamp.
- Not done: the companion-service half (a `[service]` entry in the package: a UI app's service carries on when its UI is closed) and the `PURR_APPEV_SAVE` autosave nudge before eviction. The
  "unload instead of freeze" case (an app with nothing to keep) is not distinguished: every non-service app is frozen, then evicted when the cap is hit. The device checks still to make: park
  and thaw a real app (Notes, Files) several times, with a timer running and with a permission prompt up.

## Game packages (2026-11-01; tool built and tested, nothing runs on a device yet)
Emulators and games are PURR application packages (a signed `.cat`, image type APP, installed and updated like any app), so they can be added to any device and updated one at a time.
- `purrstrap games package|inspect|target` (`purrstrap/scripts/games.py`, tests in `purrstrap/tests/test_games.py`). A manifest with `kind = "game"` and a `[game]` section (systems, extensions, needs: buttons, audio, minimum display) is wrapped as a PGAM payload (JSON metadata + the application image) with the app subtype flag `0x10` (APPF_GAME_IMAGE), so nothing mistakes it for a relocatable module. Format described at the top of `games.py`.
- `payload = "image"` (a complete ESP-IDF application image, built for one board) works in the tool. `payload = "module"` (portable across devices, built on the game interfaces) is not built.
- `purrstrap games target` writes a Retro-Go target (`config.h`, `sdkconfig`, `env.py`) from a board file's new `[game]` section (button map, menu GPIO, screen MADCTL; the display, SPI, SD, audio, battery and power pins are read from the existing sections), so a new device is a board file, and registers it in Retro-Go's `config.h` plus one small `rg_input.c` patch. Checked: the generated T-Deck Plus target has the same pins, storage, audio, screen and battery settings as the hand-written one (a test), and its launcher builds to within 16 bytes of the hand-written build.
- First set of signed packages for the T-Deck Plus: a launcher and four emulator applications (NES/PC Engine/Game & Watch/Lynx/Master System/Game Gear/ColecoVision, Mega Drive, MSX, DOOM), 0.67 to 1.06 MB each.
- **Not built, needed before any of it runs on a device:** loading an image package (flash slots for app images, a bootloader start-and-return, the trust rule, the Store installing them), and the `game` app kind with the game surface, audio and input interfaces for the portable module payload.

## Continued by Showtime Plan 1 (2026-11-05)
The SDK is brought up to spec with the standalone emulator inside it in Part A of [showtime1.md](showtime1.md) (owner-approved, waiting for the owner's keyword).

## Inventory, 2026-11-06 (Showtime Part A, step 1: checked, not remembered)
| Claim | Checked |
|---|---|
| SDK tests | 111 pass (was 74/96; two were stale: the driver `Device` struct's `defer` byte, the board profile's keyboard devices; both fixed) |
| purrstrap tests | 128 pass, 4 skipped |
| C host tests (`coreos/test/host`) | all pass (34 programs) |
| Kit version | 1.1.0 in `purrsdk/__init__.py`; a 1.1.0 kit was built into scratch only (not placed on the Desktop) |
| Emulator in the kit | built and relocated; QEMU runs from a copy with its original library folder hidden; one 75 s headless boot of the secure VM image checked |
| `purr run` | works up to the Portal's server page on the 1.0.27 Android image; the rest (find, trust, install, launch) is written as steps and was done by hand, not scripted end to end |
| Published test key, and a VM image that trusts it (problem 6 above) | **not delivered.** A kit VM image needs a trust block signed with the boot key, so the owner has to make it (passphrase); until then `purr run` works with an image you supply and an app signed with a key that image trusts |
| Flavors, drivers on a board, Studio drag-and-drop | as in the table above: not run on a device / not exercised |

## Launch pass, 2026-11-08 (owner: update the SDK for themes and the new architecture, document, package, push, Windows)
- **SDK 1.2.0.** Already supported in the code: theme and shell items in repositories (`repo.py`), `purr new theme`, `purr ui pack/check`, any `/` in an entry's `url`. Done now: the themes guide no longer says the device side is unfinished (it exists since OS 1.0.29; the theme install from the Store was run on a device); the keys and repositories guides describe the five-key set (ids 1 to 5), one key = one repository, the device limits, subfolders and the hub; the kit builder has a **Windows branch** (Python build and wheel tags, `Lib\site-packages`, `.cmd` launchers, no emulator) that is **written blind and NOT run on Windows**; `emulator.find()` answers "none" on Windows. Tests: SDK 182 pass, purrstrap 156 pass (4 skipped), C host tests all pass.
- **Documentation.** `docs/` chapters 03, 06, 07, 08, 14, 16, 17 updated (key rotation, releases 1.0.30 to 1.0.35, the hub and `hubsign`, the Store's self-update, driver put-back, opening a running app) and the new chapter 22 (Windows). 32,308 words, 1,016 links, 0 broken. `CLAUDE.md` (git-ignored, local) had its stale PSRAM-stack rule corrected to match chapter 06.
- **Pushed.** `origin/PURR2.0` moved `b752debe..67256e4f` (107 commits) after scans of the tree and the commits for passphrases, private keys and files over 20 MB (none). The Windows PC clones that branch (`docs/22-building-on-windows.md`).
- **Open:** a real Windows run of every step in chapter 22; the emulator for Windows; the front README quote slot; a 1.2.0 kit build on the Mac (not rebuilt this pass; `python3 sdk/build-kit.py`); the plan status tables in plan8 are stale against the logs.

### Studio's run button (owner, 2026-11-08; wired and tested against a fake emulator, not run against the real one)
- **What was missing:** the CLI (`purr run`, `purr emulator`) already drove the emulator (`purrsdk/emulator.py`), but Studio's run page was a placeholder ("the emulator is on its way") and its tile was greyed out.
- **Done:** Studio routes `emulator` (status and why not), `emulator/run` (a job: build the project, or take a built `.cat`, then `run_app`), `emulator/open`, `emulator/stop` and `emulator/shot` (a picture as a data URL); the run page (app, build board, emulator board, key, window or headless, pictures shown on the page); a **run in the emulator** button on an app project's build tab; the tile is live. `sdk/tests/test_studio_emulator.py` (7 tests with the fake emulator and a mocked `run_app`) and the whole SDK suite (196) pass.
- **Not done:** a real run. On this Mac the emulator checkout has no built `esp32-emulator` or `headless_tap` (only an old `headless`), and no patched QEMU binary was found. The kit needs them in `emulator/` (or `PURR_EMULATOR_DIR`) plus a VM image at `images/vm-flash.bin` (or `PURR_VM_IMAGE`). A run uses port 47801, so a dev server on that port must be stopped first.

### The Windows kit was about 900 MB (owner, 2026-11-08)
`bundle_toolchain()` in `sdk/build-kit.py` trimmed the compiler by matching folder names with `/`; on Windows the folders are joined with `\`, so nothing matched and the full toolchain was copied (and `cc1plus.exe` kept). Fixed by normalising the separator and dropping `cc1plus.exe`; the Mac kit's toolchain is unchanged (220 MB) and the SDK suite passes (196). Not run on Windows; the new size is unmeasured. On the Mac the kit builds cleanly (365 MB with the emulator; doctor all ok), but by default it takes its images from the newest `PURR-OS-release-*` folder on the Desktop (1.0.28) and has no `images/vm-flash.bin`, so the Run button reports it cannot run until one is supplied.

### The image maker (owner, 2026-11-08: "Studio must do everything start to finish, no terminal"; built and checked on the computer, not flashed)
- **Why:** the owner found Studio only produced a source file to move into a folder and build in a terminal. The new design (a four-step road: device, parts, build, deliver; the terminal an optional drawer) was drafted as an interactive canvas, approved, and the image maker is the first piece of it.
- **Done:** `sdk/purrsdk/image.py` assembles one whole flash image from the five signed parts (bootloader payload, partition table, boot package, and the kittenos, kernel and loader slot images with their trailers) with no shell script, compiler or ESP-IDF. Offsets come from the board's partition table (`PurrOS/partitions/<board>.csv`, now also copied into the kit). The table bytes equal ESP-IDF's own output for the T-Deck Plus and the Waveshare board (golden files in `sdk/tests/data/`). Parts are checked first (a PURR container, payload hash, fits its slot) and the image is checked after (the table read back). Added `purr image` (with `--plan`), Studio routes `image/boards|folders|plan|build`, a **device image** page and tile, `sdk/docs/16-device-image.md`, `sdk/tests/test_image.py` (9 tests). Run on the real 1.1 set: the image passes `purr inspect` (layout, hashes, signatures).
- **Not done:** flashing such an image to a device; a trust block of the user's own (a stock bootloader only accepts one signed by a compiled-in boot key, so without one the device uses its defaults); the file system (`root`) is left erased (apps and drivers arrive by the setup wizard or the dev server, an in-image LittleFS is a later step); the rest of the redesign (the step-by-step shell, the in-page editor, the terminal drawer, the flash progress bar).

### The UI maker and SquareLine Studio import (owner, 2026-11-08: "creating UIs with a predefined file that adapts", then "support SquareLine Studio themes: export, drop in, adapt"; built and tested on synthetic exports, not on a real one)
- **Done:** `sdk/purrsdk/uimaker.py`. `ui.toml` (name, shell, board, licence, a `[look]`, `[adapt]` scale and wallpaper count, and `[pictures]` overrides) is turned into a complete theme (`theme.conf` + `pack.bin`) for the Android or Windows style shell: every required picture drawn at the size the shell wants by a small anti-aliased glyph renderer (no other tools), anything of yours winning and resized to the slot (`resize`), then checked with the existing `ui.check_theme_folder` (names, sizes, CRC). `import_squareline()` takes an export folder, a project folder or a zip (unsafe zip paths refused): project `assets/*.png` first (lossless), then `ui_img_*_png.c` arrays (LVGL 8 with its per-depth `#if` blocks, and LVGL 9; 16-bit colour with or without alpha; 32-bit refused plainly), written as PNGs in `imported/` with a slot guessed by name. Added `purr ui new|import|make`, theme projects with a `ui.toml` build through `purr build`/Studio's build tab, Studio routes `ui/starter|upload|projects|new|project|save|map|preview|make`, a **make a UI** page and tile (editor, live preview of the drawn pictures, a menu per imported picture to choose its slot, make), `sdk/docs/17-making-a-ui.md`, 38 new tests (SDK suite 251). The contact sheet of all 64 generated pictures was looked at (readable; the handset, camera and browser glyphs are rough).
- **Not done / not verified:** a real SquareLine export (the importer follows the layouts SquareLine writes, from memory of them: if a real one fails, the file is the test case); SquareLine's colour themes (`ui_theme_variables`) are not read, so the accent is set in `ui.toml`; no layout or screen import (a theme is pictures and colours); the drawn art is placeholder; a theme on a device has not been installed from this output; `scale` other than 1.0 makes pictures today's shells would not expect.

### The new Studio: main menu, build a UI, element library, icon packs (owner, 2026-11-08: "build it to the mockup"; built and looked at in headless Chrome, not yet used on a device)
- **Done:** `sdk/studio/web/menu.js` (the main menu whose big tile is the last thing done, from `purrsdk/activity.py`; build-a-UI start screen) and `builder.js` (the builder on the device's real screen with palette click and drag, drag on the screen, arrow-key nudge, inspector, layers, undo and redo, screens as tabs, autosave; the element library; the icon pack page; the it-is-ready page), styled in `studio.css`. The engine: `uibuilder.py` compiles `[[element]]` layouts to the device's `screen "name" { ... }` file with a Python copy of the device's parser to check it, `uimaker.py` handles `[icons]`, icon-pack import (folder, zip, pack.bin, SquareLine), per-slot pictures, export, and a result summary; routes for all of it. Screens rendered headlessly (home, build a ui, builder, icon pack, ready) and compared with the mockup. SDK suite 283 tests.
- **Not done:** the **build a system** screens of the mockup (device, parts, build, deliver in one road with drivers, signing and image) as a new page: the menu's "device image" still opens the earlier image page, and keys, release, flash, server, boards and the rest still open their earlier pages (restyled by the same CSS, not redrawn); the terminal drawer; drag handles to resize on the screen (size is edited in the inspector); a firmware change that wires taps, and a layout engine beyond the 16-widget config; icons on the stage are Studio's drawn glyphs, not yet your pack's own pictures.

### Build a system: device, parts, build, deliver (owner, 2026-11-08: "build those next"; built, run on real data through Studio's API, screens rendered headlessly; not flashed)
- **Done:** `sdk/purrsdk/system.py` (a system project saved in `<workspace>/systems/<name>/system.toml`; the drivers catalog from a drivers.manifest filtered by board, the board's own entry winning over `any`; the build road with per-step reporting: apps, ui, drivers, parts, image, delivery folder), routes `systems`, `system/new|save|build|address`, `studio/web/system.js` (the four tabs, drivers and apps as switches, key and password, the live checklist, flash with a progress bar, serve over the air, emulator) styled in `studio.css`; the main menu's "device image" opens it. Real run: the 1.1 set from the dev server folder plus the hub's 8 drivers gave a 16 MB image (passes `purr inspect`) and a delivery folder with manifests, parts, system modules and drivers. 19 tests (SDK suite 302).
- **Not done:** building an app project was not exercised end to end here (the tests use no apps; the app step calls `projects.build`, which needs the kit's compiler and a key); flashing and serving were not run against a device; re-signing the system parts with your own key (they arrive signed); a LittleFS image with the apps and drivers inside the flash image (they reach the device from the delivery folder through a dev server or the setup wizard); the old image, release, server, flash and keys pages are still the earlier ones.

