# Making a UI

*Current as of PURR OS 1.1 (2026-11-08).*

**Short version.** A UI is a theme: the pictures a shell draws (app icons, battery and signal marks, wallpaper) and a few colours. You describe the look in one small file, `ui.toml`, and the kit draws every picture at the size the shell wants. Draw any of them yourself, or drop in a **SquareLine Studio export**, and yours win. In Studio it is the **make a UI** tile; from a terminal it is `bin/purr ui`.

## The file

```toml
[ui]
name = "moonlit"          # becomes /ui/themes/moonlit on the device
shell = "android"         # android or w10m: which pictures to make
board = "tdeck_plus"
author = "Your Name"
licence = "CC0-1.0"       # the licence of your pictures, shown by the Store
version = "0.1.0"

[look]
accent = "#60a917"
background = "#101214"
foreground = "#f2f2f2"
dark = true

[adapt]
scale = 1.0               # multiplies every picture size (keep 1.0: today's shells draw at fixed sizes)
wallpapers = 1

[pictures]
# fr_app = "imported/home.png"     your own picture for a slot; yours always wins
```

The slots, with their sizes, are the names in `UI/PACK-NAMES.md`: 31 for the Android style shell, 21 for the Windows style one, plus the optional ones the shell draws something else for.

## Where each picture comes from

Best first: a `[pictures]` line in `ui.toml`; a file named like the slot in the project's `images/` folder; a picture brought in from SquareLine Studio (in `imported/`); otherwise the kit draws it from `[look]` (flat glyphs with soft edges: tiles for apps, a battery that fills, signal bars, Wi-Fi arcs). A picture of yours that is not the slot's size is resized to it, and the report says which. The device never scales a picture, so sizes matter.

## Dropping in a SquareLine Studio export

Export the UI from SquareLine Studio (Export, Export UI Files) and zip the export folder, or zip the whole project folder. In Studio choose **make a UI**, start from **a SquareLine Studio export**, and drop the zip. From a terminal:

```
bin/purr ui import moonlit --squareline export.zip --shell android
```

What it reads: the project's own source PNGs (`assets/`, lossless) and the exported image arrays (`ui_img_*_png.c`, LVGL 8 and 9, 16-bit colour, with or without alpha). Each picture is written as a PNG into the project's `imported/` folder and matched to a slot **by name** (case, dashes and spaces are ignored): name a picture `fr_app` in SquareLine and it lands in the `fr_app` slot. The rest are listed, and you choose their slot from a menu (or write the line in `[pictures]`). A 32-bit export (ARGB8888) is refused with a plain message. SquareLine's screens, events and fonts are not imported: a theme is pictures and colours, not a layout.

## Make it

Studio: edit `ui.toml` (the preview redraws on save), press **make the UI**. Terminal: `bin/purr ui make moonlit`. The result is `build/moonlit/` with `theme.conf` and `pack.bin`, checked the way the device checks it (names, sizes, CRC, the pack's size cap). Copy that folder to `/ui/themes/moonlit` on the device, or onto the SD card's `ui/themes` folder, then choose it in Settings. `bin/purr build moonlit` does the same, so the Build tab works for a UI project.

## Good to know

- The drawn pictures are placeholders with a consistent look, not artwork. Replace the ones people look at most.
- A theme is accepted on the same trust tiers as a repository; fonts are not part of a theme ([21-fonts.md](../../docs/21-fonts.md) in the repository).
- The SquareLine importer was written from the export layouts and tested on exports made to those layouts; **it has not been tried on a real SquareLine export yet.** If one fails, send the file that refuses and the message it gave.
- The device side that lists and applies themes is described in [14-ui-packs-and-themes.md](14-ui-packs-and-themes.md).
