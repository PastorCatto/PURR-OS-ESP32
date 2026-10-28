#!/usr/bin/env python3
"""Mockup of a Lumia / Windows Phone style theme for the boot screens: lowercase type, flat accent tiles, five marching
dots for progress, a thin line for the countdown. Drawn with the boot package's own font. Run:
    python3 tools/boot/preview_lumia.py        -> out/boot-preview/lumia-screens.png
"""
import os, sys
sys.path.insert(0, os.path.dirname(__file__))
import preview as pv
from preview import Screen, BLACK, WHITE, GREY, W, H

ACCENT = (0, 120, 215)          # the shell's default accent (a Windows blue); the real screens would take the OS accent
DARK = (0, 84, 150)             # a darker tile
AMBER = (255, 180, 0)

def strip(s, right):
    s.text(8, 3, "purr os", GREY)
    s.text(W - 8 - s.width(right), 3, right, GREY)

def mark(s, cx, cy, col):
    for dy in range(-12, 13):
        half = int(26 * (1 - (abs(dy) / 12) ** 1.7) ** 0.6 + 0.5)
        s.fill(cx - half, cy + dy, half * 2, 1, col)
    for dy in range(-11, 12):
        s.fill(cx - (3 if abs(dy) < 7 else 2 if abs(dy) < 10 else 1), cy + dy,
               (3 if abs(dy) < 7 else 2 if abs(dy) < 10 else 1) * 2, 1, BLACK)

def dots(s, y, phase):
    """Five squares that bunch toward the middle and spread to the ends, like the Windows Phone progress dots."""
    xs = [[44, 96, 150, 204, 262], [62, 116, 156, 196, 246], [90, 138, 160, 182, 230]][phase]
    for i, x in enumerate(xs):
        s.fill(x, y, 5, 5, WHITE)

def boot(stage, pct, chipline, phase=1):
    s = Screen()
    strip(s, chipline)
    mark(s, W // 2, 82, ACCENT)
    dots(s, 140, phase)
    s.center(176, stage, WHITE, 1)
    if pct:
        s.center(192, pct, GREY, 1)
    return s

def tile(s, x, y, w, h, label, tag, sel, col):
    s.fill(x, y, w, h, col)
    if sel:
        for t in (0, 1):
            s.fill(x + t, y + t, w - 2 * t, 1, WHITE); s.fill(x + t, y + h - 1 - t, w - 2 * t, 1, WHITE)
            s.fill(x + t, y + t, 1, h - 2 * t, WHITE); s.fill(x + w - 1 - t, y + t, 1, h - 2 * t, WHITE)
    s.text(x + 8, y + h - 24, label, WHITE, 2)
    if tag:
        s.text(x + w - 8 - s.width(tag), y + 8, tag, WHITE)

def menu(sel, secs, frac, chipline, dev=False):
    s = Screen()
    strip(s, chipline)
    if dev:
        s.fill(0, 14, W, 14, AMBER); s.center(17, "developer mode", BLACK, 1)
    s.text(10, 30 if dev else 18, "boot", WHITE, 3)
    y0 = 58 if dev else 50
    tile(s, 10, y0, 300, 58, "kernel", "default", sel == 0, ACCENT)
    tile(s, 10, y0 + 66, 146, 58, "kittenos", "", sel == 1, DARK)
    tile(s, 164, y0 + 66, 146, 58, "recovery", "", sel == 2, DARK)
    s.text(10, 198, ("starting kernel in %d" % secs) if secs else "w up   s down   d select", GREY)
    s.fill(0, 232, W, 2, pv.DIM)                       # a thin line along the bottom edge, draining
    s.fill(0, 232, int(W * frac), 2, ACCENT)
    return s

SHOTS = [("Lumia theme: verifying", boot("verifying kernel", "62%", "enforce  signed")),
         ("Lumia theme: re-install progress", boot("reinstalling kernel", "12.4 of 32.0 kb  38%", "recovery  wifi", 2)),
         ("Lumia theme: boot menu", menu(0, 2, 0.66, "enforce  signed")),
         ("Lumia theme: menu, development mode", menu(1, 0, 0.0, "enforce", dev=True))]

if __name__ == "__main__":
    out = pv.OUT
    os.makedirs(out, exist_ok=True)
    SC, gap, cap_h = 2, 10, 22
    cw, ch = W * SC, H * SC
    mw, mh = 2 * cw + 3 * gap, 2 * (ch + cap_h) + 3 * gap
    mont = [[(24, 24, 24)] * mw for _ in range(mh)]
    for n, (cap, s) in enumerate(SHOTS):
        sc = pv.scaled(s)
        ox, oy = gap + (n % 2) * (cw + gap), gap + (n // 2) * (ch + cap_h + gap) + cap_h
        for y, row in enumerate(sc):
            mont[oy + y][ox:ox + cw] = row
        for i, chh in enumerate(cap):
            g = pv.FONT[ord(chh) - 32]
            for r in range(8):
                for c in range(8):
                    if g[r] & (0x80 >> c):
                        mont[oy - cap_h + 8 + r][ox + i * 8 + c] = (230, 230, 230)
    pv.png(os.path.join(out, "lumia-screens.png"), mont, mw, mh)
    print("wrote", os.path.join(out, "lumia-screens.png"))
