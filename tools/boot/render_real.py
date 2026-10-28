#!/usr/bin/env python3
"""Builds tools/boot/host_render.c against the REAL boot package sources and renders the screens to
out/boot-preview/real-*.png and real-screens.png, so the code can be compared with the mockup (preview.py).

    python3 tools/boot/render_real.py [board]        (default tdeck_plus; run `purrstrap bootpkg build` for it first)
"""
import os, subprocess, sys
sys.path.insert(0, os.path.dirname(__file__))
import preview as pv

ROOT = pv.ROOT
board = sys.argv[1] if len(sys.argv) > 1 else "tdeck_plus"
bdir = os.path.join(ROOT, "bootpkg", "build", board)          # holds the generated board.h
out = os.path.join(ROOT, "out", "boot-preview")
exe = os.path.join(out, "host_render")
os.makedirs(out, exist_ok=True)
src = [os.path.join(ROOT, "tools/boot/host_render.c"), os.path.join(ROOT, "bootpkg/src/pkg_main.c"),
       os.path.join(ROOT, "bootpkg/src/pkg_ui.c"), os.path.join(ROOT, "PurrOS/components/coreos/src/purr_menu.c"),
       os.path.join(ROOT, "PurrOS/components/kernel/src/purr_font_data.c")]
cmd = ["gcc", "-std=gnu11", "-O1", "-Wall", "-DPKG_VERSION=\"0.1.0\"", "-I", bdir, "-I", os.path.join(ROOT, "bootpkg/src"),
       "-I", os.path.join(ROOT, "PurrOS/components/coreos/include"),
       "-I", os.path.join(ROOT, "PurrOS/components/kernel/include")] + src + ["-o", exe]
r = subprocess.run(cmd, capture_output=True, text=True)
if r.returncode:
    sys.exit(r.stdout + r.stderr)

def ppm(path):
    data = open(path, "rb").read()
    head, rest = data.split(b"\n255\n", 1)
    w, h = map(int, head.split(b"\n")[1].split())
    return w, h, [[tuple(rest[(y * w + x) * 3:(y * w + x) * 3 + 3]) for x in range(w)] for y in range(h)]

shots = []
for scenario, files, caption in [
        ("verify", ["verify"], "Real code: verifying"),
        ("reinstall", ["reinstall"], "Real code: re-install progress"),
        ("menu", ["menu-t20", "handoff"], "Real code: menu mid-countdown / hand-off"),
        ("menukey", ["menu-t12"], "Real code: after S (selection moved, countdown stopped)"),
        ("dev", ["menu-t20"], "Real code: development mode")]:
    subprocess.run([exe, scenario, os.path.join(out, scenario + "-")], check=True)
    for f in files:
        w, h, px = ppm(os.path.join(out, scenario + "-" + f + ".ppm"))
        shots.append((scenario + "-" + f, caption if f != "handoff" else "Real code: hand-off screen", px, w, h))

SC = 2
cols, gap, cap_h = 2, 10, 22
w, h = shots[0][3] * SC, shots[0][4] * SC
rows = (len(shots) + cols - 1) // cols
mw, mh = cols * w + (cols + 1) * gap, rows * (h + cap_h) + (rows + 1) * gap
mont = [[(24, 24, 24)] * mw for _ in range(mh)]
for n, (slug, cap, px, sw, sh) in enumerate(shots):
    scaled = [[p for p in row for _ in range(SC)] for row in px for _ in range(SC)]
    pv.png(os.path.join(out, "real-" + slug + ".png"), scaled, sw * SC, sh * SC)
    ox, oy = gap + (n % cols) * (w + gap), gap + (n // cols) * (h + cap_h + gap) + cap_h
    for y, row in enumerate(scaled):
        mont[oy + y][ox:ox + w] = row
    for i, ch in enumerate(cap):
        g = pv.FONT[ord(ch) - 32]
        for r in range(8):
            for c in range(8):
                if g[r] & (0x80 >> c):
                    mont[oy - cap_h + 8 + r][ox + i * 8 + c] = (230, 230, 230)
pv.png(os.path.join(out, "real-screens.png"), mont, mw, mh)
print("wrote", os.path.join(out, "real-screens.png"))
