#!/usr/bin/env python3
"""Builds the REAL LVGL, the real W10M theme and widgets and the OOBE pages for the host, and renders every page to PNG.
    python3 tools/lvgl_host/render.py        -> out/lvgl_host/*.png and oobe-real.png   (needs gcc; first build takes ~1 min)
"""
import glob, os, subprocess, sys
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools", "boot"))
import preview as pv

LVGL = os.path.join(ROOT, "PurrOS", "managed_components", "lvgl__lvgl")
MAIN = os.path.join(ROOT, "PurrOS", "main")
OUT = os.path.join(ROOT, "out", "lvgl_host")
OBJ = os.path.join(OUT, "obj")
os.makedirs(OBJ, exist_ok=True)

INC = ["-I", HERE, "-I", os.path.join(HERE, "stubs"), "-I", LVGL, "-I", os.path.join(LVGL, "src"), "-I", MAIN,
       "-I", os.path.join(ROOT, "PurrOS", "components", "coreos", "include")]
DEFS = ["-DLV_CONF_INCLUDE_SIMPLE", "-DLEGAL_PATH=\"%s\"" % os.path.join(ROOT, "OTA", "legal.txt")]

lv_src = [f for f in glob.glob(os.path.join(LVGL, "src", "**", "*.c"), recursive=True)
          if "/src/drivers/" not in f and "/src/libs/" not in f and "/src/osal/" not in f]
for extra in ("osal/lv_os_none.c", "osal/lv_os.c", "libs/bin_decoder/lv_bin_decoder.c"):
    lv_src.append(os.path.join(LVGL, "src", extra))
# The firmware sources are compiled from copies, so that their quoted includes (commands.h, purr_ui_service.h...) are found
# in stubs/ first and not in main/ next to the original file.
import shutil
SRCCOPY = os.path.join(OUT, "src")
os.makedirs(SRCCOPY, exist_ok=True)
mine = [os.path.join(HERE, "host_render.c"), os.path.join(HERE, "stubs.c"),
        os.path.join(ROOT, "PurrOS", "components", "coreos", "src", "purr_reason.c")]
for n in ("purr_oobe.c", "purr_w10m_theme.c", "purr_w10m_widgets.c"):
    shutil.copyfile(os.path.join(MAIN, n), os.path.join(SRCCOPY, n))
    mine.append(os.path.join(SRCCOPY, n))

def build(src):
    obj = os.path.join(OBJ, src.replace("/", "_") + ".o")
    if os.path.exists(obj) and os.path.getmtime(obj) >= os.path.getmtime(src) and src not in mine:
        return obj, None
    flags = ["-std=gnu11", "-O1", "-w"] if src not in mine else ["-std=gnu11", "-O1", "-Wall", "-Wno-unused-function"]
    r = subprocess.run(["gcc"] + flags + DEFS + INC + ["-c", src, "-o", obj], capture_output=True, text=True)
    return obj, (r.stdout + r.stderr) if r.returncode else None

with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as ex:
    results = list(ex.map(build, lv_src + mine))
bad = [(o, e) for o, e in results if e]
if bad:
    for o, e in bad[:3]:
        print(o, "\n", e[:3000])
    sys.exit("build failed")
exe = os.path.join(OUT, "host_render")
r = subprocess.run(["gcc", "-o", exe] + [o for o, _ in results] + ["-lm"], capture_output=True, text=True)
if r.returncode:
    sys.exit(r.stderr[:3000])
subprocess.run([exe, os.path.join(OUT, "")], check=True)

def ppm(path):
    data = open(path, "rb").read()
    head, rest = data.split(b"\n255\n", 1)
    w, h = map(int, head.split(b"\n")[1].split())
    return w, h, [[tuple(rest[(y * w + x) * 3:(y * w + x) * 3 + 3]) for x in range(w)] for y in range(h)]

names = [("loader-dev-0", "D1  RECOVERY, dev server allowed: looking"), ("loader-dev-1", "D2  verified, ready to use it"), ("loader-first", "0  RECOVERY: first run, nothing installed"), ("loader-0", "1  RECOVERY: what went wrong (from the bootloader)"), ("loader-1", "2  RECOVERY: Wi-Fi"),
         ("loader-2", "3  RECOVERY: the legal stuff & cool people (a text file)"), ("loader-3", "4  RECOVERY: restoring"),
         ("first-0", "5  FIRST BOOT: pick a colour"), ("first-1", "6  FIRST BOOT: account"),
         ("first-2", "7  FIRST BOOT: date and time"), ("first-3", "8  FIRST BOOT: done")]
SC, gap, cap_h = 2, 10, 22
shots = []
for slug, cap in names:
    w, h, px = ppm(os.path.join(OUT, slug + ".ppm"))
    scaled = [[p for p in row for _ in range(SC)] for row in px for _ in range(SC)]
    pv.png(os.path.join(OUT, slug + ".png"), scaled, w * SC, h * SC)
    shots.append((cap, scaled, w * SC, h * SC))
cw, ch = shots[0][2], shots[0][3]
rows = (len(shots) + 1) // 2
mw, mh = 2 * cw + 3 * gap, rows * (ch + cap_h) + (rows + 1) * gap
mont = [[(24, 24, 24)] * mw for _ in range(mh)]
for n, (cap, sc, _, _) in enumerate(shots):
    ox, oy = gap + (n % 2) * (cw + gap), gap + (n // 2) * (ch + cap_h + gap) + cap_h
    for y, row in enumerate(sc):
        mont[oy + y][ox:ox + cw] = row
    for i, chh in enumerate(cap):
        g = pv.FONT[ord(chh) - 32]
        for r in range(8):
            for c in range(8):
                if g[r] & (0x80 >> c):
                    mont[oy - cap_h + 8 + r][ox + i * 8 + c] = (230, 230, 230)
pv.png(os.path.join(OUT, "oobe-real.png"), mont, mw, mh)
print("wrote", os.path.join(OUT, "oobe-real.png"))
