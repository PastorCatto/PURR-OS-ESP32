#!/usr/bin/env python3
"""A test client for the remote display host (RemoteDisplay/SPEC.md), standing in for a PURR OS Mini: connects, says HELLO, draws what it is sent into a 320 x 240 picture using the real
codecs (purr_rdisp.c, through ctypes), ACKs frames, and can send touches. For the emulator (reach the guest with PURR_QEMU_USER_OPTS=,hostfwd=tcp:127.0.0.1:47803-10.0.2.15:47803).

    python3 tools/rdisp/test_client.py HOST PORT OUT_PREFIX [--seconds N] [--tap X Y AT_SECONDS]...

Writes OUT_PREFIX-<n>.png (a picture after each of the first frames, and a last one) and prints what it saw. Needs gcc and Pillow."""
import argparse
import ctypes
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
CORE = os.path.join(HERE, "..", "..", "Modules", "vdisplay")
W, H = 320, 240


def load_lib():
    out = os.path.join(tempfile.mkdtemp(), "libpurr_rdisp.so")
    subprocess.check_call(["gcc", "-O2", "-shared", "-fPIC", "-I", CORE, os.path.join(CORE, "purr_rdisp.c"), "-o", out])
    return ctypes.CDLL(out)


def recv_exact(s, n):
    b = b""
    while len(b) < n:
        c = s.recv(n - len(b))
        if not c:
            raise EOFError("closed")
        b += c
    return b


def send(s, seq, typ, payload=b""):
    s.sendall(struct.pack("<BBHI", typ, 0, seq & 0xFFFF, len(payload)) + payload)


def save(fb, path):
    im = Image.new("RGB", (W, H))
    px = im.load()
    for y in range(H):
        for x in range(W):
            v = fb[(y * W + x) * 2] | (fb[(y * W + x) * 2 + 1] << 8)
            px[x, y] = (((v >> 11) & 31) << 3, ((v >> 5) & 63) << 2, (v & 31) << 3)
    im.save(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host")
    ap.add_argument("port", type=int)
    ap.add_argument("out")
    ap.add_argument("--seconds", type=float, default=20)
    ap.add_argument("--tap", nargs=3, action="append", default=[], metavar=("X", "Y", "AT"))
    a = ap.parse_args()
    lib = load_lib()
    lib.purr_rd_decode.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_int]
    s = socket.create_connection((a.host, a.port), timeout=10)
    s.settimeout(8)
    seq = 0
    hello = struct.pack("<BBHHBBHHH", 1, 0, W, H, 1, 7, 0x0009, 4096, 16384) + b"test-client".ljust(16, b"\0") + bytes(64)
    send(s, seq, 0x01, hello)
    seq += 1
    typ, _, _, ln = struct.unpack("<BBHI", recv_exact(s, 8))
    body = recv_exact(s, ln)
    if typ != 0x02:
        print("host answered type 0x%02x: %r" % (typ, body))
        return 1
    major, minor, fmt, codecs, w, h, maxpx, window, _, session = struct.unpack("<BBBBHHHBBI", body)
    print("WELCOME: %dx%d, codecs %d, up to %d px a piece, window %d, session %08x" % (w, h, codecs, maxpx, window, session))
    fb = bytearray(b"\xff" * (W * H * 2))
    stats = {"RECT": 0, "FILL": 0, "PRESENT": 0, "bytes": 8 + ln, "px": 0}
    taps = sorted((float(t[2]), int(t[0]), int(t[1])) for t in a.tap)
    t0 = time.time()
    saved = 0
    down_until = 0
    while time.time() - t0 < a.seconds:
        while taps and time.time() - t0 >= taps[0][0]:
            _, x, y = taps.pop(0)
            send(s, seq, 0x20, struct.pack("<BBHH", 0, 0, x, y))
            seq += 1
            time.sleep(0.25)
            send(s, seq, 0x20, struct.pack("<BBHH", 2, 0, x, y))
            seq += 1
            print("tap at %d,%d" % (x, y))
        s.settimeout(0.3)
        try:
            hd = recv_exact(s, 8)
        except socket.timeout:
            continue
        except EOFError:
            print("the host closed the session")
            break
        typ, _, _, ln = struct.unpack("<BBHI", hd)
        body = recv_exact(s, ln) if ln else b""
        stats["bytes"] += 8 + ln
        if typ == 0x10:
            x, y, rw, rh, codec, f = struct.unpack("<HHHHBB", body[:10])
            raw = ctypes.create_string_buffer(rw * rh * 2)
            rc = lib.purr_rd_decode(codec, body[10:], len(body) - 10, raw, rw * rh * 2)
            if rc != 0:
                print("bad rect %d,%d %dx%d codec %d" % (x, y, rw, rh, codec))
                continue
            for r in range(rh):
                fb[((y + r) * W + x) * 2:((y + r) * W + x + rw) * 2] = raw.raw[r * rw * 2:(r + 1) * rw * 2]
            stats["RECT"] += 1
            stats["px"] += rw * rh
        elif typ == 0x13:
            x, y, rw, rh, colour = struct.unpack("<HHHHH", body)
            line = struct.pack("<H", colour) * rw
            for r in range(rh):
                fb[((y + r) * W + x) * 2:((y + r) * W + x + rw) * 2] = line
            stats["FILL"] += 1
            stats["px"] += rw * rh
        elif typ == 0x12:
            stats["PRESENT"] += 1
            send(s, seq, 0x21, body[:4])
            seq += 1
            if saved < 3 or stats["PRESENT"] % 10 == 0:
                save(fb, "%s-%d.png" % (a.out, saved))
                saved += 1
        elif typ == 0x7F:
            print("ERROR from the host:", body)
            break
    save(fb, a.out + "-last.png")
    print("saw %(RECT)d rects, %(FILL)d fills, %(PRESENT)d frames, %(px)d pixels in %(bytes)d bytes" % stats)
    send(s, seq, 0x3F, b"\0")
    s.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
