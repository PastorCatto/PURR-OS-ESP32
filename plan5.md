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

| # | Item | State |
|---|---|---|
| 0 | Plan written, scope and delivery decided | done |
| 1-7 | Phases above | todo |

## Session log

- 2026-10-26: scope set (whole developer kit, self-contained download); surveyed what exists (above);
  plan written. Nothing built yet.

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
