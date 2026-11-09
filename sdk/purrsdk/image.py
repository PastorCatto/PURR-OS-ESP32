"""The image maker: one whole flash image for a device, from the signed parts, with no terminal and no compiler.

It does what tools/hw/make_hw_image.sh does for the T-Deck Plus, but in Python and for any board that has a partition table, so Studio (and `purr image`)
can do it for you:

    bootloader   at 0x0           the raw bootloader, taken out of its signed container
    table        at 0xA000        the partition table, written from the board's layout (the same bytes ESP-IDF's tool writes)
    purrcfg      at the layout    the trust block, if you give one (without it the area is left erased and the device uses its defaults)
    bootpkg      at the layout    the boot package container, as it is
    kittenos     at the layout    a whole-partition image: the container plus its signature trailer (`purrstrap coreos slot-image`)
    kernel       at the layout    the same
    loader       at the layout    the same

Nothing is hard-coded about where things go: the offsets and sizes come from the board's partition table (PurrOS/partitions/<board>.csv in a checkout
and in the kit, or the board file's [[partition]] blocks). The file system (`root`) and the saved network (`netrec`) are left erased: the picture packs,
modules and apps arrive from the setup wizard, a recovery, or the dev server.

What is NOT done here, on purpose: signing. The parts have to be signed already (the kit's prebuilt parts are; `purr release` re-signs them with your key).
"""

import hashlib
import os
import struct

from . import boards, minitoml, runner, workspace

TABLE_AT = 0xA000
BOOTLOADER_LIMIT = 0xA000                       # the bootloader gets 40 KB, then comes the table
TABLE_BYTES = 0x1000
MAGIC = 0x50555252                              # 'PURR'
HEADER = 173                                    # the signed container header (purr_abi.h)

# Which file name each part is looked for under, in a folder: the official release uses kernel_<board>.cat, a dev server or `purr release` kernel-<board>.cat.
PARTS = (("bootloader", ".cat"), ("bootpkg", ".cat"), ("kittenos", ".kitt"), ("kernel", ".cat"), ("loader", ".cat"))
SLOTS = ("kittenos", "kernel", "loader")        # these three become whole-partition images


class ImageError(Exception):
    pass


# ---------------------------------------------------------------------------------------------------------------- the layout

def _csv_layout(path):
    rows = []
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            name, typ, sub, off, size = [c.strip() for c in line.split(",")[:5]]
            part = {"name": name, "type": typ, "subtype": sub}
            rows.append(dict(name=name, type=boards.PART_TYPES.get(typ, -1), subtype=boards._sub(part), offset=int(off, 0), size=int(size, 0)))
    return sorted(rows, key=lambda p: p["offset"])


def layout_for(board):
    """The board's partition table as [{"name","type","subtype","offset","size"}], from PurrOS/partitions/<board>.csv or the board file; ImageError if neither exists."""
    for root in (workspace.kit_path("PurrOS", "partitions"), os.path.join(os.path.dirname(workspace.sdk_path()), "PurrOS", "partitions")):
        if os.path.isfile(os.path.join(root, board + ".csv")):
            return _csv_layout(os.path.join(root, board + ".csv"))
    toml = workspace.sdk_path("boards", board + ".toml")
    if os.path.isfile(toml):
        lay = boards.flash_layout(minitoml.load(toml))
        if lay:
            return lay
    raise ImageError("I do not have a partition table for the board %r. It needs PurrOS/partitions/%s.csv, or a board file with [[partition]] blocks." % (board, board))


def table_bytes(layout):
    """The partition table exactly as ESP-IDF's tool writes it: 32-byte entries, then the MD5 entry. (The caller pads it to 4 KB.)"""
    body = b""
    for p in layout:
        name = p["name"].encode("ascii")
        if len(name) > 15:
            raise ImageError("The partition name %r is too long (15 letters at most)." % p["name"])
        body += struct.pack("<2sBBLL16sL", b"\xaa\x50", p["type"], p["subtype"], p["offset"], p["size"], name.ljust(16, b"\0"), 0)
    return body + b"\xeb\xeb" + b"\xff" * 14 + hashlib.md5(body).digest()


def read_table(data):
    """The partitions in a table's bytes (the inverse of table_bytes), to check an image against its layout."""
    out = []
    for i in range(0, len(data) - 31, 32):
        if data[i:i + 2] != b"\xaa\x50":
            break
        typ, sub, off, size, name, _flags = struct.unpack_from("<BBLL16sL", data, i + 2)
        out.append({"name": name.split(b"\0")[0].decode("ascii", "replace"), "type": typ, "subtype": sub, "offset": off, "size": size})
    return out


# ---------------------------------------------------------------------------------------------------------------- the parts

def find_parts(folder, board):
    """{part: path} found in a folder (or its recovery/ or JumpingJaguar1/ subfolder), under either naming; parts it cannot find are missing from the result."""
    found = {}
    roots = [folder] + [os.path.join(folder, s) for s in ("recovery", "JumpingJaguar1") if os.path.isdir(os.path.join(folder, s))]
    for root in roots:
        for part, ext in PARTS:
            for sep in ("-", "_"):
                p = os.path.join(root, "%s%s%s%s" % (part, sep, board, ext))
                if part not in found and os.path.isfile(p):
                    found[part] = p
    return found


def _container(path):
    """(header fields, whole bytes) of a signed PURR container, or ImageError."""
    with open(path, "rb") as fh:
        data = fh.read()
    if len(data) < HEADER or struct.unpack_from("<I", data, 0)[0] != MAGIC:
        raise ImageError("%s is not a signed PURR container." % os.path.basename(path))
    off, size = struct.unpack_from("<II", data, 69)
    want = data[77:109]
    if off < HEADER or off + size > len(data):
        raise ImageError("%s: its payload runs past the end of the file." % os.path.basename(path))
    if hashlib.sha256(data[off:off + size]).digest() != want:
        raise ImageError("%s: its payload does not match the hash in its header (a damaged or edited file)." % os.path.basename(path))
    return {"payload_offset": off, "payload_size": size, "key_id": data[10], "version": data[45:57].split(b"\0")[0].decode("ascii", "replace")}, data


def plan(board, folder):
    """What the image maker would do: the layout, which parts it found, which are missing, and how each fits. Does not write anything."""
    layout = layout_for(board)
    by = {p["name"]: p for p in layout}
    folder = os.path.abspath(folder) if folder else folder
    parts = find_parts(folder, board) if folder and os.path.isdir(folder) else {}
    rows, problems = [], []
    for part, _ext in PARTS:
        slot = by.get(part)
        path = parts.get(part)
        row = {"part": part, "path": path, "found": bool(path), "offset": slot["offset"] if slot else None, "room": slot["size"] if slot else None, "size": None, "version": None, "key_id": None}
        if part == "bootloader":
            row["offset"], row["room"] = 0, BOOTLOADER_LIMIT
        if path:
            try:
                hdr, data = _container(path)
                row["version"], row["key_id"] = hdr["version"], hdr["key_id"]
                row["size"] = hdr["payload_size"] if part in SLOTS or part == "bootloader" else len(data)
            except ImageError as e:
                problems.append(str(e))
        else:
            problems.append("I cannot find the %s for %s in %s." % (part, board, folder or "(no folder)"))
        if row["size"] is not None and row["room"] is not None and row["size"] > row["room"]:
            problems.append("The %s is %d bytes and its slot holds %d: %d too many." % (part, row["size"], row["room"], row["size"] - row["room"]))
        rows.append(row)
    flash = max(p["offset"] + p["size"] for p in layout)
    return {"board": board, "layout": layout, "flash_bytes": flash, "parts": rows, "problems": problems, "ok": not problems}


# ---------------------------------------------------------------------------------------------------------------- the image

def build(board, folder, out, trust_block=None, on_line=print, work=None):
    """Make the whole flash image at `out`. Returns {"ok": True, "out", "bytes", "sha256", "regions": [...], "notes": [...]}; raises ImageError with a plain message."""
    out = os.path.abspath(out)
    trust_block = os.path.abspath(trust_block) if trust_block else None
    p = plan(board, folder)
    if not p["ok"]:
        raise ImageError(" ".join(p["problems"]))
    layout = {x["name"]: x for x in p["layout"]}
    for need in ("purrcfg", "bootpkg", "kittenos", "kernel", "loader"):
        if need not in layout:
            raise ImageError("The partition table of %s has no %r partition." % (board, need))
    work = work or os.path.join(os.path.dirname(os.path.abspath(out)), ".image-work")
    os.makedirs(work, exist_ok=True)
    img = bytearray(b"\xff" * p["flash_bytes"])
    regions, notes = [], []

    def put(name, off, data, room):
        if len(data) > room:
            raise ImageError("The %s is %d bytes and has room for %d." % (name, len(data), room))
        img[off:off + len(data)] = data
        regions.append({"name": name, "offset": off, "bytes": len(data), "room": room})
        on_line("  %-10s 0x%06X  %8d of %8d bytes" % (name, off, len(data), room))

    parts = {r["part"]: r["path"] for r in p["parts"]}
    on_line("Putting the parts in place...")
    hdr, data = _container(parts["bootloader"])
    put("bootloader", 0, data[hdr["payload_offset"]:hdr["payload_offset"] + hdr["payload_size"]], BOOTLOADER_LIMIT)
    table = table_bytes(p["layout"])
    put("table", TABLE_AT, table, TABLE_BYTES)
    if trust_block:
        with open(trust_block, "rb") as fh:
            put("purrcfg", layout["purrcfg"]["offset"], fh.read(), layout["purrcfg"]["size"])
    else:
        notes.append("No trust block: the device uses its defaults (it boots what is signed by a key it knows, and warns about the rest).")
    with open(parts["bootpkg"], "rb") as fh:
        put("bootpkg", layout["bootpkg"]["offset"], fh.read(), layout["bootpkg"]["size"])
    for slot in SLOTS:
        slot_file = os.path.join(work, slot + "_slot.bin")
        r = runner.purrstrap(["coreos", "slot-image", "--file", parts[slot], "--board", board, "--slot", slot, "--out", slot_file])
        if r["rc"] != 0:
            raise ImageError("Could not cut the %s into a slot image: %s" % (slot, r["output"].strip().splitlines()[-1] if r["output"].strip() else "no reason given"))
        with open(slot_file, "rb") as fh:
            put(slot, layout[slot]["offset"], fh.read(), layout[slot]["size"])
    notes.append("The file system (root) and the saved network are left erased: the setup wizard or a recovery fills them.")

    on_line("Checking what was written...")
    got = read_table(bytes(img[TABLE_AT:TABLE_AT + TABLE_BYTES]))
    if boards.layout_differences(got, p["layout"]):
        raise ImageError("The partition table in the image does not match the board's layout.")
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    tmp = out + ".part"
    with open(tmp, "wb") as fh:
        fh.write(img)
    os.replace(tmp, out)
    digest = hashlib.sha256(img).hexdigest()
    on_line("Wrote %s (%d MB). sha256 %s" % (out, len(img) >> 20, digest[:16]))
    return {"ok": True, "out": out, "bytes": len(img), "sha256": digest, "regions": regions, "notes": notes, "fit": p["parts"],
            "versions": {r["part"]: r["version"] for r in p["parts"]}}
