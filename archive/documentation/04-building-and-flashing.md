# 04. Building and flashing

There are **four separate things to build** and they are built by different commands. Mixing them
up is the single most expensive mistake in this project (it cost a whole session once, see the
trap below).

| Thing | Output | Built by | Flashed to |
|-------|--------|----------|------------|
| Bootloader | `bootloader/build_<board>/bootloader/bootloader.bin` | `idf.py` inside `bootloader/` | `0x0` **only** |
| Boot package | `bootpkg/build/<board>/bootpkg.bin` | `purrstrap bootpkg build` + `keys sign` | `0x12000` |
| PurrOS (any profile) | `PurrOS/build/<board>-<profile>/purros.bin` | `purrstrap coreos build` | see the profile table below |
| Standalone kernel | `Kernel/build_<board>/purr_kernel.bin` | `idf.py` inside `Kernel/` | `0x1a0000` **(read the warning)** |

## The trap: two bootloaders

`PurrOS/` builds its own copy of the **stock ESP-IDF bootloader** as a side effect. It has no boot
menu, no `purrcfg` handling, no failure ladder, no recovery dispatch and no boot package. The real
PURR bootloader is the separate project in `bootloader/`.

- `idf.py flash` and the `flash_args` file in a `PurrOS/build/...` directory include the **stock**
  bootloader. Using them overwrites the real one.
- The tell: the real bootloader logs `purr_boot: PURR OS bootloader` on the serial console. If that
  line is missing, you are running the stock bootloader and `reboot recovery`, `reboot loader`, the
  boot menu and the ladder will all appear broken. They are not. They are absent.
- Flash the app alone with `esptool` (below) or `idf.py app-flash`, never plain `idf.py flash`.

## 1. Build PurrOS

```powershell
python purrstrap/purrstrap.py coreos build --board tdeck_plus --profile full
python purrstrap/purrstrap.py coreos build --board tdeck_plus --profile recovery
python purrstrap/purrstrap.py coreos build --board tdeck_plus --profile minimal
```

Add `--clean` to delete the build directory first. Output goes to `PurrOS/build/tdeck_plus-<profile>/`,
with the full log in `purrstrap-build.log` inside it. Each profile has its own directory and its own
`sdkconfig`, so switching never reconfigures.

At the end purrstrap prints the paths of `purros.bin`, `bootloader/bootloader.bin`,
`partition_table/partition-table.bin` and `flash_args`. The binary is always called `purros.bin`
whatever the profile.

The embedded version string is `internaldirty0.2.0-<N>` where `N` counts up on every build on this
machine (`PurrOS/build/.build_number`). `DEV_VERSION_BASE` in `purrstrap/scripts/coreos.py` is the
base. The `version` shell command prints a hard-coded `0.1.0` instead ([F-18](FINDINGS.md#f-18)).

Check size: `python purrstrap/purrstrap.py coreos size --board tdeck_plus --profile full`.

### Build-time settings you can change

Set in `PurrOS/sdkconfig.defaults` (shared), `sdkconfig.board.<board>`, `sdkconfig.profile.<profile>`
and `PurrOS/main/Kconfig.projbuild`:

| Option | Default | Meaning |
|--------|---------|---------|
| `CONFIG_PURR_PROFILE_{FULL,RECOVERY,MINIMAL}` | set by profile file | which system this is |
| `CONFIG_PURR_BOARD_*` | set by board file | which board profile the kernel compiles in |
| `CONFIG_PURR_VERBOSE_BOOT` | `y` | print each module as it loads on the boot screen. Turn off for a quiet build. |
| `CONFIG_PURR_RECOVERY_MANIFEST_URL` | the author's GitHub release | where `netinstall` and the recovery loader fetch the recovery manifest |
| `CONFIG_PURR_MODULE_INDEX_URL` | the author's GitHub release | where `netinstall modules` fetches the module index |
| `CONFIG_ESP_MAIN_TASK_STACK_SIZE` | 16384 | needed for ECDSA verification. Do not lower it. |

**Gotcha:** the two URL options are set in *both* `Kconfig.projbuild` (as the default) and
`sdkconfig.defaults` (as an override). Changing one and not the other silently leaves old builds on
the old URL, because an existing `build/<board>-<profile>/sdkconfig` is not regenerated. Change both,
then build with `--clean`.

## 2. Build the bootloader

From a PowerShell with ESP-IDF activated ([03](03-development-environment.md)):

```powershell
cd bootloader
idf.py -B build_tdeck_plus -D SDKCONFIG=build_tdeck_plus/sdkconfig -D IDF_TARGET=esp32s3 `
  -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.board.tdeck_plus" build
```

Result: `build_tdeck_plus/bootloader/bootloader.bin`. The project also builds a placeholder app and a
`flash_args` file. **Do not use `flash_args`.** It would also write the placeholder app over your real
`kittenos` slot. Flash only `bootloader.bin`, at `0x0`.

The bootloader links `purr_cfg.c`, `purr_util.c`, `purr_keybag.c`, `purr_verify.c` and
`purr_default_keys.c` straight from `PurrOS/components/coreos`, so **the keys it trusts are the ones in
`PurrOS/components/coreos/keys/purr_default_keys.c`**. Rebuild it after any key change.

## 3. Build and sign the boot package

```powershell
python purrstrap/purrstrap.py bootpkg build --board tdeck_plus
python purrstrap/purrstrap.py keys sign --key signing_keys/boot.key `
  --image bootpkg/build/tdeck_plus/bootpkg.bin --key-id 1
```

`keys sign` asks for the key's password, or reads `PURRSTRAP_KEY_PASSWORD` if set. `--key-id` must be
the id the matching public key has in `purr_default_keys.c` (`1` for the boot key in this repo).
Without a valid signature the bootloader rejects the package while `secure_mode` is `warn` or
`enforce`, and you get no boot menu (the system still boots, via the fallback chain).

The package is limited to 32 KB of code and 32 KB of data plus bss. It is currently about 4 KB.

## 4. Flash a blank device

If the board does not enumerate as a serial port, put it into ROM download mode (GPIO0 held low while
it powers up; check LilyGO's instructions for which button that is on your unit). The serial port is `COMn` on Windows. Examples use `COM5`.

```powershell
$p = "COM5"
# 1. the real bootloader, at 0x0 only
python -m esptool --chip esp32s3 --port $p write_flash 0x0 bootloader\build_tdeck_plus\bootloader\bootloader.bin
# 2. the partition table (same CSV in all three projects; either build's copy is fine)
python -m esptool --chip esp32s3 --port $p write_flash 0x8000 PurrOS\build\tdeck_plus-recovery\partition_table\partition-table.bin
# 3. the signed boot package
python -m esptool --chip esp32s3 --port $p write_flash 0x12000 bootpkg\build\tdeck_plus\bootpkg.bin
# 4. KittenOS (the recovery profile) into the factory slot
python -m esptool --chip esp32s3 --port $p write_flash 0x20000 PurrOS\build\tdeck_plus-recovery\purros.bin
# 5. the recovery loader (the minimal profile)
python -m esptool --chip esp32s3 --port $p write_flash 0x380000 PurrOS\build\tdeck_plus-minimal\purros.bin
```

You can put several `address file` pairs in one `write_flash`. To start from nothing, run
`python -m esptool --chip esp32s3 --port $p erase_flash` first. That erases **everything**, including
accounts, saved Wi-Fi, `purrcfg` and the recovery network record.

Open a serial monitor at 115200 baud to watch the log (`python -m serial.tools.miniterm COM5 115200`
works, as does `idf.py monitor -p COM5` from an activated shell).

### What you should see

1. Serial: `purr_boot: PURR OS bootloader`, then `bootpkg ... loaded (key id 1, ok)`, then
   `starting the boot menu`.
2. Screen: the boot menu (see [05](05-boot-process-and-flash-layout.md)). After 3 seconds it boots the highest
   priority bootable slot.
3. With only the recovery profile at `0x20000` and nothing at `kernel`, it boots KittenOS. The first
   boot formats `root` automatically (`root: not mounted (...), formatting...`), then skips login
   because there is no PURR OS installed (`login_skip("setup", admin)`), and drops to the
   `setup@KittenOS>` prompt.

## 5. Putting the full system on a device

The `full` profile has to live in an app partition the bootloader will boot. There are two layouts
that work today.

**A. Dev layout (what the author runs).** Full profile in the factory slot, `kernel` left blank.
The bootloader prefers `kernel`, finds it is not a real image, falls back to `kittenos` (the factory
slot). You get PURR OS, with login.

```powershell
python -m esptool --chip esp32s3 --port COM5 write_flash 0x20000 PurrOS\build\tdeck_plus-full\purros.bin
```

You lose KittenOS from the boot menu in this layout (the slot is shared). To keep both, use B.

**B. Full system in `kernel`, KittenOS in `kittenos` (inference, not recorded as tested).** The
`kernel` partition is an ordinary app slot, 0x1E0000 bytes, and the full build is about 1.08 MB, so
it fits. The boot menu then lists `Kernel` and `KittenOS` as separate entries.

```powershell
python -m esptool --chip esp32s3 --port COM5 write_flash 0x1a0000 PurrOS\build\tdeck_plus-full\purros.bin
python -m esptool --chip esp32s3 --port COM5 write_flash 0x20000  PurrOS\build\tdeck_plus-recovery\purros.bin
```

This is also how a **network install can deliver the working system today**: package the full binary
as the `kernel` component (`purrstrap coreos package-component --component kernel --file
PurrOS/build/tdeck_plus-full/purros.bin --version X`), sign it with the boot key, and list it in the
recovery manifest ([10](10-updates-network-install-and-recovery.md), [18](18-making-your-own-purr-os.md)). See finding
[F-31](FINDINGS.md#f-31): as shipped, the manifest's `kernel` entry is the standalone idle kernel instead.

## 6. The standalone kernel (`Kernel/`) and its warning

```powershell
cd Kernel
idf.py -B build_tdeck_plus -D SDKCONFIG=build_tdeck_plus/sdkconfig -D IDF_TARGET=esp32s3 `
  -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.board.tdeck_plus" build
```

Output: `build_tdeck_plus/purr_kernel.bin`, about 330 KB (331,728 bytes when rebuilt for this documentation; it was 267 KB when first measured). Flash it **alone** to `0x1a0000`. The
"To flash, run" command that `idf.py` prints for this project is **wrong for this device**: it writes `purr_kernel.bin` at `0x20000`, which is the `kittenos` slot, and also writes a
bootloader and a partition table that belong to other projects. Ignore it.

What it is made of: `Kernel/main/kernel_main.c` (the `app_main` below), `kernel_table.c` (its own `purr_kernel_table_t`, filling only the console, heap, uptime and
read-mostly filesystem entries), `loader.c` (the same relocation loader the shell uses, ported because this project does not link `PurrOS/main`) and
`coreos_min/coreos_min_entry.c` (a deliberately tiny CoreOS entry point, embedded as a byte array). It reuses `PurrOS/components/kernel` directly. It is a proof that the
kernel/CoreOS split works, not a second operating system.

**It has no shell.** It brings up the display and the filesystem, loads the embedded proof CoreOS from
`/coreos.cat`, prints a listing of `root`, and idles forever. Because the bootloader prefers `kernel`,
leaving it flashed means every boot lands there. To undo: pick **KittenOS** from the boot menu (hold
a key during the 3 second countdown), or erase the partition:

```powershell
python -m esptool --chip esp32s3 --port COM5 erase_region 0x1a0000 0x1e0000
```

## 7. Fast iteration without reflashing the app

Modules and kernelmods live on the `root` filesystem, so a command can change without reflashing:
build, sign and copy a new `.cat` into `/system` or `/kernelmods` ([12](12-writing-modules.md)).
Today the only ways to get a file there are the temporary `plantmodules` command (writes the embedded
copies) and `netinstall modules` (downloads from the index). There is no USB or serial file transfer
yet (**[DESIGNED]**: MTP).

## 8. Version of an installed image

`purrstrap keys inspect --path <file.cat>` prints a container's header, including name, version, chip
id (`9` = ESP32-S3), type, subtype (low byte of flags) and key id.

Next: [05 Boot and partitions](05-boot-process-and-flash-layout.md).
