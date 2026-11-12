# Cross-platform work (handoff between the Windows agent and the Mac / OS-source agent)

One place to record what changed on which platform, and when it is confirmed on both. Each agent adds to it and does not rewrite the other's part. Branches, not `main`: the owner merges.

## Confirmation status

| Check | Windows | Mac |
|---|---|---|
| purrstrap and sdk test suites pass | yes (2026-11-11: 168 and 374, 62 skipped) | pending |
| `purr doctor` on a freshly built kit | yes (all checks; 2 notes) | pending |
| A whole system compiled from source, no workarounds | yes (21 min, exit 0, 16 MB image) | pending |
| `purr system test` on that image in the emulator | yes (6 checks, 26 s) | pending |
| The emulator window opens and closes cleanly | yes | pending |
| slirp (`-netdev user`) in the QEMU | yes, alive (not carrying a guest) | on by design in `qemu/build.sh` (static libslirp); not checked by the Windows agent |
| Kit carries the prebuilt parts, fetched from the OTA release (`build-kit.py`, no local release folder) | yes (2026-11-12: 18 files, 14.9 MB, hashes match, `doctor` finds them, kernel signature ok) | pending (the Mac normally has a local `PURR-OS-release-*`, so the fetch is not used there; check `--no-fetch`/`--images` still behave) |
| Kit carries `LICENSE` (GPL-3.0), `NOTICE.txt`, `THIRD_PARTY.md` (no more `LICENSE-TODO`) | yes | pending |
| A guest reaches the host at `10.0.2.2` / `purr run` | not run (needs a VM image) | pending |
| Flash to a device | not run (the owner will do it at a later bench test) | pending |

Mark a Mac cell "yes (date, numbers)" only when it was run.

## The Windows side, 2026-11-09 to 2026-11-11

*Windows agent, on one Windows 11 PC (Intel Core i5-13420H). Written for the agent that works on the Mac and the OS source, so it can repeat each check there and so both platforms can be confirmed against the same steps. Everything below was run; "not run" says so.*

## What is on `main` (pushed before the "use branches" rule), and what is on a branch

| Commit | Where | What |
|---|---|---|
| `b150dad1` | main (yours) | pull replaces a read-only tree, ESP-IDF found as a plain checkout, test fixes: **verified on Windows**, see below |
| `deb292fd` | main (mine) | two `purrstrap` fixes: compiler path `.EXE`, `idf.py` start |
| `e341825e` | main (mine) | the emulator on Windows: `emulator.py`, `build-kit.py` branch, tests |
| `f9ae30fd` | main (mine) | `docs/22-building-on-windows.md` rewritten |
| branch `windows-emulator-slirp` | this note, notices, doc | the slirp build (UCRT64): not merged |
| emulator repo, branch `windows-qemu-slirp` | `qemu/apply.sh`, `qemu/build.sh`, `qemu/README.md` | slirp on Windows: not merged (the earlier `windows-qemu` was merged as PR 4) |

## Files I changed, and why (so a merge or a Mac run does not surprise you)

PURR OS repo:
- `purrstrap/scripts/modules.py`, `bootpkg.py`: new `_lower_exe(path)`, used by `find_tool`. On Windows `shutil.which` returns `...gcc.EXE`; ESP-IDF's compiler drivers pick their chip configuration from their own file name, and with `.EXE` they fall back to the default (big-endian) one, so `-lgcc` took the top-level `libgcc.a` ("compiled for a big endian system and target is little endian") for any module that needs 64-bit division (`sysinfo`). Does nothing off Windows.
- `purrstrap/scripts/coreos.py`: new `_idf_py_command`, used by `wrap` for `kind == "active"` **on Windows only**. A bare `idf.py` resolved to the script, not the installer's `idf.py.exe`, and `CreateProcess` answered `[WinError 193]`. It now runs the launcher by its full path, or the script with the ESP-IDF Python (`IDF_PYTHON_ENV_PATH`, else `python` on PATH). `import sys` added.
- `sdk/purrsdk/emulator.py`: `find()` no longer returns `None` on Windows; names are `esp32-emulator.exe` / `headless_tap.exe` there (`_GUI_NAMES`, `_HEADLESS_NAMES` are chosen by platform; on the Mac they are unchanged). `Headless.start` uses `CREATE_NEW_PROCESS_GROUP` and a Windows **job object** (`_WinJob`, kill-on-close) instead of `start_new_session`/`killpg`; `_group_alive` and `_reap_group` branch on `os.name`. `TMP`/`TEMP` are set beside `TMPDIR`. `available()` lost its "not on Windows" answer. POSIX code paths are the same code as before.
- `sdk/studio/server.py`: `_emulator_status` lost its Windows-only message.
- `sdk/build-kit.py`: `IS_WIN` branch calls the new `bundle_emulator_windows` (lays a pre-built emulator folder out as `emulator\`, `emulator\vendor\qemu\bin\` with the DLLs, `...\share\qemu\` with the ROMs, `boards\`, `licenses\`, `SOURCE.txt`, `run-emulator.cmd`); new option `--emulator-win DIR` (or the newest `dist/release/esp32-emulator-windows-x86_64-*` of `--emulator-src`); it checks the folder's `SHA256SUMS.txt`. The Mac branch (`bundle_emulator`) is untouched.
- `sdk/NOTICES-emulator.md`: new section 5 (Windows build: DLL table, unsigned, Smart App Control, slirp).
- Tests: `sdk/tests/helpers.py` gets `RUNNER_NAME` and `write_fake_runner` (on Windows a `.cmd` around a Python file stands in for `headless_tap.exe`); `test_emulator.py`, `test_vmtest.py`, `test_studio_emulator.py` use it and **no longer skip on Windows** (the `skipIf(os.name == "nt")` lines are gone; `_HEADLESS_NAMES` is patched to the fake's name). `process_alive(pid)` replaces `os.kill(pid, 0)` in one test (on Windows `os.kill` with a signal number ends the process).

Emulator repo (branch `windows-qemu-slirp`): `qemu/apply.sh` asks meson for the shared libslirp on Windows (`static: host_os != 'windows'`; macOS keeps its static one), `qemu/build.sh` enables slirp on Windows when `pkg-config --exists slirp` succeeds (else disables it), `qemu/README.md` says how.

Not touched at all: anything under `PurrOS/`, signing, the release process, the VM image.

## What was run on Windows, with results

- Tests at `deb292fd` (before the emulator work): purrstrap 168 OK (1 skipped); sdk 374 OK (117 skipped). After `e341825e`: sdk 374 OK, **62 skipped** (the emulator tests now run against the fake runner).
- Kit: `python sdk\build-kit.py --out <dir> --emulator-src C:\emu`: 386 MB without the emulator, **472 MB with it** (toolchain 221 MB, python 162 MB, emulator 85 MB). `purr doctor`: all checks pass; "Emulator: Found (window, headless runner, board files)"; two notes (no VM image, no prebuilt parts).
- A whole system from source, clean workspace (`C:\p`), fresh keys (boot id 6, developer id 7), fresh pull of `deb292fd`, in an ESP-IDF-exported shell with the default `PATHEXT` (no workarounds): `purr system build` **exit 0 in 21 minutes** (below-normal priority; load about 85 %, clock 2.1 to 4.0 GHz, no throttling), `FullTest-tdeck_plus.img` 16 MB, 13 delivered files match their manifests.
- `purr system test` on that image, in the bundled emulator: **26 to 27 seconds**, six checks pass (bootloader, boot package verified "signed by key 6", no crash, system started, screen 6 % lit, got as far as the recovery loader). No emulator process left over (checked with `Get-Process`).
- The emulator window (`esp32-emulator.exe` via `emulator.open_window`) opens on that image, starts QEMU, and closes cleanly.
- Windows QEMU (emulator repo): built twice. MINGW64 (no slirp): 1,454 steps. UCRT64 (slirp): 1,455 steps, `slirp support: YES 4.9.7`. UCRT64 package: `-netdev help` lists `user`; `info usernet` shows a host-forward entry and the forwarded host port listens; `cargo test -p qemuctl` 33 pass; the ROM boots from an extracted copy of the zip with only `C:\Windows` on the PATH.

## Not run (please do not read these as passed)

- A guest getting an address and reaching the host at `10.0.2.2` (needs a VM-flavour image with the W5500 board; none exists in the kit). So `purr run` and the dev-server road are untested on Windows; slirp is only shown to be alive.
- `purr run`, `purr emulator` with a VM image; flashing; a real device; `purr release` and prebuilt parts (`images\` is empty in a kit built here); the host C tests; signing a release.
- The Rust programs on a PC without the Visual C++ Redistributable; any PC with Smart App Control on (it blocks the unsigned programs). The owner plans a Hyper-V VM with Secure Boot for the clean-PC check, and a browser check of Studio.

## To confirm the same on the Mac (so both are confirmed against the same steps)

1. `python3 -m unittest discover -s purrstrap/tests` and `-s sdk/tests -t sdk/tests`: expect OK; the Mac should show the **same test count** as before my changes plus the `FindToolTests` (3) and the new `_idf_py_command` tests (6) in purrstrap (168 total on Windows). The three emulator test files should behave as before (the fake runner is the same on POSIX).
2. `bin/purr doctor` on a freshly built kit: nothing there should have changed.
3. `purr system test <system>` on a Mac-built image: same six checks.
4. Anything in `emulator.py` that looks different on the Mac is a bug of mine: the only intended Mac-visible change is that `_GUI_NAMES`/`_HEADLESS_NAMES` are now built from a module-level `_WINDOWS` flag.

## Kit licence and prebuilt parts (branch `windows-kit-licence-and-parts`, 2026-11-12)

Owner asked for the licence to be settled and for the prebuilt parts to come from the OTA release, which keeps them as separate files.

- **Licence.** The repository already says GPL-3.0 (`LICENSE`, `README.md`, `OTA/legal.txt`); only the kit still carried a "not chosen" placeholder. `sdk/build-kit.py` now writes `LICENSE` (a copy of the repo's), `NOTICE.txt` (GPL-3.0, where the source is, the Apache-2.0 shell, the third-party parts) and `THIRD_PARTY.md`, and no longer writes `LICENSE-TODO`. The owner should confirm GPL-3.0 is meant for the kit too (plan5.md still lists the kit licence as undecided).
- **Parts.** `build-kit.py`: new `fetch_release_parts(base, dest)`, `--parts-url` (default the JumpingJaguar1 release folder, or `PURR_PARTS_URL`), `--no-fetch`. Used only when `--images` is not given and no local `PURR-OS-release-*` is found, so the Mac's usual path is unchanged. It downloads `recovery.manifest`, each listed file and `legal.txt`; each file is checked against the manifest's size and sha256 (mismatch stops the build; a file name with a path is refused); it writes `images/SOURCE.txt` and records the fetch under `"parts"` in `SDK-MANIFEST.json`.
- **`sdk/purrsdk/release.py`: `prebuilt()` now also uses `image.find_parts`** (additive: after the plain `kernel_<board>.cat` names, it asks the folder's `recovery.manifest`). The OTA files carry a hash in their name (`kernel-tdeck_plus.10a35db4cb.cat`), so without this `doctor` and `purr release` would not have found them. Same code runs on the Mac.
- Tests: `sdk/tests/test_kit_parts.py` (6: write and describe, wrong hash, path in a name, empty manifest, missing legal text, `prebuilt` finds a fetched folder). SDK suite: 380 OK, 62 skipped.
- Mixed releases: the OTA folder also holds `waveshare154` parts and the `ui_*` parts; they are mirrored too and not used by `release.prebuilt`.
- The release was built with `withdrawn_versions=*@1.0.24` in its manifest; the kit copies the manifest unchanged.

## Windows-only behaviour worth knowing

- The desktop may be redirected by OneDrive: the visible Desktop is `C:\Users\<name>\OneDrive\Desktop`, while `%USERPROFILE%\Desktop` is another folder.
- Heavy builds can make a laptop throttle (the owner's PC once dropped to 400 MHz): run them at below-normal priority and watch the effective clock (`% Processor Performance` times the base MHz); the ACPI temperature counter is a constant and useless.
- PowerShell 5.1 pitfalls met while scripting: `Set-Content -Encoding utf8` writes a BOM; embedded double quotes in native arguments are mangled (use `git commit -F file`); `Start-Sleep` is blocked in this harness.
- MSYS2's MINGW64 environment has **no libslirp package** (UCRT64 and CLANG64 do). The Windows QEMU is therefore built in UCRT64.
