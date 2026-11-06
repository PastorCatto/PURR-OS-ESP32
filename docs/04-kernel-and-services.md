# 04. The kernel and its services

## Short version

- One ESP-IDF project, [PurrOS/](../PurrOS/), builds all three runtime images. The **profile** chosen at build time decides which one: `full` (the normal OS), `recovery` (KittenOS), `minimal` (the recovery loader).
- Three layers of source: [components/kernel](../PurrOS/components/kernel/) (hardware, filesystem, network, console, driver registry), [components/coreos](../PurrOS/components/coreos/) (plain C logic with no ESP-IDF, tested on a PC), and [main/](../PurrOS/main/) (device glue, shell, runtime, UI).
- Start-up order is in [main.c](../PurrOS/main/main.c): kernel init, flash task, filesystem, modules and drivers, console, settings, clock, power, app runtime, network, shell.
- The kernel talks to loadable modules only through a table of function pointers, `purr_kernel_table_t`.
- A build picks board, profile and flavor through layered `sdkconfig` overlays; the build folders in `out/` are named `stdfull`, `stdmin`, `stdrec`, `vmfull` and so on.

## What it is for

The kernel layer owns the hardware and everything that must work before anything else can: power rails, buses, the driver registry, the flash, LittleFS, Wi-Fi. The CoreOS layer owns decisions that can be written as plain C: verification, `purrcfg`, manifests, the app runtime, permissions, users. The `main/` layer joins them to the display, the keyboard and the shells, and holds the command line. The split exists so that most logic can be tested on a laptop in seconds ([chapter 10](10-host-tests.md)).

## The files

| File or folder | What it is |
|---|---|
| [PurrOS/CMakeLists.txt](../PurrOS/CMakeLists.txt) | Project file; ESP-IDF 5.3.5 |
| [PurrOS/main/main.c](../PurrOS/main/main.c) | `app_main`: the whole start-up order, for all three profiles |
| [PurrOS/main/CMakeLists.txt](../PurrOS/main/CMakeLists.txt) | Which files each profile and shell compiles |
| [PurrOS/main/Kconfig.projbuild](../PurrOS/main/Kconfig.projbuild) | Profile choice, official shell choice, release URLs |
| [PurrOS/main/commands.c](../PurrOS/main/commands.c) | The command line (6,300 lines): fs, install, modules, drivers, secure, trust |
| [PurrOS/main/login.c](../PurrOS/main/login.c) | Accounts and sign-in |
| [PurrOS/main/recovery_loader.c](../PurrOS/main/recovery_loader.c) | The `minimal` profile's main (no shell) |
| [PurrOS/main/recovery_app.c](../PurrOS/main/recovery_app.c) | KittenOS's screen |
| [PurrOS/components/kernel/src/purr_kernel.c](../PurrOS/components/kernel/src/purr_kernel.c) | Board bring-up: power rail, buses, driver binding |
| [PurrOS/components/kernel/src/purr_driver.c](../PurrOS/components/kernel/src/purr_driver.c) | The driver registry (matching, pin claims) |
| [PurrOS/components/kernel/src/purr_fs.c](../PurrOS/components/kernel/src/purr_fs.c), [purr_fs_flash.c](../PurrOS/components/kernel/src/purr_fs_flash.c) | LittleFS behind a small API; flash backend |
| [PurrOS/components/kernel/src/purr_io.c](../PurrOS/components/kernel/src/purr_io.c) | The flash task and PSRAM-stack task helpers |
| [PurrOS/components/kernel/src/purr_net_wifi.c](../PurrOS/components/kernel/src/purr_net_wifi.c), [purr_net_eth.c](../PurrOS/components/kernel/src/purr_net_eth.c) | The two network backends (Wi-Fi; wired for the VM) |
| [PurrOS/components/kernel/src/purr_fetch.c](../PurrOS/components/kernel/src/purr_fetch.c) | Blocking HTTPS fetch, run on a PSRAM-stack worker |
| [PurrOS/components/kernel/src/purr_console.c](../PurrOS/components/kernel/src/purr_console.c), [purr_gfx.c](../PurrOS/components/kernel/src/purr_gfx.c) | Text console and drawing helpers on the display |
| [PurrOS/components/kernel/src/purr_cfgstore.c](../PurrOS/components/kernel/src/purr_cfgstore.c) | `purrcfg` as a flash object |
| [PurrOS/components/kernel/boards/](../PurrOS/components/kernel/boards/) | Board profiles: pins, buses, devices (`tdeck_plus.c` and others) |
| [PurrOS/components/coreos/src/](../PurrOS/components/coreos/src/) | 31 plain-C modules; see below |
| [PurrOS/components/coreos/include/purr_kernel_table.h](../PurrOS/components/coreos/include/purr_kernel_table.h) | The kernel-to-module call table |
| [PurrOS/sdkconfig.defaults](../PurrOS/sdkconfig.defaults), `sdkconfig.board.*`, `sdkconfig.profile.*`, `sdkconfig.flavor.*`, `sdkconfig.shell.android` | The overlays |
| [PurrOS/SPEC.md](../PurrOS/SPEC.md), [PurrOS/components/coreos/SPEC.md](../PurrOS/components/coreos/SPEC.md), [PurrOS/components/kernel/SPEC.md](../PurrOS/components/kernel/SPEC.md) | The specs (drafts) |

## The three profiles

| Profile | Image | Carries | Leaves out |
|---|---|---|---|
| `full` | `kernel` slot | Everything: shells, app runtime, Store, Settings, picture decoders | Nothing |
| `recovery` | `kittenos` slot | Shell engine, filesystem, Wi-Fi, the recovery app, the setup wizard, the installer | Apps, app runtime, the W10M and Android shells |
| `minimal` | `loader` slot | Wi-Fi, TLS, keys, flash writer, the setup wizard (part 1) | Filesystem, shell, apps |

The recovery profile left out `purr_apps_setup` and `purr_catrt_setup` on 2026-10-28 (owner: KittenOS wipes, formats, verifies and restores, and must not touch apps). The full profile holds back a lot of internal RAM through `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=256` in [sdkconfig.profile.full](../PurrOS/sdkconfig.profile.full); recovery and minimal keep the board file's values.

## How start-up goes (full profile)

1. `purr_kernel_init()`: switch on the peripheral power rail, hold idle pins high, start the SPI bus, then bind every device in the board profile to a built-in driver.
2. A headless text console, so early messages have somewhere to go before the display exists.
3. `purr_io_start()`: the flash task for PSRAM-stack tasks.
4. `purr_fs_setup()`: mount LittleFS on `root` (format it if blank).
5. `purr_modules_setup()`: plant the baked-in drivers into `/boot/drivers`, load `/system` modules and `/kernelmods`, load drivers (display, touch, keyboard first). See [chapter 07](07-drivers.md). A module that fails verification is moved to `/system/.rejected` ([Modules/SPEC.md](../Modules/SPEC.md) section 7.1).
6. If there is no display the device logs "display did not come up" every two seconds and stops there.
7. Console on the display, then in order: apps scan (`purr_apps_setup`), settings, clock (time from the last file, then SNTP), keyboard light, power manager, app runtime ([chapter 06](06-app-runtime.md)), network, UI mode.
8. `purr_mark_boot_healthy()`: reset `boot_fail_count` and raise the version floors for the running slot (only if its trailer verifies right then).
9. A one-second window to hold Space for the text shell. Otherwise sign in (automatic if the account allows), start LVGL and the chosen shell ([chapter 11](11-ui-stack-and-shells.md)).
10. After sign-in: `purr_session_started()` starts the Store companion, runs `exec-once` apps and loads the deferred drivers.

KittenOS follows the same path with extras: `purr_swap_setup()` after the filesystem (staged update swaps), the first-boot wizard when there are no accounts, and `purr_continue_install()` for stage 2 of an install. The loader skips all of this and runs `purr_recovery_loader_main()` straight after the first log line.

## The services in `main/`

| Group | Files | Job |
|---|---|---|
| Accounts | `login.c`, `purr_trust_os.c` | Sign-in, user elevation; the secure status |
| Settings | `purr_settings.c`, `purr_clock.c`, `purr_wallpaper.c` | Dotted keys in `/etc/system.conf`; time; wallpaper |
| Power | `purr_power_plat.c`, `purr_battery.c`, `purr_kbdlight.c` | Screen timeout, wake locks, battery smoothing ([chapter 15](15-power.md)) |
| Network | `purr_netsettle.c`, `purr_lan_discover.c`, `purr_recovery_src.c` | Deferred devices, dev-server discovery, the update source |
| Updates | `purr_sysupdate.c`, `purr_bootupdate.c`, `purr_slot_trailer.c` | Settings > Updates, bootloader swap, trailers |
| Hardware | `purr_hwsetup.c`, `purr_hwscan_plat.c`, `purr_sd.c` | Find hardware, driver install, the `/sd` card |
| Apps | `purr_catrt*.c`, `purr_store.c`, `purr_builtin_apps.c` | App runtime and its catcalls; the Store |
| Setup | `purr_oobe.c`, `oobe_*.c` | The first-run wizard |
| Tasks | `purr_systasks.c` | The Task Manager's list of system tasks |

The system task list counts stacks in internal RAM, which is the scarce resource. A task whose stack is in PSRAM must never call flash directly; it hands the call to the flash task ([chapter 06](06-app-runtime.md)).

## The coreos modules

Plain C, no ESP-IDF headers, compiled on the PC by the host tests. Grouped:

| Group | Source files |
|---|---|
| Trust | `purr_verify`, `purr_keybag`, `purr_trailer`, `purr_trust`, `purr_cfg`, `purr_reason`, `purr_util` |
| Update | `purr_manifest`, `purr_swap`, `purr_bootimg`, `purr_decision`, `purr_repo`, `purr_repo_trust` |
| Modules | `purr_module`, `purr_relocate` |
| Runtime | `purr_runtime`, `purr_appmgr`, `purr_perm`, `purr_power` |
| System | `purr_users`, `purr_pbkdf2`, `purr_wifi`, `purr_cli`, `purr_term`, `purr_menu` |
| Hardware | `purr_hwprofile`, `purr_hwscan`, `purr_battmodel`, `purr_statusicons` |
| Messaging | `purr_msgstore`, `purr_contacts` |

## The board profile and the driver registry

Pins and parts are data, not code. Each board has one file in [components/kernel/boards/](../PurrOS/components/kernel/boards/) that fills a `purr_board_t` ([purr_board.h](../PurrOS/components/kernel/include/purr_board.h)): the SPI bus, the I2C buses, the power-rail pin, idle pins that must start high, and a list of **devices**. A device has a name (`display`, `keyboard`, `touch`, `battery`, `gps0`, `sd0`, `lora0`), a `compatible` string that names the driver that serves it, a priority (`REQUIRED`, `IMPORTANT`, `OPTIONAL`) and its pins.

The registry in [purr_driver.c](../PurrOS/components/kernel/src/purr_driver.c) walks that list. For each device it claims the pins first, refusing the device if another device already owns one (state `PIN_CONFLICT`), then finds a registered driver whose `compatible` matches and calls its `probe()`. One device failing never stops the rest. A device with no driver ends in `NO_DRIVER`, which is what "Find hardware" and the Device Manager show as a missing driver. A device marked `PURR_DEFER_NET` (the LoRa radio on the T-Deck Plus) is not touched until Wi-Fi has an address, because starting both together used too much fast RAM ([chapter 07](07-drivers.md)). This part of the kernel is plain C and is tested on a PC with fake drivers and a fake board (`test_driver`).

## The two call tables

Code that is loaded from a file cannot link against the kernel, so it is handed a **table of function pointers** when it starts. There are two layers:

| Table | Header | Given to | Version |
|---|---|---|---|
| `purr_kernel_table_t` | [purr_kernel_table.h](../PurrOS/components/coreos/include/purr_kernel_table.h) | Kernel modules (`/kernelmods`) | `PURR_KERNEL_TABLE_ABI_VERSION` 10 |
| `purr_core_table_t` (handed in); the module returns a `purr_module_table_t` | [purr_module_abi.h](../PurrOS/components/coreos/include/purr_module_abi.h) | Command modules (`/system`) | `PURR_MODULE_ABI_VERSION` 5 |
| `purr_driver_bus_table_t` | [purr_driver_bus.h](../PurrOS/components/kernel/include/purr_driver_bus.h) | Drivers | `PURR_DRIVER_BUS_ABI_VERSION` 7 |

The rule for all three is the same, and it was learned the hard way: the layout is the ABI, so a table only grows at its end, the version number is bumped on every change, and the loader refuses a file built for a different version by name. When a function was once added in the middle of the driver table, an old touch driver called the wrong slot and froze the UI with no error. The details of the driver table are in [chapter 07](07-drivers.md). Apps do not use these tables; they use catcalls ([chapter 05](05-catcall-abi.md)).

## Profiles, boards and flavors in the build

A build is the sum of overlay files named in `SDKCONFIG_DEFAULTS`, applied in order: `sdkconfig.defaults` (everything), `sdkconfig.board.<board>` (flash size, partition table file, board-specific memory settings), `sdkconfig.profile.<profile>` (which image), and optionally `sdkconfig.flavor.<flavor>`. The flavors are `vm` (no radio; a W5500 wired Ethernet controller instead, for the emulator) and `vmdebug` (adds a built-in debug account that works only with developer mode active). A second choice in Kconfig, `PURR_OFFICIAL_SHELL`, picks the Windows 10 Mobile style shell or the Android style one; the Android shell has its own overlay, [sdkconfig.shell.android](../PurrOS/sdkconfig.shell.android). Exactly one shell is compiled into an image ([chapter 11](11-ui-stack-and-shells.md)).

Because the overlays only apply when the `sdkconfig` file for that build folder does not exist yet, changing an overlay does nothing to an existing folder. That one fact explains most "my setting did not apply" reports.

## Memory rules that shape the design

Internal RAM is the scarce part of the ESP32-S3: Wi-Fi, DMA buffers, task stacks and LVGL objects all want it. The full profile lowers the threshold for what goes to PSRAM so that nearly every ordinary allocation lands there, and keeps the main task stack at 16 KB. A TLS download runs on a worker task whose 32 KB stack lives in PSRAM ([plan7.md](../plan7.md), 2026-11-05). Because flash operations switch the cache off, a task with a PSRAM stack can never call flash itself; it hands the call to the 3 KB flash task in [purr_io.c](../PurrOS/components/kernel/src/purr_io.c). Anything that starts hardware (a driver load) keeps a fast-RAM stack, because a driver may hand stack buffers to DMA ([plan9.md](../plan9.md), 2026-11-05).

## How to build and test

Host tests need no device and run in seconds ([chapter 10](10-host-tests.md)). A firmware build needs ESP-IDF and takes more than ten minutes for a full profile. Do not run firmware builds unless the owner asks.

Configure a new build folder from `PurrOS/`. The recipe follows the `vmdbg` one in [tools/vm/README.md](../tools/vm/README.md) with the standard (not VM) overlays; the folder names come from `CLAUDE.md`:

```
. ~/esp/esp-idf-v5.3.5/export.sh
cd PurrOS
cmake -B ../out/stdfull -DIDF_TARGET=esp32s3 -DSDKCONFIG=../out/stdfull.sdkconfig \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.board.tdeck_plus;sdkconfig.profile.full" -G Ninja .
ninja -C ../out/stdfull
```

Use `sdkconfig.profile.recovery` for `stdrec` and `sdkconfig.profile.minimal` for `stdmin`. For the VM flavor add `;sdkconfig.flavor.vm`, and name the folder `vmfull`, `vmrec` or `vmmin`. `purrstrap coreos build` does the same into `PurrOS/build/<board>-<profile>` instead ([chapter 09](09-purrstrap.md)); the release scripts expect `out/stdfull`, `out/stdmin`, `out/stdrec`.

## What goes wrong and how to fix it

| Symptom | Cause | Fix |
|---|---|---|
| A new `sdkconfig` default has no effect | `sdkconfig.defaults*` only apply when the sdkconfig file does not exist | Delete the build's sdkconfig and reconfigure |
| "undeclared" LVGL font or symbol in one profile only | Same stale sdkconfig | Same fix |
| Build is for plain ESP32 and fails in `purr_kernel.c` | `IDF_TARGET` forgotten | Pass `-DIDF_TARGET=esp32s3` when configuring |
| "app partitions too small" warning on a full build | The full image is larger than the `kittenos` slot | Expected; the full image goes in `kernel` only |
| Display never comes up | Display driver missing or failed to bind | Check `/boot/drivers`, the serial log, and [chapter 07](07-drivers.md) |
| "couldn't start that" in the wizard | Not enough contiguous fast RAM for a job task | See [plan1.md](../plan1.md) "Couldn't start that"; jobs now run on the main task or have a retry |
| `Guru Meditation ... stack in external RAM` | A PSRAM-stack task called flash directly | Route it through `purr_io` ([chapter 06](06-app-runtime.md)) |

## What is not finished

- **Plan and code disagree on the kernel/CoreOS split.** [PurrOS/SPEC.md](../PurrOS/SPEC.md) and [coreos/SPEC.md](../PurrOS/components/coreos/SPEC.md) describe a small kernel that loads CoreOS as a file from `/boot`. The proof of that loader worked on 2026-09-30, but the shipped `kernel` slot is the whole monolith ([chapter 01](01-boot-chain.md)). The standalone [Kernel/](../Kernel/) project is not part of releases.
- **The CYD (ESP32) board** has no PurrOS partition table or board file in the build today ([chapter 02](02-flash-map-and-partitions.md)).
- **Internal RAM** is tight: about 50 KB free at idle on the home screen without developer mode (owner measurement, 2026-11-03, [plan6.md](../plan6.md)). The kernel image has about 90 KB left in its slot at 1.0.26 ([showtime1.md](../showtime1.md) part D).
- **Not verified on a device:** the quiet start path end to end after the 2026-11-04 changes, and most 2026-11-05 memory changes (PSRAM stacks, deferred drivers) beyond the VM.

Plan references: [plan1.md](../plan1.md) (boot, recovery, wizard), [plan7.md](../plan7.md) (shell, memory), [plan10.md](../plan10.md) (power).
