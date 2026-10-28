# 23. Cookbook

Short recipes for common jobs. Each links to the chapter with the full story. Commands are PowerShell from the repository root unless marked **(device)**, which means typed at the
device's shell. Anything marked **[untested]** follows from the code but was not run.

- [Bring up a new device](#bring-up-a-new-device)
- [Everyday use](#everyday-use)
- [Accounts](#accounts)
- [Updates and recovery](#updates-and-recovery)
- [Extending the system](#extending-the-system)
- [Keys and releases](#keys-and-releases)
- [Maintenance and inspection](#maintenance-and-inspection)
- [Small customisations](#small-customisations)

## Bring up a new device

### Flash everything onto a blank T-Deck Plus

1. Build: three profiles, the bootloader, the boot package ([04](04-building-and-flashing.md) sections 1 to 3).
2. Sign the boot package: `keys sign --key signing_keys/boot.key --image bootpkg/build/tdeck_plus/bootpkg.bin --key-id 1`.
3. Flash the five images with `esptool` ([04](04-building-and-flashing.md) section 4).
4. Power-cycle, watch for `purr_boot: PURR OS bootloader` on the serial log.
5. KittenOS boots, formats `root`, drops to a `setup@KittenOS>` prompt.
6. **(device)** `wifi connect <ssid> <password>`, `netinstall modules`, `reboot`.

### Put the working system on it

Flash the full build into the factory slot (dev layout), or into `kernel` with KittenOS in `kittenos` ([04](04-building-and-flashing.md) section 5):

```powershell
python -m esptool --chip esp32s3 --port COM5 write_flash 0x1a0000 PurrOS\build\tdeck_plus-full\purros.bin
```

First boot: `root: not mounted ... run: format --yes` on PURR OS. **(device)** `format --yes`, `reboot` (this erases `root`; skip it if KittenOS already formatted it).
Then first-time setup creates your admin account ([07](07-user-accounts.md)).

### Wipe a device completely

```powershell
python -m esptool --chip esp32s3 --port COM5 erase_flash
```

Everything goes, including `purrcfg`, `netrec` and the bootloader. Start again from the top.

## Everyday use

### Connect to Wi-Fi and see my address **(device)**

```
wifi scan
wifi connect "My Network" "my password"
net
```

### Look around **(device)**

```
info          board, chip, display
mem           memory
parts         partition table
purrcfg       boot config and failure counter
ls /          the root filesystem
ls /system    which modules are installed
df            disk space
```

### Write a note **(device)**

`write /notes.txt remember to flash the kernel` then `cat /notes.txt`. One line, about 118 characters. There is no editor.

### Install an app **(device)**

`appinstall https://host/app.cat`, `apps`, `appinfo <name>`, `appremove <name>` ([11](11-apps.md)). Apps cannot run yet.

## Accounts

### Create the first account

Reboot a device whose `/etc/passwd` is empty: it asks for name and password. Do not name it `root` ([07](07-user-accounts.md)).

### Add an admin and a standard user **(device)**

```
useradd carol admin
useradd bob
```

### Change roles, passwords, remove a user **(device)**

```
usermod bob admin
passwd bob
userdel bob
```

### Reset a forgotten admin password

No reset path. Erase `root` from the PC, which wipes everything on it ([07](07-user-accounts.md)):

```powershell
python -m esptool --chip esp32s3 --port COM5 erase_region 0x501000 0xAFF000
```

## Updates and recovery

### Restore a broken PURR OS over the network **(device, in KittenOS)**

```
wifi connect <ssid> <password>
netinstall kernel
reboot
```

### Reinstall the command modules **(device)**

`netinstall modules` then `reboot`. Without a network: `plantmodules`, `reboot`.

### Boot KittenOS or the recovery loader on purpose **(device)**

`reboot recovery` or `reboot loader`. Or hold a key at the boot menu and pick `KittenOS`.

### Recover a device whose KittenOS is dead

It happens by itself after 7 failed boots, if the loader is flashed and a Wi-Fi network is saved in `netrec`. Otherwise reach the loader with the menu entry `Internet recovery` (shown when nothing is
bootable) or reflash by wire ([10](10-updates-network-install-and-recovery.md), [19](19-troubleshooting-and-scenarios.md)).

### Roll back a bad update

File-staged components (`coreos` and friends): automatic after three failed confirmations. Partition components (`kernel`): reinstall the previous release's kernel, or flash it by wire ([10](10-updates-network-install-and-recovery.md)).

### Move to enforce mode

Write a purrcfg image with `examples/tools/mkcfg.py enforce purrcfg.bin` and flash it at `0x1e000` ([05](05-boot-process-and-flash-layout.md)). Understand first that it changes only version-floor checks for staged updates today
([17](17-keys-and-signing.md)).

## Extending the system

### Add a shell command

Copy `examples/modules/greet_module.c`, change the command, then:

```powershell
python purrstrap/purrstrap.py modules build --source greet_module.c --entry greet_module_entry --kind driver --name greet --version 0.1.0 --out greet.cat
python purrstrap/purrstrap.py keys sign --key signing_keys/developer.key --image greet.cat --key-id 2
```

Then get it onto the device ([12](12-writing-modules.md) section 3).

### Add a command that needs the kernel (files, heap, uptime, accounts)

Start from `examples/modules/count_kmod.c`, a kernelmod ([12](12-writing-modules.md)).

### Add a new hardware capability

The five-step recipe with a compiled `i2cscan` example: [13](13-writing-drivers.md).

### Change or add a display

A different panel on the same controller: edit the profile ([14](14-adding-a-display.md)). A new controller: copy `st7789.c` ([examples/kernel/ili9341.c](examples/kernel/ili9341.c)). A second display, or a
mirror: [15](15-adding-a-second-display.md).

### Port to a new board

[16](16-adding-a-board.md), with `examples/board/` as the worked files.

### Build a standalone kernel to test **(careful)**

`Kernel/` ([04](04-building-and-flashing.md) section 6). It has no shell and takes over boot while it is in `kernel`.

### Run the tests

```powershell
python PurrOS/components/coreos/test/host/run.py
python -m unittest discover -s purrstrap/tests
```

## Keys and releases

### Make my own keys and use them

[17](17-keys-and-signing.md) and [18](18-making-your-own-purr-os.md) steps 1 and 2.

### Publish a release

[18](18-making-your-own-purr-os.md) step 5: package, sign, write manifests with `examples/tools/mkmanifest.py`, upload to the compiled-in URL.

### Sign one file and check it

```powershell
python purrstrap/purrstrap.py keys sign   --key signing_keys/developer.key --image x.cat --key-id 2
python purrstrap/purrstrap.py keys verify --key signing_keys/developer.pub --image x.cat
python purrstrap/purrstrap.py keys inspect --path x.cat
```

### Rotate keys

[17](17-keys-and-signing.md) "Rotating keys". It reflashes the bootloader.

## Maintenance and inspection

### Read a partition from the device to the PC

```powershell
python -m esptool --chip esp32s3 --port COM5 read_flash 0x1e000 0x2000 purrcfg_backup.bin      # purrcfg
python -m esptool --chip esp32s3 --port COM5 read_flash 0x12000 0xC000 bootpkg_backup.bin      # boot package
python -m esptool --chip esp32s3 --port COM5 read_flash 0x501000 0xAFF000 root_backup.bin      # the filesystem (see 09)
```

`keys inspect --path bootpkg_backup.bin` shows the boot package's header and key id. Stop any serial monitor first.

### Restore a partition from a backup

`write_flash <offset> <file>`. For `root` the device must be off the filesystem (reboot into the loader or just power-cycle after).

### Check what is installed **(device)**

```
version
ls /system
ls /kernelmods
apps
```

### Check a downloaded file before putting it anywhere

`keys verify --key <pub> --image <file>` on the PC, and compare `size` and the SHA-256 with the manifest.

## Small customisations

All are source edits followed by a rebuild.

| I want to... | Edit |
|--------------|------|
| change the shell prompt | `PurrOS/main/main.c`, `run_shell()`: the `snprintf(prompt, ..., "%s@%s> ", ...)` |
| change the colours | `PurrOS/components/kernel/src/purr_console.c`: `FG`, `BG`, `CURSOR` (`PURR_RGB565(r, g, b)`) |
| change the boot countdown | `PURR_MENU_TIMEOUT_MS` in `PurrOS/components/coreos/include/purr_menu.h`, rebuild the boot package |
| turn off the verbose module-loading output | `CONFIG_PURR_VERBOSE_BOOT=n` in `sdkconfig.defaults` |
| change the boot-failure thresholds | `fail_count > 3` and `> 6` in `bootloader/.../purr_boot.c`, rebuild the bootloader |
| change the login delay curve or hash cost | `purr_login_delay_seconds` and `PURR_PBKDF2_ITERATIONS` in `purr_users.[ch]` (existing hashes keep their stored iteration count) |
| change the system name or version | [18](18-making-your-own-purr-os.md) section 3 |
| allow more modules | `PURR_MODULE_MAX`, `PURR_KERNELMOD_MAX` in `commands.c` |
| change the font | `PurrOS/tools/gen_font.py` generates `purr_font_data.c` |

Next: [24 Not built yet](24-not-built-yet.md).
