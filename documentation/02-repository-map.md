# 02. Repository map

Everything below is current work. **Ignore these, they are history:** `archive/`,
`PURR-OS-0.11/`, `CatReleases/`, `releases/`, `dist/`, `media/`, `CoreOSSpike/` and `ModuleSpike/`
(the last two are finished experiments, kept as records).

## Code

| Path | What it is | Build system |
|------|------------|--------------|
| `bootloader/` | The custom second-stage bootloader. Its own ESP-IDF project. `bootloader_components/main/` holds `purr_boot.c` (the boot decision), `purr_bootpkg.c` (loads and runs the boot package), `purr_bootcfg.c` (purrcfg access), `purr_crypto_uecc.c` (P-256 verify). `main/stub_app.c` is a placeholder app so the project links. | ESP-IDF, built by hand |
| `bootpkg/` | The boot menu. Freestanding code linked for a fixed SRAM window. `src/pkg_main.c` (menu drawing), `src/pkg_hw.c` (bit-banged display and keyboard), `boards/tdeck_plus.h` (pins), `link/esp32s3.ld`. | `purrstrap bootpkg build` |
| `Kernel/` | The standalone kernel binary and its CoreOS-loading proof. `main/kernel_main.c`, `main/kernel_table.c`, `main/loader.c`, `coreos_min/coreos_min_entry.c`. Reuses `PurrOS/components/kernel` directly. | ESP-IDF, built by hand |
| `PurrOS/` | The base system: kernel code, CoreOS, the shell, login. One project, three profiles. | `purrstrap coreos build` |
| `PurrOS/components/kernel/` | Hardware and filesystem: board profile (`boards/`), ST7789 driver, console, graphics, LittleFS wrapper, Wi-Fi service, HTTPS fetch, config store, user store, crypto backend. | part of `PurrOS/` |
| `PurrOS/components/coreos/` | Plain-C CoreOS logic (no ESP-IDF includes except where noted): image verify, key bag, `purrcfg`, shell engine, terminal grid, boot menu logic, manifest parser, accounts, PBKDF2, app manager core, relocation loader pieces, swap decisions. Has its own host tests in `test/host/`. | part of `PurrOS/` |
| `PurrOS/components/littlefs/` | Vendored LittleFS. | part of `PurrOS/` |
| `PurrOS/main/` | `main.c` (boot sequence, login, shell loop), `commands.c` (the kernel table implementation, module loader, net install, swap, accounts back end), `login.c`, `recovery_loader.c` (the `minimal` profile's whole program). | part of `PurrOS/` |
| `Modules/` | Source of the loadable modules: `network/wifi_module.c`, `netinstall/netinstall_module.c`, `appmanager/apps_module.c`, `test/about_module.c`, and the kernelmods in `coreos/` (`sysinfo`, `fs`, `login`, `appmgr`). `Modules/build/` is scratch output (git-ignored). | `purrstrap modules build` |
| `purrstrap/` | The build and packaging tool. Python, standard library only except the `keys` script. `scripts/` has `coreos.py`, `bootpkg.py`, `keys.py`, `modules.py`. `freestanding/purr_freestanding_libc.c` is a tiny libc subset for freestanding builds. | run `python purrstrap/purrstrap.py` |
| `signing_keys/` | **Git-ignored.** Your private and public keys (`boot.key`/`.pub`, `developer.key`/`.pub`). `old/` holds the previous pair. | `purrstrap keys` |

## Specs (design record, one folder per chunk)

`PurrOS/SPEC.md` is the overview. Others: `bootloader/`, `PurrOS/components/kernel/`,
`PurrOS/components/coreos/`, `KittenOS/`, `RecoveryLoader/`, `OTA/`, `Install/`, `Keys/`, `Users/`,
`Network/`, `Modules/`, `purrstrap/`, `Boards/`, `Drivers/`, `AppManager/`, `AppRuntime/`, `AppSDK/`,
`CatFormat/`, `Catcalls/`, `Permissions/`, `UI/`, `MicroPython/`, `Transfer/`.
Many describe things not built. Always check the status tags in this documentation first.

## Flash layout (T-Deck Plus, 16 MB)

Defined identically in `PurrOS/partitions/tdeck_plus.csv`, `bootloader/partitions.csv` and
`Kernel/partitions.csv`. Keep all three in step. Explained fully in
[05](05-boot-process-and-flash-layout.md).

## Where things are written on a running device

| Path or partition | Contents |
|-------------------|----------|
| `/etc/passwd`, `/etc/shadow` | accounts and password hashes ([07](07-user-accounts.md)) |
| `/etc/wifi` | saved Wi-Fi networks, plain text |
| `/home/<user>/apps/<app>/package.cat` | that user's installed apps |
| `/system/*.cat` | CoreOS-level modules; `/system/.rejected/` is the quarantine |
| `/kernelmods/*.cat` | kernel-level modules |
| `/boot/<name>.new`, `.bak`, `.bad` | staged, backup and failed copies of file-staged system components |
| `purrcfg` partition | boot configuration |
| `netrec` partition | last Wi-Fi network for the recovery loader |

Next: [03 Development environment](03-development-environment.md).
