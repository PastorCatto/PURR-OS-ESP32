import importlib.util
import os
import struct
import tempfile
import unittest
from unittest import mock

from cryptography.hazmat.primitives.asymmetric import ec

from helpers import TOOL_DIR, make_ctx

spec = importlib.util.spec_from_file_location(
    "keys_under_test", os.path.join(TOOL_DIR, "scripts", "keys.py"))
keys = importlib.util.module_from_spec(spec)
spec.loader.exec_module(keys)


def make_image(name=b"bootpkg", version=b"0.1.0", payload=b"hello world"):
    """An unsigned, correctly-hashed PURR image, the way a packer would build one."""
    header = struct.pack(
        keys.HEADER_FORMAT, keys.IMAGE_MAGIC, 1, keys.HEADER_SIZE, 9, 3, 0, 6,
        name, version, b"", keys.HEADER_SIZE, len(payload), keys.sha256(payload), b"\0" * 64)
    return header + payload


class RoundTripTests(unittest.TestCase):
    def test_raw_pub_round_trip(self):
        priv = ec.generate_private_key(ec.SECP256R1())
        raw = keys.raw_pub(priv.public_key())
        self.assertEqual(len(raw), 64)
        pub2 = keys.public_key_from_raw(raw)
        self.assertEqual(keys.raw_pub(pub2), raw)

    def test_sig_round_trip(self):
        r = (7).to_bytes(32, "big")
        s = (12345).to_bytes(32, "big")
        der = keys.raw_sig_to_der(r + s)
        self.assertEqual(keys.der_sig_to_raw(der), r + s)

    def test_is_purr_image(self):
        self.assertTrue(keys.is_purr_image(make_image()))
        self.assertFalse(keys.is_purr_image(b"not an image"))
        self.assertFalse(keys.is_purr_image(b"\0\0\0"))    # shorter than the magic itself

    def test_header_fields_round_trip(self):
        img = make_image(name=b"kernel", version=b"1.2.3")
        f = keys.header_fields(keys.parse_header(img))
        self.assertEqual(f["name"], "kernel")
        self.assertEqual(f["version"], "1.2.3")
        self.assertEqual(f["payload_offset"], keys.HEADER_SIZE)
        self.assertEqual(f["signature"], b"\0" * 64)


class ToolTests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.ctx, self.out = make_ctx(self.dir)
        self.env = mock.patch.dict(os.environ, {"PURRSTRAP_KEY_PASSWORD": "testpass123"})
        self.env.start()
        self.addCleanup(self.env.stop)

    def path(self, *parts):
        return os.path.join(self.dir, *parts)

    def test_generate_then_export_then_sign_then_verify(self):
        code = keys.generate(self.ctx, "system", self.path("k"), 2)
        self.assertEqual(code, 0)
        self.assertTrue(os.path.isfile(self.path("k", "system.key")))
        self.assertTrue(os.path.isfile(self.path("k", "system.pub")))

        code = keys.export_public(self.ctx, self.path("k", "system.pub"), "system", 2, self.path("k"))
        self.assertEqual(code, 0)
        raw = open(self.path("k", "system_2.pub.bin"), "rb").read()
        self.assertEqual(len(raw), 64)
        bag = open(self.path("k", "purr_default_keys.c"), encoding="utf-8").read()
        self.assertIn("PURR_ROLE_SYSTEM", bag)
        self.assertIn(".key_id = 2", bag)

        img_path = self.path("bootpkg.bin")
        open(img_path, "wb").write(make_image())
        code = keys.sign(self.ctx, self.path("k", "system.key"), 2, img_path, None)
        self.assertEqual(code, 0)

        signed = open(img_path, "rb").read()
        f = keys.header_fields(keys.parse_header(signed))
        self.assertEqual(f["key_id"], 2)
        self.assertNotEqual(f["signature"], b"\0" * 64)

        code = keys.verify(self.ctx, self.path("k", "system.pub"), img_path)
        self.assertEqual(code, 0)
        # A raw 64-byte public key file verifies exactly the same way.
        code = keys.verify(self.ctx, self.path("k", "system_2.pub.bin"), img_path)
        self.assertEqual(code, 0)

    def test_export_public_accumulates_across_calls(self):
        keys.generate(self.ctx, "system", self.path("k"), 2)
        keys.generate(self.ctx, "owner", self.path("k"), 3)
        keys.export_public(self.ctx, self.path("k", "system.pub"), "system", 2, self.path("k"))
        keys.export_public(self.ctx, self.path("k", "owner.pub"), "owner", 3, self.path("k"))
        bag = open(self.path("k", "purr_default_keys.c"), encoding="utf-8").read()
        self.assertIn("PURR_ROLE_SYSTEM", bag)
        self.assertIn("PURR_ROLE_OWNER", bag)
        self.assertEqual(bag.count(".key_id ="), 2)

    def test_sign_rejects_wrong_password(self):
        keys.generate(self.ctx, "system", self.path("k"), 1)
        img_path = self.path("bootpkg.bin")
        open(img_path, "wb").write(make_image())
        with mock.patch.dict(os.environ, {"PURRSTRAP_KEY_PASSWORD": "not the password"}):
            code = keys.sign(self.ctx, self.path("k", "system.key"), 1, img_path, None)
        self.assertNotEqual(code, 0)
        self.assertIn("[error]", self.out.getvalue())

    def test_generate_rejects_empty_password(self):
        with mock.patch.dict(os.environ, {"PURRSTRAP_KEY_PASSWORD": ""}):
            code = keys.generate(self.ctx, "system", self.path("k"), 1)
        self.assertNotEqual(code, 0)

    def test_generate_refuses_to_overwrite(self):
        # F-19: re-running generate used to replace an existing key pair with no warning.
        keys.generate(self.ctx, "system", self.path("k"), 1)
        code = keys.generate(self.ctx, "system", self.path("k"), 1)
        self.assertNotEqual(code, 0)
        self.assertIn("already exists", self.out.getvalue())

        code = keys.generate(self.ctx, "system", self.path("k"), 1, force=True)
        self.assertEqual(code, 0)

    def test_sign_rejects_key_id_zero(self):
        # F-19: id 0 means "no key" to the device -- signing with it used to succeed and
        # produce a container that would be rejected on the device with no clue why.
        keys.generate(self.ctx, "system", self.path("k"), 1)
        img_path = self.path("bootpkg.bin")
        open(img_path, "wb").write(make_image())
        code = keys.sign(self.ctx, self.path("k", "system.key"), 0, img_path, None)
        self.assertNotEqual(code, 0)
        self.assertIn("no key", self.out.getvalue())

        # Omitting --key-id (None, "leave as is") on an UNSIGNED image is the same danger:
        # the header's id is still 0 (make_image()'s own placeholder), so this must refuse
        # too, not just the case where --key-id 0 was typed explicitly.
        code = keys.sign(self.ctx, self.path("k", "system.key"), None, img_path, None)
        self.assertNotEqual(code, 0)

    def test_verify_catches_tampered_payload(self):
        keys.generate(self.ctx, "system", self.path("k"), 1)
        img_path = self.path("bootpkg.bin")
        open(img_path, "wb").write(make_image())
        keys.sign(self.ctx, self.path("k", "system.key"), 1, img_path, None)

        data = bytearray(open(img_path, "rb").read())
        data[-1] ^= 0xFF                          # a payload byte, not the header
        open(img_path, "wb").write(data)
        code = keys.verify(self.ctx, self.path("k", "system.pub"), img_path)
        self.assertNotEqual(code, 0)
        self.assertIn("hash does not match", self.out.getvalue())

    def test_verify_rejects_the_wrong_key(self):
        keys.generate(self.ctx, "system", self.path("k"), 1)
        keys.generate(self.ctx, "owner", self.path("k"), 2)
        img_path = self.path("bootpkg.bin")
        open(img_path, "wb").write(make_image())
        keys.sign(self.ctx, self.path("k", "system.key"), 1, img_path, None)

        code = keys.verify(self.ctx, self.path("k", "owner.pub"), img_path)
        self.assertNotEqual(code, 0)
        self.assertIn("does not verify", self.out.getvalue())

    def test_verify_rejects_a_non_image(self):
        keys.generate(self.ctx, "system", self.path("k"), 1)
        bogus = self.path("bogus.bin")
        open(bogus, "wb").write(b"not a purr image at all")
        code = keys.verify(self.ctx, self.path("k", "system.pub"), bogus)
        self.assertNotEqual(code, 0)

    def test_sign_rejects_bad_payload_bounds(self):
        keys.generate(self.ctx, "system", self.path("k"), 1)
        img = make_image(payload=b"x" * 10)
        # Claim a payload far larger than the file actually has, by index (11 = payload_size
        # in HEADER_FORMAT order), not a hand-counted byte offset.
        fields = list(keys.parse_header(img))
        fields[11] = 999999
        bad = struct.pack(keys.HEADER_FORMAT, *fields) + img[keys.HEADER_SIZE:]
        img_path = self.path("bad.bin")
        open(img_path, "wb").write(bad)
        code = keys.sign(self.ctx, self.path("k", "system.key"), 1, img_path, None)
        self.assertNotEqual(code, 0)

    def test_inspect_purr_image(self):
        img_path = self.path("bootpkg.bin")
        open(img_path, "wb").write(make_image(name=b"bootpkg", version=b"0.1.0"))
        code = keys.inspect(self.ctx, img_path)
        self.assertEqual(code, 0)
        out = self.out.getvalue()
        self.assertIn("bootpkg 0.1.0", out)
        self.assertIn("unsigned", out)

    def test_inspect_public_key(self):
        keys.generate(self.ctx, "system", self.path("k"), 1)
        code = keys.inspect(self.ctx, self.path("k", "system.pub"))
        self.assertEqual(code, 0)
        self.assertIn("fingerprint", self.out.getvalue())

    def test_inspect_unknown_file(self):
        p = self.path("junk.bin")
        open(p, "wb").write(b"not anything this tool knows about")
        code = keys.inspect(self.ctx, p)
        self.assertNotEqual(code, 0)

    def test_export_public_rejects_bad_key_id(self):
        keys.generate(self.ctx, "system", self.path("k"), 1)
        code = keys.export_public(self.ctx, self.path("k", "system.pub"), "system", 99, self.path("k"))
        self.assertNotEqual(code, 0)


if __name__ == "__main__":
    unittest.main()
