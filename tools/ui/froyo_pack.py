#!/usr/bin/env python3
"""Makes the Android shell's icon pack (UI/android-assets/) from the Android 2.2 (Froyo) resources.

The sources are the Android Open Source Project's own icons (Apache License 2.0): the launcher and
hotseat art from the Launcher2, Phone, Contacts, Browser, Settings, Camera, Messaging, DeskClock and
Music repositories, and the status-bar icons from frameworks/base (core/res/res/drawable-mdpi) of the
froyo-release branch. Nothing here is Google application artwork or a phone maker's skin.

Each source is resampled once, here, to the size the shell draws it at, with alpha handled
premultiplied (so edges do not pick up a dark fringe), and written as a PNG named for the asset it
becomes. `purrstrap uiassets build --alpha true` packs the folder for the ui_assets partition.
Nothing is ever scaled on the device.

  python3 tools/ui/froyo_pack.py --launcher <dir of launcher pngs> --status <dir of status pngs> --out UI/android-assets

Standard library only, like purrstrap.
"""
import argparse
import importlib.util
import os
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location(
    "uiassets", os.path.join(HERE, "..", "..", "purrstrap", "scripts", "uiassets.py"))
sys.path.insert(0, os.path.join(HERE, "..", "..", "purrstrap"))
_ua = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_ua)

# asset name -> (source file in --launcher, size drawn at)
LAUNCHER = {
    "fr_phone": ("phone.png", 48), "fr_contacts": ("contacts.png", 48),
    "fr_messaging": ("messaging-drawable-mdpi.png", 48), "fr_browser": ("browser.png", 48),
    "fr_camera": ("camera.png", 48), "fr_gallery": ("gallery-drawable-mdpi.png", 48),
    "fr_music": ("music.png", 48), "fr_clock": ("clock.png", 48), "fr_settings": ("settings.png", 48),
    "fr_video": ("video.png", 48), "fr_recorder": ("recorder.png", 48),
    "fr_app": ("L2_ic_launcher_application.png", 48), "fr_folder": ("L2_ic_launcher_folder.png", 48),
    # the dock (hotseat) art, 40 px
    "fr_dock_phone": ("L2_hotseat_phone_normal.png", 40), "fr_dock_browser": ("L2_hotseat_browser_normal.png", 40),
    "fr_dock_apps": ("L2_all_apps_button_normal.png", 40), "fr_dock_contacts": ("contacts.png", 40),
    "fr_dock_messaging": ("messaging-drawable-mdpi.png", 40),
    # a small icon for the status strip's notification area, 16 px
    "fr_s_messaging": ("messaging-drawable-mdpi.png", 16), "fr_s_settings": ("settings.png", 16),
    "fr_s_app": ("L2_ic_launcher_application.png", 16),
}

STATUS_SIZE = 19        # the strip is 19 px tall on a 320 x 240 screen; the sources are 25 px (medium density)
STATUS = {}
for _n in (0, 10, 20, 40, 60, 80, 100):
    STATUS[f"st_b{_n}"] = f"battery_{_n}.png"           # the seven levels of stat_sys_battery.xml
for _n in range(6):
    STATUS[f"st_bc{_n}"] = f"battery_charge_anim{_n}.png"
STATUS["st_bunk"] = "battery_unknown.png"
for _n in range(5):
    STATUS[f"st_s{_n}"] = f"signal_{_n}.png"
    STATUS[f"st_w{_n}"] = f"wifi_signal_{_n}.png"
STATUS["st_snull"] = "signal_null.png"


def resample(w, h, rgba, nw, nh):
    """Area average with premultiplied alpha. rgba is w*h*4 bytes."""
    if (w, h) == (nw, nh):
        return bytes(rgba)
    out = bytearray(nw * nh * 4)
    sx, sy = w / nw, h / nh
    for oy in range(nh):
        y0, y1 = oy * sy, (oy + 1) * sy
        for ox in range(nw):
            x0, x1 = ox * sx, (ox + 1) * sx
            r = g = b = a = tot = 0.0
            for y in range(int(y0), min(h, int(y1) + 1)):
                wy = min(y + 1, y1) - max(y, y0)
                if wy <= 0:
                    continue
                for x in range(int(x0), min(w, int(x1) + 1)):
                    wx = min(x + 1, x1) - max(x, x0)
                    if wx <= 0:
                        continue
                    k = wx * wy
                    i = (y * w + x) * 4
                    al = rgba[i + 3] / 255.0
                    r += rgba[i] * al * k
                    g += rgba[i + 1] * al * k
                    b += rgba[i + 2] * al * k
                    a += al * k
                    tot += k
            o = (oy * nw + ox) * 4
            if a > 0:
                out[o] = min(255, round(r / a))
                out[o + 1] = min(255, round(g / a))
                out[o + 2] = min(255, round(b / a))
            out[o + 3] = min(255, round(255 * a / tot)) if tot else 0
    return bytes(out)


def write_png(path, w, h, rgba):
    def chunk(tag, data):
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)
    raw = b"".join(b"\x00" + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))
    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) +
           chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))
    with open(path, "wb") as fh:
        fh.write(png)


def load_rgba(path):
    w, h, ch, px = _ua.decode_png(path)
    if ch == 4:
        return w, h, bytes(px)
    out = bytearray(w * h * 4)
    for i in range(w * h):
        out[i * 4:i * 4 + 3] = px[i * 3:i * 3 + 3]
        out[i * 4 + 3] = 255
    return w, h, bytes(out)


# The three key legends of the key strip (Menu, Home, Back), original art: white strokes with alpha on a 20 x 20 grid, which the shell recolours
# (grey at rest). Each is a list of polylines; the stroke is round-capped and 1.7 units wide, drawn with 4 x 4 supersampling.
GLYPH_SIZE = 20
GLYPH_STROKE = 1.7
GLYPHS = {
    "kb_menu": [[(3.5, 5.5), (16.5, 5.5)], [(3.5, 10), (16.5, 10)], [(3.5, 14.5), (16.5, 14.5)]],      # three lines, the hamburger
    "kb_home": [[(2.8, 10.2), (10, 3.6), (17.2, 10.2)],
                [(5, 9), (5, 16.4), (8.6, 16.4), (8.6, 12.2), (11.4, 12.2), (11.4, 16.4), (15, 16.4), (15, 9)]],
    "kb_back": [[(7.5, 4.5), (3.5, 8.5), (7.5, 12.5)],
                [(3.5, 8.5), (12, 8.5)] + [(12 + 4 * __import__("math").cos(__import__("math").radians(a)),
                                              12.5 + 4 * __import__("math").sin(__import__("math").radians(a)))
                                             for a in range(-90, 91, 15)] + [(12, 16.5), (8.5, 16.5)]],   # a return arrow
}


def _seg_dist(px, py, ax, ay, bx, by):
    dx, dy = bx - ax, by - ay
    n = dx * dx + dy * dy
    t = 0.0 if n == 0 else max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / n))
    cx, cy = ax + t * dx, ay + t * dy
    return ((px - cx) ** 2 + (py - cy) ** 2) ** 0.5


def render_glyph(lines):
    ss = 4
    out = bytearray(GLYPH_SIZE * GLYPH_SIZE * 4)
    segs = [(a, b) for line in lines for a, b in zip(line, line[1:])]
    for y in range(GLYPH_SIZE):
        for x in range(GLYPH_SIZE):
            hit = 0
            for sy in range(ss):
                for sx in range(ss):
                    px, py = x + (sx + 0.5) / ss, y + (sy + 0.5) / ss
                    if any(_seg_dist(px, py, a[0], a[1], b[0], b[1]) <= GLYPH_STROKE / 2 for a, b in segs):
                        hit += 1
            o = (y * GLYPH_SIZE + x) * 4
            out[o:o + 3] = b"\xff\xff\xff"
            out[o + 3] = round(255 * hit / (ss * ss))
    return bytes(out)


def make_glyphs(out_dir):
    for name, lines in GLYPHS.items():
        write_png(os.path.join(out_dir, name + ".png"), GLYPH_SIZE, GLYPH_SIZE, render_glyph(lines))
    return len(GLYPHS)


def make(src_dir, files, out_dir, sized):
    done = 0
    for name, spec in files.items():
        fname, size = spec if sized else (spec, STATUS_SIZE)
        path = os.path.join(src_dir, fname)
        w, h, rgba = load_rgba(path)
        write_png(os.path.join(out_dir, name + ".png"), size, size, resample(w, h, rgba, size, size))
        done += 1
    return done


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--launcher", required=True, help="folder with the Froyo launcher/app icons")
    ap.add_argument("--status", required=True, help="folder with the Froyo stat_sys_* icons (without the prefix)")
    ap.add_argument("--out", required=True, help="output folder (UI/android-assets)")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    n = make(a.launcher, LAUNCHER, a.out, True) + make(a.status, STATUS, a.out, False) + make_glyphs(a.out)
    print(f"{n} icons written to {a.out}")


if __name__ == "__main__":
    main()
