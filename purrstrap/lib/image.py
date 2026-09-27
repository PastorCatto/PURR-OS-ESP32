"""The PURR image container: layout, packing and parsing.

Shared by every subscript that reads or writes one (bootpkg, keys, coreos), so the
layout is declared exactly once. Keep in sync with
PurrOS/components/coreos/include/purr_abi.h's purr_image_header_t.
"""

import hashlib
import struct

MAGIC = 0x50555252   # 'PURR'
HEADER_VERSION = 1

# Field order: magic, header_version, header_size, chip_id, image_type, key_id, flags,
# name[32], version[12], min_boot_version[12], payload_offset, payload_size,
# payload_sha256[32], signature[64].
HEADER_FORMAT = "<IBHHBBH32s12s12sII32s64s"
HEADER_SIZE = struct.calcsize(HEADER_FORMAT)
SIGNED_LEN = HEADER_SIZE - 64                      # everything before the signature field

# image_type
IMG_OS = 1
IMG_RECOVERY = 2
IMG_MODULE = 3
IMG_APP = 4

# module subtypes (image_type == IMG_MODULE), the low byte of flags
MOD_KERNEL = 1
MOD_COREOS = 2
MOD_APPMANAGER = 3
MOD_RUNTIME = 4
MOD_DRIVER = 5
MOD_BOOTPKG = 6
MOD_DEVBUNDLE = 7
MOD_LOADER = 8

CHIP_ESP32 = 0
CHIP_ESP32S3 = 9
CHIP_ANY = 0xFFFF


def sha256(data):
    return hashlib.sha256(data).digest()


def make_image(name, version, image_type, flags, payload, chip_id,
               min_boot_version="", header_version=HEADER_VERSION):
    """An unsigned image: header (zeroed key id and signature) followed by the payload,
    with payload_sha256 already filled in."""
    header = struct.pack(
        HEADER_FORMAT, MAGIC, header_version, HEADER_SIZE, chip_id, image_type, 0, flags,
        name.encode() if isinstance(name, str) else name,
        version.encode() if isinstance(version, str) else version,
        min_boot_version.encode() if isinstance(min_boot_version, str) else min_boot_version,
        HEADER_SIZE, len(payload), sha256(payload), b"\0" * 64)
    return header + payload


def is_purr_image(data):
    return len(data) >= 4 and struct.unpack_from("<I", data, 0)[0] == MAGIC


def parse_header(buf):
    """The header fields as a tuple, in HEADER_FORMAT order. Index constants below name
    the ones code needs to modify in place (mutate the tuple as a list, then re-pack)."""
    return struct.unpack(HEADER_FORMAT, buf[:HEADER_SIZE])


# Indices into the parse_header()/make_image() tuple, for code that edits one field and
# re-packs with struct.pack(HEADER_FORMAT, *fields) rather than hand-computing byte offsets.
IX_KEY_ID = 5
IX_PAYLOAD_OFFSET = 10
IX_PAYLOAD_SIZE = 11
IX_PAYLOAD_SHA256 = 12
IX_SIGNATURE = 13


def header_fields(h):
    (magic, ver, hsize, chip, itype, key_id, flags, name, version, min_boot,
     payload_off, payload_size, payload_hash, sig) = h
    return {
        "magic": magic, "header_version": ver, "header_size": hsize, "chip_id": chip,
        "image_type": itype, "key_id": key_id, "flags": flags,
        "name": name.rstrip(b"\0").decode("ascii", "replace"),
        "version": version.rstrip(b"\0").decode("ascii", "replace"),
        "min_boot_version": min_boot.rstrip(b"\0").decode("ascii", "replace"),
        "payload_offset": payload_off, "payload_size": payload_size,
        "payload_sha256": payload_hash, "signature": sig,
    }
