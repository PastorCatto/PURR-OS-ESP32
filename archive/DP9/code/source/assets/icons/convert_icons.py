#!/usr/bin/env python3
"""
convert_icons.py — BlackPURR MDI icon converter

Downloads MDI SVGs from GitHub, renders them at target sizes,
tints them white (for dark theme), and outputs LVGL-compatible
C arrays as lv_img_dsc_t structs.

Usage:
  python3 convert_icons.py              # convert all icons in icons.json
  python3 convert_icons.py --list       # list icons without converting
  python3 convert_icons.py cog 48       # convert one icon by MDI slug + size

Output: source/assets/icons/generated/<size>/<name>.c
        source/assets/icons/blackpurr_icons.h  (master include)

Dependencies: requests, Pillow, cairosvg
  pip3 install requests Pillow cairosvg
"""

import argparse
import json
import os
import struct
import sys
import time

import requests
from PIL import Image
from io import BytesIO

# cairosvg is preferred but needs a native libcairo, which pip cannot supply on
# Windows. svglib + reportlab(rlPyCairo -> pycairo) is the self-contained
# fallback; see _render_svglib() for the alpha caveat and the cairocffi trap.
try:
    import cairosvg
    _HAVE_CAIROSVG = True
except Exception:
    cairosvg = None
    _HAVE_CAIROSVG = False

try:
    from svglib.svglib import svg2rlg
    from reportlab.graphics import renderPM
    _HAVE_SVGLIB = True
except Exception:
    svg2rlg = None
    renderPM = None
    _HAVE_SVGLIB = False

if not _HAVE_CAIROSVG and not _HAVE_SVGLIB:
    sys.exit(
        "No SVG rasterizer available.\n"
        "  Linux/macOS:  pip install cairosvg\n"
        "  Windows:      pip install svglib reportlab pycairo rlPyCairo\n"
        "                (do NOT install cairocffi — rlPyCairo prefers it and "
        "then fails to load native cairo)"
    )

SCRIPT_DIR   = os.path.dirname(os.path.abspath(__file__))
MANIFEST     = os.path.join(SCRIPT_DIR, "icons.json")
GENERATED    = os.path.join(SCRIPT_DIR, "generated")
HEADER_OUT   = os.path.join(SCRIPT_DIR, "blackpurr_icons.h")

# ── Icon packs ────────────────────────────────────────────────────────────────
# Each entry is a raw-SVG URL template plus the manifest key its slugs live
# under, so icons.json can carry slugs for several packs side by side and
# switching is a one-line change here (or --pack on the command line) rather
# than a rewrite of the manifest.
#
# ionicons is the active pack: it was built by the Ionic team to mirror iOS
# design language, which is what the Mochi springboard is going for, and its
# default (unsuffixed) slugs are the FILLED variants — solid glyphs, which is
# what actually reads at ~26px inside a 46px squircle. Thin-stroke packs
# (Feather/Lucide/Tabler) disappear at that size on this panel.
#
# Note on SF Symbols, the obvious first instinct for an iOS look: Apple's
# license permits use only in software running on Apple platforms, so it
# cannot ship inside this firmware. Ionicons is the closest freely-licensed
# stand-in.
PACKS = {
    "mdi": {
        "url": "https://raw.githubusercontent.com/Templarian/MaterialDesign/master/svg/{slug}.svg",
        "key": "mdi",
        "license": "Apache-2.0 / MIT (Pictogrammers)",
    },
    "ionicons": {
        "url": "https://raw.githubusercontent.com/ionic-team/ionicons/main/src/svg/{slug}.svg",
        "key": "ion",
        "license": "MIT (Ionic)",
    },
}

ACTIVE_PACK = "ionicons"

# ── ANSI ──────────────────────────────────────────────────────────────────────

os.system("")
C_RST = "\033[0m"; C_GRN = "\033[92m"; C_YLW = "\033[93m"
C_RED = "\033[91m"; C_CYN = "\033[96m"; C_GRY = "\033[90m"

def ok(msg):   print(f"{C_GRN}  ✓{C_RST}  {msg}")
def warn(msg): print(f"{C_YLW}  ⚠{C_RST}  {msg}")
def err(msg):  print(f"{C_RED}  ✗{C_RST}  {msg}")
def div(label=""):
    line = f"─ {label} " + "─"*max(0,50-len(label)-2) if label else "─"*50
    print(f"{C_GRY}{line}{C_RST}")

# ── SVG download ──────────────────────────────────────────────────────────────

_svg_cache = {}

def fetch_svg(slug):
    if slug in _svg_cache:
        return _svg_cache[slug]
    url = PACKS[ACTIVE_PACK]["url"].format(slug=slug)
    try:
        r = requests.get(url, timeout=10)
        if r.status_code == 200:
            _svg_cache[slug] = r.text
            return r.text
        err(f"HTTP {r.status_code} for {slug}")
        return None
    except Exception as e:
        err(f"fetch failed for {slug}: {e}")
        return None

# ── SVG → PIL Image ───────────────────────────────────────────────────────────

def _render_cairosvg(svg_text, size):
    """Preferred path: true alpha straight out of the rasterizer."""
    png_data = cairosvg.svg2png(
        bytestring=svg_text.encode(),
        output_width=size,
        output_height=size,
    )
    return Image.open(BytesIO(png_data)).convert("RGBA")


def _render_svglib(svg_text, size):
    """
    Fallback for hosts where cairosvg can't load libcairo — notably Windows,
    where cairosvg/cairocffi dlopen a native cairo that pip does not provide.
    svglib parses the SVG and reportlab's renderPM rasterizes it through
    rlPyCairo -> pycairo, whose wheels ARE self-contained.

    IMPORTANT: install pycairo but NOT cairocffi. rlPyCairo prefers cairocffi
    when both are present and then dies on the same dlopen cairosvg does.

    renderPM has no usable transparent-background mode here, so the glyph is
    rendered black-on-white and alpha is recovered from luminance: these are
    monochrome single-colour icons, so 255-grey is exactly the coverage the
    rasterizer computed, antialiased edges included.
    """
    drawing = svg2rlg(BytesIO(svg_text.encode()))
    if drawing is None or not drawing.width or not drawing.height:
        raise ValueError("svglib produced an empty drawing")

    drawing.scale(size / drawing.width, size / drawing.height)
    drawing.width = size
    drawing.height = size

    png = renderPM.drawToString(drawing, fmt="PNG", bg=0xFFFFFF)
    grey = Image.open(BytesIO(png)).convert("L")
    alpha = grey.point(lambda v: 255 - v)
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    img.putalpha(alpha)
    return img


def svg_to_image(svg_text, size, tint_color=(255, 255, 255)):
    """Render SVG at size×size, tint all opaque pixels to tint_color."""
    if _HAVE_CAIROSVG:
        img = _render_cairosvg(svg_text, size)
    else:
        img = _render_svglib(svg_text, size)

    # Tint: replace RGB channels with tint_color, preserve alpha. White is the
    # default because these sit on a coloured background — a white glyph on a
    # tinted squircle is exactly how an iOS app icon is built.
    a = img.split()[3]
    return Image.merge("RGBA", (
        Image.new("L", img.size, tint_color[0]),
        Image.new("L", img.size, tint_color[1]),
        Image.new("L", img.size, tint_color[2]),
        a,
    ))

# ── PIL Image → LVGL C array ──────────────────────────────────────────────────

def image_to_lvgl_c(img, name, size):
    """
    Convert RGBA PIL image to LVGL lv_img_dsc_t for LV_COLOR_DEPTH=16 + LV_COLOR_16_SWAP=1.

    Format: LV_IMG_CF_TRUE_COLOR_ALPHA = RGB565 (byte-swapped) + Alpha = 3 bytes per pixel.

    RGB → RGB565:
        r5 = r >> 3
        g6 = g >> 2
        b5 = b >> 3
        rgb565 = (r5 << 11) | (g6 << 5) | b5   # 16-bit value

    LV_COLOR_16_SWAP stores the high byte first:
        byte0 = rgb565 >> 8      (high byte: RRRRRGGG)
        byte1 = rgb565 & 0xFF    (low byte:  GGGBBBBB)
        byte2 = alpha
    """
    pixels = list(img.getdata())  # list of (R, G, B, A)
    total  = size * size

    data = []
    for r, g, b, a in pixels:
        rgb565 = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
        # LV_COLOR_16_SWAP: high byte first
        data += [(rgb565 >> 8) & 0xFF, rgb565 & 0xFF, a]

    c_name   = name.replace("-", "_")
    arr_name = f"_bp_icon_{c_name}_{size}_data"
    dsc_name = f"bp_icon_{c_name}_{size}"

    lines = []
    lines.append(f"// BlackPURR icon: {name} @ {size}x{size}  (RGB565+A, LV_COLOR_16_SWAP)")
    lines.append(f"// Auto-generated by convert_icons.py — do not edit")
    lines.append(f"")
    lines.append(f'#include "lvgl.h"')
    lines.append(f"")
    lines.append(f"static const uint8_t {arr_name}[] = {{")

    for i in range(0, len(data), 16):
        chunk = data[i:i+16]
        lines.append("    " + ", ".join(f"0x{b:02X}" for b in chunk) + ",")

    lines.append(f"}};")
    lines.append(f"")
    lines.append(f"const lv_img_dsc_t {dsc_name} = {{")
    lines.append(f"    .header = {{")
    lines.append(f"        .cf          = LV_IMG_CF_TRUE_COLOR_ALPHA,")
    lines.append(f"        .always_zero = 0,")
    lines.append(f"        .reserved    = 0,")
    lines.append(f"        .w           = {size},")
    lines.append(f"        .h           = {size},")
    lines.append(f"    }},")
    lines.append(f"    .data_size = {total * 3},")
    lines.append(f"    .data      = {arr_name},")
    lines.append(f"}};")
    lines.append(f"")

    return "\n".join(lines)

# ── Convert one icon ──────────────────────────────────────────────────────────

def convert_one(slug, size, label=None):
    label    = label or slug
    c_label  = label.replace("-", "_")
    out_dir  = os.path.join(GENERATED, str(size))
    out_path = os.path.join(out_dir, f"{c_label}.c")
    os.makedirs(out_dir, exist_ok=True)

    svg = fetch_svg(slug)
    if not svg:
        warn(f"skipping {label} ({slug}) @ {size} — could not fetch SVG")
        return False

    try:
        img   = svg_to_image(svg, size)
        c_src = image_to_lvgl_c(img, c_label, size)
        with open(out_path, "w", encoding="utf-8") as f:
            f.write(c_src)
        ok(f"{label} @ {size}px  →  generated/{size}/{c_label}.c")
        return True
    except Exception as e:
        err(f"{label} @ {size}: {e}")
        return False

# ── Generate master header ────────────────────────────────────────────────────

def gen_header(entries):
    """entries: list of (label, size) tuples for all successfully converted icons."""
    lines = []
    lines.append("// blackpurr_icons.h — BlackPURR MDI icon declarations")
    lines.append("// Auto-generated by convert_icons.py — do not edit")
    lines.append("// Include this header in any BlackPURR source file.")
    lines.append("")
    lines.append("#pragma once")
    lines.append('#include "lvgl.h"')
    lines.append("")
    lines.append("// ── Status bar icons (24×24) ─────────────────────────────────────────────────")
    lines.append("")

    for label, size in sorted(entries):
        c_label = label.replace("-", "_")
        if size == 24:
            lines.append(f"extern const lv_img_dsc_t bp_icon_{c_label}_{size};")

    lines.append("")
    lines.append("// ── App grid / drawer icons (48×48) ──────────────────────────────────────────")
    lines.append("")

    for label, size in sorted(entries):
        c_label = label.replace("-", "_")
        if size == 48:
            lines.append(f"extern const lv_img_dsc_t bp_icon_{c_label}_{size};")

    lines.append("")
    lines.append("// ── Convenience macros ───────────────────────────────────────────────────────")
    lines.append("")
    for label, size in sorted(entries):
        c_label  = label.replace("-", "_").upper()
        c_label_lower = label.replace("-", "_")
        lines.append(f"#define BP_ICON_{c_label}_{size}   (&bp_icon_{c_label_lower}_{size})")

    lines.append("")

    with open(HEADER_OUT, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    ok(f"blackpurr_icons.h  →  {os.path.relpath(HEADER_OUT)}")

# ── Main ──────────────────────────────────────────────────────────────────────

def load_manifest():
    with open(MANIFEST) as f:
        return json.load(f)

def _slug_for(meta):
    """
    Slug for the active pack, falling back to any other pack's slug present on
    the entry. The fallback keeps a partially-migrated manifest converting
    instead of erroring out — an entry that has no slug for the new pack yet
    still produces its old icon rather than a missing symbol at link time.
    Returns (slug, is_fallback).
    """
    key = PACKS[ACTIVE_PACK]["key"]
    if meta.get(key):
        return meta[key], False
    for other in PACKS.values():
        if meta.get(other["key"]):
            return meta[other["key"]], True
    return None, False

def all_icons(manifest):
    """Yield (label, slug, size) for every icon in the manifest."""
    for section in ("status_bar", "apps"):
        for label, meta in manifest.get(section, {}).items():
            slug, fallback = _slug_for(meta)
            if not slug:
                warn(f"{label}: no slug for any known pack — skipped")
                continue
            if fallback:
                warn(f"{label}: no '{PACKS[ACTIVE_PACK]['key']}' slug, using '{slug}' from another pack")
            for size in meta.get("sizes", [48]):
                yield label, slug, size

def cmd_list(manifest):
    div(f"icon manifest — pack: {ACTIVE_PACK} ({PACKS[ACTIVE_PACK]['license']})")
    for section in ("status_bar", "apps"):
        print(f"\n  {C_CYN}{section}{C_RST}")
        for label, meta in manifest.get(section, {}).items():
            sizes = ", ".join(str(s) for s in meta["sizes"])
            slug, fallback = _slug_for(meta)
            mark = f"{C_YLW}*{C_RST}" if fallback else " "
            print(f"    {label:<25}{mark}{slug or '(none)':<35} [{sizes}px]")
    div()

def cmd_convert_all(manifest):
    div("converting icons")
    items   = list(all_icons(manifest))
    success = []
    failed  = []

    for i, (label, slug, size) in enumerate(items, 1):
        print(f"  [{i:02d}/{len(items):02d}] {label} ({slug}) @ {size}px")
        if convert_one(slug, size, label):
            success.append((label, size))
        else:
            failed.append((label, slug, size))
        time.sleep(0.1)  # gentle on GitHub CDN

    div()
    print(f"\n  {C_GRN}converted: {len(success)}{C_RST}   {C_RED}failed: {len(failed)}{C_RST}\n")

    if failed:
        print(f"  {C_YLW}Failed icons:{C_RST}")
        for label, slug, size in failed:
            print(f"    {label} (mdi:{slug}) @ {size}px")
        print()

    if success:
        gen_header(success)

def cmd_convert_one(slug, size):
    div(f"converting {slug} @ {size}px")
    label = slug.replace("-", "_")
    if convert_one(slug, size, label):
        gen_header([(label, size)])

def main():
    parser = argparse.ArgumentParser(
        prog="convert_icons",
        description="BlackPURR MDI icon converter — MDI SVG → LVGL C array",
    )
    parser.add_argument("slug", nargs="?", help="MDI slug to convert (e.g. 'cog')")
    parser.add_argument("size", nargs="?", type=int, choices=[24, 48], help="target size in px")
    parser.add_argument("--list", action="store_true", help="list all icons without converting")
    args = parser.parse_args()

    print()
    div("BlackPURR icon converter")
    print(f"  MDI → LVGL lv_img_dsc_t  |  white tint  |  RGB565+A (LV_COLOR_16_SWAP)")
    div()
    print()

    manifest = load_manifest()

    if args.list:
        cmd_list(manifest)
    elif args.slug and args.size:
        cmd_convert_one(args.slug, args.size)
    else:
        cmd_convert_all(manifest)

if __name__ == "__main__":
    main()
