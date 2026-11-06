# 12. The picture pack and fonts

## Short version

- All UI pictures live in one flash partition, **`ui_assets`** (5 MB, at 0x501000 on the T-Deck Plus). It is read in place from flash: no copy into RAM.
- Build a pack with `python3 purrstrap/purrstrap.py uiassets build --board tdeck_plus --assets-dir <folder of PNGs> --alpha`. Wrap it for delivery with `uiassets package`.
- The pack is its own component, **`uiassets`**, signed and installed like the kernel. There is **one pack per shell**.
- Fonts are compiled in today (Montserrat sizes and a monospace face). Loading fonts from files is **planned work, not built** (Part D of [showtime1.md](../showtime1.md)).
- Plans: [plan7.md](../plan7.md) (packs, shells) and [showtime1.md](../showtime1.md) (fonts).

## What it is for

The kernel image has about 90 KB left in its slot, so pictures cannot be compiled in. Wallpapers and icons are kept in a separate partition and mapped when the UI starts. Replacing the art then means replacing a pack, not the kernel. The owner asked that icon packs be "updatable and pushable like any other part" (2026-11-04, [plan7.md](../plan7.md)).

## The files

| File | What it is |
|---|---|
| [purrstrap/scripts/uiassets.py](../purrstrap/scripts/uiassets.py) | The packer: `build`, `package`, `import-lvgl`; a small pure-Python PNG decoder |
| [purrstrap/tests/test_uiassets.py](../purrstrap/tests/test_uiassets.py) | Tests for the packer |
| [PurrOS/main/purr_ui_assets.c](../PurrOS/main/purr_ui_assets.c) | Maps the partition and finds an asset by name |
| [PurrOS/main/purr_ui_assets.h](../PurrOS/main/purr_ui_assets.h) | `purr_ui_assets_init()`, `purr_ui_assets_find()`, the `purr_asset_t` struct |
| [PurrOS/main/purr_wallpaper.c](../PurrOS/main/purr_wallpaper.c) | The `display.wallpaper` setting (0 to 10) |
| [PurrOS/main/commands.c](../PurrOS/main/commands.c) | `netinstall uiassets` and the install of the component |
| [PurrOS/partitions/tdeck_plus.csv](../PurrOS/partitions/tdeck_plus.csv) | The partition table that holds `ui_assets` |
| [UI/android-assets/](../UI/android-assets/README.md) | The Android shell's 49 source PNGs and their licence note |
| [tools/ui/froyo_pack.py](../tools/ui/froyo_pack.py) | Resamples the Froyo icons once, offline, into that folder |
| [OTA/SPEC.md](../OTA/SPEC.md) | How a package step wraps a payload (section 3) |
| [purrstrap/scripts/dev.py](../purrstrap/scripts/dev.py) | `dev recovery --uiassets` hosts the pack and lists it in `recovery.manifest` |
| [THIRD_PARTY.md](../THIRD_PARTY.md) | Credits for the Android art |

## How it works

### The pack format

A pack is one binary:

| Part | Size | Content |
|---|---|---|
| Header | 12 bytes | magic `0x54534150` ("PAST"), count, CRC32 of everything after the header |
| Directory | 40 bytes per image | name (23 characters and a NUL), offset, length, width, height, colour format |
| Pixels | per image | each image starts on a 4-byte boundary |

Two colour formats exist: `RGB565` (2 bytes a pixel) and `RGB565A8` (the RGB565 plane, then an 8-bit alpha plane, 3 bytes a pixel). The reader hands a pointer straight into mapped flash to LVGL, so nothing is decoded or scaled on the device.

### Why it is not signed per file

The partition holds no code. The signature machinery exists to keep code out of boot-critical slots ([UI/SPEC.md](../UI/SPEC.md) section 3 gives this reasoning). The pack has a CRC32 so that a half-written pack is read as "no assets" and not as garbage.

### Why it is still a signed component

Delivery is another matter. The `uiassets` component (`PURR_MOD_UIASSETS` = 10 in `purr_abi.h`) is wrapped as a file `uiassets-<board>.cat` that the **boot key** (dev server) or the **system key** (release) may sign. The signature protects the download, not the partition. Stage 2 of the recovery loader, step 3 of the setup wizard, and `netinstall uiassets` all install it. The install skips the write when the partition already holds the same payload, erases and writes only the part the pack needs, reads back the hash. There is no signature trailer inside the partition; the pack has its own CRC. A release without a pack is not an error ([plan7.md](../plan7.md)).

### Name lookup

Shell code asks for art by name: `purr_ui_assets_find("fr_phone")`. A missing name is logged and the one widget is skipped. For the Android pack the file name without `.png` is the asset name; the prefixes are in [UI/android-assets/README.md](../UI/android-assets/README.md) (`fr_` launcher, `fr_dock_` dock, `fr_s_` small notification icons, `kb_` key legends, `st_` status icons).

### Wallpapers

`display.wallpaper` is 0 (each surface keeps its own designed picture: names `img0`, `img1`, `img3` for start, lock and sign-in) or 1 to 10 (one built-in picture on every surface). The setting lives in `/etc/system.conf` and survives a change of shell. The pictures come from the pack; the Android shell has no wallpaper set of its own yet (its gradient is the only one), and its picture choices still use the W10M pack names ([plan7.md](../plan7.md)).

### Fonts today

The board file ([sdkconfig.board.tdeck_plus](../PurrOS/sdkconfig.board.tdeck_plus)) turns on Montserrat 12, 16, 20, 28 and 40. The full profile ([sdkconfig.profile.full](../PurrOS/sdkconfig.profile.full)) adds the 16 px Unscii face for the monospace `ui` role and enables the TJPGD decoder for picture frames. A new font size is a build option, not a runtime one. `UI/SHELL-OPTIMIZATION.md` also lists size 14; the board file does not set it, so check LVGL's own default if it matters.

### Fonts from files (planned, owner request of 2026-11-05)

The owner asked to run a font straight from a TTF file so fonts are not compiled in, the emoji can be updated by swapping a file, and people can add fonts through the Store. What was checked, not built:

1. LVGL 9.2.2 has Tiny TTF. `lv_tiny_ttf_create_data(data, len, size)` takes bytes already in memory, which suits PURR OS: the OS reads the file through its own file system into PSRAM.
2. `CONFIG_LV_USE_TINY_TTF` is **off** in our build. Turning it on adds code to a kernel with about 90 KB left (1,876,112 of 1,966,080 bytes at 1.0.26), so the cost has to be measured first.
3. Tiny TTF draws **outline glyphs only**. It cannot draw colour emoji fonts (bitmap and colour-layer tables). So a TTF gives text fonts and **monochrome emoji**. Colour emoji would come from LVGL's image font (`LV_USE_IMGFONT`, also off) with small pictures in the picture pack.

Design to settle with the owner:

| Question | Proposal in showtime1 |
|---|---|
| Where fonts live | A read-only system folder, and a per-user folder for installed ones |
| Fallback | Font the theme asks for, then the emoji font, then built-in Montserrat |
| Untrusted files | Size cap, header and table check before the renderer sees it, a glyph cache limit, loaded only by the UI service, a refusal message |
| Licences | Each font entry names its licence; the Store shows it; shipped fonts go in `THIRD_PARTY.md` |
| Delivery | A "Fonts" category in the Store; [Repositories/SPEC.md](../Repositories/SPEC.md) already lists fonts among assets and extras |

Owner decision 16 (2026-11-06): a UI font and an emoji font, open licences preferred; outline emoji first, then colour emoji as soon as it can be done. See [chapter 14](14-store-and-repositories.md) for the repository side.

## How to build and test it

1. Put PNGs in a folder. They must be 8 bits per channel, RGB or RGBA, not interlaced. A palette, greyscale, 16-bit or interlaced PNG is refused with an error.
2. Build the pack. It is padded to the full partition size:

   ```
   python3 purrstrap/purrstrap.py uiassets build --board tdeck_plus --assets-dir UI/android-assets --alpha
   ```

   It prints the asset count, the size, and an `esptool` line to write it at the partition offset. If the packed data is larger than the partition it says how many bytes it is over.
3. Wrap it for delivery (this does not sign):

   ```
   python3 purrstrap/purrstrap.py uiassets package --board tdeck_plus --file rel/ui_assets/tdeck_plus.ui_assets.bin --version 1.0.0 --out uiassets-tdeck_plus.cat
   ```

4. Signing is a separate step with the owner's passphrase in the environment of that one command; see the [runbook](runbook.md) and [plan3.md](../plan3.md). Never write the passphrase down.
5. Host it: `dev recovery ... --uiassets <the signed .cat>`; the manifest line is `key=system`.
6. On a device with a server set, `netinstall uiassets` installs it.
7. Tests: `cd purrstrap && python3 -m unittest discover -s tests`. The system Python lacks the `cryptography` module that `test_keys` and `test_trustblock` need (plan7).

## What goes wrong

| Symptom | Cause | Fix |
|---|---|---|
| Home screen shows words, not icons | No pack on the device | `netinstall uiassets`, or build a full image (`FULL=1 tools/hw/make_hw_image.sh`) |
| Stage 2 never fetches the pack on a minimal image | A dev server found by discovery is not remembered across the restart (unconfirmed); also developer mode off makes stage 2 use the official server | Plan3 and plan7 record both; use the full image or `netinstall uiassets` |
| `not enough free memory for the ... byte pack` | A 4.5 MB download needs a large PSRAM block | Close apps or restart, then run `netinstall uiassets` again |
| `asset name too long` | Names must be under 24 bytes | Rename the PNG |
| A pack of the wrong shell | One pack per shell | Install the pack that matches the compiled shell |
| Pack rejected as corrupt | Bad CRC or magic after an interrupted write | Install again |

## What is not finished

- The Windows-style pack (about 4.5 MB) and its 1.0.17 package were not on the server at the last note ([plan7.md](../plan7.md)); whether a 4.5 MB download fits the PSRAM allocator is open.
- Android wallpaper set and the drawer's missing icons ([chapter 11](11-ui-stack-and-shells.md)).
- Part D (fonts from files, emoji, Store category) is not started; it waits for the owner's keyword ([showtime1.md](../showtime1.md)).
- The official release carried no `uiassets` entry as of plan3's note, so a device that installs from the official server gets no pack.
- Disagreement: [UI/SPEC.md](../UI/SPEC.md) says 512 KB for the partition. The partition table (0x500000) and `SLOT_BYTES` in `dev.py` both say 5 MB; the SPEC is out of date.
- Not verified on a device: the Android pack installing through stage 2.

Next: [13 Networking](13-networking.md).
