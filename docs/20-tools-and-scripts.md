# 20. Tools and scripts

## Short version

- Helper programs live in `tools/`: **`release/`** (build the official release folders), **`vm/`** (the emulator test loop), **`hw/`** (a flash image for a real T-Deck Plus), **`boot/`, `oobe/`, `lvgl_host/`, `ui/`** (previews and art), **`docs/`** (the documentation checks), and **`broken-kitten-recovery/`** (a recovery relay in Rust).
- The build and signing tool itself is **`purrstrap`**, a separate chapter. The developer kit's command is **`purr`** ([chapter 17](17-sdk.md)).
- Several scripts need the signing passphrase. It is given in the environment of **that one command**, never written to a file, script or commit.
- Documentation checks: `python3 tools/docs/check_links.py` and `python3 tools/docs/count_words.py docs`.
- Do not run firmware builds, the emulator or anything that signs unless the owner asked.

## What it is for

These scripts exist so that working knowledge lives in the repository and not in a session. The VM scripts say so directly: they "live in the repo (not in a session scratchpad) so a wiped `/tmp` cannot lose them" ([tools/vm/README.md](../tools/vm/README.md)).

## The files

| File | What it does | Needs the passphrase? |
|---|---|---|
| [tools/release/make_release.py](../tools/release/make_release.py) | Packs a new OTA release folder from freshly built, signed images | yes |
| [tools/release/make_official_from_dev.py](../tools/release/make_official_from_dev.py) | Builds the two official folders from a tested dev-server folder; signs nothing | no |
| [tools/release/make_drivers_index.py](../tools/release/make_drivers_index.py) | Writes `drivers.manifest` from signed driver files | no |
| [tools/release/make_jj3.sh](../tools/release/make_jj3.sh) | Builds the JumpingJaguar3 app-repository folder | the repository key's password, once |
| [tools/release/gen_builtin_apps.py](../tools/release/gen_builtin_apps.py) | Embeds the Store and companion in `purr_builtin_apps.c` | no |
| [tools/release/test_make_official_from_dev.py](../tools/release/test_make_official_from_dev.py) | Test for the official-folder tool | no |
| [tools/vm/mkimage_dbg.sh](../tools/vm/mkimage_dbg.sh) | Patches a base flash image into the debug VM image | yes |
| [tools/vm/publish_apps.sh](../tools/vm/publish_apps.sh) | Builds, signs (key id 2) and publishes apps | yes |
| [tools/hw/make_hw_image.sh](../tools/hw/make_hw_image.sh) | A full 16 MB flash image for a T-Deck Plus | yes |
| [tools/hw/flash_and_hold.sh](../tools/hw/flash_and_hold.sh) | Erases and writes the blank image, then holds the board in download mode | no |
| [tools/boot/preview.py](../tools/boot/preview.py) | Mock-ups of the boot logo and boot menu | no |
| [tools/boot/preview_lumia.py](../tools/boot/preview_lumia.py) | A Lumia-style boot theme mock-up | no |
| [tools/boot/render_real.py](../tools/boot/render_real.py) | Renders the real boot package screens on the host | no |
| [tools/oobe/preview.py](../tools/oobe/preview.py) | Mock-up of the first-run setup | no |
| [tools/lvgl_host/render.py](../tools/lvgl_host/render.py) | Builds the real LVGL and shell pages for the host and renders them | no |
| [tools/ui/froyo_pack.py](../tools/ui/froyo_pack.py) | Resamples the Android icons into `UI/android-assets/` | no |
| [tools/docs/check_links.py](../tools/docs/check_links.py) | Fails on a broken relative link or missing anchor | no |
| [tools/docs/count_words.py](../tools/docs/count_words.py) | Counts prose words per file | no |
| [tools/docs/mdscan.py](../tools/docs/mdscan.py) | Shared Markdown reading for the two above | no |
| [tools/docs/test_docs_tools.py](../tools/docs/test_docs_tools.py) | Tests for them | no |
| [tools/broken-kitten-recovery/](../tools/broken-kitten-recovery/README.md) | A computer-side relay that serves official recovery images | no |
| [PurrOS/tools/gen_font.py](../PurrOS/tools/gen_font.py) | Turns LVGL's public-domain Unscii 8 font into a fixed-cell C font | no |
| [sdk/tools/extract_settings.py](../sdk/tools/extract_settings.py) | Reads the settings tables from the shell source into `sdk/data/settings.json` | no |
| [sdk/build-kit.py](../sdk/build-kit.py) | Builds the developer kit folder | no |

## How it works

### Release scripts

| Script | In | Out |
|---|---|---|
| `make_release.py --version V --out DIR` | Freshly built, signed images | `JumpingJaguar1/` (`recovery.manifest`, kernel, KittenOS, loader, boot package, `legal.txt`, other boards' files carried over), `JumpingJaguar2/` (`modules.manifest`, modules carried over), `SHA256SUMS.txt`, `README.txt` |
| `make_official_from_dev.py --release V --out DIR` | A dev-server recovery folder tested on a device | The same two folders, plus `drivers.manifest` and the picture pack; every file keeps its existing signature; the `withdrawn_versions` and `withdrawn_reason` lines are kept; every file is checked against its manifest (size and sha256) and the tool stops at the first mismatch |
| `make_drivers_index.py --out DIR --driver FILE:COMPATIBLE:BUSES:CATCALL[:ABI_MIN]` | Signed driver files | `<name>-<board>.cat` and `drivers.manifest` (upload next to `modules.manifest`) |

**Why the version must go up:** new images get a version above what is published, because a device refuses to move below the version floors it recorded, and the loader would otherwise "recover" a device to an older system. Upload to GitHub release assets is outward-facing and is the owner's step ([showtime1.md](../showtime1.md), "Official OTA catch-up").

The release names are fixed in the firmware and the tools: **JumpingJaguar1** is the recovery images, **JumpingJaguar2** the drivers and modules, **JumpingJaguar3** the app repository.

### `dev recovery` and the system modules

From CLAUDE.md: `purrstrap dev recovery` regenerates `recovery.manifest` and `modules.manifest` from the files in the folder, so the system modules (about, appmgr, apps, fs, login, netinstall, sysinfo, wifi, from the release's JumpingJaguar2 folder) must be passed with `--modules`. After publishing, fetch every file a manifest names and compare size and sha256.

### VM scripts

[chapter 18](18-emulator-and-vm.md) covers the loop. `mkimage_dbg.sh` takes `SCENARIO=normal|recover|first|auto|kitten` and `DEV_MODE=1`. `publish_apps.sh <app...>` builds each app from `Apps/<name>/<name>.c`, signs it with `signing_keys/developer.key` (key id 2) and publishes it to `out/devrepo`.

### Hardware scripts

`make_hw_image.sh [RELEASE_FOLDER] [OUT]` makes a blank image that starts in the recovery loader once. It has two switches: `MINIMAL=1` writes only the bootloader, the partition table, the trust block and the loader (the loader installs the rest from the server; this is the dev-server route), and `FULL=1` writes the kernel, KittenOS, loader, boot package, trust block and picture pack so the device boots straight into the system. `flash_and_hold.sh` waits for the board's USB port, erases the whole flash, writes the image and keeps the board in download mode until you stop it. Opening the serial port resets the board (a known trap, see the runbook).

### Preview scripts

`tools/boot/*`, `tools/oobe/preview.py` and `tools/lvgl_host/render.py` draw screens on the host and write PNGs under `out/`. The first two use the boot package's pixel font and are mock-ups; `render_real.py` and `lvgl_host/render.py` build the real code. They need `gcc`. The first LVGL build takes about a minute.

### Documentation tools

`check_links.py` checks every relative link and `#anchor` in the Markdown; it ignores code blocks and inline code, does not fetch web links, and exits 1 on any broken link. `count_words.py` counts prose only (not fenced code, table rows or link targets; a link counts as its text) and reports pages at 500 words a page.

### `purrstrap` actions

The scripts under [purrstrap/scripts/](../purrstrap/scripts/apps.py) provide, between them, actions that include `build`, `check`, `clean`, `package`, `package-component`, `slot-image`, `trust-block`, `sign`, `verify`, `inspect`, `index`, `publish`, `recovery`, `serve`, `find`, `withdraw`, `unwithdraw`, `list` and `size`. The areas are Apps, Boot package, Core OS, Games, Keys, Modules, Network recovery record, UI asset pack, and Dev. [chapter 12](12-picture-pack-and-fonts.md) covers the UI asset pack; the build tool has its own chapter.

## How to build and test it

1. Documentation checks: `python3 tools/docs/check_links.py docs/20-tools-and-scripts.md`, then `python3 tools/docs/count_words.py docs`. Tests: `python3 -m unittest discover -s tools/docs -p "test_docs_tools.py"`.
2. Release tool tests: `python3 -m unittest tools/release/test_make_official_from_dev.py`.
3. Build a release folder (the owner supplies the passphrase for this one command): `PURRSTRAP_KEY_PASSWORD=... python3 tools/release/make_release.py --version 1.0.28 --out ~/Desktop/PURR-OS-release`.
4. Build the official folders from a tested dev folder: `python3 tools/release/make_official_from_dev.py --release 1.0.27 --out ~/Desktop/PURR-OS-release-1.0.27`.
5. Check every file named in a manifest by size and sha256 from a fresh download before calling a release published.
6. The relay: `cd tools/broken-kitten-recovery && cargo build --release`, tests with `cargo test`.

## What goes wrong

| Symptom | Cause | Fix |
|---|---|---|
| A release script cannot find Python | Scripts use the IDF Python at `~/.espressif/python_env/idf5.3_py3.9_env/bin/python` | Install ESP-IDF 5.3.5, or set `PURR_PYTHON` where the script allows it (`make_jj3.sh`) |
| Signing fails | No passphrase in the environment, or the `cryptography` module missing | Give the passphrase to that one command; use the IDF Python |
| A device refuses the new release | Version not above the published one | Raise it |
| `dev recovery` drops the system modules | It regenerates manifests from the folder | Pass them with `--modules` |
| A stale `sdkconfig` in a build dir | An old option stays | Delete the build dir's `sdkconfig` or use a new dir |
| zsh splits a variable into words | zsh does not word-split like bash | Quote it, or use an array |
| Serial port opens and the board resets | The port opening toggles reset | Expect it; capture logs knowing that |
| `tools/hw/make_hw_image.sh` cannot find the ui_assets | It copies from a known-good image under the emulator project by default | Set `UI_ASSETS_FROM` |

The three runbook traps in this table are written down in [the runbook](runbook.md).

## What is not finished

- **Official OTA server is behind.** The official `recovery.manifest` says 1.0.11, the dev server 1.0.27. No official folder was built yet; it needs the passphrase and then the owner's upload ([showtime1.md](../showtime1.md)).
- **`make_jj3.sh`** needs the app repository's key made once by the owner; endorsing needs the boot key's password ([chapter 14](14-store-and-repositories.md)).
- **`broken-kitten-recovery`:** the device side of the relay (attended confirmation, pairing, a secure-mode pull), a developer role that pushes signed drivers, a version-floor check and release binaries are not done ([plan3.md](../plan3.md)).
- **Plan3's key-rotation list** names these scripts as the places that carry public keys: `make_release.py`, `make_jj3.sh`, `tools/hw/make_hw_image.sh`, `tools/vm/*`, `uiassets.py` and the relay's `keys.rs`. Rotation is held by the owner's decision (2026-11-06) and not started.
- **Previews are mock-ups** and measure nothing about speed; the real renderers are the proof of layout.
- **Not run for this chapter:** none of the release, hardware or VM scripts. I read their headers and code only.

Back to the index: [README](README.md).
