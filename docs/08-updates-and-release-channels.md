# 08. Updates, releases and channels

## Short version

- Releases are **GitHub release assets** in three tagged releases: `JumpingJaguar1` (system images and the recovery manifest), `JumpingJaguar2` (command modules, drivers, their manifests) and `JumpingJaguar3` (the app repository, still a placeholder).
- A device never trusts the download source for integrity: every image is checked against the manifest (size and SHA-256), then verified by signature, written, and read back.
- In `enforce` a device refuses to move below its **version floor** (in `warn` it only warns). The floor rises on a healthy boot, so a release must be numbered above anything already published.
- Only the **recovery loader** writes the system. KittenOS stage 2, Settings and `netinstall` ask for it or install the parts the loader leaves.
- Beta and stable **channels** are designed ([plan3.md](../plan3.md)) and **not built**. A bad release is stopped with `purrstrap dev withdraw`, built for the dev server and the manifest reader.

## What it is for

This is how new system software gets onto a device, and how a bad release is kept off. It has to work when the system is broken (recovery), when nobody is watching (an unattended reinstall) and when the owner is testing (the developer server). It is built around small files that are each checked, not one large package.

## The files

| File | What it is |
|---|---|
| [OTA/SPEC.md](../OTA/SPEC.md) | The update spec: sources, staging, the two manifests, withdrawal (section 8.1) |
| [OTA/legal.txt](../OTA/legal.txt) | The legal and credits text; shipped next to the manifest and fetched at run time |
| [PurrOS/components/coreos/src/purr_manifest.c](../PurrOS/components/coreos/src/purr_manifest.c), [include/purr_manifest.h](../PurrOS/components/coreos/include/purr_manifest.h) | The manifest parser, `purr_manifest_find()`, withdrawn handling |
| [PurrOS/components/coreos/src/purr_swap.c](../PurrOS/components/coreos/src/purr_swap.c) | The staged-file swap decision (`.new`, `.bak`, attempts) |
| [PurrOS/components/coreos/src/purr_bootimg.c](../PurrOS/components/coreos/src/purr_bootimg.c) | Checks a bootloader image before it is swapped in |
| [PurrOS/main/recovery_loader.c](../PurrOS/main/recovery_loader.c) | The loader: fetch, verify, write, read back, restart |
| [PurrOS/main/commands.c](../PurrOS/main/commands.c) | `netinstall` (`net_install_run`), stage 2, the swap |
| [PurrOS/main/purr_sysupdate.c](../PurrOS/main/purr_sysupdate.c) | Settings > Updates: versions, check, request an update |
| [PurrOS/main/purr_recovery_src.c](../PurrOS/main/purr_recovery_src.c) | Which server: official, developer server, or a relay |
| [PurrOS/main/purr_bootupdate.c](../PurrOS/main/purr_bootupdate.c) | The bootloader writer (off, see below) |
| [PurrOS/main/Kconfig.projbuild](../PurrOS/main/Kconfig.projbuild) | The two official URLs |
| [tools/release/make_release.py](../tools/release/make_release.py) | Builds a signed, self-checking release folder |
| [tools/release/make_drivers_index.py](../tools/release/make_drivers_index.py) | Writes `drivers.manifest` |
| [tools/release/make_jj3.sh](../tools/release/make_jj3.sh) | Builds the app repository release |
| [purrstrap/scripts/dev.py](../purrstrap/scripts/dev.py) | The developer server: `dev recovery`, `serve`, `withdraw` |
| [tools/broken-kitten-recovery/](../tools/broken-kitten-recovery/) | The desktop restore tool and relay (Rust) |
| [RecoveryLoader/SPEC.md](../RecoveryLoader/SPEC.md), [Install/SPEC.md](../Install/SPEC.md) | Loader and install specs |

## The three release folders

| Tag | Holds | Read by |
|---|---|---|
| `JumpingJaguar1` | `recovery.manifest`, `kernel_<board>.cat`, `kittenos_<board>.kitt`, `loader_<board>.cat`, `bootpkg_<board>.cat`, optional `bootloader_<board>.cat`, `legal.txt` | The loader and KittenOS |
| `JumpingJaguar2` | `modules.manifest`, the command modules, `drivers.manifest`, the driver files | KittenOS stage 2, `netinstall`, Settings, Device Manager |
| `JumpingJaguar3` | `repo.json`, `index.json`, their `.sig` files and the app packages | The Store ([chapter 14](14-store-and-repositories.md)). A placeholder until it is published |

The URLs are set in Kconfig (`PURR_RECOVERY_MANIFEST_URL`, `PURR_MODULE_INDEX_URL`) and in [purr_repo.h](../PurrOS/components/coreos/include/purr_repo.h). A file named in a manifest is fetched from the same folder as the manifest. The release's own `release` line is `<version>+<board>`.

## The recovery manifest

A flat text file: stanzas of `key=value` lines separated by blank lines, `#` for comments. It is flat on purpose, because the loader has no filesystem and almost no room. An entry with a missing field or a malformed size or hash is dropped, never trusted with a guess. Unknown keys are ignored, so the format can grow.

| Key | Meaning |
|---|---|
| `component` | `kernel`, `kittenos`, `loader`, `bootpkg`, `bootloader`, or a module or driver name |
| `type` | empty, `module`, `kernelmod` or `driver` |
| `version`, `chip`, `board` | `chip` and `board` may be `any` |
| `file`, `size`, `sha256` | Of the whole signed file |
| `key` | The role that must have signed it; a wrong value only makes a correct download be refused |
| `min_bootloader`, `min_coreos` | Minimum versions |
| `payload_size`, `payload_sha256` | Of the payload inside the container, so a slot can be compared without downloading |
| `compatible`, `buses`, `catcall`, `abi_min`, `abi_max`, `blocked` | Driver index fields |
| `withdrawn` | `yes` on an entry: the publisher pulled this version |

A release-wide list (`withdrawn_versions=kernel@1.0.20,*@1.0.20`, with `withdrawn_reason=`) serves devices that are already on a withdrawn version. The parser's entry table is sized by `PURR_MANIFEST_MAX_ENTRIES` (16 in the header); check that before publishing a longer manifest.

## How an update happens

1. **Choose a source.** Official (`JumpingJaguar1`), a developer server, or a recovery computer ("relay"). The developer server is used only while developer mode and the bootloader's `DEV_RECOVERY` flag are both on. A relay may be used in any secure mode, local addresses only, and is never remembered across boots; the device still verifies every image itself, so the relay is a transport and not a source of trust ([plan3.md](../plan3.md), 2026-10-28).
2. **Read the manifest.** `purr_manifest_find()` skips withdrawn entries, so every reader (loader, `netinstall`, stage 2, Settings, module and driver installs) skips them too.
3. **Compare** with what is installed, and with the floor.
4. **Download** one file at a time. A body shorter than the stated length is a failure. `purr_fetch_alloc_checked()` tries three times (waiting 2 s, then 4 s), checking size and hash each time. Drivers retry at 1.5 s and 3 s.
5. **Verify** the container ([chapter 03](03-trust-and-keys.md)), in memory, before touching flash.
6. **Write and read back.** A slot is erased, written with its trailer ([chapter 02](02-flash-map-and-partitions.md)), read back and compared. A driver file is read back and removed if it does not match.
7. **Restart.** On a healthy boot the OS raises the floor.

The recovery loader restores in two stages: it installs KittenOS and the boot package, then restarts into KittenOS, which asks for the account and installs the kernel and modules in the background. A reinstall marks the kernel and KittenOS slots bad first, so an interrupted reinstall cannot fall back to the old system ([chapter 01](01-boot-chain.md)).

### Version floors

The floor for each component lives in `purrcfg`, unsigned. It rises only when the running slot's trailer verifies and its version is higher. The bootloader enforces it per slot; the OS enforces it on installs. A release that is not above the published one would be refused in `enforce`, and in `warn` would put a device back on an older system. This is why [make_release.py](../tools/release/make_release.py) says "a version ABOVE what is published".

## Making a release

`make_release.py` needs the boot key's passphrase in the environment of that one command and refuses to overwrite an existing folder:

```
PURRSTRAP_KEY_PASSWORD=<given for this command only> python3 tools/release/make_release.py --version <x.y.z> --out <new folder>
```

It builds the kernel, loader and KittenOS packages from `out/stdfull`, `out/stdmin` and `out/stdrec`, signs them, builds and signs the boot package, verifies each against the public boot key, carries over other boards' files and the modules unchanged (checking each against the published manifest), and writes `recovery.manifest`, `SHA256SUMS.txt` and a README. Add `--bootloader` and `--bootloader-version`, and `--driver FILE:COMPATIBLE:BUSES:CATCALL`, as needed. Uploading to GitHub is the owner's step; the plans record that the version to publish is 1.0.27, with 1.0.28 next ([showtime1.md](../showtime1.md)).

**After uploading, fetch every file a manifest names and compare size and SHA-256.** A manifest does not prove the server holds everything. The full order for a release, including the dev server, is in the [runbook](runbook.md).

For testing, `purrstrap dev recovery` copies images into the developer server's `recovery/` folder under the names `<component>-<board>.cat` (note: a dash, where release files use an underscore) and regenerates both manifests **from the files in the folder**, so the system modules must be passed with `--modules`. `dev withdraw --version 1.0.20 --reason "..."` marks a bad version; the list is kept in `recovery/withdrawn.list` and written into both manifests on every later `dev recovery`.

## Moving the OTA address (built in 1.0.28)

The recovery addresses used to be constants in every image. Since 1.0.28 a device can be told, by a small signed file, that its updates come from somewhere else (the hub, [plan8.md](../plan8.md)). Read this before you change where the official releases live.

1. **The file.** `moved.txt` and `moved.txt.sig` sit next to the `recovery.manifest` the device asks. It is `key=value` lines (`moved=1`, `seq`, `key_id`, `from`, `recovery`, `modules`, `message`); the signature is ECDSA P-256 over SHA-256 of the exact bytes, 128 hex digits. Make both with `purr repo move OUTFOLDER --from-url ... --recovery ... --modules ... --seq N --key <boot key> --message "..."` (the passphrase is read from the prompt or the environment of that one command).
2. **The rules** ([purr_move.c](../PurrOS/components/coreos/src/purr_move.c), [purr_move.h](../PurrOS/components/coreos/include/purr_move.h)): a boot-role key in the device's bag that is not revoked has signed it; `from` is exactly the address the device asked; `seq` is above the last move it followed; both new addresses are https and end in `/recovery.manifest` and `/modules.manifest`. A file that names the compiled default clears the stored source and keeps `seq`, so an old file cannot be replayed.
3. **Where it is kept.** A third record in the `netrec` partition ([purr_netrec.c](../PurrOS/components/kernel/src/purr_netrec.c)), next to the network record and the dev-server record, so the recovery loader (no file system) reads it too. `purr_recovery_manifest_url()` and the other address functions in [purr_recovery_src.c](../PurrOS/main/purr_recovery_src.c) use: a chosen dev server, else the stored source, else the compiled default.
4. **When it runs.** Before the kernel's update check ([purr_sysupdate.c](../PurrOS/main/purr_sysupdate.c)) and before the loader fetches its manifest ([recovery_loader.c](../PurrOS/main/recovery_loader.c)), unless a dev server is in use. Settings > About > System update shows where updates come from and, after a move, offers "Use the default address".
5. **What it cannot do.** It cannot install code: images are verified against the boot key from any host, and the version floors still apply. A hostile host can only withhold updates.

Checked: a host test with files signed by the Python tool; in the emulator the loader followed a signed move and used the stored source after a restart. Not checked: the kernel's update check path and the Settings page in the emulator; nothing on a device.

## Channels (designed, not built)

The owner decided on 2026-11-01 ([plan3.md](../plan3.md)):

| Rule | Detail |
|---|---|
| Channels | `stable` (today's behaviour) and `beta`. A channel is a row: name, release URL base, whether rollback is allowed. A vendor ships its own rows |
| Beta | Keeps every signature rule; skips only the floor check; does not raise the floor |
| Downgrade limit | Refused when the target is 5 or more patch releases below the highest version the device has run. A release may declare a wider window in a signed field |
| Beta to stable | Staged by the loader; on any failure the device stays on beta |
| Stable to beta | Only through the recovery app, with a warning; refused on a device whose signed block has `DEV_LOCKED` |

There is no channel code in `main/` or `coreos/` (a search for "beta" finds nothing). Vendor overlays (one core OS plus a signed overlay per vendor) are an architecture goal that needs a new image type and wider signing rules.

## How to test

Host tests: `test_manifest` (162 checks, including withdrawal), `test_swap`, `test_bootimg`, `test_bootupdate`, `test_decision`, `test_reason`; purrstrap: `test_devrecovery` (manifests, safe names, the server serving only its folder, announcements). The VM test loop with the dev server is in [tools/vm/README.md](../tools/vm/README.md). Do not run the emulator unless asked.

## What goes wrong and how to fix it

| Symptom | Cause | Fix |
|---|---|---|
| Recovery goes back to an old system | The published release is older than the device | Publish above every version already run |
| Stage 2 has no modules from a dev server | `dev recovery` was run without `--modules` | Pass the system modules every time |
| "download does not match the manifest" | A short or altered body | The device tried three times; check the server file's size and hash |
| Device pulls from the official server instead of the dev server | Developer mode is off after install, **by design** (owner, 2026-11-05) | Install, restart from inside the OS, then run the Settings update |
| KittenOS aborts at start | A staged bootloader swap hit the erase of flash 0x0 | Bootloader OTA is switched off (below) |
| Drivers missing from `drivers.manifest` | The script lists only its arguments | Pass every driver each run |

## What is not finished

- **Bootloader OTA is off** (`PURR_BOOTLOADER_OTA` is 0 in [commands.h](../PurrOS/main/commands.h), 2026-11-05). Bringing it back needs `CONFIG_SPI_FLASH_DANGEROUS_WRITE_ALLOWED` in the recovery profile only, a test bootloader and a USB flasher at hand ([plan3.md](../plan3.md)). The spec in [bootloader/SPEC.md](../bootloader/SPEC.md) section 9.2 still describes it as working.
- **Withdrawal, remaining steps:** words on the device and a lower-version offer; a manifest re-read before the last write; a signed manifest with a release stamp; the companion's hourly check; the Store index flag; a downgrade guard ([OTA/SPEC.md](../OTA/SPEC.md) section 8.1). The official release does not carry withdrawal yet (make_release is not wired).
- **The system manifest in JSON** (changelog, channels) is open; only the flat text manifests exist.
- **Not verified on a device:** the whole checked-download path, the relay path, the withdrawal handling, and a published 1.0.27. JumpingJaguar3 does not exist yet.
- **The official manifest has no `uiassets` entry**, so the picture pack comes from a developer server only ([plan3.md](../plan3.md)).

Plan references: [plan3.md](../plan3.md) (channels, withdrawal, downloads, bootloader OTA), [plan1.md](../plan1.md) (the loader), [plan8.md](../plan8.md) (repositories).
