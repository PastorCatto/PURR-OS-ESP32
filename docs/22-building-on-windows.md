# 22. Building on Windows

*Current as of PURR OS 1.1 and SDK 2.0, run on Windows 11 on 2026-11-09 to 2026-11-11.*

## Short version

- The project was written on a Mac. This chapter was first written without a Windows computer in reach; since then every step below was **run on a Windows 11 PC** (Intel Core i5-13420H), and the page is corrected to what happened.
- **Works on Windows:** the Python tools (`purrstrap`, the SDK), both test suites, building the developer kit, `purr doctor`, building an app, pulling the newest source, **compiling a whole system from source** (kernel, recovery, KittenOS, parts, signed 16 MB flash image) in about 21 minutes, and **the emulator**: the kit carries a Windows build of the patched QEMU, and `purr system test` boots the image you just built in it (27 seconds, checked: bootloader, boot package signature, no crash, system start, screen, recovery loader).
- **Not run on Windows yet:** flashing over a COM port, booting the finished image on a real device, `purr release` and the prebuilt parts (they come from a release folder kept on the Mac), the host C tests, and `purr run` (it needs the VM flash image, `images\vm-flash.bin`, which is kept on the Mac).
- **What does not work:** the `zsh` scripts in `tools/` (use WSL or Git Bash, or the Python commands they wrap).
- Signing keys and passphrases follow the same rules as on the Mac: private keys stay in an ignored folder and are never committed; a passphrase is given to one command at a time, through the environment, and cleared after.

## What it is for

A Windows PC that builds the developer kit, compiles the firmware, and builds a signed system image. This page lists what to install, the commands in order, what was measured, and what went wrong on the way.

## The files

| File | What it is for |
|---|---|
| [sdk/build-kit.py](../sdk/build-kit.py) | builds the developer kit; on Windows it makes `purr-sdk-<version>-windows-x86_64`, with `bin\purr.cmd`, `PURR-Studio.cmd` and, when it finds one, the Windows emulator in `emulator\` (`--emulator-win`, see below) |
| [sdk/purrsdk/emulator.py](../sdk/purrsdk/emulator.py) | finds and drives the emulator; on Windows it looks for `esp32-emulator.exe` and `headless_tap.exe`, and stops a headless run (the runner and its QEMU) with a Windows job object instead of a POSIX process group |
| [sdk/purrsdk/flash.py](../sdk/purrsdk/flash.py) | finds serial ports (on Windows through `pyserial`, which `esptool` brings along) and flashes |
| [purrstrap/purrstrap.py](../purrstrap/purrstrap.py) | the build and signing tool |
| [purrstrap/scripts/modules.py](../purrstrap/scripts/modules.py), [bootpkg.py](../purrstrap/scripts/bootpkg.py) | `find_tool` returns the compiler with a lowercase `.exe` (see "What went wrong") |
| [purrstrap/scripts/coreos.py](../purrstrap/scripts/coreos.py) | finds ESP-IDF (also a plain checkout such as `C:\esp-idf-5.3.5`) and starts `idf.py` the Windows way |
| [CLAUDE.md](../CLAUDE.md) | the build directories and rules that apply everywhere |

## How to set it up

1. **Git settings, before cloning:** `git config --global core.longpaths true` and `git config --global core.autocrlf input`. Both are `--global`, so no admin rights are needed. `autocrlf input` keeps LF in scripts and hashed files.
2. **Install** (all of these worked through `winget`): `winget install Git.Git` and `winget install Python.Python.3.12`. If other Pythons are installed, the kit expects 3.12: use `py -3.12`.
3. **ESP-IDF 5.3.5.** Either the **ESP-IDF Tools Installer for Windows** (v5.3.5), or, **without admin rights** (this is what was tested; `winget` offers only Espressif's newer "EIM" installer, not this one): `git clone --branch v5.3.5 --depth 1 --recursive --shallow-submodules https://github.com/espressif/esp-idf.git C:\esp-idf-5.3.5`, then `cd C:\esp-idf-5.3.5; .\install.ps1 esp32s3` (about 1 GB into `%USERPROFILE%\.espressif\tools`). To use it in a PowerShell: `. C:\esp-idf-5.3.5\export.ps1`. The kit builder copies the Xtensa compiler from `%USERPROFILE%\.espressif\tools\xtensa-esp-elf`.
4. **Clone to a short folder with no spaces:** `git clone https://github.com/PastorCatto/PURR-OS-ESP32.git C:\purr` (the branch is `main`; `PURR2.0` was merged into it).
5. **Python packages**, in the ESP-IDF PowerShell: `python -m pip install cryptography esptool`.

## How to build and test it

- **Tool tests:** `cd C:\purr\purrstrap; python -m unittest discover -s tests` and `cd C:\purr\sdk; python -m unittest discover -s tests -t tests`. Measured on Windows at `deb292fd`: purrstrap 168 tests OK (1 skipped); sdk 374 tests OK (62 skipped). The emulator tests run on Windows against a fake headless runner (a `.cmd` around a Python file stands in for `headless_tap.exe`, see `helpers.write_fake_runner`).
- **The kit:** `python sdk\build-kit.py --out %USERPROFILE%\Desktop\PURR-SDK --emulator-src C:\emu`. It downloads the Windows CPython 3.12 and the `win_amd64` wheels, copies the toolchain (221 MB), and bundles the emulator (next point). Without an emulator folder it says so and makes the kit without one (**386 MB**; with the emulator **470 MB**). The result is in a **subfolder** named for the version: `PURR-SDK\purr-sdk-2.0.0-windows-x86_64\bin\purr.cmd`. Use `bin\purr.cmd doctor` there: all checks pass, with two notes (no emulator; no prebuilt parts in `images\`). Note: when OneDrive redirects the Desktop, `%USERPROFILE%\Desktop` is not the Desktop you see (`C:\Users\<you>\OneDrive\Desktop` is).
- **The emulator:** `build-kit.py` does not compile it. It takes a folder built by the emulator project (its `docs/NOTES-windows-qemu-build.md` has the MSYS2 and Rust steps; the result is `qemu-system-xtensa.exe` with 16 MSYS2 DLLs, `esp32-emulator.exe`, `headless_tap.exe`, the ROM files, `licences\` and `SHA256SUMS.txt`), given with `--emulator-win DIR` or found as the newest `dist\release\esp32-emulator-windows-x86_64-*` in the `--emulator-src` checkout. The hashes in `SHA256SUMS.txt` are checked first. It lays the folder out as `emulator\` (the two programs), `emulator\vendor\qemu\bin\` (QEMU and its DLLs: Windows loads a DLL from the program's own folder), `emulator\vendor\qemu\share\qemu\` (the ROMs), `boards\`, `licenses\`, `SOURCE.txt` and `run-emulator.cmd`. `purr doctor` then says "Emulator: Found". The programs are unsigned: **Smart App Control must be off**, and a PC without the Visual C++ Redistributable may refuse to start the Rust programs. Not in the emulator on Windows: user-mode networking (slirp).
- **An app:** `bin\purr.cmd new app hello` then `bin\purr.cmd build hello` makes an unsigned `hello.cat` under the workspace (`%USERPROFILE%\PURR-Projects` unless you set `--workspace` or `PURR_WORKSPACE`).
- **The firmware by hand:** from the ESP-IDF PowerShell, in `PurrOS\`: `idf.py -B ..\out\stdfull -D IDF_TARGET=esp32s3 -D SDKCONFIG=..\out\stdfull.sdkconfig -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.board.tdeck_plus;sdkconfig.profile.full" build`. The flavors and profiles are the same as in [CLAUDE.md](../CLAUDE.md). (The whole-system build below does this for you.)
- **Signing and releases:** `python purrstrap\purrstrap.py keys sign ...` and `python purrstrap\purrstrap.py dev serve --dir out\devrepo --port 47801` are the same as on the Mac; not run on Windows yet. The shell scripts (`tools\vm\*.sh`, `tools\release\*`) assume `zsh` or `sh`: use WSL or Git Bash, or call the Python commands they contain.
- **Flashing:** find the port in Device Manager (a "USB Serial Device" or "USB JTAG/serial debug unit", `COM5` for instance), then `python -m esptool --chip esp32s3 -p COM5 -b 460800 write_flash 0x400000 loader_slot.bin`. Not run on Windows yet. Slot images are made with `purrstrap coreos slot-image` and the offsets are in [chapter 02](02-flash-map-and-partitions.md). The T-Deck's USB port is the chip's own USB Serial/JTAG; Windows 10 and 11 normally load a driver by themselves, and OpenOCD may need Zadig for the JTAG half.

## Compile a whole system from source (SDK 2.0)

Run on Windows on 2026-11-11 with a **short workspace** (`C:\p`; the pulled source and ESP-IDF's build folders go deep). From the kit folder, in a PowerShell (with or without ESP-IDF exported; both work):

1. Two keys, ids from 6 to 31 and different from each other (ids 1 to 5 are PURR's): `bin\purr.cmd --workspace C:\p keys --role boot --key-id 6 new` and `bin\purr.cmd --workspace C:\p keys --role developer --key-id 7 new`. The passphrase can be given as `$env:PURRSTRAP_KEY_PASSWORD` for that one command; clear it after.
2. `bin\purr.cmd --workspace C:\p system new froyo --board tdeck_plus` (the board is required), then `bin\purr.cmd --workspace C:\p system set froyo --parts-from source --shell android --boot-key boot.key --boot-key-id 6 --key developer.key --key-id 7`. The ids in the command must be the ids the keys were made with: a system that has no key chosen, or the same id for both, is refused with a sentence saying so.
3. `bin\purr.cmd --workspace C:\p source pull` (the build also pulls if needed), then `$env:PURRSTRAP_KEY_PASSWORD = "..."; $env:PURRSTRAP_BOOT_KEY_PASSWORD = "..."; bin\purr.cmd --workspace C:\p system build froyo --verbose`, in the same PowerShell, then clear them.
4. Measured: **21 minutes**, below-normal priority, 12 threads busy (about 85 % load, 2.1 to 4.0 GHz), ending in `FullTest-tdeck_plus.img` (16 MB) and a delivery folder whose 13 files match their manifests.
5. `bin\purr.cmd --workspace C:\p system test froyo` boots the new image headless in the emulator and checks its serial log and screen. Run on Windows on 2026-11-11 with the image from step 4: 27 seconds, six checks passed (the boot package verified, signed by the test boot key), and no emulator process was left running afterwards.

The whole set of tests, with the Mac result for each, is [sdk/TEST-PLAN-2.0.md](../sdk/TEST-PLAN-2.0.md). `purr source install-compiler` cannot install ESP-IDF on Windows; use the steps in "How to set it up". A heavy build can make a laptop throttle (one PC dropped to 400 MHz): run it at below-normal priority (`[Diagnostics.Process]::GetCurrentProcess().PriorityClass = 'BelowNormal'` in the PowerShell that starts it) and watch the clock speed.

## What went wrong and how to fix it

| Symptom | Likely cause | Fix |
|---|---|---|
| a build fails with a path or "file not found" in a deep folder | the 260-character limit | clone near `C:\`, use a short workspace, enable long paths |
| `purrstrap` says a hash or signature does not match for a file that is fine on the Mac | CRLF line endings | `core.autocrlf input`, clone again |
| `ModuleNotFoundError: cryptography` | the system Python has no packages | `python -m pip install cryptography` |
| the kit builder says it cannot find a toolchain | ESP-IDF's tools are not installed | install ESP-IDF 5.3.5 first, or pass `--no-toolchain` |
| `purr source pull`: `[WinError 5] Access is denied: ...\tree.new -> ...\tree`, and a `.git` left in the tree | git marks its files read-only and Windows would not delete them | fixed in `b150dad1`; on an older kit, delete `source\tree` and `source\tree.new` in the workspace by hand and pull again |
| a module link fails with `libgcc.a(_udivdi3.o): compiled for a big endian system and target is little endian` | `shutil.which` returned `...gcc.EXE`; ESP-IDF's compiler picks its chip configuration from its own file name and took the default one | fixed in `deb292fd` (`find_tool` gives a lowercase `.exe`); on an older kit set `$env:PATHEXT = ".com;.exe;.bat;.cmd"` |
| `cannot run idf.py: [WinError 193] %1 is not a valid Win32 application` | ESP-IDF was already active and `idf.py` was started directly | fixed in `deb292fd`; on an older kit run from a PowerShell where ESP-IDF is not exported |
| "Choose a boot key" / "need different ids" / "id is one of PURR's" | no key chosen on the system, or ids clash | `system set` with `--boot-key`, `--key`, and ids from 6 to 31, different, as the keys were made |
| `The workspace folder ... is long` | the default workspace is 26+ characters | `--workspace C:\p` or `PURR_WORKSPACE` |
| `An Application Control policy has blocked this file` | Smart App Control blocks unsigned programs (MSYS2, the emulator build, any unsigned `.exe`); a Defender exclusion does not help | turn Smart App Control off in Windows Security (one way: read the warning in that dialog first) |
| the kit folder is not on the Desktop you see | OneDrive redirects the Desktop | look in `C:\Users\<you>\OneDrive\Desktop` |
| no serial port appears | the board is in a state without USB, or another program holds the port | unplug, replug, close serial monitors |
| the C host tests cannot run | they need a C compiler called `cc` | run them in WSL, or install MSYS2 and put its `gcc` on the path |

## What is not finished

- **Not run on Windows:** flashing, booting the image on a device, signing a release, the dev server, the host C tests, the prebuilt parts (`images\` is empty in a kit built here) and `purr release`.
- **The emulator is in the Windows kit, with limits.** It is bundled from a folder built separately (see [chapter 18](18-emulator-and-vm.md) and the emulator project's `docs/NOTES-windows-qemu-build.md`), so a kit builder needs that folder. `purr system test` and the window (`esp32-emulator.exe`) were run on Windows; `purr run` and `purr emulator` with the VM image were not (the image, `images\vm-flash.bin`, is on the Mac). No user-mode networking (slirp) on Windows, so the emulated device cannot reach the host at `10.0.2.2`: `purr run` (the dev server road) is expected not to work until that is built.
- **No Windows installer or signed Windows binaries.** The kit is a folder; its programs are unsigned.
- The host tests under `PurrOS/components/coreos/test/host` were never run with a Windows C compiler.
