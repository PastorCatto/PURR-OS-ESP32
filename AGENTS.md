# Notes for AI coding agents (and for people in a hurry)

PURR OS is an operating system for small ESP32-S3 devices (the LilyGO T-Deck Plus and T-Deck; others are in progress) with signed images, sandboxed apps and interchangeable shells. This file gets any agent up to speed so it does not have to guess at each folder. It does not depend on any particular tool.

## Read in this order
1. `README.md`, then `docs/start-here.md` (pick the path that matches the task), `docs/glossary.md`, and `docs/runbook.md` (every working procedure, and "Traps that have already cost time").
2. **The plan file for the area you touch.** The plans (`plan1.md` to `plan10.md`) are the source of truth for what is done and what is not. If a plan and the code disagree, fix one of them in the same step, and update the plan as you work.
   - `plan1.md` boot chain · `plan3.md` hardening, release channels, key rotation, the `drivers` component · `plan5.md` SDK · `plan6.md` app runtime · `plan7.md` the shells (Windows 10 Mobile style, Android/Froyo style, Type) · `plan8.md` repositories and the Store · `plan9.md` ui and files catcalls · `plan10.md` power. `plan2.md` and `plan4.md` are older areas; their first lines say what they cover.
   - New work goes into an existing plan. `plan11.md` exists only because of an explicit decision by the maintainer (the display link, design only); do not add `plan12.md`.
3. The `SPEC.md` in the component's folder (`Drivers/`, `Keys/`, `OTA/`, `bootloader/`, `UI/`, `purrstrap/`, ... 26 in all). These are the written contracts.
4. `sdk/NOTES-compile-from-source.md` before you touch `sdk/purrsdk/source.py` (it records every mistake made on that road), and `sdk/TEST-PLAN-2.0.md` before a merge.
5. Only then the code.

## Where things are
| Path | What it is |
|---|---|
| `PurrOS/` | The firmware (ESP-IDF 5.3.5). `PurrOS/main/` is the application layer; `PurrOS/components/coreos/` is the core library, testable on the host (`test/host/`) |
| `bootloader/`, `bootpkg/` | The second-stage bootloader and the boot package |
| `KittenOS/`, `RecoveryLoader/`, `Kernel/` | The recovery system, the internet loader and the kernel slot |
| `purrstrap/` | The build and signing tool: `python3 purrstrap/purrstrap.py ...` (tests in `purrstrap/tests`) |
| `sdk/` | The developer kit: `purr.py`, `purrsdk/`, `studio/` (PURR Studio), `docs/`, `tests/` |
| `Apps/`, `Modules/` | Sources of apps, drivers and system modules |
| `tools/` | Release scripts (`release/`), VM image scripts (`vm/`), hardware image scripts (`hw/`), UI tools |
| `UI/` | UI specs, fonts, icon packs and the design drafts |
| `docs/` | The numbered manual (01 to 22), glossary, runbook, start-here |
| `archive/`, `PURR-OS-0.11/`, `CoreOSSpike/`, `ModuleSpike/` | Old work kept for reference. Do not extend it. |

Two sibling repositories belong to the project: `PURR-OS-HUB` (the static files and signed indexes devices read; start with its `HUB.md`) and `ESP32-Emulator` (a QEMU based emulator with a Rust front end; see its `README.md`).

## Build and test
- ESP-IDF 5.3.5: `. ~/esp/esp-idf-v5.3.5/export.sh` (on Windows, the ESP-IDF installer and `export.ps1`).
- Host tests, fast, no device: `cd PurrOS/components/coreos/test/host && IDF_PATH=$HOME/esp/esp-idf-v5.3.5 python3 run.py [name]`.
- `cd purrstrap && python3 -m unittest discover -s tests` and `cd sdk && python3 -m unittest discover -s tests`. The signing tests need the `cryptography` package; the sample-app build needs Python 3.11 or newer (`tomllib`).
- Firmware builds are `ninja -C out/<dir>` (see `docs/runbook.md` for the directories). A full build takes over ten minutes: run it in the background, and do not start a build or the emulator unless you were asked to.

## Rules that are easy to get wrong
- **Secrets.** Never put a passphrase, a private key or a private hardware detail in a file, script, commit message, plan or log. A signing passphrase is given to one command, in the environment of that command, and nowhere else. Key files are never committed (`*.key` and `*.pem` are ignored).
- **A locked (dev-locked) device is never weakened on the device.** Do not add an unlock path.
- **Apps are freestanding modules** with no libc, loaded into PSRAM. A `switch` makes a jump table, which the relocator must treat as code (`purrstrap/scripts/modules.py`).
- **Internal RAM is the scarce resource.** An app task, its queue and its semaphores live in PSRAM; flash access goes through `purr_io`; tasks that start hardware keep an internal stack (`docs/06-app-runtime.md`, "Stacks and the PSRAM rule").
- **Apps are sideloaded** (SDK packages through the developer portal or the Store). Only Settings is built into the shell, apart from the Store and its companion.
- **The theme and the shell handle the user interface.** A layout or look change is made in the theme or the shell, never in an app. Apps name theme roles and call catcalls; the OS draws.
- **Licences are per component** (MIT, Apache 2.0, GPL 2 or 3, ...). Do not assume one licence for the whole tree: read `THIRD_PARTY.md` and the component's own notice before copying or relicensing anything.
- **Art is original.** Do not add icons, fonts or images copied from a company's product; `THIRD_PARTY.md` lists what is used and under which licence.
- **Release artefacts are signed and exact.** After publishing a release set, fetch every file a manifest names and compare size and sha256. Re-sign with `tools/release/resign_release.py`; do not edit a signed file by hand.
- **Say what you did not verify.** If a test was not run, or something was only compiled and never run on a device, write that in the plan and in your report.

## Working method that has paid off here
- Prefer small, separately committed steps (one feature per commit), with the host tests run after each.
- When something is only written and compiled, say so; when it has run on the real device, say that. The plans use the words "host-tested", "compiled for syntax" and "run on a device" for these three levels.
- Keep the plan files current as you go; they are what the next person (or agent) reads first.
