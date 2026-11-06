# Colour emoji: feasibility note

Status: a study, written 2026-11-06. Nothing here is built into the firmware or the picture-pack tools. The sizes were measured on the host with Pillow (conversion) and zlib (deflate); nothing was run on a device.

## The question

Tiny TTF (stb_truetype) draws outline glyphs only, so the monochrome Noto Emoji subset in [noto-emoji/](noto-emoji/SOURCE.txt) can be tinted by the text colour but cannot show colour. Colour emoji have to be pictures. LVGL 9.2.2 has an image font (`lv_imgfont_create(height, path_cb, user_data)` in `PurrOS/managed_components/lvgl__lvgl/src/others/imgfont/`): the callback is asked for a code point and returns the picture source to draw (a path or a pointer to an image descriptor, per LVGL's header; not tried here). `LV_USE_IMGFONT` is off in the build today, and its code size has not been measured (the kernel slot has about 70 KB free).

## Two sources

| | Noto Color Emoji (images) | Twemoji (graphics) |
|---|---|---|
| Upstream | <https://github.com/googlefonts/noto-emoji>, tag `v2026-09-24-unicode18_0`, commit e20cbc2bbec1926686be9f9bee7d1d2cfa1fea0e, `2D/png/128/emoji_u<hex>.png` (also 32, 72 and 512 px) | <https://github.com/jdecked/twemoji> (the maintained continuation of Twitter's project), tag `v17.0.3`, commit b6b55fef1e8636b540a6d016a4729ca8cdf2e60b, `assets/72x72/<hex>.png` and `assets/svg/` |
| Licence | The README says the tools and most image resources are Apache 2.0 and the fonts are OFL 1.1. The repository's top `LICENSE` file at that tag holds OFL 1.1 text, and `2D/svg/LICENSE` holds Apache 2.0, so the files disagree with the README. Both licences allow redistribution; **the exact licence of the PNGs should be confirmed upstream before shipping.** | Graphics: **CC BY 4.0** (`LICENSE-GRAPHICS`). Code: MIT. Attribution is required. |
| Look | Soft shading, the same family as the outline Noto Emoji font | Flat, bolder shapes |
| Source pictures | 128 px RGBA PNG, about 6.5 KB each | 72 px palette PNG, about 4 KB each |

## What was measured

The core set of [noto-emoji/core-set.txt](noto-emoji/core-set.txt) has 412 emoji code points (414 minus the joiner and the variation selector). All 412 exist in both sources. Each was downloaded, scaled with Lanczos to a square, and measured. "PNG RGBA" is a full-colour PNG. "PNG 64 colours" is an octree-quantised palette PNG with alpha (visibly fine at 24 px for these pictures, in the sample sheet of 16; heavier quantising to 16 colours was only measured for size). "Raw" is the picture pack's `RGB565A8` format (3 bytes a pixel, see [chapter 12](../../docs/12-picture-pack-and-fonts.md)).

| Size and source | Raw RGB565A8 per glyph | Raw, 412 glyphs | Raw, deflated | PNG RGBA avg (max) | PNG RGBA, 412 | PNG 64 colours avg, 412 |
|---|---|---|---|---|---|---|
| 20 px Noto | 1,200 B | 494,400 B | 205,909 B | 1,022 B (1,396) | 420,882 B | 624 B, 256,898 B |
| 20 px Twemoji | 1,200 B | 494,400 B | 171,805 B | 965 B (1,435) | 397,476 B | 593 B, 244,422 B |
| 24 px Noto | 1,728 B | 711,936 B | 265,313 B | 1,314 B (1,867) | 541,468 B | 685 B, 282,047 B |
| 24 px Twemoji | 1,728 B | 711,936 B | 215,219 B | 1,204 B (1,923) | 496,232 B | 642 B, 264,450 B |

Reading the table: raw pictures are the largest but need no decoder and map in place; PNG needs LVGL's PNG decoder (`LV_USE_LODEPNG`, off, code size not measured, and it needs RAM to decode each picture); the deflated column is what a transport compression of a raw pack would save (not a format the pack reader supports today).

## How it would be packed

One asset, signed like the other packs, in the repository format of [Repositories/SPEC.md](../../Repositories/SPEC.md):

1. **Format.** The existing pack layout (12-byte header with CRC32, 40-byte directory entries, `RGB565A8` pixels), one entry per code point named `u<hex>` (at most 23 characters). Directory cost: 412 x 40 = 16,480 bytes. Total for Noto at 24 px: about 728 KB; at 20 px: about 511 KB. One size per pack; a second size is a second pack.
2. **Where it lives.** A file in the system folder (and, for an installed one, the per-user folder) rather than the `ui_assets` partition, which is full of the shell's own art. It is a Store/repository asset, so the signed index carries its size, SHA-256 and licence.
3. **Loading.** The UI service reads the file into PSRAM (about 0.5 to 0.7 MB of the 8 MB), checks the CRC, and keeps an in-memory index. The image-font callback maps the code point to a pointer to an in-memory image descriptor (width, height, `RGB565A8`, data pointer), so no decode happens at draw time. Reading single pictures on demand with a small cache is the alternative if PSRAM is wanted elsewhere.
4. **Fallback.** Order: text font, colour emoji (image font) for code points the pack has, outline emoji font, Montserrat. The two emoji sets share code points, so the colour set simply wins where it exists.
5. **Tool.** A `purr font pack` style command could convert a folder of PNGs to this pack. It is not written; the packer in `purrstrap/scripts/uiassets.py` already does the PNG-to-RGB565A8 step.

## Recommendation

- **Noto Color Emoji at 24 px, raw RGB565A8, core set: about 728 KB.** It matches the outline Noto Emoji already chosen (same shapes, same code points), and its licence needs only the notice kept. The Apache-versus-OFL question about the PNGs should be settled upstream first.
- Twemoji is about 8% smaller at 24 px (496 KB against 541 KB as PNG RGBA; the raw pack is the same size for both) but its CC BY 4.0 licence requires an attribution that must be shown where users can find it (the project accepts a mention in an About screen, README or source; wording below). It is the fallback choice if Noto's PNG licence turns out to be a problem, or if the owner prefers the flat look.
- The size is a pack setting: 20 px costs 494 KB raw and suits Montserrat 16 to 20 text; 24 px suits larger text.

## Attribution text to ship

For Noto (graphics from Noto Color Emoji; the exact licence name to be confirmed as above):

> Colour emoji pictures: Noto Color Emoji, copyright Google LLC, from <https://github.com/googlefonts/noto-emoji>, used under the Apache License 2.0 (images and tools) / SIL Open Font License 1.1 (fonts). Resized for display; no other change.

For Twemoji, if it is used instead:

> Colour emoji graphics: Twemoji, copyright Twitter, Inc. and other contributors, continued by the jdecked/twemoji contributors, from <https://github.com/jdecked/twemoji>, licensed under CC BY 4.0 (<https://creativecommons.org/licenses/by/4.0/>). Changed: resized and converted to a picture pack.

The exact copyright line should be copied from the upstream notices (the code licence names Twitter for 2014 to 2021 and Jason Sofonia and Justine De Caires from 2022; the graphics licence file holds only the CC text). Put the text in `THIRD_PARTY.md` and in Settings, About, Legal, with the licence text in the pack's folder, when a colour pack is built.

## What was not checked

- The code size of `LV_USE_IMGFONT` (and `LV_USE_LODEPNG` for a PNG route) in the kernel; the 70 KB slot may not hold both.
- Rendering of pictures through the image font on a real panel; RAM use of a 0.5 to 0.7 MB pack in PSRAM next to the other users.
- Flags, skin tones and ZWJ sequences: not in the set; Tiny TTF and the image font as used here do no sequence shaping.
