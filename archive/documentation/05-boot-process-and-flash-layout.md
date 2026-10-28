# 05. Boot process, flash layout and purrcfg

What happens from power-on to the prompt, what every partition is for, and how the boot
configuration (`purrcfg`) steers it. Everything here describes the T-Deck Plus. Source of truth:
`bootloader/bootloader_components/main/purr_boot.c`, `purr_bootpkg.c`, `purr_bootcfg.c`,
`PurrOS/components/coreos/include/purr_abi.h`, and `PurrOS/main/main.c`.

## Flash layout

`PurrOS/partitions/tdeck_plus.csv`, `bootloader/partitions.csv` and `Kernel/partitions.csv` are
byte-for-byte the same. Keep them that way: the device has one layout, not one per project.

| Name | Type / subtype | Offset | Size | Holds |
|------|----------------|--------|------|-------|
| bootloader | | `0x0` | n/a | The PURR second-stage bootloader (ESP32-S3 loads it from offset 0). |
| partition table | | `0x8000` | 4 KB | |
| `nvs` | data / nvs | `0x9000` | 24 KB | ESP-IDF's Wi-Fi internals. The bootloader never touches it. |
| `otadata` | data / ota | `0xF000` | 8 KB | Left over because ESP-IDF requires it when an `ota_*` partition exists. **Not used to choose what boots.** |
| `phy_init` | data / phy | `0x11000` | 4 KB | RF calibration. |
| `bootpkg` | data / `0x40` | `0x12000` | 48 KB (`0xC000`) | The boot package, **as a full PURR container** (header included). |
| `purrcfg` | data / `0x40` | `0x1E000` | 8 KB | Boot configuration, two 4 KB A/B sectors. |
| `kittenos` | app / factory | `0x20000` | 1.5 MB (`0x180000`) | An ESP-IDF app image. The "safe" slot. |
| `kernel` | app / `ota_0` | `0x1A0000` | 1.875 MB (`0x1E0000`) | An ESP-IDF app image. The primary boot slot. Subtype `ota_0` only so ESP-IDF's stock image loader can jump to it. |
| `loader` | app / test | `0x380000` | 1.5 MB (`0x180000`) | An ESP-IDF app image: the recovery loader. Subtype `test` is how the bootloader recognises it. |
| `netrec` | data / `0x41` | `0x500000` | 4 KB | The recovery network record (SSID and password, plain text). |
| `root` | data / `0x83` | `0x501000` | about 11 MB (`0xAFF000`) | LittleFS. Everything else. |

**[PROBLEM]** The specs call the loader partition `rescue`, size `kittenos` at 1 MB and `bootpkg`
at 56 KB, and do not list `netrec`. The CSV is the truth ([F-07](FINDINGS.md#f-07)).

### What a partition actually contains

- `bootpkg` holds the **whole container**, because the bootloader verifies it from flash.
- `kernel`, `kittenos` and `loader` hold a **plain ESP-IDF app image** with no PURR header, because
  the stock ESP-IDF loader (which does the MMU setup and segment loading) insists on the ESP magic
  byte `0xE9` at offset 0. When these are written by `netinstall` or the recovery loader, the PURR
  header is verified and then **stripped**: only the payload is written. See
  [F-13](FINDINGS.md#f-13) and [F-15](FINDINGS.md#f-15).
- `root` is LittleFS with 4096-byte blocks.

## The boot sequence

```
ROM -> PURR bootloader
        1. load the partition table
        2. load purrcfg; boot_fail_count++ ; write it back
        3. choose a preferred slot from boot_fail_count (the ladder)
        4. one-shot flags FORCE_RECOVERY / FORCE_LOADER can override that
        5. otherwise load the boot package and show the menu (it may pick another slot)
        6. jump into the chosen slot with the stock ESP-IDF loader
```

### Step 2: the failure counter

The bootloader increments `boot_fail_count` on **every** boot, before anything else, and stores it.
PURR OS and KittenOS reset it to 0 once their boot setup finishes (`purr_mark_boot_healthy()` in
`main.c`, called after the filesystem, modules, apps and Wi-Fi are set up and just before the login
prompt). A boot that never gets that far leaves the count where the bootloader put it, so it climbs
across repeated failures.

### Step 3: the ladder

A slot only counts as present if its partition exists and its first byte is `0xE9`. That is the
only check the bootloader makes on `kernel`, `kittenos` and `loader` ([F-15](FINDINGS.md#f-15)).

| `boot_fail_count` after the increment | Order tried |
|---------------------------------------|-------------|
| 1 to 3 | `kernel`, `kittenos`, `loader` |
| 4 to 6 | `kittenos`, `loader`, `kernel` |
| 7 and up | `loader`, `kittenos`, `kernel` |

The first slot in the order that looks bootable wins. If none does, the bootloader logs
`no bootable app found` and resets, which loops forever ([F-28](FINDINGS.md#f-28)).

The specs say the ladder is "0-2 / 3-5 / 6+", that the boot package does the counting, and that it is
not built. All three are stale: it is built, it lives in the bootloader, and the thresholds are the
ones in the table ([F-06](FINDINGS.md#f-06)).

### Step 4: one-shot flags

Checked in this order. A flag is **cleared before it is acted on**, so a crash in what it starts
cannot loop.

1. `FORCE_RECOVERY` set and a `kittenos` partition exists: boot KittenOS, skip the menu. This is what
   `reboot recovery` writes, and what `netinstall` of a file-staged component writes.
2. `FORCE_LOADER` set and a `loader` partition exists: boot the recovery loader, skip the menu.
   This is what `reboot loader` writes.
3. The ladder picked the loader (count 7 or more) and no flag overrode it: skip the menu, boot the
   loader, and set `AUTO_REINSTALL` in `purrcfg` so the loader runs its silent automatic mode.

### Step 5: the boot package and its menu

If none of the above applied, the bootloader loads the boot package from the `bootpkg` partition:

1. Verify it: magic, chip (ESP32-S3), type `module`, subtype `bootpkg`, layout, payload SHA-256 and
   the ECDSA P-256 signature against the key bag (boot role only).
2. A package that fails verification is **rejected unless `secure_mode` is `off`**. With the default
   `warn`, an unsigned package is rejected, the menu does not appear, and the system just boots the
   ladder's choice. This is the fallback: no menu is not no boot.
3. Copy it into a fixed SRAM window (code at `0x40378000`, data at `0x3FC90000`, 32 KB each) and call
   `purr_pkg_entry(services, parts, nparts, preferred)`.

The menu (`bootpkg/src/pkg_main.c`, rules in `PurrOS/components/coreos/src/purr_menu.c`):

- Title `PURR OS`, subtitle `boot menu`, a line `Secure Boot: off` or `Secure Boot: ON` (a read-only
  look at the chip's hardware Secure Boot eFuse; nothing is ever burned), and `keyboard ok` or
  `keyboard not answering`.
- One entry per **bootable** slot, in this order: every `kernel`-subtype slot labelled `Kernel`, then
  `KittenOS`. The preselected entry is the one the ladder chose.
- A **3 second countdown** (`Booting in N...  any key stops`). Any key stops it for good.
- Keys: **W** up, **S** down, **D** or **Enter** to select. The T-Deck keyboard has no arrow keys.
- If nothing at all is bootable, after 2 seconds a single entry `Internet recovery` appears. It never
  boots by itself. Choosing it starts the recovery loader. **This is the only time the menu offers it**
  ([F-32](FINDINGS.md#f-32)). Otherwise reach the loader with `reboot loader` from a shell.

The package drives the panel with bit-banged SPI and the keyboard with bit-banged I2C, straight to the
GPIO registers. It is slow on purpose and needs no drivers, which is why it is about 4 KB.

### Step 6: the jump

`bootloader_utility_load_boot_image()`, ESP-IDF's own loader, maps and starts the chosen app.

## What the three app slots run

| Slot | Intended content | Notes |
|------|------------------|-------|
| `kernel` | the full-profile PURR OS, or the standalone `Kernel/` binary | `netinstall kernel` writes here. The standalone kernel has **no shell**, so putting it here makes every boot land in an idle screen ([04](04-building-and-flashing.md)). |
| `kittenos` | the recovery-profile build | only the recovery loader may rewrite it |
| `loader` | the minimal-profile build | the last resort |

`main.c` in the **recovery** profile also checks the `ota_0`/`ota_1`-subtype slots (so, `kernel`) for
a real image. If there is none, KittenOS skips login entirely, because requiring an account before it
can install the thing that creates accounts would be backwards.

## What PURR OS does after the jump

`PurrOS/main/main.c`, `app_main()`, in order. This is the whole "boot flow" of the running system:

1. **`minimal` profile only:** run the recovery loader and never return ([10](10-updates-network-install-and-recovery.md)).
2. `purr_kernel_init()`: peripheral power, SPI bus, keyboard I2C, display. If the display does not come up, log `display did not come up` every 2 seconds, forever.
3. Backlight to full, start the text console.
4. Print `<name> starting`, mount the filesystem (`purr_fs_setup`). **KittenOS formats a root that fails to mount** ([F-37](FINDINGS.md#f-37)); PURR OS prints `run: format --yes`.
5. **KittenOS only:** `purr_swap_setup()` acts on a pending staged update. It restarts the device and never returns if it did anything.
6. `purr_modules_setup()`: sweep `/system`, then `/kernelmods` ([12](12-writing-modules.md)).
7. `purr_apps_setup()`: make `/home/<user>/apps` for every account and remove half-finished installs.
8. `purr_net_setup()`: start Wi-Fi and the reconnect task ([08](08-networking.md)).
9. `purr_continue_install()`: stage 2, only if `CONTINUE_INSTALL` is set.
10. Wait 1.2 seconds so the status lines can be read.
11. `purr_mark_boot_healthy()`: reset `boot_fail_count` to 0, confirm a swapped update.
12. Forever: clear the screen, `run_login()`, `run_shell()` until `logout`.

A failure in steps 6 to 9 does not stop the boot. A crash anywhere before step 11 leaves the failure counter up, which is how the ladder learns something is wrong.

## purrcfg

Two 4 KB sectors, A and B, at the start of the `purrcfg` partition. A store writes the **other**
sector, bumps `seq`, seals a CRC-32 and reads it back, so a power cut leaves either the old copy or
the new one. A load takes the valid copy with the higher `seq`. If neither is valid (a blank or
erased partition), defaults are used: `secure_mode = warn`, everything else zero.

The struct (`purr_cfg_t`, 752 bytes, packed, little-endian) is in
[21](21-reference-formats-abis-limits.md). The fields that matter to you:

| Field | Meaning | Who writes it |
|-------|---------|---------------|
| `secure_mode` | 0 off, 1 warn, 2 enforce | **nobody at runtime** ([F-08](FINDINGS.md#f-08)) |
| `flags` | one-shot flags, below | shell, bootloader, loader |
| `boot_fail_count` | the ladder's counter | bootloader up, OS down |
| `boot_seq` | boot counter | **nothing** ([F-08](FINDINGS.md#f-08)); the spec says the bootloader does |
| `update` | state machine for a staged system file | `netinstall`, KittenOS, `purr_mark_boot_healthy` |
| `floors[8]` | per-component version floors | raised when an update is confirmed |
| `keys[8]`, `revoked_keys` | key-bag overrides and a revocation mask | **nothing writes them** |
| `latest_time` | certificate time floor | **nothing** |

### The flags

| Flag | Bit | Status | Effect |
|------|-----|--------|--------|
| `IGNORE_ONCE` | 0 | **[DESIGNED]** | defined, no code reads it |
| `UPDATE_KEY` | 1 | **[DESIGNED]** | defined, no code reads it |
| `SECURE_OFF_ONCE` | 2 | **[DESIGNED]** | defined, no code reads it |
| `FORCE_RECOVERY` | 3 | **[WORKS]** | boot KittenOS once |
| `FORCE_LOADER` | 4 | **[WORKS]** | boot the recovery loader once |
| `AUTO_REINSTALL` | 5 | **[WORKS]** | set by the bootloader when the ladder chose the loader; read and cleared by the loader, which then runs silently |
| `CONTINUE_INSTALL` | 6 | **[WORKS]** | set by the loader after a successful install; read and cleared by KittenOS on its next boot, which then installs `kernel` and the modules |

The eFuse arming flow in `bootloader/SPEC.md` section 8 is **[DESIGNED]**: no code, and
`purr_cfg_t.efuse_arm` is unused.

### Reading purrcfg

From a shell: `purrcfg`. Example output:

```
copy:        stored
seq:         37
secure mode: warn
flags:       0x0
boot count:  0, fails 0
```

(`boot count` is `boot_seq`, which never changes, so it reads 0.)

### Changing purrcfg from the PC

There is no shell command or purrstrap action to set `secure_mode`. To change it, write a purrcfg
image into the partition. This Python builds a valid one; it was checked against the real C
`purr_cfg_load` (it reads back `secure_mode=2 seq=1 size=752`):

```python
import struct, sys, zlib

FMT = "<IHHI BB2s I I I I I  BBBBI  64s 544s 32s 64s"
def build(secure_mode=1, flags=0, seq=1):
    body = struct.pack(FMT,
        0x47464350, 1, 752, seq,          # magic 'PCFG', version, size, seq
        secure_mode, 0, b"\0\0",          # secure_mode, boot_target, reserved
        flags, 0, 0, 0, 0,                # flags, revoked_keys, boot_seq, boot_fail_count, latest_time
        0, 0, 0, 0, 0,                    # update: target, state, attempts, reserved, version
        b"\0"*64, b"\0"*544, b"\0"*32, b"\0"*64)   # floors, keys, efuse_arm, reserved1
    assert len(body) == 748
    return body + struct.pack("<I", zlib.crc32(body) & 0xFFFFFFFF)

mode = {"off": 0, "warn": 1, "enforce": 2}[sys.argv[1]]
open(sys.argv[2], "wb").write(build(mode).ljust(4096, b"\xff") + b"\xff" * 4096)
```

```powershell
python mkcfg.py enforce purrcfg.bin
python -m esptool --chip esp32s3 --port COM5 write_flash 0x1e000 purrcfg.bin
```

This resets `seq`, `boot_fail_count`, floors and the update state. To go back to defaults
(`warn`, all counters zero), erase the partition instead:

```powershell
python -m esptool --chip esp32s3 --port COM5 erase_region 0x1e000 0x2000
```

## The handoff

`purr_handoff_t` (16 bytes) is defined in `purr_abi.h` and the specs say the bootloader leaves it in
RTC memory for CoreOS. **[PARTIAL]** Nothing writes it and nothing reads it; there is no
`CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC` in any sdkconfig ([F-05](FINDINGS.md#f-05)). The decision table
(`purr_decision.c`, 460 host checks) that would act on it is also not called by the running system.

## Secure boot, honestly

The bootloader verifies the boot package by signature. It does **not** verify `kernel`, `kittenos`
or `loader`. Nothing verifies the bootloader itself (no eFuse is burned). Everything else is verified
when it is *installed* or *loaded* by the running system, not when it boots. Real per-image
verification for the three slots is designed (a separate metadata slot, or loading an ESP image at an
offset inside a container) and unbuilt; the spec records both options.

Next: [06 Using the shell](06-using-the-shell.md).
