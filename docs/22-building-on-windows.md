# 22. Building on Windows

## Short version

- The project was written and tested on a Mac. This chapter is the **first Windows guide**, written on 2026-11-08 **without a Windows computer in reach**: every step below is expected to work and **none has been run on Windows**. If one fails, fix this page in the same step ([CLAUDE.md](../CLAUDE.md) rule: when a plan or page and the code disagree, fix one of them).
- What should work: the Python tools (`purrstrap`, the SDK, `hubsign.py`), the SDK tests, building the kit, building the firmware with ESP-IDF's Windows tools, and flashing over a COM port.
- What does **not** work on Windows: the emulator (the patched QEMU is not built for Windows), and the `zsh` scripts in `tools/` (run them in WSL or Git Bash, or use the Python tools they wrap).
- Signing keys and passphrases follow the same rules as on the Mac: the private keys are in an ignored folder and are never committed; a passphrase is given to one command at a time.

## What it is for

The owner has a Windows PC and wants a Windows build of the developer kit and a second place to compile the firmware. This page lists what to install, the commands in order, and the places where Windows differs.

## The files

| File | What it is for |
|---|---|
| [sdk/build-kit.py](../sdk/build-kit.py) | builds the developer kit; on Windows it makes `windows-x86_64`, with `bin\purr.cmd` and `PURR-Studio.cmd`, and no emulator |
| [sdk/purrsdk/flash.py](../sdk/purrsdk/flash.py) | finds serial ports (on Windows through `pyserial`, which `esptool` brings along) and flashes |
| [purrstrap/purrstrap.py](../purrstrap/purrstrap.py) | the build and signing tool; its terminal helper already has a Windows branch |
| [CLAUDE.md](../CLAUDE.md) | the build directories and rules that apply everywhere |

## How to set it up

1. **Short folder, no spaces.** Clone to something like `C:\purr`. ESP-IDF builds hit Windows' 260-character path limit in deep folders; also turn on long paths (`git config --system core.longpaths true`).
2. **Install:** Git for Windows, Python 3.12 (python.org, tick "Add to PATH"), and the **ESP-IDF Tools Installer for Windows**, choosing **v5.3.5** (it installs the Xtensa toolchain, CMake, Ninja and an "ESP-IDF PowerShell"). Run everything below from that PowerShell.
3. **Line endings:** `git config --global core.autocrlf input` before cloning, so scripts and hashed files keep LF. The repository has a [.gitattributes](../.gitattributes).
4. **Clone:** `git clone https://github.com/PastorCatto/PURR-OS-ESP32.git C:\purr`, then `git checkout PURR2.0`.
5. **Python packages:** `python -m pip install cryptography esptool` (the key and signature tests need `cryptography`).

## How to build and test it

- **Tool tests:** `cd purrstrap; python -m unittest discover -s tests` and `cd sdk; python -m unittest discover -s tests -t tests`. Expect the same counts as on the Mac, with the emulator tests that need a runner skipped or failing for lack of QEMU.
- **The kit:** `python sdk\build-kit.py --out %USERPROFILE%\Desktop\PURR-SDK`. It downloads the Windows build of CPython 3.12 and the `win_amd64` wheels, and copies the toolchain from `%USERPROFILE%\.espressif\tools\xtensa-esp-elf`. It prints that the emulator is left out. Run `bin\purr.cmd doctor` in the result.
- **The firmware:** from the ESP-IDF PowerShell, in `PurrOS\`: `idf.py -B ..\out\stdfull -D IDF_TARGET=esp32s3 -D SDKCONFIG=..\out\stdfull.sdkconfig -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.board.tdeck_plus;sdkconfig.profile.full" build`. The flavors and profiles are the same as in [CLAUDE.md](../CLAUDE.md). A full build takes a long time; the first one on a new machine downloads components.
- **Signing and releases:** `python purrstrap\purrstrap.py keys sign ...` and `python purrstrap\purrstrap.py dev serve --dir out\devrepo --port 47801` work as on the Mac. The shell scripts (`tools\vm\*.sh`, `tools\release\*`) assume `zsh` or `sh`: use WSL or Git Bash, or call the Python commands they contain.
- **Flashing:** find the port in Device Manager (a "USB Serial Device" or "USB JTAG/serial debug unit", `COM5` for instance), then `python -m esptool --chip esp32s3 -p COM5 -b 460800 write_flash 0x400000 loader_slot.bin`. Slot images are made with `purrstrap coreos slot-image` and the offsets are in [chapter 02](02-flash-map-and-partitions.md). The T-Deck's USB port is the chip's own USB Serial/JTAG; Windows 10 and 11 normally load a driver by themselves, and OpenOCD may need Zadig for the JTAG half.

## What goes wrong and how to fix it

| Symptom | Likely cause | Fix |
|---|---|---|
| a build fails with a path or "file not found" in a deep folder | the 260-character limit | clone near `C:\`, enable long paths |
| `purrstrap` says a hash or signature does not match for a file that is fine on the Mac | CRLF line endings | `core.autocrlf input`, clone again |
| `ModuleNotFoundError: cryptography` | the system Python has no packages | `python -m pip install cryptography` |
| the kit builder says it cannot find a toolchain | ESP-IDF's tools are not installed | install ESP-IDF 5.3.5 first, or pass `--no-toolchain` |
| no serial port appears | the board is in a state without USB, or another program holds the port | unplug, replug, close serial monitors |
| the C host tests cannot run | they need a C compiler called `cc` | run them in WSL, or install MSYS2 and put its `gcc` on the path |

## What is not finished

- **Everything above is untested on Windows.** The kit builder's Windows branch was written blind (the Python build tag, the wheel platform, `Lib\site-packages`, the `.cmd` launchers); it passes a syntax check and the Mac behaviour is unchanged (the SDK and purrstrap suites pass).
- **No emulator on Windows** until the patched QEMU has a Windows build ([chapter 18](18-emulator-and-vm.md)).
- **No Windows installer or signed Windows binaries.** The kit is a folder.
- The host tests under `PurrOS/components/coreos/test/host` were never run with a Windows C compiler.
