"""The purrcfg layout and its signed trust block (bootloader/SPEC.md section 5, plan3.md).

Keep in sync with PurrOS/components/coreos/include/purr_abi.h's purr_cfg_t and
PurrOS/components/coreos/src/purr_trust.c. test_trust.c pins the same offsets on the C side, so
a layout change cannot drift quietly.
"""

import struct
import zlib

CFG_MAGIC = 0x47464350            # 'PCFG'
CFG_VERSION = 1
CFG_SIZE = 752                    # sizeof(purr_cfg_t)
SECTOR = 4096                     # purrcfg is two sectors, A and B
PARTITION_SIZE = 2 * SECTOR

# Byte offsets in purr_cfg_t (packed).
OFF_MAGIC, OFF_VERSION, OFF_SIZE, OFF_SEQ = 0, 4, 6, 8
OFF_SECURE_MODE, OFF_BOOT_TARGET, OFF_TIGHTEN, OFF_TRUST_FLAGS = 12, 13, 14, 15
OFF_FLAGS, OFF_REVOKED = 16, 20
OFF_KEYS, KEY_SLOT_SIZE, KEY_SLOTS = 108, 68, 8
OFF_TRUST_SIG, OFF_CRC = 684, 748

SECURE = {"off": 0, "warn": 1, "enforce": 2}
CFGF_DEV_MODE = 1 << 8
TRUSTF_DEV_LOCKED = 1 << 0
ROLE_IDS = {"boot": 1, "system": 2, "owner": 3, "developer": 4, "vendor": 5}


def key_slot(key_id, role, pub64):
    """One purr_key_slot_t: key_id, role, valid=1, reserved, 64 raw public key bytes."""
    if len(pub64) != 64:
        raise ValueError("a P-256 public key is 64 raw bytes")
    return struct.pack("<BBBB", key_id & 0xFF, role, 1, 0) + pub64


def trust_signed_bytes(secure_mode, trust_flags, revoked, keys_blob):
    """The exact bytes the trust signature covers (purr_trust_signed_bytes in C)."""
    if len(keys_blob) != KEY_SLOTS * KEY_SLOT_SIZE:
        raise ValueError("keys_blob must be 8 slots of 68 bytes")
    return (b"PURRTRUST\x00" + bytes([secure_mode & 0xFF, trust_flags & 0xFF]) +
            struct.pack("<I", revoked & 0xFFFFFFFF) + keys_blob)


def keys_blob(slots):
    """Up to 8 key slots, padded with empty (valid=0) slots to the full table."""
    if len(slots) > KEY_SLOTS:
        raise ValueError("at most 8 override keys")
    return b"".join(slots) + b"\x00" * ((KEY_SLOTS - len(slots)) * KEY_SLOT_SIZE)


def build_cfg(secure_mode=SECURE["warn"], trust_flags=0, revoked=0, slots=(), flags=0,
              tighten=0, seq=1, trust_sig=b"\x00" * 64):
    """A sealed purr_cfg_t: defaults for everything but the trust fields."""
    cfg = bytearray(CFG_SIZE)
    struct.pack_into("<IHHI", cfg, 0, CFG_MAGIC, CFG_VERSION, CFG_SIZE, seq)
    cfg[OFF_SECURE_MODE] = secure_mode
    cfg[OFF_TIGHTEN] = tighten
    cfg[OFF_TRUST_FLAGS] = trust_flags
    struct.pack_into("<II", cfg, OFF_FLAGS, flags, revoked & 0xFFFFFFFF)
    blob = keys_blob(list(slots))
    cfg[OFF_KEYS:OFF_KEYS + len(blob)] = blob
    cfg[OFF_TRUST_SIG:OFF_TRUST_SIG + 64] = trust_sig
    struct.pack_into("<I", cfg, OFF_CRC, zlib.crc32(bytes(cfg[:OFF_CRC])) & 0xFFFFFFFF)
    return bytes(cfg)


def partition_image(cfg):
    """The whole 8 KB purrcfg partition: the config in sector A, sector B erased."""
    return cfg + b"\xff" * (SECTOR - len(cfg)) + b"\xff" * SECTOR


def parse_cfg(data):
    """The fields a person cares about, from a purrcfg image (sector A). Raises ValueError if
    the magic, size or CRC are wrong."""
    if len(data) < CFG_SIZE:
        raise ValueError("shorter than a purrcfg")
    magic, version, size, seq = struct.unpack_from("<IHHI", data, 0)
    if magic != CFG_MAGIC or version != CFG_VERSION or size != CFG_SIZE:
        raise ValueError("not a purrcfg image")
    crc = struct.unpack_from("<I", data, OFF_CRC)[0]
    if crc != zlib.crc32(bytes(data[:OFF_CRC])) & 0xFFFFFFFF:
        raise ValueError("purrcfg checksum does not match")
    flags, revoked = struct.unpack_from("<II", data, OFF_FLAGS)
    return {
        "seq": seq, "secure_mode": data[OFF_SECURE_MODE], "tighten": data[OFF_TIGHTEN],
        "trust_flags": data[OFF_TRUST_FLAGS], "flags": flags, "revoked": revoked,
        "keys": bytes(data[OFF_KEYS:OFF_KEYS + KEY_SLOTS * KEY_SLOT_SIZE]),
        "trust_sig": bytes(data[OFF_TRUST_SIG:OFF_TRUST_SIG + 64]),
    }
