# 24. Not built yet

Everything below is described in a `SPEC.md` and **does not exist as code** (or exists only as a proof). It is listed so nobody builds on it by accident and so the
gap between the specs and the system is in one place. Where something is partly there, the second column says exactly how much.

Legend: **none** no code at all; **proof** a spike or a standalone demo, not integrated; **types only** a header or a host-tested function that nothing calls.

## Boot and security

| Feature | Spec | State |
|---------|------|-------|
| Per-image signature check of `kernel`, `kittenos`, `loader` at boot | `bootloader/SPEC.md` 3, 6 | none. Only the `0xE9` magic byte is checked. Two designs written down (metadata slot, or an ESP image inside a container). |
| Handoff struct from bootloader to OS (RTC memory) | `bootloader/SPEC.md` 7, `coreos/SPEC.md` 3.2 | types only. Not written, not read. |
| Decision table (continue, warn, recovery, prompt, degraded) | `coreos/SPEC.md` 5 | types only: `purr_decide` has 460 host checks and no caller. |
| `purr_coreos_boot()`, boot report, self-check of the running image | `coreos/SPEC.md` 4 | none |
| `IGNORE_ONCE`, `SECURE_OFF_ONCE`, `UPDATE_KEY` flags | `bootloader/SPEC.md` 5 | types only (bits defined) |
| Changing `secure_mode` or keys at run time through a signed request | `bootloader/SPEC.md` 5.1 | none |
| eFuse Secure Boot burn with three arm flags across three reboots | `bootloader/SPEC.md` 8 | none (the boot menu only **reads** the fuse) |
| Vendor certificates, per-vendor scope, revocation list, `keys cert`, `keys revoke` | `Keys/SPEC.md` 4 | none |
| Certificate expiry and the authenticated time floor (`latest_time`) | `Keys/SPEC.md` 5 | none |
| Owner key, system key in use, published test key for previews | `Keys/SPEC.md` 2, 6 | none. The bag has only `boot` and `developer`. |
| YubiKey (PIV) signer | `Keys/SPEC.md` 3 | none |
| Flash encryption for saved Wi-Fi and the recovery record | `Network/SPEC.md` 1 | none |
| Version-floor check for partition-level installs | `OTA/SPEC.md` 3 | none (file-staged only) |

## Kernel and drivers

| Feature | Spec | State |
|---------|------|-------|
| Driver registry (linker-section registration, `compatible` matching, device states) | `kernel/SPEC.md` 5 | none |
| Pin registry and bus layer with locks | `kernel/SPEC.md` 6 | none |
| Catcall registry, legacy and native generations, bridging adapters | `kernel/SPEC.md` 7 | none (only the display struct) |
| Board profile as data in a signed devices bundle | `kernel/SPEC.md` 3, 13; `Drivers/SPEC.md` | none. Profiles are C files. |
| Data-description drivers from an SD card; user code drivers | `kernel/SPEC.md` 14 | none; file format undecided |
| Driver packs with pinned hashes and per-board pins | `Drivers/SPEC.md` | none |
| Hardware discovery: `i2cscan`, `spiprobe`, device database, discovered profile | `Install/SPEC.md` 3, 4 | none (a worked `i2cscan` is in [13](13-writing-drivers.md)) |
| Display `set_rotation`, `blit_async`, `wait_idle` | `kernel/SPEC.md` 8 | none |
| Touch (CST816S), SD card, trackball, LoRa, GPS, audio, battery, RTC | `kernel/SPEC.md`, `Boards/`, `Catcalls/` | none |
| Second display, multiple input devices | | none ([15](15-adding-a-second-display.md) is a patch, not in the tree) |
| Hot-plug | `kernel/SPEC.md` 2 | none |
| ESP32 (non-S3) support: boot package RAM window, CYD board | `Boards/`, `PurrOS/SPEC.md` 4.2 | none ([F-01](FINDINGS.md#f-01)) |
| e-paper display and its refresh policy | `Boards/SPEC.md` 2 | none |

## CoreOS and the shell

| Feature | Spec | State |
|---------|------|-------|
| Pipes, redirection, `;`, `&&`, `||`, `&`, variables, scripts, comments | `coreos/SPEC.md` 3.7 | none |
| `log`, `status`, `report`, `clear-failures`, `sh`, `ps`, `top`, `kill`, `stop`, `cont`, `fg`, `bg`, `run` | `coreos/SPEC.md` 3.7, `AppManager/SPEC.md` 8.1 | none |
| In-RAM log buffer | `coreos/SPEC.md` 3.1 | none |
| Memory pressure service and out-of-memory killer | `coreos/SPEC.md` 3.6 | none |
| CoreOS as a **relocatable file loaded by the kernel** | `PurrOS/SPEC.md` 6 | proof. All 16 source files built as one blob and a tiny entry point loaded and run by `Kernel/`. The real system is still one monolith. |
| Loading a swapped `/boot/coreos.kitt`, AppManager and runtimes from `/boot` | `coreos/SPEC.md` 6 | none (the swap itself works) |
| Kernel module "driver" subtype loading real hardware drivers | `Drivers/SPEC.md` 4 | none |
| A real splash screen in place of the verbose boot | `Kconfig.projbuild` | none |
| `plantmodules` and the embedded module bytes retired | `coreos/SPEC.md` 10 | still the dev loop |

## Apps

| Feature | Spec | State |
|---------|------|-------|
| App runtime (`catrt`), multitasking, foreground and background, OOM killer | `AppRuntime/SPEC.md` | none |
| Catcalls for apps: `console`, `input`, `sched`, `app`, `storage`, `net`, `wifi`, `radio`, `gps`, `gpio/i2c/spi`, `system`, `audio`, `appmgr`, `update`, `ui` | `Catcalls/SPEC.md` | none |
| `.cat` payload table, manifest block, relocation list | `CatFormat/SPEC.md` | none (installation treats the payload as opaque bytes) |
| App SDK, toolchain wrapper, templates, `purrstrap apps` | `AppSDK/SPEC.md` | none |
| Permissions: install-time list, first-use prompts, grants, `perms`, `grant`, `revoke`, `superuser` | `Permissions/SPEC.md` | none |
| Privileged apps and the owner-key gate | `Permissions/`, `AppSDK/` | none |
| MTP, SD and serial transports | `AppManager/SPEC.md` 7 | none |
| App repo, generated index, OTA app, app storage app | `OTA/SPEC.md` 6, 8; `Transfer/SPEC.md` | none |
| Device-to-device transfer (Wi-Fi, ESP-NOW) | `Transfer/SPEC.md` | none |
| MicroPython runtime and the `python` command | `MicroPython/SPEC.md` | none |

## User interface

| Feature | Spec | State |
|---------|------|-------|
| LVGL and MiniWin backends, the `ui` catcall, widgets, canvas | `UI/SPEC.md` | none (the text console is the only UI) |
| Themes as signed `.kit` packages in a `themes` partition | `UI/SPEC.md` 3 | none |
| System status bar, graphical launcher, task manager | `UI/SPEC.md` | none |
| On-screen keyboard for touch boards | `UI/SPEC.md` 5 | none |
| KittenOS update UI | `KittenOS/SPEC.md` 8 | none |

## Network

| Feature | Spec | State |
|---------|------|-------|
| `ping`, `nslookup`, `fetch <url>` | `Network/SPEC.md` 1 | none (HTTPS exists only inside installers) |
| Sockets for apps, TLS for apps | `Catcalls/SPEC.md` 5 | none |
| Bluetooth | `Network/SPEC.md` | none |
| Network time (NTP, authenticated time) | `Network/SPEC.md` 1, `Keys/SPEC.md` 5 | none |
| Hotspot mode, captive portals, enterprise Wi-Fi, static addresses, IPv6, power saving | `Network/SPEC.md` 6 | none |
| Provisioning without a keyboard (serial console, `wifi.conf` on SD) | `Network/SPEC.md` 3 | none |
| Remote access | `Users/SPEC.md` 6 | none (pinned until a UI exists) |

## Updates and install

| Feature | Spec | State |
|---------|------|-------|
| `update check`, `update install`, `update status`, `update cancel`, `rollback` | `OTA/SPEC.md` 7 | none (only `netinstall`) |
| The system manifest (JSON) with changelog and channels | `OTA/SPEC.md` 5 | none |
| Download resume (range requests), free-space check | `OTA/SPEC.md` 4 | none |
| An "update KittenOS" boot-menu entry, "Internet recovery" while something boots | `RecoveryLoader/SPEC.md` 8 | none ([F-32](FINDINGS.md#f-32)) |
| Monolithic-board update through an SD card | `PurrOS/SPEC.md` 6.1 | none |
| Install profiles per board in the manifest, installing apps from the repo during install | `Install/SPEC.md` 1 | none |
| Serial-only fallback install | `Install/SPEC.md` 5 | none |
| Reinstall policy ("how many `.bak` copies", factory reset) | `KittenOS/SPEC.md` 8 | undecided |
| Live end-to-end run of the network install against a published release over real Wi-Fi | `OTA/SPEC.md` 3 | the project's notes say it has not been done |

## Accounts

| Feature | Spec | State |
|---------|------|-------|
| Auto-login boards and the ordinary auto-login user | `Users/SPEC.md` 1 | none (`login_skip` exists for KittenOS only) |
| Groups, password rules, per-user quotas, screen lock, switching users | `Users/SPEC.md` 8 | none |
| File ownership and permissions | `Users/SPEC.md` 2 | none ([F-10](FINDINGS.md#f-10)) |
| Reset of a forgotten admin password | `Users/SPEC.md` 8 | none |

## Process

| Feature | State |
|---------|-------|
| Continuous integration | none |
| Automated hardware tests | none |

Next: [FINDINGS](FINDINGS.md).
