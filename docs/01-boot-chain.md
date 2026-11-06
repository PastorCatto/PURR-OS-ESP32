# 01. The boot chain

## Short version

- Power on runs the chip's ROM, then the **PURR bootloader** at flash 0x0, then (usually) the **boot package** for a splash or menu, then one of three slots: the **kernel** (the full PURR OS), **KittenOS** (recovery), or the **recovery loader** (reinstall from the network).
- The bootloader chooses the slot with a failure counter: starts 1 to 3 prefer the kernel, 4 to 6 prefer KittenOS, 7 and up prefer the loader.
- Every slot is checked by a signature trailer in its last 4 KB before it runs. In `warn` mode (the default) a bad slot boots with a warning; in `enforce` it is refused.
- The full OS clears the failure counter when it reaches a healthy state (`purr_mark_boot_healthy()`, called late in start-up from [main.c](../PurrOS/main/main.c)).
- The bootloader source lives only in [bootloader/](../bootloader/); `PurrOS/` and `Kernel/` build that same code.

## What it is for

The boot chain gets a T-Deck Plus from "power on" to a running system, and gets it back to a running system when something is wrong. It does three jobs:

1. Pick a slot to run.
2. Check that the slot is the one we signed.
3. Hand the OS a small note saying what happened (the handoff).

The design goal is that a device with a damaged kernel, or a bad update, can always reach a screen that fixes it. That is why there are three slots and a counter, not one slot.

## The files

| File | What it is |
|---|---|
| [bootloader/bootloader_components/main/purr_boot.c](../bootloader/bootloader_components/main/purr_boot.c) | Entry point `call_start_cpu0`: parks the second CPU, reads `purrcfg`, runs the failure ladder, the final gate, writes the handoff, jumps |
| [bootloader/bootloader_components/main/purr_slotverify.c](../bootloader/bootloader_components/main/purr_slotverify.c) | Checks a slot's trailer: aligned flash reader, key bag, hardware SHA |
| [bootloader/bootloader_components/main/purr_bootpkg.c](../bootloader/bootloader_components/main/purr_bootpkg.c) | Loads and verifies the boot package, calls it, feeds the watchdog while it waits |
| [bootloader/bootloader_components/main/purr_bootcfg.c](../bootloader/bootloader_components/main/purr_bootcfg.c) | Reads and writes `purrcfg` from inside the bootloader |
| [bootloader/bootloader_components/main/purr_handoff.c](../bootloader/bootloader_components/main/purr_handoff.c) | Writes the handoff note to RTC memory |
| [bootloader/SPEC.md](../bootloader/SPEC.md) | The bootloader spec (sections 6, 7, 9 are the ones to read) |
| [bootloader/partitions.csv](../bootloader/partitions.csv) | The partition table the bootloader project builds with |
| [bootpkg/src/pkg_main.c](../bootpkg/src/pkg_main.c) | The boot package entry and menu logic |
| [bootpkg/src/pkg_ui.c](../bootpkg/src/pkg_ui.c) | The look of every boot screen |
| [bootpkg/src/pkg_hw.c](../bootpkg/src/pkg_hw.c) | Bit-banged display and keyboard for colour boards ([pkg_hw_epd.c](../bootpkg/src/pkg_hw_epd.c) for the e-ink board) |
| [bootpkg/link/esp32s3.ld](../bootpkg/link/esp32s3.ld) | Linker script for the fixed RAM window |
| [KittenOS/SPEC.md](../KittenOS/SPEC.md) | The recovery system spec |
| [RecoveryLoader/SPEC.md](../RecoveryLoader/SPEC.md) | The recovery loader spec |
| [PurrOS/main/recovery_loader.c](../PurrOS/main/recovery_loader.c) | The recovery loader's code (profile `minimal`) |
| [PurrOS/main/recovery_app.c](../PurrOS/main/recovery_app.c) | The screen KittenOS shows (profile `recovery`) |
| [PurrOS/main/main.c](../PurrOS/main/main.c) | `app_main` for all three profiles |
| [PurrOS/components/coreos/include/purr_abi.h](../PurrOS/components/coreos/include/purr_abi.h) | The shared structs: image header, `purrcfg`, handoff, boot package ABI |
| [Kernel/README.md](../Kernel/README.md) | A separate, standalone kernel project (see "What is not finished") |

## How it works, step by step

1. **ROM.** The chip's ROM starts the second-stage bootloader from flash 0x0. We cannot change the ROM, and it checks nothing of ours.
2. **Park the second CPU.** `call_start_cpu0` first stalls, resets and clock-gates core 1 (`park_app_cpu()`). This fixes a crash loop seen on a real device after a restart from a task on core 1 ([plan1.md](../plan1.md), "Hardware crash loop after a restart"; moved to [plan3.md](../plan3.md) item 6).
3. **Read `purrcfg`.** Two 4 KB copies, A and B. The newest copy with a good CRC wins. If neither is good, defaults apply (mode `warn`). See [chapter 02](02-flash-map-and-partitions.md) and [chapter 03](03-trust-and-keys.md).
4. **Evaluate trust.** The bootloader works out the effective security mode once. It uses the signed trust block, if its signature checks against the keys compiled into the bootloader.
5. **Count the start.** `boot_fail_count` goes up by one, before anything else reads it. The OS resets it to 0 when it is healthy.
6. **Choose a slot** with the ladder:

   | `boot_fail_count` | Prefers | Then tries |
   |---|---|---|
   | 1 to 3 | `kernel` | `kittenos`, then `loader` |
   | 4 to 6 | `kittenos` | `loader`, then `kernel` |
   | 7 and up | `loader` | `kittenos`, then `kernel` |

   The thresholds are `> 3` and `> 6` in the code, because the count is already 1 on the first attempt. An older draft of the spec said 0-2, 3-5, 6+; the code is the authority ([bootloader/SPEC.md](../bootloader/SPEC.md) section 6 says so too). Reaching the loader this way sets `AUTO_REINSTALL` so the loader runs unattended.
7. **One-shot requests.** `FORCE_RECOVERY` and `FORCE_LOADER` (from `reboot recovery` and `reboot loader`) override the pick once. They are cleared before use, so a crash cannot trap the device in a loop.
8. **Splash or menu.** The bootloader loads the boot package into a fixed RAM window, verifies it first, and calls it. The package draws the splash and watches the keyboard for 300 ms. The menu appears instead when Space was pressed, when the last start never became healthy, when the kernel is present but fails its check, or when Settings asked for it ([bootloader/SPEC.md](../bootloader/SPEC.md) section 9.1). With nobody pressing a key for 20 seconds the menu starts the default entry.
9. **Final gate.** Whatever chose the slot, the slot actually launched must pass the policy. A request is not a way around `enforce`. If the slot is not usable, the bootloader takes the first usable slot in ladder order; if none, it resets, which raises the counter and walks down the ladder.
10. **Handoff and jump.** The bootloader writes the handoff (boot state, secure state, key id, flags used, bootloader version) to a reserved RTC block, then jumps through ESP-IDF's stock loader.

The boot state in the handoff, strongest first: `FAILED_WARNED`, `AUTO_REINSTALL`, `FORCED_RECOVERY`, `VERIFIED`, `CONFIG_DEFAULT`, `UNCHECKED` (constants in [purr_abi.h](../PurrOS/components/coreos/include/purr_abi.h)).

## What each slot runs

All three slots are built from one source tree, [PurrOS/](../PurrOS/), with a profile chosen at build time ([chapter 04](04-kernel-and-services.md)).

| Slot | Profile | What it is |
|---|---|---|
| `kernel` (0x1A0000) | `full` | The normal system: kernel, CoreOS, shell, apps runtime, Store |
| `kittenos` (0x20000) | `recovery` | Recovery: filesystem, Wi-Fi, a small recovery app, no apps. Applies staged updates and installs the system |
| `loader` (0x380000) | `minimal` | Wi-Fi, TLS, keys, a flash writer, and the setup wizard. No filesystem. Reinstalls KittenOS and the kernel |

The shipped `kernel` slot holds the **whole full-profile image** (`purros.bin`), not a bare kernel. `tools/release/make_release.py` packages `out/stdfull/purros.bin` as the kernel, and [plan7.md](../plan7.md) measures it at 1,876,112 of 1,966,080 bytes at 1.0.26.

## The recovery path in short

When the system cannot be trusted, the loader reinstalls it. Only the loader writes the `kittenos` slot. The loader installs KittenOS and the boot package, then KittenOS asks for an account and installs the kernel and modules in a second stage in the background ([plan1.md](../plan1.md), "Staged recovery" and "One way to restore"). Both slots carry a `BAD_KERNEL` or `BAD_KITTENOS` mark during a reinstall so an interrupted reinstall can never fall back to the old system. The release and the sources of images are in [chapter 08](08-updates-and-release-channels.md).

## How to build and test it

Build the bootloader alone (from `bootloader/`, after `. ~/esp/esp-idf-v5.3.5/export.sh`):

```
idf.py -B build_tdeck_plus -D SDKCONFIG=build_tdeck_plus/sdkconfig -D IDF_TARGET=esp32s3 -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.board.tdeck_plus" build
```

The output is `build_tdeck_plus/bootloader/bootloader.bin`. Flash only that file at 0x0. Never flash the whole suggested `flash_args` from this project: it also writes a placeholder app. Builds in the repository's `out/` folder (for example `out/boot_tdeck_plus`) are what the release scripts use ([runbook](runbook.md)). Do not start firmware builds unless the owner asks.

Build the boot package with `python3 purrstrap/purrstrap.py bootpkg build --board tdeck_plus` ([chapter 09](09-purrstrap.md)). `tools/boot/render_real.py` draws the real boot screens on the host as PNGs in `out/boot-preview/`.

Host tests that cover this chain ([chapter 10](10-host-tests.md)): `test_trailer`, `test_trust`, `test_cfg`, `test_menu`, `test_reason`, `test_keybag`, `test_swap`.

## What goes wrong and how to fix it

| Symptom | Likely cause | Fix |
|---|---|---|
| No `purr_boot: PURR OS bootloader` line in the serial log | A stock IDF bootloader is running, not ours | Rebuild from `PurrOS/` or `Kernel/` (they now build ours), delete the stale `sdkconfig` in the build folder, and flash the bootloader |
| Reset loop under `enforce` | The slot has no trailer and nothing else is usable | Flash with a trailer (`purrstrap coreos slot-image`, [chapter 09](09-purrstrap.md)) or set `warn`; see [chapter 03](03-trust-and-keys.md) |
| Menu shows up every start | `boot_fail_count` is above 1: the last start never reached a healthy state | Find why the OS never marked itself healthy |
| Menu restarts the device after about 5 s | Old bootloader without the watchdog feed | Bootloader 0.4.0 feeds the RTC watchdog while the package waits ([plan3.md](../plan3.md), "Bootloader 0.4.0"); a bootloader change needs a flash of 0x0 |
| Handoff reads as missing | Cold power-on, or stock bootloader | The block survives soft resets, not power loss; this is normal on cold boot |
| Build fails with a `sdkconfig` symbol error | Old `sdkconfig` in an existing build folder ignores new defaults | Delete that folder's `sdkconfig` and configure again |

The bootloader had 8,352 bytes free of its 40 KB when last measured ([bootloader/SPEC.md](../bootloader/SPEC.md) section 2.1, 2026-10-25). Later changes (the quiet start, the watchdog feed) used some of it, and [plan3.md](../plan3.md) says about 5 KB were free in November. Re-measure before adding code.

## What is not finished

- **Fast startup is disabled** (owner, 2026-11-04). The OS refuses to turn it on and clears the flag every start. Quick check and Really fast boot are designed and not built ([plan3.md](../plan3.md)).
- **Bootloader updates over the air are off** (2026-11-05): ESP-IDF aborts erasing flash 0x0. The bootloader reaches a device by cable. Steps to bring it back are in [plan3.md](../plan3.md).
- **What `enforce` does with nothing usable** (reset loop, halt, or prompt) is decided in part: KittenOS stays bootable (owner, 2026-10-28, [plan3.md](../plan3.md) item 6).
- **Not verified on a device:** the loader rung of the ladder, `reboot recovery` / `reboot loader` from the boot menu, and the full seven-power-cycle ladder ([plan1.md](../plan1.md) checklist). The reasons the bootloader gives the loader have run in the VM only.
- **Plan and code differ in two places.** [PurrOS/SPEC.md](../PurrOS/SPEC.md) describes CoreOS as a file loaded by a thin kernel; the shipped kernel slot is the monolith above. [bootloader/SPEC.md](../bootloader/SPEC.md) section 2 lists older partition sizes; the CSV is right ([chapter 02](02-flash-map-and-partitions.md)).
- **`Kernel/`** is a standalone kernel project of about 267 KB that idles after bring-up ([Kernel/README.md](../Kernel/README.md)). It is not part of the release.

Plan references: [plan1.md](../plan1.md) (boot chain, closed 2026-10-28) and [plan3.md](../plan3.md) (hardening).
