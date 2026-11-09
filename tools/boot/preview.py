#!/usr/bin/env python3
"""Mockups of the boot logo and boot menu, drawn the way the boot package draws: only filled rectangles and the
8x8 font (PurrOS/components/kernel/src/purr_font_data.c), on a 320x240 black screen. Standard library only.

    python3 tools/boot/preview.py            writes out/boot-preview/*.png and boot-screens.png
"""
import os, re, struct, zlib

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
OUT = os.path.join(ROOT, "out", "boot-preview")
W, H, SC = 320, 240, 2

def rgb565(v):
    return ((v >> 11) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31)

BLACK, WHITE = (0, 0, 0), (255, 255, 255)
ORANGE, GREY = rgb565(0xFDA0), rgb565(0x94B2)
DIM = (60, 60, 60)                                  # the empty part of a bar
GREEN = (80, 200, 120)

FONT = []
for m in re.finditer(r"\{(0x[0-9A-F]{2}(?:, 0x[0-9A-F]{2}){7})\}", open(
        os.path.join(ROOT, "PurrOS/components/kernel/src/purr_font_data.c")).read()):
    FONT.append([int(x, 16) for x in m.group(1).split(", ")])

class Screen:
    def __init__(self):
        self.px = [[BLACK] * W for _ in range(H)]
    def fill(self, x, y, w, h, c):
        for yy in range(max(0, y), min(H, y + h)):
            for xx in range(max(0, x), min(W, x + w)):
                self.px[yy][xx] = c
    def text(self, x, y, s, fg, scale=1, bg=None):
        for i, ch in enumerate(s):
            g = FONT[(ord(ch) - 32) if 32 <= ord(ch) < 128 else 31]
            for r in range(8):
                for cidx in range(8):
                    on = g[r] & (0x80 >> cidx)
                    if on or bg is not None:
                        self.fill(x + (i * 8 + cidx) * scale, y + r * scale, scale, scale, fg if on else bg)
    def width(self, s, scale=1):
        return len(s) * 8 * scale
    def center(self, y, s, fg, scale=1, gap=0):
        w = self.width(s, scale) + gap * (len(s) - 1)
        x = (W - w) // 2
        for ch in s:
            self.text(x, y, ch, fg, scale)
            x += 8 * scale + gap

# ---------------------------------------------------------------- the shared top: mark, wordmark, line, chips

def mark(s, cy=44):
    """A cat's eye: a lens of orange with a black vertical slit, built from one rectangle per row."""
    rx, ry = 26, 12
    for dy in range(-ry, ry + 1):
        half = int(rx * (1 - (abs(dy) / ry) ** 1.7) ** 0.6 + 0.5)
        s.fill(W // 2 - half, cy + dy, half * 2, 1, ORANGE)
    for dy in range(-ry + 1, ry):                          # the pupil, narrower toward the ends
        half = 3 if abs(dy) < ry - 5 else (2 if abs(dy) < ry - 2 else 1)
        s.fill(W // 2 - half, cy + dy, half * 2, 1, BLACK)

def header(s, dev=False):
    mark(s)
    s.center(70, "PURR OS", WHITE, 2, gap=5)
    s.center(94, "BlackDeck+   1.0.0", GREY, 1)

def chip(s, x, y, label, kind="ok"):
    """A status bit: 12 px tall, 1 px border. kind: ok (grey, with a dot), warn (orange), hot (filled orange)."""
    w = s.width(label) + 14 + (8 if kind == "ok" else 0)
    col = ORANGE if kind in ("warn", "hot") else GREY
    if kind == "hot":
        s.fill(x, y, w, 13, ORANGE)
        s.text(x + 7, y + 3, label, BLACK)
        return w
    s.fill(x, y, w, 1, col); s.fill(x, y + 12, w, 1, col)
    s.fill(x, y, 1, 13, col); s.fill(x + w - 1, y, 1, 13, col)
    tx = x + 7
    if kind == "ok":
        s.fill(x + 5, y + 5, 3, 3, GREEN)
        tx += 8
    s.text(tx, y + 3, label, WHITE if kind == "ok" else ORANGE)
    return w

def chips(s, items):
    total = sum(s.width(l) + 14 + (8 if k == "ok" else 0) for l, k in items) + 6 * (len(items) - 1)
    x = (W - total) // 2
    for l, k in items:
        x += chip(s, x, 112, l, k) + 6

# ---------------------------------------------------------------- the bottom band

BAR_X, BAR_W, BAR_Y = 40, 240, 206                      # the bar is in the same place on every screen

def bar(s, frac, color=ORANGE):
    s.fill(BAR_X, BAR_Y, BAR_W, 3, DIM)
    s.fill(BAR_X, BAR_Y, int(BAR_W * frac), 3, color)

def splash(stage, detail, frac, items, right=""):
    s = Screen(); header(s); chips(s, items)
    s.text(BAR_X, 188, stage, WHITE)
    if right:
        s.text(BAR_X + BAR_W - s.width(right), 188, right, GREY)
    bar(s, frac)
    s.text(BAR_X, 214, detail, GREY)
    return s

def menu(entries, sel, secs_frac, secs, items):
    s = Screen(); header(s); chips(s, items)
    for i, (name, tag) in enumerate(entries):
        y = 138 + i * 18
        if i == sel:                                     # a border, not a fill: reads on the two-tone e-ink panel
            s.fill(BAR_X - 6, y - 3, BAR_W + 12, 1, ORANGE); s.fill(BAR_X - 6, y + 13, BAR_W + 12, 1, ORANGE)
            s.fill(BAR_X - 6, y - 3, 1, 17, ORANGE); s.fill(BAR_X + BAR_W + 5, y - 3, 1, 17, ORANGE)
        s.text(BAR_X + 4, y, name, WHITE, 1)
        s.text(BAR_X + BAR_W - s.width(tag) - 4, y, tag, GREY if tag != "default" else ORANGE)
    bar(s, secs_frac)
    s.text(BAR_X, 214, "Booting in %d...  any key stops" % secs if secs else "W up   S down   D select", GREY)
    return s

# ---------------------------------------------------------------- png

def png(path, rows, w, h):
    raw = b"".join(b"\x00" + b"".join(bytes(p) for p in r) for r in rows)
    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                           chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))

def scaled(s):
    return [[p for p in row for _ in range(SC)] for row in s.px for _ in range(SC)]

SHOTS = [
    ("1-verify", "Boot logo: verifying",
     splash("Verifying kernel", "slot 1 of 3   signature + hash", 0.62,
            [("ENFORCE", "ok"), ("SIGNED", "ok"), ("HW SB off", "ok")], "62%")),
    ("2-reinstall", "Boot logo: re-install progress",
     splash("Reinstalling kernel", "12.4 / 32.0 KB   internet", 0.38,
            [("RECOVERY", "warn"), ("WIFI ok", "ok"), ("SIGNED", "ok")], "38%")),
    ("3-menu", "Boot menu: same top, entries in the bottom band",
     menu([("kernel", "default"), ("loader", ""), ("recovery", "")], 0, 0.66, 2,
          [("ENFORCE", "ok"), ("SIGNED", "ok"), ("kbd ok", "ok")])),
    ("4-dev", "Boot logo with development mode (chip cannot be missed)",
     splash("Starting PURR OS", "kernel verified", 0.92,
            [("DEV MODE", "hot"), ("UNSIGNED OK", "warn"), ("kbd ok", "ok")], "92%")),
]

if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    cap_h, gap = 22, 10
    cw, ch = W * SC, H * SC
    mw, mh = 2 * cw + 3 * gap, 2 * (ch + cap_h) + 3 * gap
    mont = Screen.__new__(Screen)
    mont.px = [[(24, 24, 24)] * mw for _ in range(mh)]
    for n, (slug, caption, s) in enumerate(SHOTS):
        png(os.path.join(OUT, slug + ".png"), scaled(s), cw, ch)
        ox = gap + (n % 2) * (cw + gap)
        oy = gap + (n // 2) * (ch + cap_h + gap) + cap_h
        for y, row in enumerate(scaled(s)):
            mont.px[oy + y][ox:ox + cw] = row
        for i, chh in enumerate(caption):                    # caption in the 1x font
            g = FONT[ord(chh) - 32]
            for r in range(8):
                for c in range(8):
                    if g[r] & (0x80 >> c):
                        mont.px[oy - cap_h + 8 + r][ox + i * 8 + c] = (230, 230, 230)
    png(os.path.join(OUT, "boot-screens.png"), mont.px, mw, mh)
    print("wrote", OUT)
