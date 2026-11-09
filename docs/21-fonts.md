# 21. Fonts from files

*Current as of PURR OS 1.1 (2026-11-08).*

## Short version

- Today every font is compiled in (Montserrat sizes and one monospace face). Loading fonts from **files** is **designed, not built**: the kernel side does not exist yet.
- Two fonts are ready as files in [UI/fonts/](../UI/fonts/): **Roboto** (UI text, SIL OFL 1.1, about 85 KB per weight) and **Noto Emoji** (monochrome outline emoji, SIL OFL 1.1, 192 KB, 414 code points).
- The plan: the OS reads a TTF into PSRAM and gives the bytes to LVGL's Tiny TTF. Colour emoji will be pictures, not a TTF. Tiny TTF draws outlines only.
- `purr font check FILE.ttf` in the SDK is built and tested on the host. It is the check the OS is meant to apply before a font reaches the renderer.
- Nothing here has run on a device. The cost to the kernel (which has about 70 KB free) is not measured.

## What it is for

The owner asked (Part D of [showtime1.md](../showtime1.md), decision 16 of 2026-11-06) for fonts that are files, so that the emoji can be updated by swapping one file and people can add fonts through the Store. The kernel slot is nearly full, so fonts cannot grow it. A UI font and an emoji font, both under open licences, come first; colour emoji follow.

## The files

| File | What it is |
|---|---|
| [UI/fonts/roboto/](../UI/fonts/roboto/SOURCE.txt) | Roboto 3.016 Regular and Bold, subset to Latin, Latin Extended, Greek, Cyrillic, Vietnamese and common punctuation (1,193 code points, 85,124 and 85,804 bytes), the licence, and the exact subset command |
| [UI/fonts/noto-emoji/](../UI/fonts/noto-emoji/SOURCE.txt) | Noto Emoji 3.002 fixed at weight 400 and subset to a core set (191,612 bytes), the licence, and [the list of code points](../UI/fonts/noto-emoji/core-set.txt) |
| [UI/fonts/COLOUR-EMOJI.md](../UI/fonts/COLOUR-EMOJI.md) | Feasibility note for colour emoji: Noto Color Emoji against Twemoji, measured sizes, how a pack would look |
| [sdk/purrsdk/font.py](../sdk/purrsdk/font.py) | The font check, standard library only |
| [sdk/tests/test_font.py](../sdk/tests/test_font.py) | 22 tests: generated tiny fonts, the two stored fonts, and the command |
| [sdk/docs/10-cli.md](../sdk/docs/10-cli.md) | The `purr font check` command in the kit's command list |
| [THIRD_PARTY.md](../THIRD_PARTY.md) | Credits and licences for both fonts |
| [PurrOS/main/purr_catrt_ui.c](../PurrOS/main/purr_catrt_ui.c) | Where the roles are tied to compiled-in fonts today (the monospace role uses `lv_font_unscii_16`) |

## How it works (as designed)

**Rendering.** LVGL 9.2.2 has Tiny TTF, built on `stb_truetype`. `lv_tiny_ttf_create_data(data, len, size)` takes bytes already in memory, so the OS reads the file through its own file system into PSRAM and hands the pointer over; no LVGL file driver is needed. A glyph cache sets how much RAM a font instance uses; where that memory comes from was not checked. `CONFIG_LV_USE_TINY_TTF` is off today, and switching it on adds code to a kernel with about 70 KB left. That cost is the first thing to measure.

**Outlines only.** `stb_truetype` draws `glyf` or `CFF` outlines. It ignores colour and bitmap tables (`CBDT`, `COLR`, `sbix`, `SVG`). So the stored emoji font is the monochrome Noto Emoji, drawn in the text colour. It is a static instance because Tiny TTF ignores variation tables; the variable original is 1.98 MB. The subsets drop hinting and layout tables, since neither is used.

**Colour emoji** are pictures. LVGL's image font (`LV_USE_IMGFONT`, also off) asks a callback for a picture per code point. The note in [UI/fonts/COLOUR-EMOJI.md](../UI/fonts/COLOUR-EMOJI.md) measured 412 core emoji at 24 px as raw `RGB565A8` (711,936 bytes) and as PNG (about 0.5 MB), and recommends Noto Color Emoji. The picture-pack format of [chapter 12](12-picture-pack-and-fonts.md) would carry it.

**Fallback chain** (proposed). The font a theme asks for, then the colour emoji pictures, then the outline emoji font, then Montserrat. A missing glyph never shows as a blank. Neither stored font has the plain arrows U+2190 to U+2193, the check mark U+2713, or the star and triangles; those stay with the symbols that Montserrat carries, which is why it is the last link.

**Where files live** (proposed, not decided). A read-only system folder such as `/system/fonts` for fonts that ship, and `/home/<user>/fonts` for installed ones, next to the other per-user folders ([chapter 02](02-flash-map-and-partitions.md)). A Store category "Fonts" needs no new mechanism: fonts are already among the "assets and extras" of [Repositories/SPEC.md](../Repositories/SPEC.md), so the signed index gives size, SHA-256 and the licence name shown before installing ([chapter 14](14-store-and-repositories.md)).

**Untrusted files.** The copy of `stb_truetype` inside LVGL says at the top: **no security guarantee, do not use this on untrusted font files**. That is the main risk of the whole feature. The design answers it in layers: only the UI service loads fonts, never an app; a size cap (1 MB by default); the structure check described below; a limit on the glyph cache; and a refusal message instead of a crash. A signed repository asset adds a trust step that a loose file lacks. The structure check cannot prove a file safe, because it does not walk each glyph's contour data. Whether per-glyph checks, or loading only signed fonts, is required is an open decision.

## How to build and test it

Check a font:

```
python3 sdk/purr.py font check UI/fonts/roboto/Roboto-Regular-ui.ttf
```

It reads the file itself. In order, it: refuses a file over the cap (`--max-size`, default 1,048,576 bytes) without reading it; reads the sfnt header and table directory and tests every offset and length against the file size; looks for `head`, `hhea`, `maxp`, `cmap`, `hmtx` and `glyf` with `loca` (or `CFF`); tests the `head` magic, the units per em, the metric and `loca` lengths and that `loca` offsets rise and stay inside `glyf`; reads the best `cmap` subtable (formats 4 and 12) and lists the covered code points as ranges, with the share of Latin-1 and of a list of 22 shell symbols; and prints the copyright and licence strings from the `name` table. It warns, without failing, about colour or bitmap tables, variation tables and a missing licence. It exits 1 with a plain message for a file that fails, and `--json` gives the same report as data.

Tests: `cd sdk && python3 -m unittest discover -s tests`. The last run was 135 tests, 15 skipped (as before), all passing; 22 of them are in `test_font.py`. They include damaged and truncated copies of the real font, which must be reported and never crash the checker.

To make a subset again, follow the commands in the two `SOURCE.txt` files. They need fontTools, which only the subset step uses; the kit does not. Check the links with `python3 tools/docs/check_links.py docs/21-fonts.md UI/fonts`.

## What goes wrong

| Symptom | Cause | Fix |
|---|---|---|
| `purr font check` says "colour" with a warning | The font has `COLR`, `CBDT` or similar tables | Tiny TTF would draw outlines only (or nothing for a bitmap-only font); use a picture pack for colour |
| "variable font" warning | `fvar` is present | Make a static instance (`fontTools.varLib.instancer`, as in the emoji `SOURCE.txt`) |
| "no Unicode cmap subtable of format 4 or 12" | An old or unusual font with only formats 0 or 6 | The checker reads 4 and 12; the renderer reads more. Convert the font |
| "this is a font collection" or "WOFF" | `.ttc` or web font file | Extract one font as a plain `.ttf` |
| Boxes where a symbol should be | Neither font has the code point | The fallback chain ends at Montserrat; see the missing-symbol list in the check output |
| Fonts do not load on a device | The loader is not built | Expected today |

## What is not finished

- **The kernel side is not built.** There is no loader, no Tiny TTF in the build, no `ui` catcall for choosing a font, no Settings page and no fallback chain. All of the design above is design only.
- The kernel size cost of Tiny TTF and of the image font is not measured. About 70 KB are free now (the showtime1 text still says 90 KB; the slot has been filling).
- `purr font add` and `purr font pack` are not written. The check is the first step.
- The colour emoji pack is a study. No pictures are stored in the repository, and the exact licence of the Noto PNGs needs confirming upstream (the README and the top licence file of that repository disagree).
- The two fonts were checked with `purr font check` and by reading the glyph tables with fontTools. **They were not rendered by Tiny TTF**, so how they look at 12 to 20 px on the 320x240 screen is not known.
- Not verified on a device: anything in this chapter.
