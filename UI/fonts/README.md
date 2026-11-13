# The Type shell's fonts

The Type shell (plan7.md, "The Type shell") sets its screens in two typefaces, both under the **SIL Open Font License 1.1** (the licence texts are next to this file):

| Typeface | Used for | Licence file | Where it came from |
|---|---|---|---|
| Space Grotesk Bold (2.0.0) | the clocks, the dates, list names, headings | `OFL-SpaceGrotesk.txt` | https://github.com/floriankarsten/space-grotesk (release 2.0.0, `ttf/static/SpaceGrotesk-Bold.ttf`) |
| Space Mono Bold | the small capital labels: the strip, the key hints, the tiles | `OFL-SpaceMono.txt` | https://github.com/googlefonts/spacemono (`fonts/ttf/SpaceMono-Bold.ttf`) |

The TrueType files are not in the repository. What is in it is what the firmware uses: bitmap fonts in LVGL's format, one C file per size, in
`PurrOS/main/tpfonts/`, made by `tools/ui/gen_lvgl_font.py` (needs `freetype-py`; no Node). They are a rendering of the typefaces, so the licence travels with them
(THIRD_PARTY.md has the entry). The fonts are not renamed: the OFL's reserved-name rule applies to modified versions of the fonts themselves, and these are
glyph bitmaps at fixed sizes, not a font file that anyone installs; if that reading is ever doubted, the answer is to rename them.

To make them again:

```sh
python3 -m venv /tmp/fontenv && /tmp/fontenv/bin/pip install freetype-py
G=tools/ui/gen_lvgl_font.py; O=PurrOS/main/tpfonts; B=SpaceGrotesk-Bold.ttf; M=SpaceMono-Bold.ttf
/tmp/fontenv/bin/python $G --ttf $B --size 92  --bpp 4 --chars "0123456789:" --name tp_font_clock_xl    --out $O/tp_font_clock_xl.c
/tmp/fontenv/bin/python $G --ttf $B --size 116 --bpp 4 --chars "0123456789"  --name tp_font_clock_lock  --out $O/tp_font_clock_lock.c
/tmp/fontenv/bin/python $G --ttf $B --size 60  --bpp 4 --chars "0123456789"  --name tp_font_clock_panel --out $O/tp_font_clock_panel.c
/tmp/fontenv/bin/python $G --ttf $B --size 56  --bpp 4 --chars "0123456789"  --name tp_font_date_big    --out $O/tp_font_date_big.c
/tmp/fontenv/bin/python $G --ttf $B --size 26  --bpp 4 --range 0x20-0x7E     --name tp_font_bold_26     --out $O/tp_font_bold_26.c
/tmp/fontenv/bin/python $G --ttf $B --size 20  --bpp 4 --range 0x20-0x7E     --name tp_font_bold_20     --out $O/tp_font_bold_20.c
/tmp/fontenv/bin/python $G --ttf $B --size 16  --bpp 4 --range 0x20-0x7E     --name tp_font_bold_16     --out $O/tp_font_bold_16.c
/tmp/fontenv/bin/python $G --ttf $M --size 14  --bpp 4 --range 0x20-0x7E     --name tp_font_mono_14     --out $O/tp_font_mono_14.c
/tmp/fontenv/bin/python $G --ttf $M --size 12  --bpp 4 --range 0x20-0x7E     --name tp_font_mono_12     --out $O/tp_font_mono_12.c
```

About 70 KB of bitmap in all, in flash, read in place. The shared screens (Settings, the portal, app windows) still use Montserrat; moving them is a theme job
(plan7.md).
