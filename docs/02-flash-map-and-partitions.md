# 02. The flash map and the partitions

*Current as of PURR OS 1.1 (2026-11-08).*

## Short version

- The T-Deck Plus has 16 MB of flash. The layout is in one file: [PurrOS/partitions/tdeck_plus.csv](../PurrOS/partitions/tdeck_plus.csv). If any document disagrees with that file, the file is right.
- The bootloader sits at 0x0 and may use 40 KB. The partition table is at 0xA000 (moved from 0x8000 on 2026-10-25).
- Three app slots, all fixed size: `kittenos` 0x20000 (1.75 MB), `kernel` 0x1E0000 (2.125 MB), `loader` 0x400000 (1.75 MB). In flash map v2 each is 256 KB larger than it was up to 1.0.28, room to grow into. There is no OTA pair and no `otadata`.
- **Flash map v2 (release 1.0.29).** Only those three slots, besides what is read without a file system (`nvs`, `purrcfg`, `phy_init`, `bootpkg`, `netrec`), have partitions of their own. Everything else (picture packs, themes, shells, apps, drivers, modules) is a file in `root` (LittleFS), which now runs from 0x5C1000 to the end of the flash: 0xA3F000 bytes, about 10.2 MiB (the three slots took 768 KB of the 5 MB the picture partition freed). The old 5 MB `ui_assets` partition is gone. This is **designed and written into the tables and tools, and has not yet run on a device**; the 1.0.28 and older layout is described under "The old layout" below.
- The last 4096 bytes of each app slot hold the signature trailer, so an image may be at most the slot size minus 4096.
- Changing the table means rebuilding every image (the app reads the table at the compiled offset) and erasing `root` on devices flashed with an older layout. The table is never replaced by an update (bootloader update is off), so moving to flash map v2 takes a **cable flash and a full erase of `root`** ([plan1.md](../plan1.md)).

## What it is for

The flash map fixes where each part of the system lives, so that the bootloader, the boot package, the recovery loader, the OS and the build tools all agree on addresses. It is also the reason updates are possible without a second copy of everything: each slot has one job, and the recovery tiers write the slots they are allowed to write.

## The files

| File | What it is |
|---|---|
| [PurrOS/partitions/tdeck_plus.csv](../PurrOS/partitions/tdeck_plus.csv) | T-Deck Plus, 16 MB. The layout the OS images are built against |
| [PurrOS/partitions/tdeck.csv](../PurrOS/partitions/tdeck.csv) | T-Deck (non-Plus). Same layout as the Plus |
| [PurrOS/partitions/waveshare154.csv](../PurrOS/partitions/waveshare154.csv) | Waveshare e-paper 1.54, 8 MB flash, smaller slots. Its own table in the flash map v2 shape (2026-11-08), scaled to 8 MB |
| [bootloader/partitions.csv](../bootloader/partitions.csv) | The bootloader project's copy of the T-Deck Plus table (identical to the one above, apart from comments) |
| [bootloader/partitions/waveshare154.csv](../bootloader/partitions/waveshare154.csv) | The bootloader project's copy for the e-paper board |
| [Kernel/partitions.csv](../Kernel/partitions.csv) | Table for the standalone kernel project. Same rows as the T-Deck Plus table (flash map v2) |
| [sdk/boards/tdeck_plus.toml](../sdk/boards/tdeck_plus.toml) | The SDK's board profile, with the same table as `[[partition]]` blocks; a test keeps it equal to the CSVs, `purr inspect` compares a flash image against it |
| [PurrOS/sdkconfig.board.tdeck_plus](../PurrOS/sdkconfig.board.tdeck_plus) | Selects the table file and the 16 MB flash size |
| [PurrOS/sdkconfig.defaults](../PurrOS/sdkconfig.defaults) | Sets `CONFIG_PARTITION_TABLE_OFFSET=0xA000` |
| [PurrOS/components/coreos/include/purr_abi.h](../PurrOS/components/coreos/include/purr_abi.h) | `purrcfg` and handoff layout |
| [PurrOS/components/kernel/src/purr_fs_flash.c](../PurrOS/components/kernel/src/purr_fs_flash.c) | LittleFS on the `root` partition |
| [bootloader/SPEC.md](../bootloader/SPEC.md) | Sizing rules, section 2.1 |
| [PurrOS/SPEC.md](../PurrOS/SPEC.md) | Section 4: the older flash-layout discussion |

## The T-Deck Plus map

Addresses are flash offsets. "End" is the first byte after the partition.

| Name | Type, subtype | Offset | Size | End | What is in it |
|---|---|---|---|---|---|
| bootloader | (not a partition) | 0x0 | 40 KB | 0xA000 | The PURR bootloader |
| partition table | | 0xA000 | 4 KB | 0xB000 | The table itself |
| `nvs` | data, nvs | 0xB000 | 16 KB | 0xF000 | ESP-IDF key/value store (Wi-Fi driver data); used by the OS, never by the bootloader |
| `purrcfg` | data, 0x40 | 0xF000 | 8 KB | 0x11000 | Two 4 KB copies (A, B) of the boot configuration |
| `phy_init` | data, phy | 0x11000 | 4 KB | 0x12000 | Radio calibration data |
| `bootpkg` | data, 0x40 | 0x12000 | 56 KB | 0x20000 | The signed boot package |
| `kittenos` | app, factory | 0x20000 | 0x1C0000 (1.75 MB) | 0x1E0000 | KittenOS (recovery profile) |
| `kernel` | app, ota_0 | 0x1E0000 | 0x220000 (2.125 MB, 2,228,224 bytes) | 0x400000 | The full OS image |
| `loader` | app, test | 0x400000 | 0x1C0000 (1.75 MB) | 0x5C0000 | The recovery loader (minimal profile) |
| `netrec` | data, 0x41 | 0x5C0000 | 4 KB | 0x5C1000 | Last Wi-Fi network, for recovery |
| `root` | data, 0x83 | 0x5C1000 | 0xA3F000 (about 10.2 MiB) | 0x1000000 | LittleFS root filesystem: the system, the apps and the picture packs |

The map fills the 16 MB exactly. No bytes are unused between rows. This is the **flash map v2** table of release 1.0.29 (owner's approval, 2026-11-06, [plan1.md](../plan1.md)); it is written into the CSVs and the build and SDK tools but has not yet run on a device.

### The old layout (1.0.28 and older)

Up to 1.0.28 the table had one more row, and `root` was smaller: `ui_assets` (data, 0x42) at 0x501000 for 5 MB (0x500000), a raw partition that held the picture pack and was read in place from flash, and `root` at 0xA01000 for 0x5FF000 bytes (about 6 MB). Up to 1.0.28 the slots were `kittenos` 0x20000 / 0x180000 (1.5 MB), `kernel` 0x1A0000 / 0x1E0000 (1.875 MB), `loader` 0x380000 / 0x180000 (1.5 MB) and `netrec` at 0x500000. Flash map v2 removed `ui_assets`, gave each of the three slots 256 KB more (so `netrec` moved up to 0x5C0000) and gave the rest to `root` (about 6 MB to about 10.2 MiB, +4.25 MB). The pack is now a file in LittleFS, `/ui/<shell>/pack.bin` ([chapter 12](12-picture-pack-and-fonts.md)).

## How it works

### Why the app slots have odd subtypes

ESP-IDF's stock code that jumps to an app only knows certain subtypes. The `kernel` slot keeps the `ota_0` subtype and `loader` the `test` subtype so the stock loader can still jump to them. They are not an A/B pair. The PURR bootloader picks the slot itself ([chapter 01](01-boot-chain.md)); nothing reads `otadata`, and that partition was removed on 2026-10-25 to make room.

### The trailer sector

Each of `kernel`, `kittenos` and `loader` is a real ESP-IDF app image starting at offset 0 of its partition. The signature cannot go first, because the stock loader needs the ESP magic byte there. It goes in the last 4096 bytes of the partition: a verbatim copy of the signed container header, then 0xFF. The usable image size is therefore the partition size minus 4096.

| Slot | Partition size | Largest image |
|---|---|---|
| `kittenos` | 1,835,008 | 1,830,912 |
| `kernel` | 2,228,224 | 2,224,128 |
| `loader` | 1,835,008 | 1,830,912 |

The builder checks this: `purrstrap coreos slot-image` refuses an image that does not fit ([purrstrap/scripts/coreos.py](../purrstrap/scripts/coreos.py)).

### `purrcfg`: two copies

The newest copy with a good CRC wins. A store writes the other sector, reads it back, and only then counts. A power cut leaves either the old copy or the new one. The struct is `purr_cfg_t` in [purr_abi.h](../PurrOS/components/coreos/include/purr_abi.h) and must fit one sector. It holds the secure mode, the boot target, flags, revoked keys, the failure count, the version floors, up to 8 override keys, and the signature of the trust block. The code that reads and writes it is [purr_cfg.c](../PurrOS/components/coreos/src/purr_cfg.c). Trust fields are explained in [chapter 03](03-trust-and-keys.md).

### The bootpkg RAM window

The boot package is not run from flash. The bootloader copies it into the bottom of internal SRAM, which is free before the OS loads. The window is seen twice by the chip: as data at 0x3FC88000 and as code at 0x40378000. The first 32 KB is code and the next 32 KB is data and bss. The addresses are in `purr_abi.h` and the linker script is [bootpkg/link/esp32s3.ld](../bootpkg/link/esp32s3.ld).

### `root` and its folders

`root` is LittleFS, mounted by the kernel through [purr_fs.c](../PurrOS/components/kernel/src/purr_fs.c). The code creates or uses these top-level folders:

| Folder | Holds |
|---|---|
| `/boot` | Staged files (`.new`, `.bak`), the drivers folder `/boot/drivers`, the staged bootloader |
| `/system`, `/kernelmods` | Signed command modules and kernel modules |
| `/apps` | Apps visible to every user (owner decision, 2026-10-28, [plan9.md](../plan9.md)) |
| `/home/<user>` | Each user's files, apps, data and `permissions.conf` |
| `/etc` | Settings, `apps.conf`, `board.conf`, `system.conf` |
| `/var/state` | Small runtime state |

### The other boards

The e-paper board has 8 MB of flash, so its slots are smaller. Since 2026-11-08 its table has the flash map v2 shape (the files in LittleFS, the slots with room to grow): `kittenos` 0x20000 / 0x160000 (1.375 MB), `kernel` 0x180000 / 0x180000 (1.5 MB), `loader` 0x300000 / 0x160000 (1.375 MB), `netrec` 0x460000 and `root` 0x461000 / 0x39F000 (about 3.6 MiB, to the end of the flash). Before that: `kittenos` 0x120000, `kernel` 0x140000, `loader` 0x120000 and `root` of 0x45F000 at 0x3A1000. Each slot is 256 KB larger, the step the T-Deck took. The KittenOS image measures 1.19 MB and the loader 1.13 MB; the full image does not link for this board yet, so the kernel slot is not measured. The table is not run on a device and needs a cable flash and a full erase of `root`; `purrstrap dev recovery` checks the board's own slot sizes. The first four rows (`nvs`, `purrcfg`, `phy_init`, `bootpkg`) are the same as the T-Deck Plus. The original ESP32 board (the CYD 2.4C) appears in [coreos.py](../purrstrap/scripts/coreos.py) and in the bootloader sdkconfig, but [PurrOS/partitions/](../PurrOS/partitions/) has no table for it, so the PurrOS images cannot be built for it today.

## What each partition holds, byte for byte

The partitions are not all written the same way. This matters when you flash by hand or write a tool.

| Partition | What is written | Checked by |
|---|---|---|
| `kernel`, `kittenos`, `loader` | The payload only (the raw ESP-IDF image) from offset 0, 0xFF fill, and the 173-byte header copy plus 0xFF in the last 4096 bytes | The bootloader, from the trailer |
| `bootpkg` | The **whole signed container**: header, then payload (a 16-byte preamble, code, data) | The bootloader, as a container, before it copies the code into RAM |
| `purrcfg` | Two sectors holding `purr_cfg_t` | CRC, and the trust signature for the trust fields |
| `netrec` | One small record: magic, network name, password, CRC | The loader |
| `root` | LittleFS, including the picture packs (`/ui/<shell>/pack.bin`, each with its own CRC; the signed container around a pack is checked before the file is written) | LittleFS itself; each file by the OS that loads it |

The password in `netrec` is plain data. Real protection would need flash encryption, which is not used ([SECURITY.md](../SECURITY.md) section 2).

## Building a whole-flash image

`tools/hw/make_hw_image.sh` assembles a 16 MB image for a blank T-Deck Plus. It starts from a block of 0xFF and writes each part at the offsets in the table above: the bootloader at 0x0 (at most 0xA000 bytes), the partition table at 0xA000, a signed `purrcfg` at 0xF000, the boot package at 0x12000, and the loader slot at 0x400000. With `FULL=1` it also writes the kernel at 0x1E0000 and KittenOS at 0x20000. `netrec` and `root` (0x5C0000 to the end) stay blank: the picture pack, the modules and the apps arrive from the setup wizard or a recovery. `MINIMAL=1` writes only the bootloader, table, trust block and loader; the loader then installs the rest from the network. Every `put()` asserts that the data fits its partition, and the script refuses a partition table that is not flash map v2 (so a stale `out/stdfull` cannot put the old layout on a device), so a layout mistake stops the script and not the device. `tools/vm/mkimage_dbg.sh` (the VM test image) writes the build's table into the base image too, and erases everything from 0x20000 up when the base carried another table. The script needs the boot key's passphrase in the environment of that one command, to sign the `purrcfg` ([chapter 03](03-trust-and-keys.md), [runbook](runbook.md)).

Flashing writes over whatever is there, so an older layout's data (a different `root`) stays until erased. When a layout changes, erase the whole chip first.

## How to check and change it

To see what a build thinks the map is, open the CSV, or read the build's `partition_table/partition-table.bin`. To print a binary table from an ESP-IDF shell:

```
python3 $IDF_PATH/components/partition_table/gen_esp32part.py out/stdfull/partition_table/partition-table.bin
```

To change a partition:

1. Edit [PurrOS/partitions/tdeck_plus.csv](../PurrOS/partitions/tdeck_plus.csv) and copy the same change to [bootloader/partitions.csv](../bootloader/partitions.csv). They must stay identical.
2. Check that nothing overlaps and that the end is still 0x1000000.
3. Delete the `sdkconfig` in every build folder, so the new table is picked up.
4. Rebuild every image. Each app finds the table at the offset compiled into it.
5. Reflash the table (0xA000) and everything whose address changed. A device flashed with an older layout needs `root` erased and formatted again.
6. Update this chapter and [bootloader/SPEC.md](../bootloader/SPEC.md) in the same step.

Host tests do not read the CSV. The check that ties the map to the code is `purrstrap coreos slot-image`, which reads the CSV for sizes (no purrstrap test covers it; the unit tests for the trailer itself are in [chapter 10](10-host-tests.md)).

## What goes wrong and how to fix it

| Symptom | Cause | Fix |
|---|---|---|
| App finds no partitions, or the wrong ones | Image built against a different table offset | Rebuild all images; keep 0xA000 everywhere |
| `root` will not mount after a flash | Older layout, `root` moved | Erase `root` and let the OS format it; the data on it is lost |
| "app partitions too small" warning on a full build | Full image is bigger than `kittenos` or `loader` | Expected: the full image goes in `kernel` only |
| `slot-image` says the image does not fit | Image plus 4096 trailer is over the slot | Shrink the image, or move the layout (steps above) |
| Kernel image near its limit | 1,876,112 of 1,966,080 bytes at 1.0.26 (the slot is 2,228,224 bytes in flash map v2) | Measure before enabling features that add code ([showtime1.md](../showtime1.md) part D) |
| Boot loop after flashing only some slots | Slots from different layouts | Flash the table and all slots together |
| A device from 1.0.28 or older shows a boot loop or an unformatted `root` after an update of only the kernel | The partition table did not change (an update never replaces it), so the kernel looks for `root` at the new address in an old table | Cable flash of a whole flash map v2 image with the chip erased |

## What is not finished

- **Plan and code disagree in several places.** [bootloader/SPEC.md](../bootloader/SPEC.md) section 2 still shows `kittenos` and the loader at about 1 MB, and `purrcfg` at 0xD000. The CSV has 1.75 MB slots (flash map v2; 1.5 MB up to 1.0.28) and `purrcfg` at 0xF000. The spec should be corrected; this chapter follows the CSV.
- **Flash map v2 has not run on a device.** The tables, the board profile, `make_hw_image.sh` and `mkimage_dbg.sh` are written to it; the firmware, the pack tools and the Store side are the other halves ([plan1.md](../plan1.md), [plan7.md](../plan7.md)). Until a cable flash has proved it, treat the whole chapter as the design.
- **The LittleFS lookahead buffer** is small for about 2,600 blocks and is meant to grow with the map ([plan1.md](../plan1.md)); that is firmware, not a table matter.
- **No tables for other chips yet.** The CYD and the Heltec boards have no PurrOS table ([Boards/SPEC.md](../Boards/SPEC.md)).
- **Secure Boot and flash encryption are not used**, so nothing protects the partition table or the bootloader against someone with a flash programmer ([SECURITY.md](../SECURITY.md) section 2, [chapter 03](03-trust-and-keys.md)).

Plan references: [plan1.md](../plan1.md) (the slots and trailer), [plan3.md](../plan3.md) item 3b (the layout change on 2026-10-25).
