# PURR Developer Kit 2.0: the set of tests

One list, run first on the Mac bench (by Claude, 2026-11-09) and then by hand on the Windows machine. Each test says how to run it, what a pass looks like, and the Mac result.
Where a test needs a password or a key, use **throwaway keys** made for the test (never the project's keys) and type the passwords yourself.
Windows notes: the compile needs ESP-IDF 5.3.5 installed with Espressif's installer; run everything from the kit's `PURR-Studio` shortcut or `bin\purr`. Use a **short workspace** (`PURR_WORKSPACE=C:\p`): the pulled source and the build folders are deep and Windows stops at 260 characters. Every test below can also be run from a terminal with `purr system ...` and `purr source ...` ([10-cli.md](docs/10-cli.md)); the virtual-device test (15) needs the emulator, which the Windows kit does not have. Anything marked **(Windows: unknown)** has not been run on Windows by anyone yet.

Result words: **pass**, **fail** (and what), **not run**.

| # | Test | How | A pass looks like | Mac bench |
|---|------|-----|-------------------|-----------|
| 1 | Version | `bin/purr version` | `PURR Developer Kit 2.0.0`  **pass** (2.0.0) |
| 2 | Doctor | `bin/purr doctor` | no `bad` lines (warnings about missing hardware are fine)  **pass** (all green; two `i` notes: no VM image, no prebuilt parts in this checkout) |
| 3 | Unit tests | `python -m unittest discover -s sdk/tests` (kit: its own Python) | `OK`, the number of skipped tests is only those that need a tool the machine lacks  **pass** (SDK suite 371 tests; purrstrap suite 158 with the kit's Python, which has `cryptography`; skips are tests needing a tool the machine lacks) |
| 4 | Studio opens | start `PURR-Studio`, open the link | the main menu shows, no red error  **pass** (device and build tabs rendered headless; the real browser window was not opened by Claude) |
| 5 | Keys | Studio, keys: make a **boot** key (id 7) and a **developer** key (id 6), each with a password | `<name>.key` and `<name>.pub` for each in the workspace's keys folder  **pass** with `purrstrap keys generate` (boot id 7, developer id 6); the Studio keys page was not clicked |
| 6 | Compiler found | build a system, device step, "compile from the newest source" | it says `Compiler: ESP-IDF 5.3.5` **(Windows: unknown: found through the installer's manifest)**  **pass** (ESP-IDF 5.3.5 found at `~/esp/esp-idf-v5.3.5`) |
| 7 | Pull | same box, branch `main` (the default; PR #10 merged `PURR2.0` into it, so the Android shell config is there), "check for something newer", then build | step 1 passes; a second build says "already the newest" and fetches nothing  **pass** (pulled `PURR2.0` from a local repository; a second build fetched nothing; GitHub `main` stops with `sdkconfig.shell.android is missing`) |
| 8 | Key choice is checked | give the developer key id 4 while "Keep PURR's keys" is on | the page lists the problem (ids 6 to 31) and Build refuses with the same sentence  **pass** (unit tests; the real build refuses before pulling) |
| 9 | Version is checked | set the version to `src-1` | Build refuses before pulling anything ("numbers separated by dots")  **pass** (unit tests; refuses before pulling anything) |
| 10 | Build, PURR's keys kept | toggle on, boot key 7, developer key 6, both passwords | every step passes; image is 16 MB; the key list in `source/tree/PurrOS/components/coreos/keys/purr_default_keys.c` has ids 1, 2, 3, 4, 5, 6, 7  **pass** (key list 1 to 7; image 16 MB; every step done) |
| 11 | Build, PURR's keys out | toggle off, build again | every step passes; the key list has only ids 6 and 7  **pass** (key list 6 and 7 only; image 16 MB) |
| 12 | Signatures | `bin/purr keys verify --key <own boot .pub> --image builds/system-<name>/parts/kernel-<board>.cat`, then with a project key | verifies with your key, fails with PURR's (when the toggle was off)  **pass** (kernel and every app verify with the throwaway key; the kernel of the toggle-off build failed with PURR's boot key, an app failed with PURR's developer key) |
| 13 | Everything compiled | look at the build's checklist | firmware (3 compiles), baked-in binaries (14) and the Store, 8 system modules, drivers, apps: all `done`, none ready-made  **pass** (3 compiles, 14 baked-in binaries and 2 Store apps, 8 modules, 8 drivers, 13 source apps and one own app) |
| 14 | Delivery check | last step of the build, "Check every file the manifests name" | `N files match their manifests`  **pass** (36 files match: the three manifests and the app index) |
| 15 | Virtual device | deliver tab, "test it automatically" | all checks ok: bootloader starts, boot package verified **signed by your key id**, kernel slot ok, no crash, screen drawn, reached the recovery loader. No `bad-version` or `display did not come up` in the log  **pass**, three times (toggle off, toggle off with all apps, toggle on): boot package verified signed by key 7, kernel slot ok, no crash, screen drawn, recovery loader reached, zero rejection lines in the log |
| 16 | Apps | tick your own app project and the source's apps, build | the apps step and the source-apps step pass; every `.cat` verifies with your developer key  **pass** (13 source apps and `myhello` compiled and signed; all verify with the developer key; not run on a device) |
| 17 | Flash by cable | deliver tab, by cable (a T-Deck on its port) | the board reads back and restarts **(hardware: not run by Claude)**  not run (needs a T-Deck on a cable) |
| 18 | Own image still takes PURR's update | toggle ON image on a device; OTA a PURR release | the update installs **(hardware: not run by Claude)**  not run (needs a device and an OTA) |
| 19 | Own image refuses PURR's update | toggle OFF image on a device; OTA a PURR release | the device refuses it **(hardware: not run by Claude)**  not run (needs a device and an OTA) |

## If a boot test fails
Read the serial log (the test saves `s-serial.txt` next to its screenshots) for the **first** line that rejects something: `bad-version`, `bad-sig`, `no-key`, `slot ... did not verify`. The last line (`display did not come up`) is only the symptom. See `NOTES-compile-from-source.md`.

## Failure messages (bench, real tools)
A wrong key password stops at the first signing step with git/purrstrap's own words; a branch that is not there, an unreachable source address and a key with no `.pub` each stop at their step with a plain sentence naming the cause.

## What is not covered
Apps running on a device or in the emulator (needs the VM-flavor image and a populated file system); the trust block (`purrcfg`) is still left blank; Windows is unverified for everything marked above.
