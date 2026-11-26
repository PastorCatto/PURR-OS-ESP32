#!/usr/bin/env python3
"""What would a remote display cost on the wire? Uses the real codecs (purr_rdisp.c, through ctypes) on real screenshots.

    python3 tools/rdisp/measure.py shotA.png [shotB.png ...]

Each picture is a 320 x 240 screen. For every picture it prints the cost of sending the whole screen; for every pair of neighbours it prints the cost of the change from one to the next
(16 x 16 tiles that differ go through the damage merger, the splitter and the codec chooser, as a host would). Times are estimates: the link at 1.5 MB/s, the panel's SPI at 40 MHz.
Needs gcc and Pillow."""
import ctypes
import os
import subprocess
import sys
import tempfile

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
CORE = os.path.join(HERE, "..", "..", "Modules", "vdisplay")
W, H, MAX_PX = 320, 240, 4096
HDR, RECT_HEAD = 8, 10


class Rect(ctypes.Structure):
    _fields_ = [("x", ctypes.c_uint16), ("y", ctypes.c_uint16), ("w", ctypes.c_uint16), ("h", ctypes.c_uint16)]


class Damage(ctypes.Structure):
    _fields_ = [("r", Rect * 8), ("n", ctypes.c_int), ("w", ctypes.c_int), ("h", ctypes.c_int)]


def load_lib():
    out = os.path.join(tempfile.mkdtemp(), "libpurr_rdisp.so")
    subprocess.check_call(["gcc", "-O2", "-shared", "-fPIC", "-I", CORE, os.path.join(CORE, "purr_rdisp.c"), "-o", out])
    lib = ctypes.CDLL(out)
    lib.purr_rd_encode_best.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_int, ctypes.c_uint, ctypes.c_char_p, ctypes.c_int, ctypes.POINTER(ctypes.c_uint16), ctypes.POINTER(ctypes.c_uint8)]
    lib.purr_rd_split.argtypes = [Rect, ctypes.c_int, ctypes.POINTER(Rect), ctypes.c_int]
    lib.purr_rd_damage_init.argtypes = [ctypes.POINTER(Damage), ctypes.c_int, ctypes.c_int]
    lib.purr_rd_damage_add.argtypes = [ctypes.POINTER(Damage), ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int]
    return lib


def rgb565(path):
    im = Image.open(path).convert("RGB").resize((W, H))
    px = im.load()
    out = bytearray()
    for y in range(H):
        for x in range(W):
            r, g, b = px[x, y]
            v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
            out += bytes((v & 255, v >> 8))
    return bytes(out)


def rect_cost(lib, frame, r, table):
    pieces = (Rect * 400)()
    n = lib.purr_rd_split(r, MAX_PX, pieces, 400)
    total = 0
    for i in range(n):
        p = pieces[i]
        raw = b"".join(frame[((p.y + row) * W + p.x) * 2:((p.y + row) * W + p.x + p.w) * 2] for row in range(p.h))
        out = ctypes.create_string_buffer(len(raw) + 16)
        codec = ctypes.c_uint8()
        sz = lib.purr_rd_encode_best(raw, len(raw), 0, 7, out, len(raw) + 16, table, ctypes.byref(codec))
        total += HDR + RECT_HEAD + sz
    return total, n


def changed(lib, a, b, table):
    d = Damage()
    lib.purr_rd_damage_init(ctypes.byref(d), W, H)
    for ty in range(0, H, 16):
        for tx in range(0, W, 16):
            if any(a[((ty + r) * W + tx) * 2:((ty + r) * W + tx + 16) * 2] != b[((ty + r) * W + tx) * 2:((ty + r) * W + tx + 16) * 2] for r in range(16)):
                lib.purr_rd_damage_add(ctypes.byref(d), tx, ty, 16, 16)
    total, pieces, px = HDR, 0, 0                # one PRESENT
    for i in range(d.n):
        c, n = rect_cost(lib, b, d.r[i], table)
        total += c
        pieces += n
        px += d.r[i].w * d.r[i].h
    return total, pieces, px, d.n


def show(label, nbytes, px):
    link = nbytes / 1.5e6 * 1000
    panel = px * 16 / 40e6 * 1000
    print("%-34s %7d bytes  link ~%5.1f ms  panel ~%5.1f ms  (raw would be %d)" % (label, nbytes, link, panel, px * 2))


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    lib = load_lib()
    table = (ctypes.c_uint16 * 4096)()
    frames = [(os.path.basename(p), rgb565(p)) for p in sys.argv[1:]]
    for name, f in frames:
        c, n = rect_cost(lib, f, Rect(0, 0, W, H), table)
        show("whole screen: " + name, c + HDR, W * H)
    for (na, a), (nb, b) in zip(frames, frames[1:]):
        c, pieces, px, rects = changed(lib, a, b, table)
        show("change %s -> %s" % (na, nb), c, px)
        print("    %d rectangle(s), %d piece(s), %d of %d pixels changed" % (rects, pieces, px, W * H))


if __name__ == "__main__":
    main()
