#!/usr/bin/env python3
"""Builds a LittleFS root image on the host (tools/vm/lfs_image.c) and, optionally, splices it into a flash image at the `root` partition of flash map v2.

    python3 tools/vm/make_root.py --out out/root.bin [--user dev:dev:admin] [--file /ui/w10m/pack.bin=out/ui/pack-w10m.bin] [--into out/vm-flash.bin]

The root partition is read from the flash image's own partition table when --into is given (the table must be flash map v2: `root` is the last partition);
without --into the size is the one of PurrOS/partitions/tdeck_plus.csv. The accounts use a fixed salt: for test images only. Needs gcc.
"""
import argparse
import os
import struct
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
PO = os.path.join(ROOT, "PurrOS", "components")


def build_tool(dest):
    src = [os.path.join(ROOT, "tools", "vm", "lfs_image.c"), os.path.join(PO, "kernel", "src", "purr_fs.c"), os.path.join(PO, "kernel", "src", "purr_userstore.c"),
           os.path.join(PO, "littlefs", "lfs.c"), os.path.join(PO, "littlefs", "lfs_util.c"), os.path.join(PO, "coreos", "src", "purr_users.c"),
           os.path.join(PO, "coreos", "src", "purr_pbkdf2.c"), os.path.join(PO, "coreos", "src", "purr_sha256.c"), os.path.join(PO, "coreos", "src", "purr_util.c")]
    src = [s for s in src if os.path.exists(s)]
    cmd = ["gcc", "-std=gnu11", "-O1", "-w", "-I", os.path.join(PO, "coreos", "include"), "-I", os.path.join(PO, "kernel", "include"), "-I", os.path.join(PO, "littlefs"),
           "-I", os.path.join(PO, "coreos", "test", "host")] + src + ["-o", dest]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("could not build lfs_image:\n" + r.stderr[-1500:])


def root_of(image):
    """(offset, size) of the `root` partition in a flash image's table (0xA000)."""
    with open(image, "rb") as fh:
        fh.seek(0xA000)
        t = fh.read(0x1000)
    for i in range(0, len(t) - 31, 32):
        if t[i:i + 2] != b"\xaa\x50":
            break
        off, size = struct.unpack("<II", t[i + 4:i + 12])
        if t[i + 12:i + 28].split(b"\0")[0] == b"root":
            return off, size
    sys.exit("%s has no `root` partition in its table" % image)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--out", required=True)
    ap.add_argument("--user", action="append", default=[], help="name:password:admin|standard")
    ap.add_argument("--file", action="append", default=[], help="/device/path=host/file")
    ap.add_argument("--into", help="a flash image to splice the root image into")
    ap.add_argument("--blocks", type=int, help="block count (default: from the image's table, else flash map v2: 0xA3F000 / 4096)")
    a = ap.parse_args()
    tool = os.path.join(ROOT, "out", "lfs_image")
    os.makedirs(os.path.dirname(tool), exist_ok=True)
    build_tool(tool)
    offset = size = None
    if a.into:
        offset, size = root_of(a.into)
    blocks = a.blocks or ((size // 4096) if size else 0xA3F000 // 4096)
    cmd = [tool, "--blocks", str(blocks), "--out", a.out]
    for u in a.user:
        cmd += ["--user", u]
    for f in a.file:
        cmd += ["--file", f]
    r = subprocess.run(cmd, capture_output=True, text=True)
    sys.stdout.write(r.stdout)
    if r.returncode != 0:
        sys.exit(r.stderr)
    if a.into:
        data = open(a.out, "rb").read()
        img = bytearray(open(a.into, "rb").read())
        assert len(data) <= size, "the root image is bigger than the partition"
        img[offset:offset + size] = b"\xff" * size
        img[offset:offset + len(data)] = data
        open(a.into, "wb").write(img)
        print("spliced into %s at 0x%X (%d bytes)" % (a.into, offset, len(data)))


if __name__ == "__main__":
    main()
