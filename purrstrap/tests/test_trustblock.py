"""The signed trust block: layout, and `keys trust-block` end to end (plan3.md).

The C side pins the same offsets in test_trust.c and evaluates the signature; here the tool's
bytes are checked against the layout and against an independent verification with `cryptography`.
"""

import os
import struct
import tempfile
import unittest
import zlib
from unittest import mock

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec, utils

from tests.helpers import make_ctx
from lib import purrcfg as pcfg
from scripts import keys


class LayoutTests(unittest.TestCase):
    def test_sizes_and_offsets_match_the_c_struct(self):
        self.assertEqual(pcfg.CFG_SIZE, 752)
        self.assertEqual((pcfg.OFF_SECURE_MODE, pcfg.OFF_TIGHTEN, pcfg.OFF_TRUST_FLAGS), (12, 14, 15))
        self.assertEqual((pcfg.OFF_FLAGS, pcfg.OFF_REVOKED, pcfg.OFF_KEYS), (16, 20, 108))
        self.assertEqual((pcfg.OFF_TRUST_SIG, pcfg.OFF_CRC), (684, 748))
        self.assertEqual(pcfg.KEY_SLOT_SIZE, 68)
        self.assertEqual(pcfg.OFF_KEYS + pcfg.KEY_SLOTS * pcfg.KEY_SLOT_SIZE + 32, pcfg.OFF_TRUST_SIG)

    def test_build_cfg_is_sealed_and_parses_back(self):
        cfg = pcfg.build_cfg(secure_mode=2, trust_flags=1, revoked=0x20, flags=pcfg.CFGF_DEV_MODE,
                             tighten=1, seq=7)
        self.assertEqual(len(cfg), pcfg.CFG_SIZE)
        self.assertEqual(struct.unpack_from("<I", cfg, pcfg.OFF_CRC)[0],
                         zlib.crc32(cfg[:pcfg.OFF_CRC]) & 0xFFFFFFFF)
        f = pcfg.parse_cfg(cfg)
        self.assertEqual((f["secure_mode"], f["trust_flags"], f["tighten"], f["seq"]), (2, 1, 1, 7))
        self.assertEqual((f["revoked"], f["flags"]), (0x20, pcfg.CFGF_DEV_MODE))

    def test_parse_rejects_corruption(self):
        cfg = bytearray(pcfg.build_cfg())
        cfg[20] ^= 1
        with self.assertRaises(ValueError):
            pcfg.parse_cfg(bytes(cfg))
        with self.assertRaises(ValueError):
            pcfg.parse_cfg(b"\xff" * pcfg.CFG_SIZE)
        with self.assertRaises(ValueError):
            pcfg.parse_cfg(b"short")

    def test_signed_bytes_layout(self):
        slot = pcfg.key_slot(5, pcfg.ROLE_IDS["developer"], bytes(range(64)))
        blob = pcfg.keys_blob([slot])
        b = pcfg.trust_signed_bytes(2, 1, 0x04030201, blob)
        self.assertEqual(len(b), 10 + 2 + 4 + 8 * 68)
        self.assertEqual(b[:10], b"PURRTRUST\x00")
        self.assertEqual(b[10:12], bytes([2, 1]))
        self.assertEqual(b[12:16], bytes([1, 2, 3, 4]))
        self.assertEqual(b[16:20], bytes([5, 4, 1, 0]))                  # key_id, role, valid, reserved
        self.assertEqual(b[20:84], bytes(range(64)))
        self.assertEqual(b[84:], b"\x00" * (7 * 68))

    def test_partition_image_is_two_sectors(self):
        img = pcfg.partition_image(pcfg.build_cfg())
        self.assertEqual(len(img), pcfg.PARTITION_SIZE)
        self.assertEqual(img[pcfg.SECTOR:], b"\xff" * pcfg.SECTOR)       # sector B erased
        self.assertEqual(img[pcfg.CFG_SIZE:pcfg.SECTOR], b"\xff" * (pcfg.SECTOR - pcfg.CFG_SIZE))


class TrustBlockToolTests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.env = mock.patch.dict(os.environ, {"PURRSTRAP_KEY_PASSWORD": "testpass123"})
        self.env.start()
        self.addCleanup(self.env.stop)
        self.ctx, self.out = make_ctx(self.dir)
        # a boot key and a second key, written the way `keys generate` writes them
        self.boot = ec.generate_private_key(ec.SECP256R1())
        self.key_path = os.path.join(self.dir, "boot.key")
        with open(self.key_path, "wb") as fh:
            fh.write(self.boot.private_bytes(
                serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                serialization.BestAvailableEncryption(b"testpass123")))
        other = ec.generate_private_key(ec.SECP256R1())
        self.other_pub = os.path.join(self.dir, "dev.pub")
        with open(self.other_pub, "wb") as fh:
            fh.write(other.public_key().public_bytes(
                serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo))
        self.dest = os.path.join(self.dir, "purrcfg.bin")

    def build(self, **kw):
        args = dict(key=self.key_path, mode="enforce", dev_locked=False, dev_mode=False,
                    tighten="off", revoke="", add_keys="", out=self.dest)
        args.update(kw)
        return keys.trust_block(self.ctx, **args)

    def read(self):
        with open(self.dest, "rb") as fh:
            return fh.read()

    def verify_signature(self, cfg):
        f = pcfg.parse_cfg(cfg)
        signed = pcfg.trust_signed_bytes(f["secure_mode"], f["trust_flags"], f["revoked"], f["keys"])
        r, s = int.from_bytes(f["trust_sig"][:32], "big"), int.from_bytes(f["trust_sig"][32:], "big")
        self.boot.public_key().verify(utils.encode_dss_signature(r, s), keys.sha256(signed),
                                      ec.ECDSA(utils.Prehashed(hashes.SHA256())))

    def test_builds_a_signed_partition_image(self):
        self.assertEqual(self.build(), 0)
        img = self.read()
        self.assertEqual(len(img), pcfg.PARTITION_SIZE)
        f = pcfg.parse_cfg(img)
        self.assertEqual((f["secure_mode"], f["trust_flags"], f["flags"], f["tighten"]), (2, 0, 0, 0))
        self.verify_signature(img)

    def test_dev_lock_and_dev_mode(self):
        self.assertEqual(self.build(dev_locked=True, dev_mode=True), 0)
        f = pcfg.parse_cfg(self.read())
        self.assertEqual(f["trust_flags"], pcfg.TRUSTF_DEV_LOCKED)
        self.assertEqual(f["flags"], pcfg.CFGF_DEV_MODE)
        self.verify_signature(self.read())

    def test_revocations_keys_and_tighten(self):
        self.assertEqual(self.build(mode="warn", tighten="enforce", revoke="3, 5",
                                    add_keys=f"9:developer:{self.other_pub}"), 0)
        f = pcfg.parse_cfg(self.read())
        self.assertEqual(f["revoked"], (1 << 3) | (1 << 5))
        self.assertEqual(f["tighten"], 2)
        self.assertEqual(f["keys"][:4], bytes([9, pcfg.ROLE_IDS["developer"], 1, 0]))
        self.verify_signature(self.read())

    def test_signature_covers_the_signed_fields_only(self):
        self.build()
        img = bytearray(self.read())
        f = pcfg.parse_cfg(bytes(img))
        # tighten is outside the signature: changing it leaves the same signed bytes
        signed = pcfg.trust_signed_bytes(f["secure_mode"], f["trust_flags"], f["revoked"], f["keys"])
        img[pcfg.OFF_TIGHTEN] = 2
        f2 = pcfg.parse_cfg(pcfg.build_cfg(secure_mode=f["secure_mode"], tighten=2,
                                           trust_sig=f["trust_sig"]))
        self.assertEqual(pcfg.trust_signed_bytes(f2["secure_mode"], f2["trust_flags"], f2["revoked"],
                                                 f2["keys"]), signed)

    def test_bad_arguments_are_rejected(self):
        self.assertEqual(self.build(revoke="40"), 1)
        self.assertEqual(self.build(revoke="x"), 1)
        self.assertEqual(self.build(add_keys="0:boot:" + self.other_pub), 1)         # id 0 is "no key"
        self.assertEqual(self.build(add_keys="9:wizard:" + self.other_pub), 1)
        self.assertEqual(self.build(add_keys="9:boot:/no/such/file"), 1)
        self.assertEqual(self.build(add_keys=",".join(f"{i}:vendor:{self.other_pub}" for i in range(1, 10))), 1)

    def test_wrong_password_writes_nothing(self):
        with mock.patch.dict(os.environ, {"PURRSTRAP_KEY_PASSWORD": "wrong"}):
            self.assertEqual(self.build(), 1)
        self.assertFalse(os.path.exists(self.dest))


if __name__ == "__main__":
    unittest.main()
