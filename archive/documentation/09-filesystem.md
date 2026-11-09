# 09. Filesystem

One filesystem holds everything that is not raw boot flash: the `root` partition, formatted as
**LittleFS** (vendored version 2.11, on-disk format 2.1). Source: `PurrOS/components/kernel/src/purr_fs.c`,
`purr_fs_flash.c`, `PurrOS/components/littlefs/`. Commands: the `fs` kernelmod
(`Modules/coreos/fs_module.c`).

**Status: [WORKS]** (85 host checks on a RAM disk, plus the shell cluster recorded as confirmed on hardware).

## Properties

| Property | Value |
|----------|-------|
| Partition | `root`, `0x501000`, `0xAFF000` bytes (about 11.0 MB) |
| Block size | 4096 bytes (the flash erase unit); about 2815 blocks |
| Wear levelling | LittleFS's, with a block-cycle limit of 500 |
| Power loss | LittleFS is copy-on-write and journals metadata. A cut leaves the old or the new state. |
| Rename | atomic. The whole update system depends on this ([10](10-updates-network-install-and-recovery.md)). |
| Write | replaces the file. The old content stays until the new file is closed. |
| Name length | up to 255 characters per component (LittleFS), but shell paths are capped at 79 |
| Open files | **one at a time**: the wrapper has a single file buffer and no lock |
| Mount | PURR OS mounts at boot and never formats on its own. KittenOS formats a blank or damaged `root` automatically. |

## Directory layout

Created on demand; none of it is mandatory except what the system itself writes.

| Path | Used by | Contents |
|------|---------|----------|
| `/etc/passwd`, `/etc/shadow` | login | accounts ([07](07-user-accounts.md)) |
| `/etc/wifi` | Wi-Fi service | saved networks, plain text ([08](08-networking.md)) |
| `/home/<user>/apps/<app>/package.cat` | app manager | one folder per installed app ([11](11-apps.md)) |
| `/home/<user>/apps/<app>.tmp/` | app manager | a half-finished install, removed at the next boot |
| `/system/<name>.cat` | module loader | CoreOS-level modules |
| `/system/.rejected/<name>.cat` | module loader | modules that failed verification, kept as evidence |
| `/kernelmods/<name>.cat` | module loader | kernel-level modules |
| `/boot/<name>.new`, `.bak`, `.bad` | update swap | staged, previous and failed copies of CoreOS, AppManager, runtime, devices bundle |
| `/coreos.cat` | standalone `Kernel/` only | the embedded proof CoreOS, written at every boot of that kernel |

`/boot` file names are fixed by `purr_swap_filename()`: `coreos.kitt`, `appmanager.kitt`, `runtime.kitt`,
`devbundle.kitt` (so `.kitt`, unlike modules; see [F-16](FINDINGS.md#f-16)).

The specs also propose `/data`, `/var/log` and `/tmp`. **[DESIGNED]**, nothing creates or uses them.

## Working with files

```
alice@PURR OS> ls /
  etc/
  home/
  system/
  kernelmods/
alice@PURR OS> ls /system
  about.cat            2737
  apps.cat             ...
alice@PURR OS> mkdir /notes
alice@PURR OS> write /notes/todo.txt buy milk
alice@PURR OS> cat /notes/todo.txt
buy milk
alice@PURR OS> mv /notes/todo.txt /notes/done.txt
alice@PURR OS> rm /notes/done.txt
alice@PURR OS> rm /notes
alice@PURR OS> df
root: 164K used of 11260K (2790 blocks free)
```

Rules and quirks:

- There is no current directory. `ls` with no argument lists `/`. A missing leading `/` is added.
- `mkdir` makes one level. `mkdir /a/b` fails with `no such file or directory` unless `/a` exists.
- `rm` removes a file or an **empty** directory. There is no recursive delete. To remove a tree, delete the
  files, then the directories, deepest first.
- `mv` can move a file between directories. Per LittleFS's `rename` rules, renaming onto an existing **file** replaces
  it atomically, and onto a non-empty directory it fails. (Not exercised by this documentation pass.)
- `write` is for notes, not data: it stores your words joined by single spaces plus a newline, at most about
  118 characters. There is no editor and no way to append.
- `cat` of binary data prints `.` for each unprintable byte.
- `df` counts whole blocks and is approximate. Directories show size 0.
- Free space: a file update briefly needs room for the new copy as well as the old one, so keep some headroom.
  `appinstall` also needs room for `<name>.tmp` next to the old version.

## Getting files onto the device

There is **no USB file transfer, no SD card reading and no serial transfer** yet (MTP, SD and serial transports are
**[DESIGNED]**; see `AppManager/SPEC.md` section 7). What works today:

| Way | What it can place |
|-----|-------------------|
| `write` | a short text line |
| `appinstall <https URL>` | an app (to `/home/<user>/apps`) |
| `netinstall modules` | the module and kernelmod `.cat` files (to `/system` and `/kernelmods`) |
| `netinstall coreos/appmanager/runtime/devbundle` | a staged file under `/boot`, swapped in by KittenOS |
| `plantmodules` | the eight modules embedded in the firmware image |

Anything else needs a network host. For development a quick option is a throwaway local web server on your PC
(`python -m http.server`) plus `appinstall http://<pc-ip>:8000/app.cat`. Plain `http://` is accepted by `appinstall`.
Only apps can be installed this way: `netinstall` reads fixed URLs from its manifest, and the manifest's own
download URLs resolve next to the manifest URL.

## Formatting and wiping

- `format --yes`: unmount, erase, create a fresh empty LittleFS and remount. **Deletes everything**:
  accounts (you will be asked to create one at the next login), modules (`help` will be nearly empty until you
  reinstall them), apps, saved Wi-Fi, staged updates.
- `appformat --yes`: removes only your own apps.
- From the PC, a full wipe of `root` is `esptool erase_region 0x501000 0xAFF000`. KittenOS then formats on boot.
  PURR OS would show `root: not mounted (...)` and `run: format --yes`.

## Inspecting `root` from a PC (advanced, untested here)

You can read the partition and open it with a LittleFS library. Block size is 4096, read and program size 16.

```powershell
python -m esptool --chip esp32s3 --port COM5 read_flash 0x501000 0xAFF000 root.bin
```

then, for example, the `littlefs-python` package (`pip install littlefs-python`) with
`block_size=4096, block_count=0xAFF`. This documentation pass did not run that, so treat the library parameters as a
starting point. Close the serial monitor and do not write the image back while the device is running.

## Reliability notes

- The wrapper is **not thread-safe** (one shared file buffer, no mutex). Today the shell, the Wi-Fi task and the
  loader touch it from different tasks, but only in ways that rarely overlap ([F-33](FINDINGS.md#f-33)).
- A full `root` makes writes fail with `no space left`. User accounts are written to `/etc` whole, so a nearly full
  disk can fail `passwd` or `useradd` (`could not save`).

Next: [10 Updates, network install and recovery](10-updates-network-install-and-recovery.md).
