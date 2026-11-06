# 12. The picture pack and fonts

## Short version

- UI pictures live in **pack files in LittleFS**, one default pack per shell: `/ui/w10m/pack.bin` and `/ui/android/pack.bin`. There is no `ui_assets` partition in the new flash map ([chapter 2](02-flash-map-and-partitions.md), [plan1.md](../plan1.md) "Flash map v2"). The UI reads a pack into PSRAM at start.
- A **theme** is a pack plus a `theme.conf` in `/ui/themes/<id>/` (three at most on the device, more on the SD card). Lookup is layered: the active theme's pack first, then the shell's default pack. The choice is `/ui/active.conf` and applies at the next restart.
- The picture names each shell asks for, and which are required, are in [UI/PACK-NAMES.md](../UI/PACK-NAMES.md). `purr ui check` ([sdk/docs/14-ui-packs-and-themes.md](../sdk/docs/14-ui-packs-and-themes.md)) checks a pack against them.
- Build a pack: `python3 purrstrap/purrstrap.py uiassets build --assets-dir <folder of PNGs> --alpha --out pack.bin` (Android) or `python3 -I tools/ui/w10m_pack.py` (Windows, from the upstream project). Wrap it for the release with `uiassets package --name ui_w10m|ui_android`.
- The release components are **`ui_w10m`** and **`ui_android`**, signed with the system key and installed by the recovery loader's second stage as the files above. The old single `uiassets` component and `netinstall uiassets` are gone.
- Fonts are compiled in today (Montserrat sizes and a monospace face). Loading fonts from files is **planned work, not built** (Part D of [showtime1.md](../showtime1.md)).
- **Status:** the kit side (packer, `dev recovery`, the SDK commands) is written and tested on the host. The device side (reading the files, themes, the install) is written in the working tree for 1.0.29 and has **not been built or run on a device** as of this note. Plans: [plan7.md](../plan7.md) (packs, themes, shells), [plan1.md](../plan1.md) (the flash map) and [showtime1.md](../showtime1.md) (fonts).

## What it is for

The kernel image has about 90 KB left in its slot, so pictures cannot be compiled in. Wallpapers and icons are kept in files and loaded when the UI starts. Replacing the art then means replacing a file, not the kernel. The owner asked that icon packs be "updatable and pushable like any other part" (2026-11-04, [plan7.md](../plan7.md)), and on 2026-11-06 that each shell be a self-contained package with its own pack, and that a user be able to install a custom theme from the Store.

Until 1.0.29 all pictures sat in one raw 5 MB partition and whichever pack was published won: the pack on the dev server had only the Android art and the Windows shell lost its icons. Two packs, one per shell, remove that.

## The files

| File | What it is |
|---|---|
| [purrstrap/scripts/uiassets.py](../purrstrap/scripts/uiassets.py) | The packer: `build`, `package` (with `--name`), `import-lvgl`; a small pure-Python PNG decoder. Packs are files, capped at 5 MB |
| [purrstrap/tests/test_uiassets.py](../purrstrap/tests/test_uiassets.py) | Tests for the packer |
| [PurrOS/main/purr_ui_assets.c](../PurrOS/main/purr_ui_assets.c) | Reads the shell's pack and the active theme's pack from LittleFS and finds a picture by name |
| [PurrOS/main/purr_ui_assets.h](../PurrOS/main/purr_ui_assets.h) | `purr_ui_assets_init()`, `purr_ui_assets_find()`, the `purr_asset_t` struct, the theme list and choice functions |
| [PurrOS/main/purr_wallpaper.c](../PurrOS/main/purr_wallpaper.c) | The `display.wallpaper` setting (0 to 10) |
| [PurrOS/main/commands.c](../PurrOS/main/commands.c) | The install of the `ui_w10m` and `ui_android` components into `/ui/<shell>/pack.bin` |
| [UI/PACK-NAMES.md](../UI/PACK-NAMES.md) | Every picture name each shell asks for, its size, required or fallback |
| [UI/android-assets/](../UI/android-assets/README.md) | The Android shell's 49 source PNGs and their licence note |
| [UI/w10m-assets/](../UI/w10m-assets/README.md) | The Windows shell's source PNGs (from the upstream MIT project) and their notice |
| [tools/ui/froyo_pack.py](../tools/ui/froyo_pack.py) | Resamples the Froyo icons once, offline, into the Android folder |
| [tools/ui/w10m_pack.py](../tools/ui/w10m_pack.py) | Fetches the upstream Windows 10 Mobile project into a temporary folder, converts the pictures the shell asks for, writes the folder and `out/ui/pack-w10m.bin` |
| [sdk/purrsdk/ui.py](../sdk/purrsdk/ui.py), [sdk/docs/14-ui-packs-and-themes.md](../sdk/docs/14-ui-packs-and-themes.md) | `purr ui pack`, `purr ui check`, `purr new theme`: for people who make a pack or a theme |
| [OTA/SPEC.md](../OTA/SPEC.md) | How a package step wraps a payload (section 3) |
| [purrstrap/scripts/dev.py](../purrstrap/scripts/dev.py) | `dev recovery --ui-w10m --ui-android` hosts the packs and lists them in `recovery.manifest` (`key=system`, 5 MB cap) |
| [THIRD_PARTY.md](../THIRD_PARTY.md) | Credits for the Android and Windows art |

## How it works

### The pack format

A pack is one binary, `pack.bin`, unchanged by the move to files:

| Part | Size | Content |
|---|---|---|
| Header | 12 bytes | magic `0x54534150` ("PAST"), count, CRC32 of everything after the header |
| Directory | 40 bytes per image | name (23 characters and a NUL), offset, length, width, height, colour format |
| Pixels | per image | each image starts on a 4-byte boundary |

Two colour formats exist: `RGB565` (2 bytes a pixel) and `RGB565A8` (the RGB565 plane, then an 8-bit alpha plane, 3 bytes a pixel). The reader hands a pointer into the pack in PSRAM to LVGL, so nothing is decoded or scaled on the device. The device refuses a pack over 5 MB, with more than 128 pictures, or with an entry outside the file ([purr_ui_assets.c](../PurrOS/main/purr_ui_assets.c)).

### Where packs and themes live

| Path (LittleFS) | What |
|---|---|
| `/ui/w10m/pack.bin`, `/ui/android/pack.bin` | the default pack of a shell (a shell reads only its own) |
| `/ui/themes/<id>/pack.bin`, `/ui/themes/<id>/theme.conf` | an installed theme; at most three |
| `/ui/active.conf` | `theme=<id>` (absent or empty: none), read at start |
| `/sdcard/ui/themes/<id>/...` | the same on the SD card, any number; one chosen there is copied into a slot |

`theme.conf` is `key=value` lines: `name`, `author`, `licence`, `version`, `shell` (`w10m`, `android` or `any`), and optionally `accent` (an index or `#RRGGBB`) and `dark` (`1`/`0`). A theme is assets plus settings, not code, and carries no font ([docs/21-fonts.md](21-fonts.md)). If the active theme cannot be loaded (a damaged file, the wrong shell, a missing card) the shell starts with its default pack. The loader does not read `accent` and `dark` yet.

### Why it is not signed per file

The pack holds no code. The signature machinery exists to keep code out of boot-critical slots ([UI/SPEC.md](../UI/SPEC.md) section 3 gives this reasoning). The pack has a CRC32 so that a half-written pack is read as "no assets" and not as garbage; since the file is in PSRAM, the check runs after the shell has drawn its first frame.

### Why it is still a signed component

Delivery is another matter. A shell's pack is a component (`ui_w10m` or `ui_android`; the container subtype is `PURR_MOD_UIASSETS` = 10 in `purr_abi.h`), wrapped as a file `ui_w10m-<board>.cat` that the **boot key** (dev server) or the **system key** (release) may sign; the manifest line is `key=system`. The signature protects the download. Stage 2 of the recovery loader installs the pack into `/ui/<shell>/pack.bin` with `purr_fs_write_kept`, skipping the write when the same pack is already there and reading the hash back. A release without a pack is not an error. The `uiassets` component and `netinstall uiassets` belonged to the raw partition and are retired; `dev recovery --uiassets` still accepts the old name but no device uses it.

### Name lookup

Shell code asks for art by name: `purr_ui_assets_find("fr_phone")`, the theme first, then the default pack. A missing name is a fallback in the shell (a letter tile, words on the key strip, a gradient) or, for the pictures that have none, an empty place. What each shell asks for is in [UI/PACK-NAMES.md](../UI/PACK-NAMES.md): Android uses `fr_` launcher, `fr_dock_` dock, `fr_s_` small notification, `kb_` key legend and `st_` status names; Windows uses `wp_` Settings and tile, `*_ic` and `wifi_icon` names. The Android pack must not contain `wp_*` names, because the Android build also draws the Settings screens of the Windows shell and would show them.

### Wallpapers

`display.wallpaper` is 0 (each surface keeps its own designed picture: names `img0`, `img1`, `img3` for start, lock and sign-in) or 1 to 10 (one built-in picture, `img0` to `img9`, on every surface). The setting lives in `/etc/system.conf` and survives a change of shell. The ten pictures are 320 x 569 and are in the **Windows** pack from `w10m_pack.py` (3.6 MB of the 5 MB cap); both shells ask for the same names, but the Android pack from `froyo_pack.py` has none, so the Android shell shows its gradient ([plan7.md](../plan7.md)).

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
2. Build the pack (the output is the pack itself, no padding; over 5 MB is refused):

   ```
   python3 purrstrap/purrstrap.py uiassets build --assets-dir UI/android-assets --alpha --out out/ui/pack-android.bin
   python3 -I tools/ui/w10m_pack.py          # fetches upstream; --no-fetch packs UI/w10m-assets as it is
   ```

   Each prints the picture count and size. `w10m_pack.py` also lists any required name upstream has no picture for (none are missing today).
3. Check it: `python3 sdk/purr.py ui check out/ui/pack-w10m.bin --shell w10m` (header, directory, CRC, sizes and the required names).
4. Wrap it for delivery (this does not sign):

   ```
   python3 purrstrap/purrstrap.py uiassets package --board tdeck_plus --file out/ui/pack-w10m.bin --version 1.0.29 --name ui_w10m --out ui_w10m-tdeck_plus.cat
   ```

5. Signing is a separate step with the owner's passphrase in the environment of that one command; see the [runbook](runbook.md) and [plan3.md](../plan3.md). Never write the passphrase down.
6. Host it: `dev recovery ... --ui-w10m <signed .cat> --ui-android <signed .cat>`; the manifest lines are `key=system`. Each is capped at 5 MB. Fetch every file the manifest names and compare size and sha256 ([CLAUDE.md](../CLAUDE.md)).
7. Tests: `cd purrstrap && python3 -m unittest discover -s tests` (the system Python lacks the `cryptography` module that `test_keys` and `test_trustblock` need; the IDF Python has it) and `cd sdk && python3 -m unittest discover -s tests`.

## What goes wrong

| Symptom | Cause | Fix |
|---|---|---|
| Home screen shows words or letters, not icons | No pack file for this shell on the device | Install the component for the shell (stage 2 does it when the release carries it), or build a full image |
| Windows shell has no icons although a pack is installed | The Android pack is in `/ui/w10m/` or an old single pack was used | Each shell reads only its own file; build the Windows pack with `w10m_pack.py` |
| Settings in the Android shell shows Windows icons | `wp_*` pictures are in the Android pack or its theme | Remove them |
| `asset name too long` | Names must be under 24 bytes | Rename the PNG |
| `purr ui check` says CRC mismatch or a picture runs past the end | A damaged or partly written file | Install or copy it again |
| A theme is chosen but the shell starts with its own look | The theme's `shell` is the other shell, its pack is damaged, or its card is missing | Settings names the theme that failed; check it with `purr ui check` |
| `its payload is ... and the limit for a UI pack is 5242880` | `dev recovery` refuses a pack over 5 MB | Drop wallpapers (`w10m_pack.py --wallpapers defaults`) or shrink pictures |

## What is not finished

- The device side of this chapter has not been built or run on a device: the file reader, the layered lookup, the install into `/ui/<shell>/pack.bin`, Settings' theme list, and the cable flash that the new partition table needs ([chapter 2](02-flash-map-and-partitions.md)).
- Themes in the Store (a `theme` item type in the repository format) and a theme's `accent` and `dark`.
- The Windows art is the upstream project's, much of it Windows-style or of unstated origin: placeholder art to be replaced before release ([THIRD_PARTY.md](../THIRD_PARTY.md)). Anything new is original.
- The Android shell has no wallpaper set of its own yet; its drawer's missing icons are in [chapter 11](11-ui-stack-and-shells.md).
- Part D (fonts from files, emoji, Store category) is not started; it waits for the owner's keyword ([showtime1.md](../showtime1.md)).
- A whole UI as a signed shell package (plan7.md) is designed, not built.
- Disagreement: [UI/SPEC.md](../UI/SPEC.md) still says 512 KB for the partition; there is no partition now and the cap is 5 MB a file.

Next: [13 Networking](13-networking.md).
