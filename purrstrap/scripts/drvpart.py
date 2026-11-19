"""The `drivers` partition (Drivers/SPEC.md, "The driver partition"; PurrOS/components/coreos/include/purr_drvpart.h): the drivers a board cannot start without, as
signed driver modules (.cat) in ten 8 KB slots, written when the image is made and again whenever one is added or replaced. The kernel is not rebuilt for any of it.

    slot n at n * 8192:  64-byte header ("PDRV", version 1, flags, length, crc32 of the .cat, the file name it is planted under, 0xFF), the .cat, then 0xFF

`pack` writes the whole partition image (0x14000 bytes) from a list of .cat files; `list` reads one back. The format is the C library's (test_drvpart.c has a golden slot,
and purrstrap/tests/test_drvpart.py checks this side against the same bytes). Nothing here signs: the drivers have to be signed already.
"""

import os
import struct
import zlib

from lib.model import Action, Param, Script

SLOTS = 10
SLOT_SIZE = 8192
SIZE = SLOTS * SLOT_SIZE                       # 0x14000: the partition's size in every board's table
HEADER = 64
MAX_CAT = SLOT_SIZE - HEADER
NAME_LEN = 32
MAGIC = b"PDRV"
VERSION = 1


class DrvpartError(Exception):
    pass


def check_name(name):
    raw = name.encode("ascii", "replace")
    if not name or len(raw) >= NAME_LEN or any(c in name for c in "/\\") or any(ord(c) < 0x20 or ord(c) > 0x7E for c in name):
        raise DrvpartError("%r is not a file name that fits (1 to 31 letters, no slash)." % name)
    return raw


def build_slot(name, cat):
    """One 8 KB slot image for a file name and its .cat."""
    raw = check_name(name)
    if not cat or len(cat) > MAX_CAT:
        raise DrvpartError("%s is %d bytes; a slot holds 1 to %d." % (name, len(cat), MAX_CAT))
    header = MAGIC + struct.pack("<HHII", VERSION, 0, len(cat), zlib.crc32(cat) & 0xFFFFFFFF) + raw.ljust(NAME_LEN, b"\0") + b"\xff" * 16
    assert len(header) == HEADER
    return (header + cat).ljust(SLOT_SIZE, b"\xff")


def pack(files):
    """The whole partition image from [(file name, .cat bytes)], at most ten, each under its own name; the free slots stay erased."""
    if len(files) > SLOTS:
        raise DrvpartError("%d drivers do not fit: the partition has %d slots." % (len(files), SLOTS))
    names = [n for n, _c in files]
    if len(set(names)) != len(names):
        raise DrvpartError("Two drivers have the same file name.")
    img = bytearray(b"\xff" * SIZE)
    for i, (name, cat) in enumerate(files):
        img[i * SLOT_SIZE:(i + 1) * SLOT_SIZE] = build_slot(name, cat)
    return bytes(img)


def read(image):
    """[{"slot", "state", "file", "length"}] for every slot of a partition image: state is "empty", "ok" or "damaged" (and the .cat's crc is checked)."""
    if len(image) < SIZE:
        raise DrvpartError("The image is %d bytes; the partition is %d." % (len(image), SIZE))
    rows = []
    for i in range(SLOTS):
        base = i * SLOT_SIZE
        head = image[base:base + HEADER]
        if head == b"\xff" * HEADER:
            rows.append({"slot": i, "state": "empty", "file": "", "length": 0})
            continue
        magic, version, _flags, length, crc = head[0:4], *struct.unpack_from("<HHII", head, 4)
        raw = head[16:16 + NAME_LEN]
        name = raw.split(b"\0")[0].decode("ascii", "replace")
        ok = (magic == MAGIC and version == VERSION and b"\0" in raw and name and "/" not in name and "\\" not in name and 0 < length <= MAX_CAT)
        if ok:
            ok = (zlib.crc32(image[base + HEADER:base + HEADER + length]) & 0xFFFFFFFF) == crc
        rows.append({"slot": i, "state": "ok" if ok else "damaged", "file": name if ok else "", "length": length if ok else 0})
    return rows


def extract(image, slot):
    """(file name, .cat bytes) of one good slot."""
    row = read(image)[slot]
    if row["state"] != "ok":
        raise DrvpartError("Slot %d is %s." % (slot, row["state"]))
    base = slot * SLOT_SIZE + HEADER
    return row["file"], bytes(image[base:base + row["length"]])


def _pack_action(ctx, out, files):
    paths = [f for f in (files or "").split(",") if f.strip()]
    if not paths:
        ctx.error("--files is required (comma-separated .cat files)")
        return 1
    items = []
    for path in paths:
        path = path.strip()
        try:
            with open(path, "rb") as fh:
                items.append((os.path.basename(path), fh.read()))
        except OSError as e:
            ctx.error("%s: %s" % (path, e.strerror))
            return 1
    try:
        img = pack(items)
    except DrvpartError as e:
        ctx.error(str(e))
        return 1
    os.makedirs(os.path.dirname(os.path.abspath(out)) or ".", exist_ok=True)
    with open(out, "wb") as fh:
        fh.write(img)
    ctx.ok("%s: %d bytes, %d driver(s)" % (out, len(img), len(items)))
    for r in read(img):
        if r["state"] == "ok":
            ctx.info("  slot %d  %-24s %5d bytes" % (r["slot"], r["file"], r["length"]))
    return 0


def _list_action(ctx, image):
    try:
        with open(image, "rb") as fh:
            rows = read(fh.read())
    except (OSError, DrvpartError) as e:
        ctx.error(str(getattr(e, "strerror", None) or e))
        return 1
    for r in rows:
        ctx.info("  slot %d  %-8s %-24s %5d bytes" % (r["slot"], r["state"], r["file"], r["length"]))
    return 0 if all(r["state"] != "damaged" for r in rows) else 1


SCRIPT = Script(
    name="drvpart",
    title="Driver partition",
    description="Pack signed driver modules (.cat) into the `drivers` partition image (ten slots), or read one back.",
    actions=(
        Action("pack", "Pack", _pack_action, help="write the partition image from a list of signed .cat files",
               params=(Param("out", "path", required=True, help="the output file (0x14000 bytes)"),
                       Param("files", "str", required=True, help="the .cat files, comma-separated; each is planted under its own file name"))),
        Action("list", "List", _list_action, help="show what a partition image holds",
               params=(Param("image", "path", required=True, help="a partition image made by pack"),)),
    ),
)
