"""The UI maker: make a theme (a UI picture pack) from one predefined file, `ui.toml`, adapted to the device and the shell.

A theme is `theme.conf` plus `pack.bin`, and a pack holds the pictures a shell asks for BY NAME at fixed sizes (UI/PACK-NAMES.md, purrsdk/ui.py REQUIRED and
FALLBACK). Drawing sixty small pictures by hand is the part nobody wants, so this module writes them from a few choices (accent, background, foreground, style)
and lets anything you have drawn win. Where each picture comes from, best first:

    [pictures] in ui.toml     name = "path"            a picture you point at
    images/<name>.png         a file named like the picture
    imported/<name>.png       what import_squareline() brought in from a SquareLine Studio export
    generated                 drawn from the look in ui.toml (flat glyphs, anti-aliased, no other tools)

A picture that is not the size the shell draws it at is resampled to that size (the device never scales: UI/PACK-NAMES.md), and the report says which. The
`scale` in ui.toml multiplies every size, for a shell that reads sizes from a theme; today's two shells draw at fixed sizes, so it stays 1.0 for them.

SquareLine Studio is the LVGL designer whose export people already have: import_squareline() takes the export folder (or a .zip of it, or the project folder),
reads the project's source PNGs (assets/) and the exported image arrays (ui_img_*_png.c, LVGL 8 and 9, 16-bit), and writes them as PNG files with a guess at
which slot each one is for. Nothing is invented: a picture that matches no slot is listed, and you can point a slot at it in ui.toml.
"""

import math
import os
import re
import shutil
import struct
import tempfile
import zipfile
import zlib

from . import minitoml, ui, uibuilder

DEVICE_SIZES = {"tdeck_plus": (320, 240), "tdeck": (320, 240), "waveshare154": (200, 200)}

# ---------------------------------------------------------------------------------------------------------------- PNG in and out


def write_png(path, w, h, rgba):
    """An RGBA PNG (8 bits a channel). `rgba` is w*h*4 bytes."""
    raw = bytearray()
    stride = w * 4
    for y in range(h):
        raw.append(0)
        raw += rgba[y * stride:(y + 1) * stride]

    def chunk(tag, body):
        return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "wb") as fh:
        fh.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))


def read_png(path):
    """(w, h, rgba bytes) of a PNG the packer can read (8-bit grey, grey+alpha, RGB or RGBA, not interlaced)."""
    w, h, ch, px = ui._u().decode_png(path)
    px = bytes(px)
    if ch == 4:
        return w, h, px
    out = bytearray(w * h * 4)
    for i in range(w * h):
        if ch == 3:
            out[i * 4:i * 4 + 3] = px[i * 3:i * 3 + 3]
            out[i * 4 + 3] = 255
        elif ch == 2:
            out[i * 4:i * 4 + 3] = bytes([px[i * 2]]) * 3
            out[i * 4 + 3] = px[i * 2 + 1]
        else:
            out[i * 4:i * 4 + 3] = bytes([px[i]]) * 3
            out[i * 4 + 3] = 255
    return w, h, bytes(out)


def resize(w, h, rgba, nw, nh):
    """Area/bilinear resample of premultiplied RGBA. Good enough for icons and wallpapers; pure Python, so it is not for photographs."""
    if (w, h) == (nw, nh):
        return bytes(rgba)
    out = bytearray(nw * nh * 4)
    for y in range(nh):
        sy0, sy1 = y * h / nh, (y + 1) * h / nh
        for x in range(nw):
            sx0, sx1 = x * w / nw, (x + 1) * w / nw
            r = g = b = a = tot = 0.0
            for yy in range(int(sy0), min(h, int(math.ceil(sy1)))):
                wy = min(yy + 1, sy1) - max(yy, sy0)
                for xx in range(int(sx0), min(w, int(math.ceil(sx1)))):
                    wt = wy * (min(xx + 1, sx1) - max(xx, sx0))
                    i = (yy * w + xx) * 4
                    al = rgba[i + 3] / 255.0
                    r += rgba[i] * al * wt
                    g += rgba[i + 1] * al * wt
                    b += rgba[i + 2] * al * wt
                    a += al * wt
                    tot += wt
            j = (y * nw + x) * 4
            if a > 0 and tot > 0:
                out[j] = min(255, int(r / a + .5))
                out[j + 1] = min(255, int(g / a + .5))
                out[j + 2] = min(255, int(b / a + .5))
                out[j + 3] = min(255, int(a / tot * 255 + .5))
    return bytes(out)


# ---------------------------------------------------------------------------------------------------------------- drawing

def hexcolour(s, default=(255, 255, 255)):
    m = re.match(r"^#?([0-9A-Fa-f]{6})$", str(s or ""))
    if not m:
        return default
    v = int(m.group(1), 16)
    return (v >> 16 & 255, v >> 8 & 255, v & 255)


def shade(c, k):
    return tuple(max(0, min(255, int(x * k))) for x in c)


def rect(x0, y0, x1, y1):
    return lambda x, y: x0 <= x <= x1 and y0 <= y <= y1


def rrect(x0, y0, x1, y1, r):
    def f(x, y):
        if not (x0 <= x <= x1 and y0 <= y <= y1):
            return False
        cx = min(max(x, x0 + r), x1 - r)
        cy = min(max(y, y0 + r), y1 - r)
        return (x - cx) ** 2 + (y - cy) ** 2 <= r * r
    return f


def circle(cx, cy, r):
    return lambda x, y: (x - cx) ** 2 + (y - cy) ** 2 <= r * r


def ring(cx, cy, r, t):
    return lambda x, y: (r - t) ** 2 <= (x - cx) ** 2 + (y - cy) ** 2 <= r * r


def seg(x0, y0, x1, y1, t):
    dx, dy = x1 - x0, y1 - y0
    L2 = dx * dx + dy * dy or 1e-9

    def f(x, y):
        u = max(0.0, min(1.0, ((x - x0) * dx + (y - y0) * dy) / L2))
        return (x - (x0 + u * dx)) ** 2 + (y - (y0 + u * dy)) ** 2 <= (t / 2) ** 2
    return f


def tri(a, b, c):
    def sign(p, q, r):
        return (p[0] - r[0]) * (q[1] - r[1]) - (q[0] - r[0]) * (p[1] - r[1])

    def f(x, y):
        p = (x, y)
        d1, d2, d3 = sign(p, a, b), sign(p, b, c), sign(p, c, a)
        return not ((d1 < 0 or d2 < 0 or d3 < 0) and (d1 > 0 or d2 > 0 or d3 > 0))
    return f


def arc(cx, cy, r, t, a0, a1):
    """Part of a ring between two angles in degrees, 0 = up, clockwise."""
    ring_ = ring(cx, cy, r, t)

    def f(x, y):
        if not ring_(x, y):
            return False
        ang = (math.degrees(math.atan2(x - cx, -(y - cy)))) % 360
        return a0 % 360 <= ang <= a1 % 360 if a0 % 360 <= a1 % 360 else (ang >= a0 % 360 or ang <= a1 % 360)
    return f


def frame(x0, y0, x1, y1, t):
    return [rect(x0, y0, x1, y0 + t), rect(x0, y1 - t, x1, y1), rect(x0, y0, x0 + t, y1), rect(x1 - t, y0, x1, y1)]


def render(w, h, layers, ss=None):
    """layers = [(predicate in unit coordinates, (r, g, b[, a]))] drawn in order; anti-aliased by supersampling. Returns w*h*4 RGBA bytes."""
    ss = ss or (3 if max(w, h) <= 32 else 2)
    out = bytearray(w * h * 4)
    n = ss * ss
    cols = [(p, (c + (255,))[:4]) for p, c in layers]
    for py in range(h):
        for px in range(w):
            ar = ag = ab = aa = 0.0
            for sy in range(ss):
                y = (py + (sy + .5) / ss) / h
                for sx in range(ss):
                    x = (px + (sx + .5) / ss) / w
                    r = g = b = a = 0.0
                    for pred, c in cols:
                        if pred(x, y):
                            ca = c[3] / 255.0
                            r, g, b = c[0] * ca + r * (1 - ca), c[1] * ca + g * (1 - ca), c[2] * ca + b * (1 - ca)
                            a = ca + a * (1 - ca)
                    ar += r
                    ag += g
                    ab += b
                    aa += a
            i = (py * w + px) * 4
            if aa > 0:
                out[i] = min(255, int(ar / aa + .5))
                out[i + 1] = min(255, int(ag / aa + .5))
                out[i + 2] = min(255, int(ab / aa + .5))
                out[i + 3] = min(255, int(aa / n * 255 + .5))
    return bytes(out)


# glyphs: functions of a colour that return layers in unit coordinates
def g_grid(c):
    return [(rect(a, b, a + .26, b + .26), c) for a in (.2, .54) for b in (.2, .54)]


def g_gear(c):
    return [(ring(.5, .5, .27, .12), c)] + [(seg(.5 + .22 * math.cos(k * math.pi / 4), .5 + .22 * math.sin(k * math.pi / 4), .5 + .38 * math.cos(k * math.pi / 4), .5 + .38 * math.sin(k * math.pi / 4), .13), c) for k in range(8)]


def g_folder(c):
    return [(rrect(.12, .26, .5, .4, .04), c), (rrect(.12, .34, .88, .78, .06), c)]


def g_gallery(c):
    return [(frame(.12, .2, .88, .8, .06)[i], c) for i in range(4)] + [(tri((.2, .72), (.42, .4), (.62, .72)), c), (tri((.45, .72), (.64, .5), (.8, .72)), c), (circle(.7, .36, .07), c)]


def g_chat(c):
    return [(rrect(.14, .2, .86, .66, .1), c), (tri((.3, .6), (.3, .86), (.5, .62)), c)]


def g_person(c):
    return [(circle(.5, .34, .15), c), (rrect(.2, .56, .8, .9, .26), c)]


def g_globe(c):
    return [(ring(.5, .5, .36, .07), c), (seg(.5, .14, .5, .86, .06), c), (seg(.14, .5, .86, .5, .06), c), (ring(.5, .5, .17, .05), c)]


def g_clock(c):
    return [(ring(.5, .5, .36, .08), c), (seg(.5, .5, .5, .27, .08), c), (seg(.5, .5, .68, .58, .08), c)]


def g_lock(c):
    return [(arc(.5, .42, .2, .08, 270, 90), c), (seg(.3, .42, .3, .5, .08), c), (seg(.7, .42, .7, .5, .08), c), (rrect(.22, .48, .78, .88, .08), c)]


def g_info(c):
    return [(ring(.5, .5, .38, .07), c), (circle(.5, .3, .05), c), (seg(.5, .45, .5, .72, .09), c)]


def g_monitor(c):
    return [(r, c) for r in frame(.1, .2, .9, .68, .07)] + [(seg(.5, .68, .5, .82, .08), c), (seg(.3, .84, .7, .84, .08), c)]


def g_phone(c):
    return [(r, c) for r in frame(.3, .1, .7, .9, .07)] + [(circle(.5, .8, .03), c)]


def g_brush(c):
    return [(seg(.25, .75, .65, .35, .14), c), (circle(.75, .25, .12), c), (circle(.25, .78, .08), c)]


def g_sun(c):
    return [(circle(.5, .5, .17), c)] + [(seg(.5 + .27 * math.cos(k * math.pi / 4), .5 + .27 * math.sin(k * math.pi / 4), .5 + .38 * math.cos(k * math.pi / 4), .5 + .38 * math.sin(k * math.pi / 4), .07), c) for k in range(8)]


def g_plane(c):
    return [(seg(.12, .5, .88, .5, .1), c), (tri((.45, .5), (.62, .12), (.72, .5)), c), (tri((.45, .5), (.62, .88), (.72, .5)), c), (tri((.12, .5), (.12, .3), (.3, .5)), c)]


def g_bars(c, dim, n, total=4):
    out = []
    for i in range(total):
        x0 = .1 + i * (.8 / total)
        out.append((rect(x0, .9 - (i + 1) * (.7 / total), x0 + .8 / total * .72, .9), c if i < n else dim))
    return out


def g_wifi(c, dim, n):
    out = [(circle(.5, .8, .07), c if n >= 1 else dim)]
    for k, r in enumerate((.24, .42, .6)):
        out.append((arc(.5, .8, r, .09, 305, 55), c if n >= k + 2 else dim))
    return out


def g_bt(c):
    pts = [(.34, .3), (.66, .7), (.5, .86), (.5, .14), (.66, .3), (.34, .7)]
    return [(seg(pts[i][0], pts[i][1], pts[i + 1][0], pts[i + 1][1], .08), c) for i in range(5)]


def g_battery(c, dim, fill, bolt=None):
    out = [(r, c) for r in frame(.06, .28, .86, .72, .07)] + [(rect(.86, .42, .95, .58), c)]
    if fill > 0:
        out.append((rect(.14, .36, .14 + .64 * fill, .64), bolt or c))
    return out


def g_pin(c):
    return [(circle(.5, .38, .22), c), (tri((.3, .5), (.7, .5), (.5, .92)), c)]


def g_key(c):
    return [(ring(.32, .5, .2, .09), c), (seg(.5, .5, .9, .5, .09), c), (seg(.78, .5, .78, .68, .09), c)]


def g_hotspot(c):
    return [(circle(.5, .5, .09), c), (arc(.5, .5, .26, .08, 300, 60), c), (arc(.5, .5, .26, .08, 240, 120), c), (arc(.5, .5, .42, .08, 300, 60), c), (arc(.5, .5, .42, .08, 120, 240), c)]


def g_menu(c):
    return [(seg(.2, y, .8, y, .1), c) for y in (.28, .5, .72)]


def g_home(c):
    return [(tri((.12, .5), (.5, .14), (.88, .5)), c), (rect(.24, .48, .76, .86), c)]


def g_back(c):
    return [(tri((.1, .5), (.5, .14), (.5, .86)), c), (seg(.4, .5, .88, .5, .14), c)]


def g_note(c):
    return [(circle(.34, .76, .13), c), (seg(.45, .76, .45, .2, .08), c), (seg(.45, .2, .78, .32, .09), c)]


def g_video(c):
    return [(rrect(.1, .26, .68, .74, .06), c), (tri((.72, .5), (.92, .32), (.92, .68)), c)]


def g_camera(c):
    return [(rrect(.1, .28, .9, .78, .08), c), (rect(.34, .2, .56, .3), c)]


def g_rec(c):
    return [(circle(.5, .5, .16), c), (ring(.5, .5, .38, .07), c)]


def g_handset(c):
    return [(seg(.3, .72, .7, .28, .22), c), (circle(.28, .74, .12), c), (circle(.72, .26, .12), c)]


def g_dots(c):
    return [(circle(a, b, .075), c) for a in (.22, .5, .78) for b in (.22, .5, .78)]


def g_snull(c, dim):
    return g_bars(dim, dim, 0) + [(seg(.15, .85, .85, .15, .1), c)]


def g_bunk(c, dim):
    return g_battery(c, dim, 0) + [(seg(.35, .62, .65, .38, .09), c)]


# ---------------------------------------------------------------------------------------------------------------- the look

class Look:
    def __init__(self, accent="#60a917", background="#101214", foreground="#f2f2f2", dark=True, style="filled"):
        self.accent = hexcolour(accent, (96, 169, 23))
        self.bg = hexcolour(background, (16, 18, 20))
        self.fg = hexcolour(foreground, (242, 242, 242))
        self.dark = bool(dark)
        self.style = style if style in ("filled", "outline") else "filled"
        self.dim = tuple(int(self.fg[i] * .32 + self.bg[i] * .68) for i in range(3))
        self.tile = shade(self.accent, .78)


# which glyph each picture name is drawn with: (kind, glyph) where kind says how it sits (tile = white on an accent tile, plain = in the foreground colour)
APP_GLYPH = {"app": g_grid, "settings": g_gear, "folder": g_folder, "gallery": g_gallery, "messaging": g_chat, "phone": g_handset, "contacts": g_person,
             "browser": g_globe, "camera": g_camera, "music": g_note, "clock": g_clock, "video": g_video, "recorder": g_rec}
WP_GLYPH = {"settings": g_gear, "system": g_monitor, "devices": g_phone, "network": g_globe, "personalization": g_brush, "apps": g_grid, "account": g_person,
            "time": g_clock, "privacy": g_lock, "about": g_info}
IC_GLYPH = {"brightness": g_sun, "settings": g_gear, "airplane": g_plane, "bluetooth": g_bt, "location": g_pin, "vpn": g_key, "hotspot": g_hotspot}


def layers_for(name, look):
    """The layers of the generated picture `name`, or None when this module has no drawing for it (the caller then draws a plain mark)."""
    fg, dim, acc, white = look.fg, look.dim, look.accent, (255, 255, 255)
    m = re.match(r"^st_b(\d+)$", name)
    if m:
        return g_battery(fg, dim, int(m.group(1)) / 100.0)
    m = re.match(r"^st_bc(\d)$", name)
    if m:
        return g_battery(fg, dim, (int(m.group(1)) + 1) / 6.0, bolt=acc)
    if name == "st_bunk":
        return g_bunk(fg, dim)
    m = re.match(r"^st_s(\d)$", name)
    if m:
        return g_bars(fg, dim, int(m.group(1)))
    if name == "st_snull":
        return g_snull(fg, dim)
    m = re.match(r"^st_w(\d)$", name)
    if m:
        return g_wifi(fg, dim, int(m.group(1)))
    if name in ("wifi_icon", "wifi_ic"):
        return g_wifi(fg, dim, 4)
    if name in ("cellular_ic",):
        return g_bars(fg, dim, 4)
    if name == "battery_ic":
        return g_battery(fg, dim, .7)
    m = re.match(r"^(\w+)_ic$", name)
    if m and m.group(1) in IC_GLYPH:
        return [(p, c) for p, c in IC_GLYPH[m.group(1)](fg)]
    m = re.match(r"^wp_(\w+)$", name)
    if m and m.group(1) in WP_GLYPH:
        return WP_GLYPH[m.group(1)](fg)
    if name == "fr_dock_apps":
        return g_dots(white)
    for key, fn in (("kb_menu", g_menu), ("kb_home", g_home), ("kb_back", g_back)):
        if name == key:
            return fn(white)
    m = re.match(r"^fr_(?:s_|dock_)?(\w+)$", name)
    if m and m.group(1) in APP_GLYPH:
        inner = APP_GLYPH[m.group(1)](white)
        base = [(rrect(.04, .04, .96, .96, .2), look.tile), (rrect(.04, .04, .96, .5, .2), shade(look.tile, 1.12))]
        return base + inner
    return None


def wallpaper(w, h, look, variant=0):
    """A vertical gradient from the background to a darkened accent with two soft discs, drawn pixel by pixel."""
    top, bot = look.bg, tuple(int(look.bg[i] * .45 + look.accent[i] * .55) for i in range(3))
    discs = [(.8, .22 + .05 * variant, .55), (.2, .72, .42)]
    out = bytearray(w * h * 4)
    for y in range(h):
        t = y / max(1, h - 1)
        base = [top[i] * (1 - t) + bot[i] * t for i in range(3)]
        for x in range(w):
            col = list(base)
            for cx, cy, r in discs:
                d = math.hypot((x / w - cx) * (w / h), y / h - cy)
                if d < r * (w / h) * 0.9:
                    k = 0.10 * (1 - d / (r * (w / h) * 0.9))
                    for i in range(3):
                        col[i] += (look.accent[i] - col[i]) * k
            i = (y * w + x) * 4
            out[i], out[i + 1], out[i + 2], out[i + 3] = int(col[0]), int(col[1]), int(col[2]), 255
    return bytes(out)


def draw(name, w, h, look):
    """RGBA bytes for the generated picture `name` at w x h."""
    if re.match(r"^img\d+$", name):
        return wallpaper(w, h, look, int(name[3:]))
    layers = layers_for(name, look)
    if layers is None:
        layers = [(rrect(.1, .1, .9, .9, .2), look.tile), (ring(.5, .5, .24, .09), (255, 255, 255))]
    return render(w, h, layers)


# ---------------------------------------------------------------------------------------------------------------- the template

TEMPLATE = """# ui.toml: the one file that describes a UI. Everything not drawn by you is drawn from [look].
# Run `purr ui make` (or press build in Studio) to turn it into theme.conf + pack.bin for the device.

[ui]
name = "%(name)s"
shell = "%(shell)s"            # android or w10m: which shell this UI is for
board = "%(board)s"            # the device it is for; its screen decides the wallpaper shape
author = "Your Name"
licence = "CC0-1.0"            # the licence of your pictures (the Store shows it before installing)
version = "0.1.0"

[look]
accent = "%(accent)s"
background = "#101214"
foreground = "#f2f2f2"
dark = true                    # the base the shell uses for lists and dialogs
style = "filled"               # filled or outline (outline is drawn the same for now)

[adapt]
scale = 1.0                    # multiplies every picture size; today's shells draw at fixed sizes, so keep 1.0
wallpapers = 1                 # how many wallpapers to draw (img0, img1, ...)

[pictures]
# Point a slot at a picture of your own (a PNG path, relative to this file). Yours always wins over the drawn one.
# fr_app = "imported/home.png"
"""


def starter(name, shell="android", board="tdeck_plus", accent=None):
    return TEMPLATE % {"name": name, "shell": shell, "board": board, "accent": accent or ("#60a917" if shell == "android" else "#0050ef")}


def load_template(text_or_path):
    """Read ui.toml (a path, or the text) and fill in the defaults. Returns (config, problems)."""
    if "\n" not in text_or_path and os.path.isfile(text_or_path):
        with open(text_or_path, encoding="utf-8") as fh:
            text_or_path = fh.read()
    problems = []
    try:
        d = minitoml.loads(text_or_path)
    except Exception as e:                                          # a TomlError names the line
        return None, ["ui.toml does not read: %s" % e]
    u, lk, ad = d.get("ui", {}), d.get("look", {}), d.get("adapt", {})
    cfg = {
        "name": str(u.get("name", "")), "shell": str(u.get("shell", "")), "board": str(u.get("board", "tdeck_plus")), "author": str(u.get("author", "")),
        "licence": str(u.get("licence", "")), "version": str(u.get("version", "0.1.0")),
        "look": Look(lk.get("accent", "#60a917"), lk.get("background", "#101214"), lk.get("foreground", "#f2f2f2"), lk.get("dark", True), lk.get("style", "filled")),
        "accent_text": str(lk.get("accent", "#60a917")),
        "scale": float(ad.get("scale", 1.0)), "wallpapers": int(ad.get("wallpapers", 1)), "pictures": dict(d.get("pictures", {})),
    }
    ic = d.get("icons", {})
    cfg["icons"] = {"use": bool(ic.get("use", True)), "pack": str(ic.get("pack", "")), "fill_missing": bool(ic.get("fill_missing", True)), "resize": bool(ic.get("resize", True)), "recolour": bool(ic.get("recolour", False))}
    cfg["size"] = DEVICE_SIZES.get(cfg["board"]) or (int(u.get("width", 320)), int(u.get("height", 240)))
    try:
        cfg["elements"] = uibuilder.elements_from(d)
    except ValueError as e:
        cfg["elements"] = []
        problems.append(str(e))
    if not ui.THEME_ID_RE.match(cfg["name"]):
        problems.append("[ui] name needs 1 to 39 letters, digits, underscores or dashes: it becomes the folder /ui/themes/<name> on the device.")
    if cfg["shell"] not in ui.SHELLS:
        problems.append("[ui] shell must be android or w10m (it decides which pictures are drawn).")
    if not re.match(r"^#[0-9A-Fa-f]{6}$", cfg["accent_text"]):
        problems.append("[look] accent must be a colour like #60a917.")
    if not 0.25 <= cfg["scale"] <= 4:
        problems.append("[adapt] scale must be between 0.25 and 4.")
    if not 0 <= cfg["wallpapers"] <= 10:
        problems.append("[adapt] wallpapers is 0 to 10.")
    return cfg, problems


# ---------------------------------------------------------------------------------------------------------------- SquareLine Studio

_IMG_ARRAY = re.compile(r"uint8_t\s+(ui_img_\w+?)(?:_data)?\s*\[\s*\]\s*=\s*\{(.*?)\};", re.S)
_DSC = re.compile(r"\.header\.(w|h|cf)\s*=\s*([\w]+)")
_V8_SECTION = re.compile(r"#if\s+LV_COLOR_DEPTH\s*==\s*16\s*&&\s*LV_COLOR_16_SWAP\s*==\s*0(.*?)(?:#endif|#if\s)", re.S)


def _bytes_of(body):
    return bytes(int(b, 16) for b in re.findall(r"0[xX][0-9A-Fa-f]{1,2}", body))


def parse_squareline_c(text):
    """Read one exported ui_img_*_png.c. Returns (id, w, h, rgba) or raises ValueError. 16-bit colour only (RGB565, with alpha), LVGL 8 and 9."""
    arr = _IMG_ARRAY.search(text)
    if arr is None:
        raise ValueError("no image array in it")
    fields = dict(_DSC.findall(text))
    if "w" not in fields or "h" not in fields:
        raise ValueError("no size in it")
    w, h, cf = int(fields["w"]), int(fields["h"]), fields.get("cf", "")
    body = arr.group(2)
    sec = _V8_SECTION.search(body)
    if sec is not None:
        body = sec.group(1)                                      # LVGL 8 keeps one block of bytes per colour depth; take 16-bit, not swapped
    data = _bytes_of(body)
    n = w * h
    out = bytearray(n * 4)
    has_alpha = "ALPHA" in cf or "A8" in cf
    if len(data) == n * 2 and not has_alpha:
        for i in range(n):
            v = data[i * 2] | data[i * 2 + 1] << 8
            r, g, b = v >> 11 & 31, v >> 5 & 63, v & 31
            out[i * 4:i * 4 + 4] = bytes([r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2, 255])
    elif len(data) == n * 3 and has_alpha:
        planar = "COLOR_FORMAT" in cf                           # LVGL 9: a colour plane then an alpha plane; LVGL 8: three bytes a pixel
        for i in range(n):
            if planar:
                v, a = data[i * 2] | data[i * 2 + 1] << 8, data[n * 2 + i]
            else:
                v, a = data[i * 3] | data[i * 3 + 1] << 8, data[i * 3 + 2]
            r, g, b = v >> 11 & 31, v >> 5 & 63, v & 31
            out[i * 4:i * 4 + 4] = bytes([r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2, a])
    else:
        raise ValueError("colour format %s with %d bytes for %dx%d is not 16-bit colour (RGB565, with or without alpha)" % (cf or "?", len(data), w, h))
    ident = re.sub(r"^ui_img_", "", arr.group(1))
    ident = re.sub(r"_png$", "", ident)
    return ident, w, h, bytes(out)


def _norm(s):
    s = re.sub(r"^ui_img_", "", s.lower())
    s = re.sub(r"(_png|\.png)$", "", s)
    return re.sub(r"[^a-z0-9_]+", "_", s).strip("_")


def guess_slot(name, shell):
    """Which slot a picture is probably for: its normalised name equals a slot name (case, dashes and spaces ignored). None when nothing matches."""
    want = {_norm(k): k for k in ui.known(shell)}
    return want.get(_norm(name))


def _extract(src, tmp):
    if os.path.isdir(src):
        return src
    if zipfile.is_zipfile(src):
        with zipfile.ZipFile(src) as z:
            for n in z.namelist():
                if n.startswith(("/", "\\")) or ".." in n.replace("\\", "/").split("/"):
                    raise ValueError("The zip holds a path that leaves its folder (%s): I will not unpack it." % n)
            z.extractall(tmp)
        return tmp
    raise ValueError("%s is neither a folder nor a zip." % src)


def import_squareline(src, dest, shell="android", on_line=print, any_png=False):
    """Bring a SquareLine Studio export into `dest/imported/` as PNG files. `src` is the export folder, the project folder, or a .zip of either.
    Returns {"pictures": [{"id", "w", "h", "slot", "file", "from"}], "skipped": [...]}; the slot is a guess by name and may be None."""
    tmp = tempfile.mkdtemp(prefix="purr-sqline-")
    try:
        root = _extract(src, tmp)
        out_dir = os.path.join(dest, "imported")
        os.makedirs(out_dir, exist_ok=True)
        pics, skipped, seen = [], [], set()

        def add(ident, w, h, rgba, origin):
            nid = re.sub(r"[^A-Za-z0-9_.-]+", "_", ident)[:23] or "picture"
            base, k = nid, 2
            while nid in seen:
                nid = (base[:20] + "_%d" % k)[:23]
                k += 1
            seen.add(nid)
            write_png(os.path.join(out_dir, nid + ".png"), w, h, rgba)
            pics.append({"id": nid, "w": w, "h": h, "slot": guess_slot(nid, shell), "file": "imported/%s.png" % nid, "from": origin})

        pngs, arrays = [], []
        for dp, dns, fns in os.walk(root):
            dns[:] = sorted(d for d in dns if not d.startswith(".") and d not in ("node_modules", "__MACOSX"))
            for f in sorted(fns):
                full = os.path.join(dp, f)
                rel = os.path.relpath(full, root).replace(os.sep, "/")
                if f.lower().endswith(".png") and (any_png or rel.startswith("assets/") or "/assets/" in rel):
                    pngs.append((full, rel, f))
                elif f.startswith("ui_img_") and f.endswith("_png.c"):
                    arrays.append((full, rel, f))
        for full, rel, f in pngs:                                        # the project's own PNGs first: they are lossless
            try:
                w, h, rgba = read_png(full)
                add(os.path.splitext(f)[0], w, h, rgba, rel)
            except ValueError as e:
                skipped.append("%s: %s" % (rel, e))
        for full, rel, f in arrays:                                      # then the exported arrays, for pictures the PNGs did not cover
            try:
                with open(full, encoding="utf-8", errors="replace") as fh:
                    ident, w, h, rgba = parse_squareline_c(fh.read())
                if _norm(ident) in {_norm(p["id"]) for p in pics}:
                    continue
                add(ident, w, h, rgba, rel)
            except ValueError as e:
                skipped.append("%s: %s" % (rel, e))
        if not pics:
            raise ValueError("I found no pictures in %s. A SquareLine export has ui_img_*_png.c files; a project has an assets/ folder of PNGs." % src)
        on_line("Imported %d picture(s): %d match a slot by name." % (len(pics), sum(1 for p in pics if p["slot"])))
        return {"pictures": pics, "skipped": skipped}
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


# ---------------------------------------------------------------------------------------------------------------- compose and pack

def _candidates(project, cfg, name):
    """The files a slot may take its picture from, best first: a [pictures] line, images/<name>.png, then the imported pack (unless the pack is switched off, which
    also switches off any [pictures] line that points into imported/)."""
    use = cfg["icons"]["use"]
    mapped = cfg["pictures"].get(name)
    out = []
    if mapped and (use or not mapped.replace("\\", "/").startswith("imported/")):
        out.append(os.path.join(project, mapped))
    out.append(os.path.join(project, "images", name + ".png"))
    if use:
        out.append(os.path.join(project, "imported", name + ".png"))
    return out


def wanted(cfg):
    """{name: (w, h)} for this shell and scale: every required picture, the fallback ones except wallpapers beyond the asked number."""
    out = {}
    for name, (w, h) in ui.known(cfg["shell"]).items():
        m = re.match(r"^img(\d+)$", name)
        if m and int(m.group(1)) >= cfg["wallpapers"]:
            continue
        out[name] = (max(4, round(w * cfg["scale"])), max(4, round(h * cfg["scale"])))
    return out


def compose(project, on_line=print):
    """Turn <project>/ui.toml into the finished theme under <project>/build/<name>/ (theme.conf and pack.bin), and check it.
    Returns {"ok", "out", "pack", "count", "generated", "yours", "resized", "problems", "checks"}."""
    cfg, problems = load_template(os.path.join(project, "ui.toml"))
    if problems:
        return {"ok": False, "problems": problems}
    names = wanted(cfg)
    src_dir = os.path.join(project, "build", "pack-src")
    shutil.rmtree(src_dir, ignore_errors=True)
    os.makedirs(src_dir)
    generated, yours, resized, notes, missing = [], [], [], [], []
    for name, (w, h) in names.items():
        path = None
        mapped = cfg["pictures"].get(name)
        for cand in _candidates(project, cfg, name):
            if os.path.isfile(cand):
                path = cand
                break
        if mapped and path is None:
            notes.append("%s points at %s, which is not there: it is drawn instead." % (name, mapped))
        if path:
            try:
                pw, ph, rgba = read_png(path)
            except ValueError as e:
                notes.append("%s: %s: drawn instead." % (os.path.basename(path), e))
                path = None
        if path:
            if (pw, ph) != (w, h):
                if not cfg["icons"]["resize"]:
                    notes.append("%s is %dx%d and the slot is %dx%d: left as it is (resize is off)." % (name, pw, ph, w, h))
                    w, h = pw, ph
                else:
                    rgba = resize(pw, ph, rgba, w, h)
                    resized.append("%s %dx%d -> %dx%d" % (name, pw, ph, w, h))
            if cfg["icons"]["recolour"] and not re.match(r"^img\d+$", name):
                rgba = tint(rgba, cfg["look"].accent)
            yours.append(name)
        elif not cfg["icons"]["fill_missing"] and cfg["icons"]["pack"] and not re.match(r"^img\d+$", name):
            missing.append(name)
            continue
        else:
            rgba = draw(name, w, h, cfg["look"])
            generated.append(name)
        write_png(os.path.join(src_dir, name + ".png"), w, h, rgba)
    if missing:
        return {"ok": False, "problems": ["Your icon pack has no picture for %d slot(s) and 'use Studio's icon for anything missing' is off: %s." % (len(missing), ", ".join(missing[:8]) + (", ..." if len(missing) > 8 else ""))]}
    # pictures of yours that are not slots at all (a theme may carry extras the shell's own layout names) are kept as they are
    for extra_dir in ("images",):
        d = os.path.join(project, extra_dir)
        if os.path.isdir(d):
            for f in sorted(os.listdir(d)):
                if f.lower().endswith(".png") and os.path.splitext(f)[0] not in names:
                    shutil.copyfile(os.path.join(d, f), os.path.join(src_dir, f))
                    yours.append(os.path.splitext(f)[0])
    out = os.path.join(project, "build", cfg["name"])
    os.makedirs(out, exist_ok=True)
    try:
        r = ui.pack_folder(src_dir, os.path.join(out, "pack.bin"))
    except ValueError as e:
        return {"ok": False, "problems": ["The pack did not build: %s" % e]}
    acc = cfg["accent_text"]
    conf = "name=%s\nauthor=%s\nlicence=%s\nversion=%s\nshell=%s\naccent=%s\ndark=%d\n" % (cfg["name"], cfg["author"], cfg["licence"], cfg["version"], cfg["shell"], acc, 1 if cfg["look"].dark else 0)
    with open(os.path.join(out, "theme.conf"), "w", encoding="utf-8", newline="\n") as fh:
        fh.write(conf)
    screens_out, layout_notes, layout_problems = [], [], []
    if cfg["elements"]:
        names_set = set(names)
        shutil.rmtree(os.path.join(out, "screens"), ignore_errors=True)
        for k, scr in enumerate(uibuilder.screens(cfg["elements"])):
            text, nts, probs = uibuilder.to_screen_conf(cfg["elements"], scr, cfg["size"], names_set)
            try:
                uibuilder.parse_screen_conf(text)
            except ValueError as e:
                probs.append("The screen file for %r does not read back: %s" % (scr, e))
            layout_notes += ["[%s] %s" % (scr, n) for n in nts]
            layout_problems += probs
            os.makedirs(os.path.join(out, "screens"), exist_ok=True)
            with open(os.path.join(out, "screens", scr + ".conf"), "w", encoding="utf-8", newline="\n") as fh:
                fh.write(text)
            if k == 0:
                with open(os.path.join(out, "shell.conf"), "w", encoding="utf-8", newline="\n") as fh:
                    fh.write(text)
            screens_out.append(scr)
    rep = ui.check_theme_folder(out, cfg["shell"])
    checks = rep.get("checks", []) if isinstance(rep, dict) else []
    bad = [c for c in checks if c.get("status") == "bad"]
    on_line("Packed %d picture(s), %s bytes: %d drawn, %d yours, %d resized." % (r["count"], format(r["bytes"], ","), len(generated), len(yours), len(resized)))
    return {"ok": not bad and not layout_problems, "out": out, "pack": r["path"], "count": r["count"], "bytes": r["bytes"], "generated": generated, "yours": yours, "resized": resized,
            "problems": [c.get("text", "") for c in bad] + layout_problems, "notes": notes + layout_notes, "checks": checks, "screens": screens_out}


def apply_import(text, info):
    """ui.toml text with a [pictures] line for every imported picture whose name matches a slot (so it is used), and none for the rest."""
    for p in info["pictures"]:
        if p["slot"]:
            text = set_mapping(text, p["slot"], p["file"])
    return text


def new_project(ws, name, shell="android", board="tdeck_plus", squareline=None, on_line=print, start="android"):
    """Start a UI project in the workspace (a theme project with a ui.toml and a starting layout: blank, android or metro); with `squareline` (a path) the export's
    pictures are imported and matched to slots."""
    from . import projects
    projects.new(ws, "theme", name)
    path = os.path.join(ws.projects, name)
    text = starter(name, shell, board)
    info = None
    if squareline:
        info = import_squareline(squareline, path, shell, on_line)
        text = apply_import(text, info)
        text = set_icons(text, pack=os.path.basename(squareline))
    size = DEVICE_SIZES.get(board, (320, 240))
    text = save_elements(text, uibuilder.starter_layout("metro" if start == "metro" else "blank" if start == "blank" else "android", size))
    with open(os.path.join(path, "ui.toml"), "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)
    return {"path": path, "import": info}


def set_mapping(text, slot, file, old_slot=None):
    """ui.toml text with `slot = "file"` set under [pictures] (or the line for `slot` removed when file is None). `old_slot` is removed first, so a picture
    that was assigned to one slot and is now assigned to another does not stay in both."""
    if file is not None and not re.match(r"^[\w./-]+$", file):
        raise ValueError("That is not a plain relative path.")
    if slot and not re.match(r"^[A-Za-z0-9_.-]{1,23}$", slot):
        raise ValueError("That is not a picture name.")
    lines = text.split("\n")
    if "[pictures]" not in [l.strip() for l in lines]:
        lines += ["", "[pictures]"]
    out = []
    in_pic, done = False, False
    for l in lines:
        st = l.strip()
        if st.startswith("["):
            if in_pic and not done and file is not None and slot:
                out.append('%s = "%s"' % (slot, file))
                done = True
            in_pic = st == "[pictures]"
            out.append(l)
            continue
        if in_pic:
            key = re.match(r"^\s*([A-Za-z0-9_.-]+)\s*=", l)
            if key and key.group(1) in (slot, old_slot):
                if key.group(1) == slot and file is not None and not done:
                    out.append('%s = "%s"' % (slot, file))
                    done = True
                continue
        out.append(l)
    if in_pic and not done and file is not None and slot:
        while out and out[-1] == "":
            out.pop()
        out.append('%s = "%s"' % (slot, file))
    return "\n".join(out).rstrip("\n") + "\n"


def tint(rgba, colour):
    """The picture with every pixel's colour set to `colour` and its own transparency kept: a one-colour icon recoloured to the accent."""
    out = bytearray(rgba)
    for i in range(0, len(out), 4):
        out[i], out[i + 1], out[i + 2] = colour
    return bytes(out)


def _pack_to_pngs(path, out_dir, seen, add):
    """Decode the pictures of a pack.bin (RGB565, or RGB565 + an alpha plane) into PNGs through add()."""
    ua = ui._u()
    with open(path, "rb") as fh:
        data = fh.read()
    entries, problems, facts = ui.read_pack(data)
    if problems:
        raise ValueError("%s is not a usable pack: %s" % (os.path.basename(path), problems[0]))
    base = struct.calcsize(ua.HEADER_FMT) + facts["count"] * ua.ENTRY_SIZE
    for e in entries:
        w, h = e["w"], e["h"]
        px = data[base + e["offset"]:base + e["offset"] + e["length"]]
        n = w * h
        rgba = bytearray(n * 4)
        alpha = e["cf"] == ua.CF_RGB565A8 and len(px) >= n * 3
        for i in range(n):
            v = px[i * 2] | px[i * 2 + 1] << 8
            r, g, b = v >> 11 & 31, v >> 5 & 63, v & 31
            rgba[i * 4:i * 4 + 4] = bytes([r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2, px[n * 2 + i] if alpha else 255])
        add(e["name"], w, h, bytes(rgba), os.path.basename(path))


def import_icon_pack(src, dest, shell="android", on_line=print):
    """Take a person's icon pack into `dest/imported/`: a folder or zip of PNG files (anywhere inside), a pack.bin, or a SquareLine Studio export. A picture is
    matched to a slot by name. Returns {"pictures": [...], "skipped": [...]} like import_squareline()."""
    if os.path.isfile(src) and not zipfile.is_zipfile(src):
        out_dir = os.path.join(dest, "imported")
        os.makedirs(out_dir, exist_ok=True)
        pics, seen = [], set()

        def add(ident, w, h, rgba, origin):
            nid = re.sub(r"[^A-Za-z0-9_.-]+", "_", ident)[:23] or "picture"
            if nid in seen:
                return
            seen.add(nid)
            write_png(os.path.join(out_dir, nid + ".png"), w, h, rgba)
            pics.append({"id": nid, "w": w, "h": h, "slot": guess_slot(nid, shell), "file": "imported/%s.png" % nid, "from": origin})
        if src.lower().endswith(".png"):
            w, h, rgba = read_png(src)
            add(os.path.splitext(os.path.basename(src))[0], w, h, rgba, os.path.basename(src))
        else:
            _pack_to_pngs(src, out_dir, seen, add)
        if not pics:
            raise ValueError("I found no pictures in %s." % src)
        on_line("Imported %d picture(s): %d match a slot by name." % (len(pics), sum(1 for p in pics if p["slot"])))
        return {"pictures": pics, "skipped": []}
    return import_squareline(src, dest, shell, on_line, any_png=True)


def icon_report(project):
    """How the project's pictures stand against the shell's slots, for the icon pack page: for each slot, where its picture comes from."""
    cfg, problems = load_template(os.path.join(project, "ui.toml"))
    if cfg is None:
        return {"problems": problems, "slots": []}
    slots = []
    for name, (w, h) in wanted(cfg).items():
        mapped = cfg["pictures"].get(name)
        path = None
        for cand in _candidates(project, cfg, name):
            if os.path.isfile(cand):
                path = cand
                break
        row = {"name": name, "w": w, "h": h, "state": "studio", "from": "", "pw": None, "ph": None}
        if path:
            try:
                pw, ph, _ = read_png(path)
                row.update(state="resized" if (pw, ph) != (w, h) else "yours", **{"from": os.path.relpath(path, project), "pw": pw, "ph": ph})
            except ValueError:
                pass
        slots.append(row)
    return {"slots": slots, "matched": sum(1 for r in slots if r["state"] in ("yours", "resized")), "resized": sum(1 for r in slots if r["state"] == "resized"),
            "fallback": sum(1 for r in slots if r["state"] == "studio"), "options": cfg["icons"], "problems": problems}


def save_elements(text, elements):
    """ui.toml text with its [[element]] blocks replaced by `elements` (they are always the last thing in the file)."""
    lines = text.split("\n")
    for i, l in enumerate(lines):
        if l.strip() == "[[element]]":
            lines = lines[:i]
            break
    base = "\n".join(lines).rstrip("\n") + "\n"
    out = [base]
    for e in elements:
        e = uibuilder.element(e)
        out.append("\n[[element]]\n")
        out.append('screen = "%s"\nkind = "%s"\nglyph = "%s"\nname = "%s"\nx = %d\ny = %d\nw = %d\nh = %d\nlabel = "%s"\naction = "%s"\ncolor = "%s"\n' % (
            e["screen"], e["kind"], e["glyph"], e["name"].replace('"', "'"), e["x"], e["y"], e["w"], e["h"], e["label"].replace('"', "'"), e["action"], e["color"]))
    return "".join(out)


def set_icons(text, **opts):
    """ui.toml text with the [icons] section set (pack, fill_missing, resize, recolour). Replaces the whole section; other sections stay as they are."""
    lines = text.split("\n")
    out, skip = [], False
    for l in lines:
        st = l.strip()
        if st == "[icons]":
            skip = True
            continue
        if skip and st.startswith("["):
            skip = False
        if not skip:
            out.append(l)
    body = "\n".join(out).rstrip("\n")
    cur = {"use": True, "pack": "", "fill_missing": True, "resize": True, "recolour": False}
    cur.update(opts)
    sec = '\n\n[icons]\nuse = %s\npack = "%s"\nfill_missing = %s\nresize = %s\nrecolour = %s\n' % (str(bool(cur["use"])).lower(), cur["pack"].replace('"', "'"), str(bool(cur["fill_missing"])).lower(), str(bool(cur["resize"])).lower(), str(bool(cur["recolour"])).lower())
    first_el = body.find("\n[[element]]")
    if first_el >= 0:
        return body[:first_el].rstrip("\n") + sec + body[first_el:] + "\n"
    return body + sec
