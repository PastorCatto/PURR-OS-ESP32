import struct, sys, zlib

# purr_cfg_t, packed, little-endian (PurrOS/components/coreos/include/purr_abi.h)
FMT = "<IHHI BB2s I I I I I  BBBBI  64s 544s 32s 64s"
def build(secure_mode=1, flags=0, seq=1):
    body = struct.pack(FMT,
        0x47464350, 1, 752, seq,          # magic 'PCFG', version, size, seq
        secure_mode, 0, b"\0\0",          # secure_mode, boot_target, reserved0
        flags, 0, 0, 0, 0,                # flags, revoked_keys, boot_seq, boot_fail_count, latest_time
        0, 0, 0, 0, 0,                    # update: target,state,attempts,reserved,version
        b"\0"*64, b"\0"*544, b"\0"*32, b"\0"*64)
    assert len(body) == 748, len(body)
    return body + struct.pack("<I", zlib.crc32(body) & 0xFFFFFFFF)

if __name__ == "__main__":
    mode = {"off":0, "warn":1, "enforce":2}[sys.argv[1]]
    sector_a = build(mode).ljust(4096, b"\xff")
    open(sys.argv[2], "wb").write(sector_a + b"\xff"*4096)
