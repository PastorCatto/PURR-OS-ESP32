#!/usr/bin/env python3
"""Makes the Windows 10 Mobile shell's picture pack (UI/w10m-assets/ and out/ui/pack-w10m.bin) from the upstream project.

The pictures are the unchanged artwork of windows-10-mobile-lvgl by Felix Biego (MIT licence),
https://github.com/fbiego/windows-10-mobile-lvgl, as THIRD_PARTY.md credits. This script

  1. clones the project (git clone --depth 1) into a NEW EMPTY temporary folder outside the repository and records the commit;
     the clone is treated as untrusted data: it is only read (PNG files and the byte tables of the SquareLine .c files, never
     executed or built), and the temporary folder is removed at the end,
  2. takes, for every picture name the shell asks for (UI/PACK-NAMES.md), the upstream PNG if there is one (squareline/assets/<name>.png,
     copied as it is) or else the SquareLine image array (src/ui/images/ui_img_<name>_png.c, whose RGB565 and alpha planes are
     written out as a PNG), and saves the result in UI/w10m-assets/ with a NOTICE (the licence, the commit, what was converted),
  3. packs the folder with purrstrap's own packer (purrstrap/scripts/uiassets.py): pictures with an alpha channel as RGB565A8,
     opaque ones (the wallpapers) as RGB565, 4-byte aligned, CRC32,
  4. prints the names in the pack and the REQUIRED names (UI/PACK-NAMES.md) that upstream has no picture for.

  python3 -I tools/ui/w10m_pack.py                      # fetch upstream, convert, pack
  python3 -I tools/ui/w10m_pack.py --no-fetch           # pack the pictures already in UI/w10m-assets
  python3 -I tools/ui/w10m_pack.py --src DIR            # use a checkout you made yourself (also read as data only)

Then `purrstrap uiassets package --name ui_w10m --file out/ui/pack-w10m.bin --version <release>` wraps it for the release and
`purrstrap keys sign` signs it (neither is done here). Standard library only, like purrstrap and froyo_pack.py.
"""
import argparse
import importlib.util
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
_spec = importlib.util.spec_from_file_location("uiassets", os.path.join(ROOT, "purrstrap", "scripts", "uiassets.py"))
sys.path.insert(0, os.path.join(ROOT, "purrstrap"))
_ua = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_ua)

UPSTREAM = "https://github.com/fbiego/windows-10-mobile-lvgl"
OUT_DIR = os.path.join(ROOT, "UI", "w10m-assets")
PACK = os.path.join(ROOT, "out", "ui", "pack-w10m.bin")

WALL_W, WALL_H = 320, 569

# name -> (width, height, required, where the shell asks for it). Required = a complete pack carries it (UI/PACK-NAMES.md): without it the
# shell shows a wrong symbol or nothing where the picture belongs. The wallpapers have a plain fallback in code, so they are not required.
NAMES = {}
for _n, _where in (("wp_system", "Settings: System"), ("wp_devices", "Settings: Devices"), ("wp_network", "Settings: Network"),
                   ("wp_personalization", "Settings: Personalization"), ("wp_apps", "Settings: Apps"), ("wp_account", "Settings: Accounts"),
                   ("wp_time", "Settings: Time & Language"), ("wp_privacy", "Settings: Privacy & security"), ("wp_about", "Settings: About")):
    NAMES[_n] = (32, 32, True, _where)
NAMES["wp_settings"] = (32, 32, True, "the Settings tile and its row in the app list")
NAMES["wifi_icon"] = (24, 24, True, "the status bar's network icon")
for _n, _where in (("wifi_ic", "Network"), ("brightness_ic", "Brightness"), ("settings_ic", "Settings"), ("airplane_ic", "Airplane"),
                   ("cellular_ic", "Cellular"), ("bluetooth_ic", "Bluetooth"), ("battery_ic", "Battery"), ("hotspot_ic", "Hotspot"),
                   ("vpn_ic", "VPN"), ("location_ic", "Location")):
    NAMES[_n] = (16, 16, True, "quick action: " + _where)
for _i in range(10):
    NAMES["img%d" % _i] = (WALL_W, WALL_H, False, "wallpaper %d (the default choice uses img0 on the start screen, img1 on the lock screen, img3 on sign-in)" % (_i + 1))



def required():
    return [n for n, v in NAMES.items() if v[2]]


# ---------------------------------------------------------------------------------------------- reading upstream

def run_git(args, cwd=None):
    env = dict(os.environ, GIT_TERMINAL_PROMPT="0")
    return subprocess.run(["git"] + args, cwd=cwd, env=env, check=True, capture_output=True, text=True).stdout.strip()


def fetch(into):
    """Shallow clone into `into` (a new, empty folder). Returns (checkout folder, commit)."""
    dest = os.path.join(into, "upstream")
    run_git(["clone", "--depth", "1", "--quiet", UPSTREAM, dest])
    return dest, run_git(["rev-parse", "HEAD"], cwd=dest)


def commit_of(folder):
    try:
        return run_git(["rev-parse", "HEAD"], cwd=folder)
    except (subprocess.CalledProcessError, OSError):
        return "unknown (not a git checkout)"


def write_png(path, w, h, rgba):
    def chunk(tag, data):
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)
    raw = b"".join(b"\x00" + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))
    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) +
           chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))
    with open(path, "wb") as fh:
        fh.write(png)


def array_to_rgba(w, h, data, cf):
    """The data of a SquareLine array as RGBA: RGB565 little-endian, and for CF_RGB565A8 the alpha plane after the colour plane.
    (Checked against the PNG of the same picture where upstream has both: the pixels are identical.)"""
    out = bytearray(w * h * 4)
    for i in range(w * h):
        v = data[i * 2] | (data[i * 2 + 1] << 8)
        r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
        out[i * 4:i * 4 + 3] = bytes(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)))
        out[i * 4 + 3] = data[w * h * 2 + i] if cf == _ua.CF_RGB565A8 else 255
    return bytes(out)


def convert(src, out_dir, names):
    """For each name: PNG copy or array conversion. Returns (rows, missing); a row is (name, how, w, h)."""
    rows, missing = [], []
    os.makedirs(out_dir, exist_ok=True)
    for name in names:
        png = os.path.join(src, "squareline", "assets", name + ".png")
        arr = os.path.join(src, "src", "ui", "images", "ui_img_%s_png.c" % name)
        dest = os.path.join(out_dir, name + ".png")
        if os.path.isfile(png):
            w, h, _ch, _px = _ua.decode_png(png)                     # refuses anything but 8-bit RGB/RGBA, non-interlaced
            shutil.copyfile(png, dest)
            rows.append((name, "upstream PNG, copied", w, h))
        elif os.path.isfile(arr):
            got, w, h, data, cf = _ua.parse_lvgl_array(arr)
            if got != name or len(data) != w * h * (3 if cf == _ua.CF_RGB565A8 else 2):
                raise ValueError("%s: the array does not match its own header" % arr)
            write_png(dest, w, h, array_to_rgba(w, h, data, cf))
            rows.append((name, "SquareLine array, converted", w, h))
        else:
            missing.append(name)
            continue
        want = NAMES[name][:2]
        if (w, h) != want:
            print("  note: %s is %dx%d, the shell expects %dx%d" % (name, w, h, want[0], want[1]))
    return rows, missing


NOTICE_HEAD = """The pictures in this folder are the artwork of windows-10-mobile-lvgl by Felix Biego,
{upstream}
used under the MIT licence below (also quoted in THIRD_PARTY.md at the top of this repository).

Upstream commit: {commit}

What was taken, and how (tools/ui/w10m_pack.py): for each picture name the Windows 10 Mobile shell asks for (UI/PACK-NAMES.md),
the upstream PNG (squareline/assets/<name>.png) is copied unchanged; where upstream has only the SquareLine image array
(src/ui/images/ui_img_<name>_png.c) its RGB565 colour plane and 8-bit alpha plane are expanded to an RGBA PNG (the same pixels:
checked against the pictures upstream has both of). Nothing is resized or recoloured.
{table}
Some of this imagery (the Windows-style icons and the wallpapers) is not Felix Biego's to license; THIRD_PARTY.md says it is placeholder
art to be replaced before anything is shipped.

----------------------------------------------------------------------------------------------------------------------

"""


def write_notice(out_dir, commit, rows, licence):
    table = "\n" + "\n".join("  %-20s %3d x %-3d  %s" % (n, w, h, how) for n, how, w, h in rows) + "\n"
    with open(os.path.join(out_dir, "NOTICE"), "w", encoding="utf-8", newline="\n") as fh:
        fh.write(NOTICE_HEAD.format(upstream=UPSTREAM, commit=commit, table=table) + licence.rstrip() + "\n")


# ---------------------------------------------------------------------------------------------- packing

def build_pack(folder, out, skip=()):
    """Packs every PNG in `folder` (name = file name) with purrstrap's packer. Returns the list of (name, w, h, cf, bytes) that went in."""
    images, info = [], []
    for fname in sorted(f for f in os.listdir(folder) if f.lower().endswith(".png")):
        name = fname[:-4]
        if name in skip:
            continue
        w, h, ch, px = _ua.decode_png(os.path.join(folder, fname))
        if ch == 4:
            data, cf = _ua.to_rgb565a8(w, h, ch, px), _ua.CF_RGB565A8
        else:
            data, cf = _ua.to_rgb565(w, h, ch, px), _ua.CF_RGB565
        images.append((name, w, h, data, cf))
        info.append((name, w, h, cf, len(data)))
    blob = _ua.pack(images)
    if len(blob) > _ua.MAX_PACK_BYTES:
        raise ValueError("the pack is %d bytes, more than the %d-byte cap" % (len(blob), _ua.MAX_PACK_BYTES))
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    with open(out, "wb") as fh:
        fh.write(blob)
    return info, len(blob)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--out-dir", default=OUT_DIR, help="where the converted PNGs go (default UI/w10m-assets)")
    ap.add_argument("--pack", default=PACK, help="the pack file (default out/ui/pack-w10m.bin)")
    ap.add_argument("--src", help="use this checkout of the upstream project instead of cloning")
    ap.add_argument("--no-fetch", action="store_true", help="do not fetch or convert: pack the PNGs already in --out-dir")
    ap.add_argument("--wallpapers", choices=("one", "all", "defaults", "none"), default="one",
                    help="which wallpapers go in the pack: one (img0, the default), all ten (about 3.6 MB), the three the default choice uses (img0, img1, img3), or none")
    a = ap.parse_args()

    skip = set()
    if a.wallpapers != "all":
        keep = {"one": {"img0"}, "defaults": {"img0", "img1", "img3"}}.get(a.wallpapers, set())
        skip = {"img%d" % i for i in range(10)} - keep

    missing = []
    if not a.no_fetch:
        tmp = None
        try:
            if a.src:
                src = os.path.abspath(a.src)
            else:
                tmp = tempfile.mkdtemp(prefix="w10m-upstream-")      # new and empty, outside the repository
                print("cloning %s into %s ..." % (UPSTREAM, tmp))
                src, _ = fetch(tmp)
            commit = commit_of(src)
            print("upstream commit %s" % commit)
            with open(os.path.join(src, "LICENSE"), encoding="utf-8", errors="replace") as fh:
                licence = fh.read()
            if "MIT License" not in licence[:200] or "Felix Biego" not in licence[:300]:
                print("error: the upstream LICENSE is not the MIT licence of Felix Biego; not converting", file=sys.stderr)
                return 1
            rows, missing = convert(src, a.out_dir, {n: v for n, v in NAMES.items() if n not in skip})
            write_notice(a.out_dir, commit, rows, licence)
            print("%d picture(s) converted into %s" % (len(rows), a.out_dir))
        finally:
            if tmp:
                shutil.rmtree(tmp, ignore_errors=True)
    else:
        missing = [n for n in NAMES if not os.path.isfile(os.path.join(a.out_dir, n + ".png"))]

    info, size = build_pack(a.out_dir, a.pack, skip)
    print("\n%s: %d picture(s), %d bytes (cap %d)" % (a.pack, len(info), size, _ua.MAX_PACK_BYTES))
    for name, w, h, cf, n in info:
        print("  %-20s %3d x %-3d %-9s %7d bytes" % (name, w, h, "RGB565A8" if cf == _ua.CF_RGB565A8 else "RGB565", n))
    have = {i[0] for i in info}
    gone = [n for n in required() if n not in have]
    extra = sorted(have - set(NAMES))
    print("\nrequired names: %d, in the pack: %d" % (len(required()), len(required()) - len(gone)))
    if gone:
        print("MISSING required names (no upstream picture; original art or the code's fallback?): " + ", ".join(gone))
    else:
        print("no required name is missing")
    if extra:
        print("pictures in the folder the shell does not ask for: " + ", ".join(extra))
    if missing and not a.no_fetch:
        print("names with no upstream picture: " + ", ".join(missing))
    return 1 if gone else 0


if __name__ == "__main__":
    sys.exit(main())
