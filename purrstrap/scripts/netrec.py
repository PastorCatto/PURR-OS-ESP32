"""Pre-provision the `netrec` partition (purr_netrec.c): the saved Wi-Fi credentials
RecoveryLoader/KittenOS try before falling back to an interactive prompt
(PurrOS/main/recovery_loader.c's `recover()`). Exists so a board with no keyboard
(InkyTuxedo/waveshare154) can still exercise the real internet-recovery path end to end in
testing, without a human able to type the network password into it.

The record layout here must match `purr_netrec.c`'s `record_t` exactly: magic ('PREC',
little-endian), a fixed-width SSID and password (null-terminated, zero-padded), 3 pad bytes
to keep the crc32 on a 4-byte boundary, then a crc32 (purr_crc32: zlib's CRC-32) over
everything before it. This script never touches the device by itself -- same convention
`coreos.py build` uses (it prints the esptool command, it doesn't run it) -- it only writes
the binary image; flash it with the printed `esptool write_flash` command.
"""

import binascii
import csv
import os
import struct

from lib.model import Action, Param, Script

MAGIC = 0x43455250            # 'PREC', little-endian (matches purr_netrec.c)
SSID_LEN = 33                 # PURR_NETREC_SSID_LEN
PASS_LEN = 65                 # PURR_NETREC_PASS_LEN
# record_t is {uint32 magic; char ssid[33]; char pass[65]; uint8 pad[3]; uint32 crc32}.
# The explicit pad[3] only gets offsetof(crc32) from 102 to 105 -- still not 4-aligned, so
# the compiler inserts 3 MORE invisible bytes before crc32. Verified against the real
# xtensa-esp32s3-elf cross-compiler with offsetof()/sizeof() (not assumed): offsetof(crc32)
# is 108, sizeof(record_t) is 112, not the 105/109 naive field-width arithmetic gives.
BODY_LEN = 108                 # offsetof(record_t, crc32)
RECORD_LEN = 112                # sizeof(record_t)
RECORD_FMT = f"<I{SSID_LEN}s{PASS_LEN}s6xI"   # magic, ssid, pass, pad[3]+compiler pad[3], crc32

BOARDS = ("tdeck_plus", "cyd_24c", "waveshare154")


def find_partition(ctx, repo_root, board, name):
    path = os.path.join(repo_root, "PurrOS", "partitions", f"{board}.csv")
    if not os.path.isfile(path):
        ctx.error(f"{path}: no such file")
        return None
    with open(path, newline="", encoding="utf-8") as fh:
        for row in csv.reader(fh):
            row = [c.strip() for c in row]
            if not row or not row[0] or row[0].startswith("#"):
                continue
            if row[0] == name:
                return int(row[3], 0), int(row[4], 0)
    ctx.error(f"{path}: no {name!r} partition")
    return None


def build_record(ssid, password):
    ssid_b = ssid.encode("utf-8")
    pass_b = password.encode("utf-8")
    if len(ssid_b) >= SSID_LEN:
        raise ValueError(f"ssid is {len(ssid_b)} bytes, must be < {SSID_LEN}")
    if len(pass_b) >= PASS_LEN:
        raise ValueError(f"password is {len(pass_b)} bytes, must be < {PASS_LEN}")
    body = struct.pack(f"<I{SSID_LEN}s{PASS_LEN}s6x", MAGIC, ssid_b, pass_b)
    assert len(body) == BODY_LEN
    crc = binascii.crc32(body) & 0xFFFFFFFF
    record = body + struct.pack("<I", crc)
    assert len(record) == RECORD_LEN
    return record


def write(ctx, board, ssid, password, out):
    if board not in BOARDS:
        ctx.error(f"unknown board {board}")
        return 1
    if not ssid:
        ctx.error("--ssid is required")
        return 1

    found = find_partition(ctx, ctx.repo_root, board, "netrec")
    if found is None:
        return 1
    offset, size = found
    if size < RECORD_LEN:
        ctx.error(f"netrec partition is {size} bytes, smaller than one record ({RECORD_LEN})")
        return 1

    record = build_record(ssid, password)
    image = record + b"\xff" * (size - len(record))

    dest = out or os.path.join(ctx.repo_root, "rel", "netrec", f"{board}.netrec.bin")
    os.makedirs(os.path.dirname(dest) or ".", exist_ok=True)
    with open(dest, "wb") as fh:
        fh.write(image)

    ctx.ok(f"{dest}: {len(image)} bytes (ssid {ssid!r}, partition offset {offset:#x})")
    ctx.info("flash it with:")
    ctx.info(f"  python -m esptool --chip esp32s3 -b 460800 write_flash {offset:#x} {dest}")
    ctx.info("(add -p <PORT> for the device's serial port; this only overwrites the "
              "netrec partition, nothing else on the device.)")
    return 0


SCRIPT = Script(
    name="netrec",
    title="Network recovery record",
    description="Pre-provision saved Wi-Fi credentials (netrec partition) for a keyboardless "
                "board to exercise internet recovery without interactive input.",
    actions=(
        Action("write", "Write", write,
               help="build a netrec image with the given Wi-Fi credentials",
               params=(
                   Param("board", "choice", default="waveshare154", choices=BOARDS,
                         help="the board (decides the netrec partition's offset/size)"),
                   Param("ssid", "str", required=True, help="the Wi-Fi network name"),
                   Param("password", "str", default="", help="the Wi-Fi password (blank for open)"),
                   Param("out", "path", default="",
                         help="output file (default: rel/netrec/<board>.netrec.bin)"),
               )),
    ),
)
