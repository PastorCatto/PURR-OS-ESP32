# 16. The apps

## Short version

- Apps live in `Apps/<name>/` as one C file and a `manifest.toml`. They are freestanding modules (no libc), loaded into PSRAM, signed with the **developer key (id 2)** and **sideloaded**.
- Built into the shell: **Settings**, the Developer Portal (developer mode only), the task manager, and the **Store with its companion**. Everything else is an SDK package.
- Build and publish: `PURRSTRAP_KEY_PASSWORD=... tools/vm/publish_apps.sh <app...>`. The passphrase belongs to that one command only and is never written down.
- Each app below has: what it does, its folder, the catcalls and permissions it asks for, and its status.
- Almost none of this has run on a device. Status words below say what *has* been seen. Plans: [plan6.md](../plan6.md), [plan9.md](../plan9.md).

## What it is for

Apps are how PURR OS grows without growing the kernel. The runtime rules (stack, state, kinds) are in [AppRuntime/SPEC.md](../AppRuntime/SPEC.md) and [AppSDK/SPEC.md](../AppSDK/SPEC.md); the owner's rule is that only Settings is pre-installed (owner memory note, repeated in CLAUDE.md).

## The files

| Path | What it is |
|---|---|
| [Apps/](../Apps/) | One folder per app |
| [tools/vm/publish_apps.sh](../tools/vm/publish_apps.sh) | Builds, signs (key id 2) and publishes apps to `out/devrepo` |
| [tools/release/gen_builtin_apps.py](../tools/release/gen_builtin_apps.py) | Embeds the Store and companion in the firmware |
| [AppRuntime/SPEC.md](../AppRuntime/SPEC.md), [AppSDK/SPEC.md](../AppSDK/SPEC.md) | Runtime and SDK specs |
| [Messaging/SPEC.md](../Messaging/SPEC.md) | Messages, Contacts and messaging services |
| [Permissions/SPEC.md](../Permissions/SPEC.md) | Permission model |
| [purr_catrt.c](../PurrOS/main/purr_catrt.c) | The runtime: tasks, catcall gating by kind |

## How an app is made

1. `manifest.toml` gives `name`, `version`, `kind` (`ui`, `game`, `daemon`), `class = "unprivileged"`, `entry`, the `[catcalls]` it wants with minimum versions, `[permissions] requests` (information only; the real ask happens at first use), `[runtime]` (stack 8192, heap size, `background`) and a `[display]` name.
2. The C file exports the entry function and fills in callbacks. A `switch` makes a jump table, which `purrstrap/scripts/modules.py` treats as code ([plan9.md](../plan9.md), fixed 2026-11-01).
3. `kind = "ui"` is the only kind that gets the `ui` catcall.
4. Install: Developer Portal (Find servers, Trust, Install) or the Store.

## The apps

### Settings (built into the shell)

The device settings: Network, Personalization, Apps and permissions, Accounts, About, plus a Display page with "Screen off after". The Windows layout has System, Devices, Network, Personalization, Apps, Accounts, About. The Android layout regroups the same pages: Applications with Development inside, Hardware, About phone. Files: [purr_w10m_settings.c](../PurrOS/main/purr_w10m_settings.c), [purr_settings.c](../PurrOS/main/purr_settings.c). Status: Windows layout bench-tested in the VM (owner, 2026-10-26: "looks good"); Android layout compiled.

### Notes (`Apps/notes/`, 1.1.0)

Plain-text notes, one file per note, in the app's own data folder. Uses `ui` 1.0 and `storage` 1.0. No permissions. About 350 lines. Status: **the first sideloaded UI app**; the owner reported on 2026-10-28 that it "works well on the device" ([plan9.md](../plan9.md)). A visual reference, [Apps/notes-ui-draft/](../Apps/notes-ui-draft/purr_w10m_notes.c.txt), is kept and not shipped.

### Files (`Apps/files/`, 1.1.0)

A file manager. Home folder for everyone, system files read-only for administrators; browse, copy, rename, delete (second tap), new folder, paste, text viewer (first 2000 bytes). Uses `ui` 2.0 and `files` 1.0; requests `shared-storage` and `system-files`. Status: the "going back" hard lock was traced to jump tables and fixed (verified in the VM, [plan9.md](../plan9.md)); the owner still needs to try it on a device. It does not browse `/sd` yet.

### Device Manager (`Apps/devices/`, 1.2.0)

Finds hardware, installs drivers one at a time, sets the developer server, shows system versions and offers updates (administrators). Uses `ui` 2.0 and `devices` 1.0; requests `manage-devices`. A driver that stops the device during start is set aside and listed. Status: the app crashed the device on a PSRAM stack in 1.0.22 and was reworked in 1.0.23 ([plan7.md](../plan7.md)); not verified on a device after that.

### Store and Store companion (`Apps/store/` 1.0.2, `Apps/storesvc/` 1.0.0)

Covered in [chapter 14](14-store-and-repositories.md). The Store is a `ui` app; `storesvc` is a `daemon` with `sched` and `store` catcalls and no screen. Both request `install-apps`. Built into the firmware and also a hub item, so it can be updated without a new kernel. Its Install and Remove buttons sit side by side (ui version 3). Status: the Store was bench-tested on a device (it reads the hub); the version 1.0.2 update path has not been run.

### Calculator (`Apps/calculator/`, 1.0.0)

Four operations, percent, sign change; a button grid. Uses `ui` 3.0, no permissions. About 380 lines. Status: written with ui v3 in 1.0.23; not verified on a device.

### Clock (`Apps/clock/`, 1.0.0)

The time (in the device's time zone through `system` v2 `local_time`), a stopwatch with laps and a timer. Uses `ui` 3.0 and `sched` 1.0. Runs while open; a background alarm needs the `power` wake alarm, which it does not use yet ([plan9.md](../plan9.md)). Status: not verified on a device.

### Photos (`Apps/photos/`, 0.2.0)

Browse and view pictures (JPEG, PNG, BMP) with a slideshow and delete. Uses `ui` 3.0 (`image`, `set_image`) and `files`; requests `shared-storage`. Limits come from the `ui` catcall: about a megapixel and 2 MB per picture. In plan9 this is the "Image viewer". Status: not verified on a device.

### Messages (`Apps/messages/`, 0.1.2)

One front end for messaging services (Meshtastic, Reticulum, MeshCore; none ships yet). Conversation list, thread with bubbles, compose, emoji picker, attach a picture. Uses `ui` 5.0, `sched`, `messaging`, `contacts`, `files`; requests `messaging`, `contacts`, `shared-storage`; runs in the background. About 870 lines, the largest app. Status: compiled, host-tested store; **seen in the Android shell in the VM, headless, on 2026-11-06** (permission prompts, the list row, thread, a received Echo reply). Typing through the emulator's key injection did not reach the fields, so typing is unseen. The emoji picker shows word labels and a sent emoji draws as an empty box: no emoji font ([chapter 12](12-picture-pack-and-fonts.md)).

### Contacts (`Apps/contacts/`, 0.1.2)

The address book: a person has one address per messaging service. Uses `ui` 5.0, `contacts`, `messaging`; requests `contacts` and `messaging`. Status: seen in the VM on 2026-11-06 as a white page with a search field and an empty list; a populated list could not be shown.

### Echo (`Apps/msgecho/`, 1.0.1)

A test messaging service that answers every message so Messages can be tried without a radio. Uses `ui` 1.0, `sched`, `messaging`; requests `messaging`; background. Status: its reply was seen arriving and updating the list row in the VM.

### Beacon (`Apps/beacon/`, 1.0.0)

A sample for [chapter 15](15-power.md): sets a 30 s aligned alarm, counts wakes, calls `done()`. Uses `ui` 1.0 and `power` 1.0; requests `wake-alarm` and `keep-awake`; background. Status: builds as a module; plan10 records it as not run in the VM or on a device.

### Game demo (`Apps/gamedemo/`, 1.0.0)

A bouncing square, button lights and a frame-time bar; tests the `game` catcall. `kind = "game"`. Status: test app.

### Hello and rtdemo (test fixtures)

`Apps/hello/` is the first `.cat` app, built to prove the runtime on real hardware. `Apps/rtdemo/` holds test sources and `gen_testapps.py`, which turns a folder of `.cat` files into the runtime's self-test (`CONFIG_PURR_RT_SELFTEST`, on in the VM flavor). Neither is for users.

### Developer Portal (built into the shell, developer mode only)

Finds SDK servers on the local network, trusts one, installs its apps into `/home/<user>/devapps`. Files: [purr_w10m_devportal.c](../PurrOS/main/purr_w10m_devportal.c). See [chapter 13](13-networking.md).

## Planned apps (owner list, 2026-11-05)

| App | State |
|---|---|
| Terminal | Later. Administrators only; needs a `shell` catcall with a refusal list (no flashing, no key or trust commands) in code, not a setting |
| Music player | Later. No audio in the firmware yet; I2S speaker is in the board file. Needs a driver, a WAV player first, an `audio` catcall |
| Video | Last; needs audio and an MJPEG decoder |
| Meshtastic, Reticulum, MeshCore services | On hold until after 10 November; Reticulum and Meshtastic expected from the owner |
| Image viewer | Exists as Photos |

## How to build and test

1. Per CLAUDE.md, apps are built with `purrstrap modules build --kind app`. The `purrstrap apps build` command needs Python 3.11 for `tomllib` ([plan9.md](../plan9.md)).
2. Publish: `PURRSTRAP_KEY_PASSWORD=<given for this one command> tools/vm/publish_apps.sh notes clock` (default: `notes beacon`). It builds, signs with `signing_keys/developer.key` (key id 2), and runs `dev publish`.
3. Serve: `python3 purrstrap/purrstrap.py dev serve --dir out/devrepo --port 47801`.
4. On the device: Developer Portal, Find servers, Trust, Install.
5. The SDK path is [chapter 17](17-sdk.md).

## What goes wrong

| Symptom | Cause | Fix |
|---|---|---|
| App crashes on the first call after a menu item | A `switch` jump table not treated as code | Fixed in `modules.py`; rebuild |
| App will not open on a loaded device | Not enough fast RAM for the task | Since 1.0.22 stacks are in PSRAM; check the task manager's free RAM |
| App calls flash and the device aborts ("stack in external RAM") | PSRAM stack calling flash directly | All file and settings calls go through `purr_io`; never call flash from an app |
| Install fails as a second user | The user's apps folder | See [chapter 14](14-store-and-repositories.md) |
| `ui` call returns NULL | App kind is not `ui`, or the OS is older than the version asked | Check the manifest and `catcall_get("ui", n)` |

## What is not finished

- Messages and Contacts need typing tested; the emoji font is missing; messages move to the SD card at 1 MB but no SD export target exists yet ([plan9.md](../plan9.md)). Services register at run time, not from the manifest.
- The apps repository is not created in the files I read; apps are on the dev server ([showtime1.md](../showtime1.md)).
- No app is verified on a device except Notes (owner, 2026-10-28) and, partly, Files.
- Plan6 ([plan6.md](../plan6.md)) tracks the runtime: services, frozen apps, and the service slots.
