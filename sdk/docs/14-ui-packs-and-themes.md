# UI packs and themes

## Short version

- A **UI pack** is one file, `pack.bin`: pictures, looked up by name. Each shell (the Windows 10 Mobile style one, `w10m`, and the Android 2.2 style one, `android`) has its own default pack.
- A **theme** is a folder with a `theme.conf` and a `pack.bin`. It is pictures and settings, never code. A theme can hold just a few pictures; the shell's default pack answers for the rest.
- Make one: `purr new theme dusk`, put PNGs in `images/`, then `purr ui pack images --out pack.bin` and `purr ui check . --shell w10m`.
- `purr ui check` is the same test the device applies (size, directory, offsets, CRC) plus the picture names a shell asks for. The list of names is `UI/PACK-NAMES.md` in the repository.
- **What is built:** the kit side on this page (`purr ui pack`, `purr ui check`, `purr new theme`, `purr build` for a theme) and purrstrap's component names. **What is not finished:** the device side (reading `/ui/<shell>/pack.bin`, choosing a theme in Settings, installing a theme from the Store) is being written and has **not been run on a device** at the time of this note. A theme folder you make here will not do anything on a device until that lands. See [plan7.md](../../plan7.md) and [plan1.md](../../plan1.md).

## The pack format

The format `purrstrap uiassets` has always written, unchanged:

| Part | Size | Content |
|---|---|---|
| Header | 12 bytes | magic `0x54534150` ("PAST"), picture count, CRC32 of everything after the header |
| Directory | 40 bytes a picture | name (up to 23 characters and a NUL), offset, length, width, height, colour format |
| Pixels | per picture | each picture starts on a 4-byte boundary |

Two colour formats: RGB565 (2 bytes a pixel) and RGB565A8 (the RGB565 plane, then an 8-bit alpha plane: 3 bytes a pixel). `purr ui pack` stores a picture with real transparency as RGB565A8 and an opaque one (no alpha channel, or every alpha 255) as RGB565. PNGs must be 8 bits per channel, RGB or RGBA, not interlaced. Nothing is scaled on the device, so make each picture the size the shell draws it at.

Limits the device applies (read from `PurrOS/main/purr_ui_assets.c`; `purr ui check` tests the same): at most **5 MB** a file, at most **128 pictures** in a pack, every picture's pixels inside the file and starting at an even offset, the CRC matching. A pack that fails is not used and the shell shows its fallbacks.

## Where packs live on the device

The contract (plan1.md, "Flash map v2"), in LittleFS:

| Path | What |
|---|---|
| `/ui/w10m/pack.bin`, `/ui/android/pack.bin` | a shell's default pack (installed from the release components `ui_w10m` and `ui_android`) |
| `/ui/themes/<id>/pack.bin`, `/ui/themes/<id>/theme.conf` | an installed theme; three at most are kept |
| `/ui/active.conf` | `theme=<id>`; absent or empty means none. The choice applies at the next restart |
| `/sdcard/ui/themes/<id>/...` | the same tree on the SD card, any number |

Lookup is layered: the active theme's pack first, then the shell's default pack, by name. A theme whose `shell` is not the compiled shell is not used. `<id>` is the folder name: 1 to 39 letters, digits, underscores or dashes (the device skips any other folder name).

## theme.conf

`key=value` lines, no spaces around `=`; a line starting with `#` is a comment. The device reads at most 511 bytes of the file.

| Key | Needed | Value |
|---|---|---|
| `name` | yes | the name Settings shows, up to 31 characters |
| `author` | yes | up to 31 characters |
| `licence` | yes | the licence of your pictures, for example `CC-BY-4.0` |
| `version` | yes | starts with a digit, up to 11 characters, like `1.0.0` |
| `shell` | yes | `w10m`, `android` or `any` |
| `accent` | no | an index 0 to 19 (the shell's accent list) or a colour like `#336699` |
| `dark` | no | `1` for a dark base, `0` for light |

`accent` and `dark` are in the contract and `purr ui check` accepts them. The device code does not read them from `theme.conf` yet: it reads `name`, `author`, `version` and `shell`.

**Fonts are not part of a theme.** The font renderer must not be given untrusted files ([docs/21-fonts.md](../../docs/21-fonts.md)), so a theme carries pictures only.

## The check

```
purr ui check pack.bin                      # the file: size, header, directory, offsets, CRC
purr ui check pack.bin --shell android      # and every REQUIRED picture name of that shell is in it
purr ui check mytheme/ --shell w10m         # a theme folder: theme.conf, the pack, the names
```

For a **pack file** with `--shell`, every required name must be present, because that is what a shell's default pack needs; a missing one fails the check and is named. For a **theme folder** the missing names are only listed (a warning), because the default pack answers for them. Both warn when a known picture has a different size from the one the shell draws, and list names the shell never asks for (usually a misspelling). An Android pack with `wp_*` pictures gets a warning: the Android build also compiles the Windows shell's Settings code, which would draw them there.

A theme folder holds `theme.conf` and either `pack.bin` or an `images/` folder (checked, and `purr ui pack` builds it). Exit code 1 means something is wrong; `--json` prints the report.

## Make a theme

1. `purr new theme dusk` makes `dusk/` in your workspace with a `theme.conf` (author and licence are empty on purpose: the check fails until you fill them in), an empty `images/` and a two-line `README.txt`.
2. Fill in `theme.conf`. Pick `shell=w10m` or `shell=android` if your pictures use one shell's names.
3. Put PNGs in `images/`, named for what they replace. The names and sizes are in `UI/PACK-NAMES.md`; the wallpapers are `img0` to `img9`, 320 x 569, drawn centred and cropped.
4. `purr build dusk` writes `build/dusk/theme.conf` and `build/dusk/pack.bin` (or run `purr ui pack images --out pack.bin` yourself), then `purr ui check build/dusk --shell w10m`.
5. The folder `build/dusk` is the theme. Today you would copy it to `/ui/themes/dusk` on the device; installing from the Store comes with the device side.

Your pictures are yours; say their licence in `theme.conf`. Do not ship art you do not have the right to. Art in the shells' own packs that came from other projects is credited in `THIRD_PARTY.md`.

## Making a shell's default pack

The two default packs are built by tools in the repository, not by the kit:

- Android: `tools/ui/froyo_pack.py` (Froyo resources, Apache 2.0) writes `UI/android-assets/`, packed with `purrstrap uiassets build --assets-dir UI/android-assets --alpha`.
- Windows: `tools/ui/w10m_pack.py` fetches the upstream MIT project into a temporary folder, converts what the shell asks for into `UI/w10m-assets/` and packs it into `out/ui/pack-w10m.bin`.

For the release, `purrstrap uiassets package --name ui_w10m|ui_android --file pack.bin --version V` wraps a pack as a component and `purrstrap keys sign` signs it with the release's system key; `purrstrap dev recovery --ui-w10m FILE --ui-android FILE` hosts them (size cap 5 MB each). Neither the signing nor the install by the recovery loader's second stage has been run on a device with this layout.

## What is not done

- The device reading these files, the Settings list of themes, the restart choice and the Store's `theme` item type: designed in [plan7.md](../../plan7.md), being written by the project lead, not run on a device.
- A theme's `accent` and `dark` taking effect.
- Wallpapers larger than a slot on the SD card, and sounds: open questions in plan7.md.
- A whole custom shell as a signed package: designed, not built.
