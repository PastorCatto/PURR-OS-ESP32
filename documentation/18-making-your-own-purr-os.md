# 18. Making your own PURR OS

A guide for someone who wants **their own build**: their own name on the screen, their own keys, their own release
host, possibly their own board. It is a fork-and-rebrand guide, not a promise of a stable SDK: the project is in
flux, and the places you edit are plain source files, not configuration.

> **Licence.** The repository is GNU GPL v3 (`LICENSE`). A fork you distribute has to comply with it, including
> offering its source. This is a pointer, not legal advice.

**Status:** every step here uses things that exist. The key tool, the package commands and the manifest generator
below were run during this documentation pass; the on-device steps (flashing, `netinstall` against your own host) are
as described by the specs and the code and were **not** run on hardware by this pass.

## What "your own" has to change, and why

A PURR OS device trusts three kinds of thing. Make them yours, and the others follow.

| Thing | Why it matters | Where |
|-------|----------------|-------|
| **Signing keys** | Whoever holds the private key decides what runs. If you ship the author's public keys, the author's key (and anyone who has it) can sign for your devices. | [17](17-keys-and-signing.md) |
| **Where updates come from** | The device fetches its manifest from a URL compiled in. If you leave the author's, your devices install the author's builds. | two Kconfig/sdkconfig places |
| **Identity** | Names on screen and in logs, so nobody confuses the two systems. Optional but expected. | a handful of strings |

## 1. Get a clean tree and keys

```powershell
git clone <the repo> MyOS ; cd MyOS
git checkout -b my-os
```

Keep the path **short** (for example `C:\MyOS`). ESP-IDF fails with `fatal error: opening dependency file ...: No such
file or directory` once the build path passes Windows' 260-character limit. This was hit while writing this
documentation.

Generate **your own** keys and make them the compiled-in defaults ([17](17-keys-and-signing.md) has every detail):

```powershell
$env:PURRSTRAP_KEY_PASSWORD = "a long passphrase"
python purrstrap/purrstrap.py keys generate --role boot      --out signing_keys --key-id 1
python purrstrap/purrstrap.py keys generate --role developer --out signing_keys --key-id 2
python purrstrap/purrstrap.py keys export-public --pub signing_keys/boot.pub      --role boot      --key-id 1 --out PurrOS/components/coreos/keys
python purrstrap/purrstrap.py keys export-public --pub signing_keys/developer.pub --role developer --key-id 2 --out PurrOS/components/coreos/keys
```

This **replaces** `boot_1.pub.bin`, `developer_2.pub.bin` and `purr_default_keys.c`. Back up `signing_keys/` somewhere
offline now. If you lose the password you rebuild and reflash everything. If you want a `system` key too
(for AppManager/runtime/devices-bundle modules, which nothing ships yet), generate and export it with `--key-id 3`.

Do **not** use the same keys as the author. Do not commit `signing_keys/`. (The upstream project signs owner-level sign-offs and publishes under the name "PastorCatto Collective"; use your own
publisher name wherever the specs call for one, for example the future vendor-certificate issuer field in `Keys/SPEC.md`.)

## 2. Point updates at your own host

Two URLs are compiled in. Change **both places for both URLs**, or old builds keep the old value
([04](04-building-and-flashing.md)):

| File | Setting |
|------|---------|
| `PurrOS/main/Kconfig.projbuild` | `default` of `PURR_RECOVERY_MANIFEST_URL` and `PURR_MODULE_INDEX_URL` |
| `PurrOS/sdkconfig.defaults` | `CONFIG_PURR_RECOVERY_MANIFEST_URL` and `CONFIG_PURR_MODULE_INDEX_URL` |

Example for GitHub releases (any HTTPS file host works; plain `http://` also works for testing):

```
https://github.com/<you>/<repo>/releases/download/v1.0.0/recovery.manifest
https://github.com/<you>/<repo>/releases/download/v1.0.0/modules.manifest
```

Files named in a manifest must sit **next to** the manifest (same directory); `file=` is a bare name, never a URL.
Release assets on GitHub satisfy that. The fetcher follows GitHub's redirect to its object store.

## 3. Change the identity

All of these are plain edits. Search for the string, do not trust line numbers.

| What | File | Change |
|------|------|--------|
| System names on screen, prompt and `version` (`PURR OS`, `KittenOS`, `PURR Loader`) | `PurrOS/main/commands.c`: `SYSTEM_NAME` in the three `#if` blocks | your names |
| Version printed by `version` | `PurrOS/main/commands.c`: `#define VERSION "0.1.0"` | your version |
| Loader screen text (`PURR internet recovery`, `not a KittenOS image`) | `PurrOS/main/recovery_loader.c` | your wording |
| Boot menu title (`PURR OS`) | `bootpkg/src/pkg_main.c`: `draw_header()` | your title |
| Boot menu entry names (`Kernel`, `KittenOS`) | `bootloader/bootloader_components/main/purr_bootpkg.c` | your names |
| Bootloader log line (`PURR OS bootloader`) | `bootloader/bootloader_components/main/purr_boot.c` | **update the tell in [04](04-building-and-flashing.md)**: if you change it, your own "is the real bootloader running" check changes too |
| Kconfig menu names | `PurrOS/main/Kconfig.projbuild`, `PurrOS/components/kernel/Kconfig` | cosmetic |
| Standalone kernel text | `Kernel/main/kernel_main.c` | cosmetic |
| Release image names `kittenos`, `purros` | `purrstrap/scripts/coreos.py`: `PACKAGE_KIND` | names inside the signed header |
| Dev version base | `purrstrap/scripts/coreos.py`: `DEV_VERSION_BASE` | |

**Leave alone unless you know why:** the file names `purros.bin` and the ESP-IDF project name `project(purros)` in
`PurrOS/CMakeLists.txt` (purrstrap looks for `purros.bin`; rename both together if you do), the partition labels
(`kernel`, `kittenos`, `loader`, `bootpkg`, `purrcfg`, `netrec`, `root`: the bootloader, installers and loader find
partitions by these names), the board name `tdeck_plus` (it is matched against manifests).

### Making your family incompatible with the original

Different keys already make signed files mutually unacceptable. If you also want containers that **parse** differently,
change the magic: `PURR_IMAGE_MAGIC` in `PurrOS/components/coreos/include/purr_abi.h` (`0x50555252`, the ASCII `PURR`
read little-endian) **and** `MAGIC` in `purrstrap/lib/image.py`, then fix the tests and any byte-array literals that embed
the old magic (the embedded modules in `commands.c` start with `0x52, 0x52, 0x55, 0x50`). It is a deep edit for little gain; keys
are the real boundary.

## 4. Build everything

```powershell
python purrstrap/purrstrap.py coreos build --board tdeck_plus --profile full     --clean
python purrstrap/purrstrap.py coreos build --board tdeck_plus --profile recovery --clean
python purrstrap/purrstrap.py coreos build --board tdeck_plus --profile minimal  --clean
python purrstrap/purrstrap.py bootpkg build --board tdeck_plus
# then the bootloader, by hand (rebuild it: it embeds your public keys):
cd bootloader ; idf.py -B build_tdeck_plus -D SDKCONFIG=build_tdeck_plus/sdkconfig -D IDF_TARGET=esp32s3 `
  -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.board.tdeck_plus" build ; cd ..
```

Sign the boot package, flash a first device by wire ([04](04-building-and-flashing.md) section 4), and confirm it boots.
Do this before anything else: if the keys are wrong, you find out now.

## 5. Make a release

A release is a folder of signed files plus two manifests, uploaded to the URLs from step 2.

### 5.1 The system components

```powershell
# KittenOS (the recovery profile), as a signed PURR_IMG_RECOVERY container
python purrstrap/purrstrap.py coreos package --board tdeck_plus --profile recovery --version 1.0.0 --out rel/kittenos-tdeck_plus-1.0.0.kitt
python purrstrap/purrstrap.py keys sign --key signing_keys/boot.key --image rel/kittenos-tdeck_plus-1.0.0.kitt --key-id 1

# the working system, shipped as the "kernel" component (a normal ESP app in the kernel slot)
python purrstrap/purrstrap.py coreos package-component --component kernel --file PurrOS/build/tdeck_plus-full/purros.bin --version 1.0.0 --board tdeck_plus --out rel/kernel-tdeck_plus-1.0.0.cat
python purrstrap/purrstrap.py keys sign --key signing_keys/boot.key --image rel/kernel-tdeck_plus-1.0.0.cat --key-id 1

# the recovery loader (optional; the loader is normally flashed by wire and rarely changes)
python purrstrap/purrstrap.py coreos package-component --component loader --file PurrOS/build/tdeck_plus-minimal/purros.bin --version 1.0.0 --board tdeck_plus --out rel/loader-tdeck_plus-1.0.0.cat
python purrstrap/purrstrap.py keys sign --key signing_keys/boot.key --image rel/loader-tdeck_plus-1.0.0.cat --key-id 1
```

Notes on those three:

- `coreos package` supports only `recovery` and `full`. For the **full** profile it makes an `IMG_OS` named `purros`,
  which `netinstall` cannot install (it only knows partition components). Ship the full build as the **`kernel`
  component** with `package-component`, as above. The size must fit the kernel partition (`0x1E0000`, 1.875 MB). The full build is
  about 1.08 MB. This is the arrangement that ends a network install at a working PURR OS shell ([F-31](FINDINGS.md#f-31)).
- The installer writes the **payload** (the plain ESP image) into the partition and strips the container. That is correct for
  `kernel`, `kittenos` and `loader`.
- **Do not list `bootpkg` in the manifest.** The bootloader needs the whole container in that partition, and the installer
  strips it ([F-13](FINDINGS.md#f-13)). Reflash the boot package by wire.
- A manifest `size` and `sha256` describe the **whole file you upload**, container included.

### 5.2 Modules

```powershell
python purrstrap/purrstrap.py modules build --source Modules/test/about_module.c --entry about_module_entry --kind driver --name about --version 1.0.0 --out rel/mods/about.cat
python purrstrap/purrstrap.py keys sign --key signing_keys/developer.key --image rel/mods/about.cat --key-id 2
# repeat for each module and kernelmod (see Modules/ and Modules/coreos/), kernelmods into rel/kmods/
python purrstrap/purrstrap.py modules index --board tdeck_plus --modules-dir rel/mods --kernelmods-dir rel/kmods --coreos-version 1.0.0 --key developer --out rel/modules.manifest
```

([12](12-writing-modules.md) has the entry-point names and the rules.) Upload `rel/modules.manifest` and every `.cat`
next to it. Remember `netinstall modules` installs `/system/<header name>.cat`.

### 5.3 The recovery manifest

There is no purrstrap action for this (the module index is generated, the recovery manifest is not,
[F-20](FINDINGS.md#f-20)). This script computes sizes and hashes. It was checked against the real C parser
(`purr_manifest_parse` reads back both entries, and `purr_manifest_find` finds them for `esp32s3` and `tdeck_plus`):

```python
# mkmanifest.py <release> <chip> <board> component=file:keyrole ...
import datetime, hashlib, os, sys

release, chip, board, *items = sys.argv[1:]
print(f"release={release}")
print(f"released={datetime.date.today().isoformat()}")
for item in items:
    comp, rest = item.split("=", 1)
    fname, role = rest.rsplit(":", 1)
    data = open(fname, "rb").read()
    print()
    print(f"component={comp}")
    print(f"version={release}")
    print(f"chip={chip}")
    print(f"board={board}")
    print(f"file={os.path.basename(fname)}")
    print(f"size={len(data)}")
    print(f"sha256={hashlib.sha256(data).hexdigest()}")
    print(f"key={role}")
```

```powershell
cd rel
python ..\mkmanifest.py 1.0.0 esp32s3 tdeck_plus kernel=kernel-tdeck_plus-1.0.0.cat:boot `
  kittenos=kittenos-tdeck_plus-1.0.0.kitt:boot loader=loader-tdeck_plus-1.0.0.cat:boot > recovery.manifest
```

At most **16** entries are read, and a malformed stanza is dropped silently, so after writing, count them.

### 5.4 Publish

Upload to your URL: `recovery.manifest`, `modules.manifest`, and every file they name. On GitHub: create a release with the
tag in your URL and attach the files. Check each URL in a browser: it must download the raw file.

## 6. Test the whole chain

1. Flash a device by wire ([04](04-building-and-flashing.md) section 4).
2. In KittenOS or PURR OS: `wifi connect ...`, then `netinstall kernel` and `netinstall modules`, then `reboot`.
3. Break it on purpose and watch the recovery paths ([19](19-troubleshooting-and-scenarios.md)).
4. Tamper with a file after signing; confirm the device says `verification failed: bad-hash` or `bad-signature`.

## 7. A different board

If your board is not a T-Deck Plus, read [16](16-adding-a-board.md) first. In outline: a board profile, a display driver if the
panel differs, `sdkconfig.board.<board>`, a partition CSV, entries in purrstrap's `BOARDS` tables, and a boot package
build for the new pins. Only modular boards (PSRAM, at least 4 MB) fit the current design.

## 8. Keeping your fork current

The project is moving quickly and most of its constants are scattered. Before you merge upstream changes, check
`FINDINGS.md`, and expect to redo: ABI version bumps (rebuild and re-sign all modules), key-bag format, partition
CSV edits (keep the three copies identical), and purrstrap action changes ([22](22-purrstrap-reference.md)).

## Checklist

- [ ] Own keys generated, backed up offline, `signing_keys/` not tracked
- [ ] `purr_default_keys.c` regenerated and `.pub.bin` files replaced
- [ ] Both URLs changed in **both** files, builds done with `--clean`
- [ ] Identity strings changed
- [ ] Bootloader rebuilt and flashed after the key change
- [ ] Boot package built, **signed with your boot key**, flashed
- [ ] Components signed with the right key and the **right `--key-id`**
- [ ] Manifests generated from the files you actually uploaded
- [ ] A first device flashed, booted, and a network install tested end to end

Next: [19 Troubleshooting and scenarios](19-troubleshooting-and-scenarios.md).
