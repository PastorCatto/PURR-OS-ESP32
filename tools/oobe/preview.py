#!/usr/bin/env python3
"""Mockup of the first-run setup (OOBE) built from the Settings app's pages and widgets: header, accent titles, drop-downs,
switches, an input field, accent swatches, a bottom bar with Back / Next. 320x240, drawn with the boot package's pixel font
(the real thing uses Montserrat). Standard library only.
    python3 tools/oobe/preview.py     -> out/oobe-preview/oobe-flow.png
"""
import os, sys
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "boot"))
import preview as pv
from preview import Screen, WHITE, GREY, BLACK, W, H, FONT

ACC = (0x00, 0x50, 0xEF)                     # Cobalt, the shell's default accent
PANEL = (28, 28, 28)                         # a raised block (PURR_TH_SURFACE)
EDGE = (110, 110, 110)
DIMT = (150, 150, 150)

def disc(s, cx, cy, r, col):
    for dy in range(-r, r + 1):
        half = int((r * r - dy * dy) ** 0.5)
        s.fill(cx - half, cy + dy, half * 2, 1, col)

def box(s, x, y, w, h, edge=EDGE, fill=None):
    if fill: s.fill(x, y, w, h, fill)
    s.fill(x, y, w, 1, edge); s.fill(x, y + h - 1, w, 1, edge); s.fill(x, y, 1, h, edge); s.fill(x + w - 1, y, 1, h, edge)

def header(s, title, step, total):
    s.text(10, 8, title, WHITE, 2)
    t = "%d/%d" % (step, total)
    s.text(W - 10 - s.width(t), 12, t, DIMT)
    s.fill(0, 29, W, 1, (50, 50, 50))

def label(s, y, text, dim=False):
    s.text(10, y, text, DIMT if dim else WHITE)

def dropdown(s, y, text, w=190):
    box(s, 10, y, w, 24); s.text(18, y + 8, text, WHITE); s.text(10 + w - 16, y + 8, "v", DIMT)

def switch(s, y, text, on):
    s.text(10, y + 7, text, WHITE)
    x = W - 10 - 44
    s.fill(x, y + 2, 44, 22, ACC if on else (60, 60, 60))
    disc(s, x + (33 if on else 11), y + 13, 8, WHITE)

def field(s, y, placeholder, value="", secret=False, w=300):
    box(s, 10, y, w, 26, (170, 170, 170), (24, 24, 24))
    if value:
        s.text(18, y + 9, "*" * len(value) if secret else value, WHITE)
    else:
        s.text(18, y + 9, placeholder, (120, 120, 120))

def bottom(s, step, total, back=True, nxt="Next", enabled=True, skip=False):
    s.fill(0, 204, W, 36, (14, 14, 14)); s.fill(0, 204, W, 1, (50, 50, 50))
    if back:
        box(s, 10, 212, 84, 22); s.center_text = None; s.text(10 + (84 - s.width("Back")) // 2, 219, "Back", WHITE)
    for i in range(total):
        s.fill(W // 2 - total * 6 + i * 12, 221, 6, 6, ACC if i == step - 1 else (70, 70, 70))
    bw = 84
    if skip:
        box(s, W - 10 - bw, 212, bw, 22); s.text(W - 10 - bw + (bw - s.width("Skip")) // 2, 219, "Skip", WHITE)
    else:
        s.fill(W - 10 - bw, 212, bw, 22, ACC if enabled else (45, 45, 45))
        s.text(W - 10 - bw + (bw - s.width(nxt)) // 2, 219, nxt, WHITE if enabled else (110, 110, 110))

N = 4
frames = []

def mark(s, cx, cy, col=ACC):
    for dy in range(-12, 13):
        half = int(26 * (1 - (abs(dy) / 12) ** 1.7) ** 0.6 + 0.5)
        s.fill(cx - half, cy + dy, half * 2, 1, col)
    for dy in range(-11, 12):
        h = 3 if abs(dy) < 7 else 2 if abs(dy) < 10 else 1
        s.fill(cx - h, cy + dy, h * 2, 1, BLACK)

def bg_progress(s, y, text, frac):
    """The install that carries on in the background while the user answers questions."""
    s.fill(0, y, W, 1, (50, 50, 50))
    s.text(10, y + 6, text, DIMT)
    s.fill(10, y + 20, 300, 2, (50, 50, 50)); s.fill(10, y + 20, int(300 * frac), 2, ACC)

# ---------------- part 1: the internet recovery loader (no filesystem: it only needs the network and the flash)
s = Screen(); mark(s, W // 2, 56)
s.center(86, "Internet recovery", WHITE, 2)
s.center(110, "Restores PURR OS from the internet.", DIMT)
s.center(124, "Your files are not touched.", DIMT)
s.fill(40, 150, 240, 26, ACC); s.text(40 + (240 - s.width("Restore PURR OS")) // 2, 159, "Restore PURR OS", WHITE)
box(s, 40, 182, 240, 22); s.text(40 + (240 - s.width("Full restore (every part)")) // 2, 189, "Full restore (every part)", WHITE)
s.text(10, 222, "S: network shell", DIMT)
frames.append(("1  LOADER: what to restore (today: press S or F within 3 s)", s))

s = Screen(); header(s, "Network", 1, N)
box(s, 10, 38, 300, 30, ACC, PANEL); s.text(18, 44, "HomeWifi", WHITE); s.text(18, 56, "remembered network", DIMT)
s.fill(236, 44, 66, 18, ACC); s.text(236 + (66 - s.width("Connect")) // 2, 49, "Connect", WHITE)
for i, (n, sig) in enumerate([("Workshop", 2), ("Guest", 1)]):
    y = 76 + i * 28
    box(s, 10, y, 300, 24, (60, 60, 60), PANEL); s.text(18, y + 8, n, WHITE)
    for bb in range(3):
        s.fill(266 + bb * 10, y + 18 - (bb + 1) * 5, 6, (bb + 1) * 5, WHITE if bb < sig else (70, 70, 70))
field(s, 138, "Password", "", True, 214)
s.fill(230, 138, 80, 26, (45, 45, 45)); s.text(230 + (80 - s.width("Connect")) // 2, 147, "Connect", (110, 110, 110))
label(s, 172, "Wired networks skip this step.", True)
bottom(s, 1, N, back=True, nxt="Next", enabled=False)
frames.append(("2  LOADER: Wi-Fi (the network is saved for the next boot)", s))

# the restore screen is the boot look (mark, stage line, bar, chips) so it reads as one process with the boot screens
s = Screen(); mark(s, W // 2, 48, (0xF0, 0xA3, 0x0A))
s.center(74, "PURR OS", WHITE, 2, gap=5); s.center(98, "BlackDeck+   recovery", DIMT)
for i, (n, st, col) in enumerate([("Kernel", "done", (80, 200, 120)), ("Boot package", "already good", DIMT),
                                   ("KittenOS", "62%", WHITE)]):
    y = 124 + i * 16
    s.text(40, y, n, WHITE); s.text(280 - s.width(st), y, st, col)
s.text(40, 178, "Downloading KittenOS", WHITE); s.text(280 - s.width("62%"), 178, "62%", DIMT)
s.fill(40, 194, 240, 3, (50, 50, 50)); s.fill(40, 194, 148, 3, (0xF0, 0xA3, 0x0A))
for i, (l, k) in enumerate([("RECOVERY", 1), ("WIFI ok", 0), ("SIGNED", 0)]):
    pass
x = 56
for l, k in [("RECOVERY", "warn"), ("WIFI ok", "ok"), ("SIGNED", "ok")]:
    w = s.width(l) + 14 + (8 if k == "ok" else 0)
    box(s, x, 208, w, 13, (0xF0, 0xA3, 0x0A) if k == "warn" else GREY)
    if k == "ok": s.fill(x + 5, 213, 3, 3, (80, 200, 120))
    s.text(x + 7 + (8 if k == "ok" else 0), 211, l, (0xF0, 0xA3, 0x0A) if k == "warn" else WHITE)
    x += w + 6
frames.append(("3  LOADER: restoring (the screen already built), then it restarts", s))

# ---------------- part 2: KittenOS first boot (it has a filesystem): the rest installs while the user answers
s = Screen(); header(s, "Your account", 1, N)
field(s, 40, "User name", "catto", False, 300)
field(s, 72, "Password", "mypassword", True, 300)
field(s, 104, "Confirm password", "mypassword", True, 300)
s.text(10, 138, "[ok] The passwords match", (80, 200, 120))
label(s, 154, "This is the administrator account.", True)
bg_progress(s, 176, "Installing the system...", 0.28)
bottom(s, 1, N, back=False)
frames.append(("4  KITTENOS first boot: account (replaces the text setup)", s))

s = Screen(); header(s, "Make it yours", 2, N)
label(s, 38, "Time zone", True); dropdown(s, 50, "Pacific Time (US)", 220)
switch(s, 80, "24-hour clock", True)
cols = [(0xA4, 0xC4, 0x00), (0x00, 0xBA, 0x00), (0x00, 0xAB, 0xA9), (0x1B, 0xA1, 0xE2), ACC, (0x6A, 0x00, 0xFF),
        (0xD8, 0x00, 0x73), (0xE5, 0x14, 0x00)]
label(s, 112, "Accent colour", True)
for i, c in enumerate(cols):
    x = 10 + i * 38
    s.fill(x, 126, 32, 24, c)
    if i == 4: box(s, x - 2, 124, 36, 28, WHITE)
bg_progress(s, 176, "Installing the system...", 0.52)
bottom(s, 2, N)
frames.append(("5  KITTENOS: time zone and look (Settings pages, same widgets)", s))

s = Screen(); header(s, "Security", 3, N)
switch(s, 40, "Only run signed software", True)
label(s, 68, "Blocks images not signed by a trusted key.", True)
s.fill(10, 88, 300, 1, (50, 50, 50))
switch(s, 96, "Developer mode", False)
label(s, 124, "For building apps. Needs a restart.", True)
bg_progress(s, 176, "Installing the system...", 0.74)
bottom(s, 3, N)
frames.append(("6  KITTENOS: security (Settings > Privacy & security)", s))

s = Screen(); header(s, "All set", 4, N)
for i, (k, v) in enumerate([("Account", "catto"), ("Wi-Fi", "HomeWifi"), ("Time", "Pacific, 24-hour"), ("Look", "Cobalt"),
                            ("Policy", "signed only")]):
    s.text(10, 40 + i * 18, k, DIMT); s.text(110, 40 + i * 18, v, WHITE)
bg_progress(s, 150, "System installed", 1.0)
s.fill(40, 184, 240, 22, ACC); s.text(40 + (240 - s.width("Start PURR OS")) // 2, 191, "Start PURR OS", WHITE)
label(s, 214, "It restarts, then you sign in.", True)
frames.append(("7  KITTENOS: done, restart into PURR OS", s))

if __name__ == "__main__":
    out = os.path.join(pv.ROOT, "out", "oobe-preview"); os.makedirs(out, exist_ok=True)
    SC, gap, cap_h = 2, 10, 22
    cw, ch = W * SC, H * SC
    rows_n = (len(frames) + 1) // 2
    mw, mh = 2 * cw + 3 * gap, rows_n * (ch + cap_h) + (rows_n + 1) * gap
    mont = [[(24, 24, 24)] * mw for _ in range(mh)]
    for n, (cap, s) in enumerate(frames):
        sc = pv.scaled(s)
        ox, oy = gap + (n % 2) * (cw + gap), gap + (n // 2) * (ch + cap_h + gap) + cap_h
        for y, row in enumerate(sc):
            mont[oy + y][ox:ox + cw] = row
        for i, chh in enumerate(cap[: cw // 8]):
            g = FONT[ord(chh) - 32]
            for r in range(8):
                for c in range(8):
                    if g[r] & (0x80 >> c):
                        mont[oy - cap_h + 8 + r][ox + i * 8 + c] = (230, 230, 230)
    pv.png(os.path.join(out, "oobe-flow.png"), mont, mw, mh)
    print("wrote", os.path.join(out, "oobe-flow.png"))
