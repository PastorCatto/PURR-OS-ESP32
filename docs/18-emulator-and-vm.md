# 18. The emulator and the VM flavor

*Current as of PURR OS 1.1 (2026-11-08).*

## Short version

- The **emulator** is a separate project (`~/Projects/ESP32-Emulator`, outside this repository). It is a Rust shell around a patched QEMU that runs an ESP32-S3 flash image with a screen, touch, keyboard and SD card. The kit carries a built copy in `emulator/`.
- The **VM flavor** is PURR OS built for it: `sdkconfig.flavor.vm`. No Wi-Fi radio; a **W5500 Ethernet** controller instead. The guest reaches your computer at `10.0.2.2`.
- From the kit: `bin/purr emulator` opens the window; `bin/purr run <app>` builds, signs, serves and installs an app on the VM, headless, with pictures.
- **No VM image ships in the kit yet.** `purr run` works with an image you supply.
- Per CLAUDE.md and the owner's memory notes, **do not run the emulator or firmware builds unless asked**, and never close a window the owner is using.

## What it is for

Testing without a device on the desk, and giving the SDK a "run" button. The VM flavor lets the whole network path (DHCP, HTTPS, recovery, the dev server) run in an emulator that has no radio ([plan4.md](../plan4.md)). This chapter describes the emulator only as the repository's own documents describe it.

## The files

| File | What it is |
|---|---|
| [sdk/docs/12-emulator.md](../sdk/docs/12-emulator.md) | User guide: start it, headless mode, what it models, what it does not |
| [sdk/docs/13-first-hour.md](../sdk/docs/13-first-hour.md) | Walks `purr run` |
| [sdk/purrsdk/emulator.py](../sdk/purrsdk/emulator.py) | Finds the emulator, opens the window, the `Headless` class, `run_steps`, `run_app` |
| [sdk/purrsdk/runner.py](../sdk/purrsdk/runner.py) | Runs purrstrap for the SDK |
| [sdk/tests/test_emulator.py](../sdk/tests/test_emulator.py) | Tests for the above |
| [sdk/NOTICES-emulator.md](../sdk/NOTICES-emulator.md) | Licences of what the kit carries, and the written source offer |
| [tools/vm/README.md](../tools/vm/README.md) | The VM test loop in the repository |
| [tools/vm/mkimage_dbg.sh](../tools/vm/mkimage_dbg.sh) | Builds the debug flash image |
| [tools/vm/publish_apps.sh](../tools/vm/publish_apps.sh) | Builds and publishes apps to the dev server |
| [PurrOS/sdkconfig.flavor.vm](../PurrOS/sdkconfig.flavor.vm) | The VM flavor |
| [PurrOS/sdkconfig.flavor.vmdebug](../PurrOS/sdkconfig.flavor.vmdebug) | Layered on `vm`: a known test account and automatic sign-in |
| [PurrOS/components/kernel/src/purr_net_eth.c](../PurrOS/components/kernel/src/purr_net_eth.c) | The wired backend |
| [plan4.md](../plan4.md) | The VM flavor plan and contract |
| [plan7.md](../plan7.md) | VM findings (the touch panics) |
| [showtime1.md](../showtime1.md) | Part A and the VM image design (decision 5) |

## How it works

### What the emulator models

From [12-emulator.md](../sdk/docs/12-emulator.md): the ESP32-S3 with two cores and 8 MB of PSRAM; the 320 x 240 panel drawn in the window and read back as pictures; the touch panel (mouse); the keyboard; an SD card on the SPI bus backed by an image file; on the VM board a W5500 on its own SPI bus; and a small "VM info" device that tells the OS it is in a VM.

It does **not** model Wi-Fi, Bluetooth, LoRa, GPS, trackball, microphone, speaker or USB. It runs slower than the chip, so timing-sensitive code (audio, radio) cannot be judged. **It is not a security test:** a locked device's rules are checked on the device.

### The VM flavor

| What | Where |
|---|---|
| Wired backend selected | `CONFIG_PURR_NET_ETH_W5500=y` |
| Default shell | `CONFIG_PURR_UI_DEFAULT_MODE="android"` |
| Recovery and module index | `http://10.0.2.2:47801/recovery-vm/...` on the host's dev server |
| Build | `purrstrap coreos build --board tdeck_plus --profile full --flavor vm` |

The VM has its own `recovery-vm` folder on the dev server, because it needs the VM kernel, KittenOS and loader (virtual Ethernet, no Wi-Fi), not the real device's. Images are still verified against the boot key. See [chapter 13](13-networking.md) for the network, and the self-test that plants test apps at boot (`CONFIG_PURR_RT_SELFTEST`, on in the VM flavor).

### The debug image

`sdkconfig.flavor.vmdebug` adds a build-in account with a **known test password** and signs in at boot, because automatic sign-in works only while developer mode is active. The build refuses it on anything but the VM flavor. It is not for any device that matters.

### The test loop

1. Build the kernel for the VM (background, over ten minutes the first time): `cd PurrOS && . ~/esp/esp-idf-v5.3.5/export.sh`, then `cmake -B ../out/vmdbg -DIDF_TARGET=esp32s3 -DCCACHE_ENABLE=0 -DSDKCONFIG=../out/vmdbg.sdkconfig -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.board.tdeck_plus;sdkconfig.profile.full;sdkconfig.flavor.vm;sdkconfig.flavor.vmdebug" -G Ninja .`, then `ninja -C ../out/vmdbg`. Only if the owner asks.
2. `mkimage_dbg.sh` patches a base flash image with the new signed kernel slot and a developer-mode trust block. It needs `PURRSTRAP_KEY_PASSWORD` in the environment of that one command. Scenarios (`SCENARIO=...`): `normal`, `recover` (the loader once, system untouched), `first` (a blank device that starts in the loader), `auto` (the loader installs unattended) and `kitten` (KittenOS starts once). The README lists the first three; `auto` and `kitten` are in the script. Developer mode is off unless `DEV_MODE=1`.
3. `tools/vm/publish_apps.sh <app...>` publishes apps; `python3 purrstrap/purrstrap.py dev serve --dir out/devrepo --port 47801` serves them.
4. Open the emulator on the image.

### What `purr run` does

`run_app` in `emulator.py`:

1. Makes a one-app repository from the signed `.cat` and serves it on port 47801 (it refuses a port that is already taken; commit 40abd366).
2. Starts the emulator headless on a **copy** of the image, so the original is never changed.
3. Waits for a serial line that says the shell is up (`android-home:`), then runs a list of steps: taps with waits, drags, pictures, serial waits. The steps open the Developer Portal, tap Find servers, Trust, and Install.
4. Copies the pictures to `--shots`, stops the emulator, and raises an error if the board panicked.

Without `--headless` (`--window`) the window opens and the instructions are printed. The steps and coordinates were worked out by hand on the 1.0.27 Android image; on another shell, do it by eye with `--window`.

### Headless commands

The `headless_tap` program reads one command per line from a file: `tap X Y`, `drag X1 Y1 X2 Y2`, `type TEXT`, `shot NAME`, `serial TEXT`, `dump`, `quit`. It does no waiting; the caller paces the commands. Environment variables used with it include `HEADLESS_SHOTS`, `HEADLESS_CMD`, `ESP32_EFUSE` (an eFuse file), `ESP32_SERIAL_LOG`, and `ESP32_QEMU_ARGS`. A trace run uses `ESP32_QEMU_ARGS='-d guest_errors,unimp'` and `VPB_TRACE=i2c` ([plan7.md](../plan7.md)).

### The planned VM image (owner decision 5, 2026-11-06)

A trust block must be signed with the boot key, and that passphrase is the owner's. So a ready-made blank image is made **once** by the owner's signing step and shipped in the kit. Two ways are planned:

| Way | What it does |
|---|---|
| (a) | `purr run` finds no image, asks "to run in the VM we need an image; build one now?", and on yes **assembles** one from the signed parts the kit carries (bootloader, partition table, boot package, loader, KittenOS, kernel, trust block) into a 16 MB image. This takes seconds; it is not a compile |
| (b) | The kit also ships a **blank** image that installs itself from the **official OTA server**. That needs a VM loader that points at the official release, and the official release to carry the VM-flavor images |

The official server is about 16 releases behind (its `recovery.manifest` says 1.0.11; the dev server has 1.0.27). Uploading is the owner's step. Order: a `purr vm` command and the prompt, then the VM loader with the official address as 1.0.28, then the owner publishes, then the owner's one signing step.

## How to build and test it

1. Python tests for the driver: `python3 -m unittest discover -s sdk/tests -t sdk/tests -p "test_emulator.py"`.
2. With the kit: `bin/purr doctor` lists the emulator and the VM image. `bin/purr run hello --shots shots`.
3. Port 47801 must be free before `purr run`.
4. A scripted run needs steps close together: the emulated screen locks after about a minute without input.
5. Check what is running first with `pgrep` before any run, and leave windows that are not yours alone.

## What goes wrong

| Symptom | Cause | Fix |
|---|---|---|
| `purr run` has no image | None ships | Supply one, or wait for `purr vm` |
| Run stops at "Find servers" | The first panic below | Open |
| Typed text does not reach fields | The emulator's key injection | Not solved; the owner checks typing himself (decision 19) |
| Kernel illegal-instruction panic in `lv_indev_read` after an early tap | Open; a bad callback pointer or heap corruption near the `lv_indev_t` | Repeat a traced run until it panics ([plan7.md](../plan7.md)) |
| Load/store panic in the I2C interrupt after screen-off then tap | Probably firmware: a bus torn down across screen-off ([chapter 15](15-power.md)) | Read the power wake path; the emulator's maintainer says the emulator cannot return a bus error there |
| A panic decodes to nonsense | The decode used an ELF from a later build | Match the ELF to the kernel in the flash image (find a 4 KB block) |
| Emulator refuses to open | macOS quarantine | `xattr -dr com.apple.quarantine <the kit folder>` |
| Emulator needs macOS 26 | Today's build | Decision 9 asks for a lower target; Rosetta 2 runs the x86_64 build on Apple Silicon |

## What is not finished

- **No VM image ships.** The plan above is not built (showtime1 steps 1 to 4).
- **Two (now three) VM touch panics** are open. None has been seen on a device.
- **Shared-bus stall:** the W5500 sharing the display's bus stalled the VM; mitigated by giving it its own bus, with the root cause in the controller model still open ([plan4.md](../plan4.md)).
- **Typing in the emulator** is for the owner to check.
- **Board naming:** the emulator's board file for one of the boards is being renamed; I do not describe it here (decision 6).
- **Licences:** no licence file shows on the emulator's public repository, and the ROM licence is noted only ([chapter 17](17-sdk.md)).
- The W5500 contract in plan4 lists pins and registers; this chapter leaves them to the plan.

Next: [19 Boards](19-boards.md).
