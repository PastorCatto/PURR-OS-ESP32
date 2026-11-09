# Runbook: how the work is actually done

*Current as of PURR OS 1.1 (2026-11-08).*

## Short version

- This chapter is the working knowledge that used to live in people's heads and chat notes: how to build, sign, publish, flash, test and debug, and the traps that have already cost time.
- Firmware builds with ESP-IDF 5.3.5 (`. ~/esp/esp-idf-v5.3.5/export.sh`, then `ninja -C out/<dir>`). Full builds take over 10 minutes: run them in the background.
- Anything that signs needs the boot-key or developer-key passphrase **in the environment of that one command**: `PURRSTRAP_KEY_PASSWORD=... command`. It is never written to a file, a script, a commit message or a plan.
- A version that has been tested stays byte for byte as it was. New work goes into the next number.
- If you are stuck, read the "traps" section first. Most of the time lost so far was one of those.

## Where things live

| What | Where |
|------|-------|
| Firmware (ESP-IDF project) | [PurrOS/](../PurrOS/) |
| Bootloader | [bootloader/](../bootloader/) |
| Build and signing tool | [purrstrap/purrstrap.py](../purrstrap/purrstrap.py), run as `python3 purrstrap/purrstrap.py ...` |
| Apps and drivers (sources) | [Apps/](../Apps/), [Modules/](../Modules/) |
| Developer kit sources | [sdk/](../sdk/) |
| Release and VM scripts | [tools/](../tools/) |
| Build output (ignored by git) | `out/` |
| Plans (the source of truth for each workstream) | [plan1.md](../plan1.md) boot chain, [plan3.md](../plan3.md) hardening and release channels, [plan5.md](../plan5.md) SDK, [plan6.md](../plan6.md) app runtime, [plan7.md](../plan7.md) shells, [plan8.md](../plan8.md) repositories and Store, [plan9.md](../plan9.md) ui and files catcalls, [plan10.md](../plan10.md) power |
| The emulator (separate project) | `~/Projects/ESP32-Emulator`; its notes are in [sdk/docs/12-emulator.md](../sdk/docs/12-emulator.md) |

Read the plan for the area you touch before you touch it, and update it in the same step as the code. If the plan and the code disagree, fix one of them.

## Build directories

Each flavor of the firmware has its own build directory and its own `.sdkconfig`:

| Directory | What it is |
|-----------|------------|
| `out/stdfull` | The kernel (full OS) for the real T-Deck Plus |
| `out/stdmin` | The recovery loader for the real device |
| `out/stdrec` | KittenOS (the recovery system) for the real device |
| `out/vmfull`, `out/vmmin`, `out/vmrec` | The same three for the emulator (VM flavor: wired Ethernet instead of Wi-Fi) |
| `out/vmdbg` | The VM kernel with automatic sign-in (account `dev`), only meaningful with developer mode on |

### Make a new build directory

1. `cd PurrOS && . ~/esp/esp-idf-v5.3.5/export.sh`
2. Configure once:
   `cmake -B ../out/<dir> -DIDF_TARGET=esp32s3 -DCCACHE_ENABLE=0 -DSDKCONFIG=../out/<dir>.sdkconfig -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.board.tdeck_plus;sdkconfig.profile.<full|minimal|recovery>[;sdkconfig.flavor.vm]" -G Ninja .`
3. Build in the background: `ninja -C ../out/<dir>`. Watch the log; do not poll in a loop.

The kernel image is `out/<dir>/purros.bin`. A "partitions too small" warning when building the loader or KittenOS from a `stdfull` directory is expected; it describes the kernel image.

## Sign and assemble

Everything that goes to a device is signed. The steps for one component:

1. Package it: `python3 purrstrap/purrstrap.py coreos package-component --component kernel --file out/stdfull/purros.bin --version <V> --board tdeck_plus --out K.cat`
2. Sign it: `PURRSTRAP_KEY_PASSWORD=... python3 purrstrap/purrstrap.py keys sign --key signing_keys/boot.key --image K.cat --key-id 1`
3. Check it: `python3 purrstrap/purrstrap.py keys verify --key signing_keys/boot.pub --image K.cat`

KittenOS is wrapped as a recovery image (see [tools/vm/mkimage_dbg.sh](../tools/vm/mkimage_dbg.sh) for the exact few lines), the loader is a `--component loader` package. Apps and drivers are signed with the developer key (key id 2) instead of the boot key.

Flash slot offsets on the T-Deck Plus: the partition table is at `0xA000`; `bootpkg` at `0x12000`, `kittenos` at `0x20000`, `kernel` at `0x1E0000`, `loader` at `0x400000`, and `root` (LittleFS, with the picture packs inside it as files) at `0x5C1000` to the end of the flash. That is flash map v2 (1.0.29, not yet run on a device; the three slots are 256 KB larger than in 1.0.28); up to 1.0.28 the picture pack had its own 5 MB partition at `0x501000` and `root` began at `0xA01000`, and a device with the old table needs a cable flash of a whole image and a full chip erase to move over. Never write a raw `purros.bin` into a slot: each slot needs its signature trailer, or it boots with a warning (and in enforce mode is refused). Cut a slot image with `coreos slot-image`.

## Publish to the dev server (what the owner's device updates from)

The dev server serves `out/devrepo/`:

    python3 purrstrap/purrstrap.py dev serve --dir out/devrepo --port 47801

It listens on all interfaces. The emulated device reaches the computer at `10.0.2.2`. Check whether it is already running before you start another: `pgrep -fl "dev serve"`, and `lsof -nP -iTCP:47801 -sTCP:LISTEN`. A second server on the same port is silently shadowed by the first.

| Folder in `out/devrepo/` | What it holds |
|--------------------------|---------------|
| `purr/v1/` | Apps (publish with `dev publish`, or [tools/vm/publish_apps.sh](../tools/vm/publish_apps.sh)) |
| `recovery/` | The real device's recovery source: manifests, kernel, KittenOS, loader, boot package, modules, drivers |
| `recovery-vm/` | The same for the VM flavor (separate builds) |

### Publish a new kernel, KittenOS and loader

1. Build `stdfull`, `stdmin`, `stdrec`.
2. **Back up the live set first:** `cp -R out/devrepo/recovery out/ota/recovery-<version>-before-<what>`.
3. Package and sign the three images as above, with the same version number.
4. Regenerate the manifests:
   `python3 purrstrap/purrstrap.py dev recovery --dir out/devrepo --board tdeck_plus --kernel <k.cat> --kittenos <r.kitt> --loader <l.cat>`
5. **How the files are named (since 1.0.28):** `dev recovery` publishes each image under a name that holds its hash (`kernel-tdeck_plus.<10 hex digits>.cat`), keeps the newest three generations, and writes the manifest last, atomically. Before that, files were replaced in place under fixed names, and an OTA that overlapped a publish failed its size and hash check (it happened on 2026-11-06). The fixed names are still written next to them for tools that look for them.
6. **Verify:** fetch every file each manifest names from the live server and compare size and sha256. A short Python loop with `urllib` does it. Do not trust a manifest to prove that the server has everything: it is regenerated from the files in the folder, so it only knows what is there.

`dev recovery` regenerates `recovery.manifest` and `modules.manifest` from the files in the folder. The system modules (about, appmgr, apps, fs, login, netinstall, sysinfo, wifi) are not rebuilt each time, so they are passed with `--modules` from the last official release's modules folder. Leave them out and the setup wizard's second stage has nothing to download when the dev server is the source.

### Drivers

1. Build the module: `python3 purrstrap/purrstrap.py modules build --kind driver --board tdeck_plus --source Modules/drivers/X_module.c --entry X_entry --name X --version V --out X.cat`
2. Sign it with the developer key (id 2).
3. Write the index with `python3 tools/release/make_drivers_index.py --out DIR --driver FILE:COMPAT:BUSES:CATCALL:ABI_MIN ...`. **The script writes the manifest from only the `--driver` arguments you give it**, so list every driver every time.
4. Copy the `.cat` files and `drivers.manifest` into `out/devrepo/recovery/`.
5. A fixed driver needs a higher version number to be offered as an update.

### Apps

`tools/vm/publish_apps.sh <app...>` builds apps from [Apps/](../Apps/), signs them with developer key 2 and publishes them to `out/devrepo/`. Test packages that should be refused (an unsigned one, one with a tampered hash) are worth keeping next to the good ones.

## Releases and versions

- **A tested version stays as it was.** Once the owner has tested a version, do not sign and publish different content under the same number. New work builds as the next number, in a scratch folder, and is published only when the owner says to swap. Exception: the owner can explicitly ask for a rebuild under the same number (it happened for the 1.0.27 lock-screen fix); then the old set is backed up first.
- **A device refuses to go below the version floors it has recorded.** New images must carry a version at or above what the device runs.
- **1.1 is the stable general release** (owner, 2026-11-08); it replaces 1.0.27 as the known-good one. Later work is a higher number (the owner's EDGE numbers above 1.1 are for internal development), built in a scratch folder.
- The official releases are GitHub release assets: `JumpingJaguar1` (recovery images), `JumpingJaguar2` (modules and drivers), `JumpingJaguar3` (the app repository). [tools/release/make_release.py](../tools/release/make_release.py) builds the folder to upload. Uploading is done by the owner.
- Read [08-updates-and-release-channels.md](08-updates-and-release-channels.md) for channels, rollback rules and the withdrawn mechanism.

## Flash and debug a T-Deck Plus

1. The device appears as a USB serial/JTAG port (`/dev/cu.usbmodem*`). **Opening the port resets the board.** A serial capture therefore reboots the device, and whoever is using it must sign in again (about 15 seconds after boot).
2. A full erase and flash is done with `esptool` only when asked. Write the bootloader at `0x0`, the table at `0xA000`, then the signed slots at their offsets. In zsh put `esptool` in a function, not a variable.
3. **Bootloader over the air is off** (`PURR_BOOTLOADER_OTA 0`): erasing flash at `0x0` aborted and left the board in a boot loop with a black display. A new bootloader reaches a device by a cable flash. How to bring it back is in [plan3.md](../plan3.md).
4. **A hang without a reset:** run `openocd -f board/esp32s3-builtin.cfg` in the background, then
   `xtensa-esp32s3-elf-gdb -batch out/stdfull/purros.elf -ex "target extended-remote :3333" -ex "monitor halt" -ex "thread apply all bt"`.
   Kill `openocd` afterwards. macOS has no `timeout`; use Python's `subprocess` instead.
5. **Decode a panic:** `xtensa-esp32s3-elf-addr2line -pfiaC -e <elf> <addresses>`. **Use the ELF of the kernel that is really in the image.** A build from later the same day gives a plausible but wrong answer. To find the right one, compare a block from the middle of each candidate `purros.bin` with the flash image (the first 4 KB is the same in every build).
6. Policy tests without the keyboard: write a trust block with `keys trust-block --key signing_keys/boot.key --mode enforce [--dev-mode] [--dev-locked] --out x.bin` and write it at `0xF000`. A locked device is never weakened on the device.

## The emulator and the VM

Never kill an emulator window that is open: it may be a demo. Check first: `pgrep -fl 'esp32-emulator|qemu-system-xtensa'`. Stop only processes you started.

1. Make an image: [tools/vm/mkimage_dbg.sh](../tools/vm/mkimage_dbg.sh) patches a base flash image with a signed kernel and a trust block. Scenarios: `normal`, `recover`, `first` (a blank device that starts in the loader), `auto` (the loader installs unattended), `kitten`. It needs the passphrase, supplied in the environment of that one command.
2. Run it: `purr emulator <image>` opens the window; `purr run` builds an app and shows it on the emulated device. The kit version is in [sdk/docs/12-emulator.md](../sdk/docs/12-emulator.md).
3. A scripted run without a window (the "headless" runner) takes a command file: `tap X Y`, `drag X1 Y1 X2 Y2`, `shot NAME`, `dump`, `type TEXT`, `quit`. The screen is 320 by 240. **No emulated time passes between two back-to-back commands**, so every step carries its own wait. The guest locks the screen after about a minute of no input; keep scripted steps close together.
4. The emulator traces I2C and QEMU guest errors when asked: `ESP32_QEMU_ARGS='-d guest_errors,unimp -D <file>'`, `VPB_TRACE=i2c`, `VPB_TRACE_FILE=<file>`.
5. Two known panics in the VM are tracked in [plan7.md](../plan7.md) (a touch during start-up, a touch after the screen switched off). Both are believed to be firmware-side; not seen on a device.

## Host tests

| What | Command |
|------|---------|
| C host tests (no device) | `cd PurrOS/components/coreos/test/host && IDF_PATH=$HOME/esp/esp-idf-v5.3.5 python3 run.py [name]` |
| purrstrap tests | `cd purrstrap && python3 -m unittest discover -s tests` |
| SDK tests | `cd sdk && python3 -m unittest discover -s tests` |
| Documentation links | `python3 tools/docs/check_links.py <files or folders>` |
| Documentation word count | `python3 tools/docs/count_words.py docs` |

New `src/*.c` files for host tests are picked up by a glob; the device's CMake still needs them listed.

## Traps that have already cost time

| Trap | What happens | What to do |
|------|--------------|------------|
| Stale `sdkconfig` | `sdkconfig.defaults*` only apply when `<dir>.sdkconfig` does not exist. A symbol shows up "undeclared" in one flavor only. | Delete that `.sdkconfig` and run `cmake -B ... .` again. |
| Forgetting the target | Without `-DIDF_TARGET=esp32s3` the build is for plain ESP32 and fails in `purr_kernel.c`. | Always pass it when configuring. |
| Editing sources during a build | A bogus link error. | Do not edit a file ninja still has to compile. |
| `switch` in an app or driver | It makes a jump table, which the relocator must treat as code. | Fixed in [purrstrap/scripts/modules.py](../purrstrap/scripts/modules.py); check with `objdump -d x.o` for `jx`. |
| Flash calls on a PSRAM stack | Since 1.0.22 an app's task stack, queue and semaphores are in PSRAM by default (setting `apps.psram_stacks`, 1 = on). IDF aborts a flash operation made from a PSRAM stack. | Flash access goes through `purr_io` (the `flash_io` task); a missed direct call aborts loudly and names itself. Internal RAM is still the scarce resource. Details in [06-app-runtime.md](06-app-runtime.md) and [plan7.md](../plan7.md). |
| Baked-in drivers | The display, touch and keyboard driver files in `/boot/drivers` are planted every start. | Deferral and quarantine must exempt them (the 1.0.24 hang). |
| `dev recovery` forgets files | It regenerates manifests from the folder. | Pass the system modules with `--modules`; verify every file afterwards. |
| New programs hang in state `U` | A macOS prompt to allow debugging other apps is waiting for an answer. | Answer the prompt. Do not `kill -9` `lldb` or `debugserver`. |
| Two dev servers on one port | The first one answers; the second one is never reached. | Check with `lsof` before starting one. |
| zsh | `echo == x` fails; unquoted variables are not split; `grep --include=*.py` fails with "no matches". | Use `echo "-- x"`, arrays, and quote globs. macOS `sed -i` needs `-i ''`: prefer a small Python edit. |
| Wiped scratch folder | A hard lock once wiped the session scratch folder, with build directories and scripts in it. | Keep anything worth keeping in the repository. |
| Developer mode off after install | After an unattended install the device starts in a safe state with developer mode off, so the setup wizard's second stage uses the official server. | Intended; see [plan3.md](../plan3.md). A restart from inside the OS brings developer mode back. |

## Rules that are easy to get wrong

- Apps are freestanding modules with no libc, loaded into PSRAM.
- Apps are sideloaded (through the Developer Portal or the Store). Only Settings is built into the shell, apart from the Store and its companion.
- A locked (dev-locked) device is never weakened on the device.
- Never put a passphrase, a key or a private hardware detail in a file, a script, a commit message or a plan.
- Do not run the emulator or the firmware builds in `out/` unless the owner asked, and ask when it is unclear whether the owner is building.
- Keep the plans current. When you finish a chunk of work, commit it as its own commit, and scan it for secrets first.

## What is not finished

- Key rotation: planned in [plan3.md](../plan3.md), not started (held by the owner on 2026-11-06).
- The VM image for the SDK kit and the official release for the VM flavor: see [showtime1.md](../showtime1.md).
