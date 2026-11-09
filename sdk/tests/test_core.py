import os
import unittest

import helpers
from purrsdk import brand, configs, inspector, keybag, minitoml, p256


class Toml(unittest.TestCase):
    def test_round_trip(self):
        d = {"a": 1, "brand": {"x": "hi \"there\"", "n": 7, "ok": True, "arr": [1, 2]}, "device": [{"name": "a", "pin": 3}, {"name": "b"}]}
        self.assertEqual(minitoml.loads(minitoml.dumps(d)), d)

    def test_comments_and_hex(self):
        self.assertEqual(minitoml.loads('x = 0x10  # sixteen\n[t]\ny = "a#b"\n'), {"x": 16, "t": {"y": "a#b"}})

    def test_error_names_the_line(self):
        with self.assertRaises(minitoml.TomlError) as cm:
            minitoml.loads("good = 1\nbad line\n")
        self.assertIn("line 2", str(cm.exception))


@unittest.skipUnless(helpers.HAVE_CRYPTO, "needs the cryptography package")
class Signatures(unittest.TestCase):
    def test_p256_agrees_with_cryptography(self):
        from cryptography.hazmat.primitives import hashes
        from cryptography.hazmat.primitives.asymmetric import ec, utils
        k = helpers.Key()
        der = k.priv.sign(b"msg", ec.ECDSA(hashes.SHA256()))
        r, s = utils.decode_dss_signature(der)
        sig = r.to_bytes(32, "big") + s.to_bytes(32, "big")
        self.assertTrue(p256.verify(k.pub, b"msg", sig))
        self.assertFalse(p256.verify(k.pub, b"other", sig))
        self.assertFalse(p256.verify(k.pub, b"msg", sig[:-1] + bytes([sig[-1] ^ 1])))
        self.assertFalse(p256.verify(k.pub[:-1] + bytes([k.pub[-1] ^ 1]), b"msg", sig))   # not on the curve

    def inspect(self, data, key=None, role="developer", kid=4, name="x.cat"):
        d = helpers.tmpdir()
        path = os.path.join(d, name)
        with open(path, "wb") as fh:
            fh.write(data)
        kd = helpers.keybag_dir(role, kid, key) if key else None
        return inspector.inspect_path(path, [kd] if kd else [])

    def status(self, rep, name):
        return next(c["status"] for c in rep["checks"] if c["name"] == name)

    def test_signed_driver_is_good(self):
        k = helpers.Key()
        rep = self.inspect(k.sign_image(helpers.make(), 4), k)
        self.assertEqual(self.status(rep, "Signature"), "ok")
        self.assertEqual(self.status(rep, "Payload hash"), "ok")

    def test_unsigned_is_a_warning(self):
        rep = self.inspect(helpers.make())
        self.assertEqual(self.status(rep, "Signature"), "warn")

    def test_tampered_payload_is_bad(self):
        k = helpers.Key()
        data = bytearray(k.sign_image(helpers.make(), 4))
        data[-1] ^= 0xFF
        rep = self.inspect(bytes(data), k)
        self.assertEqual(self.status(rep, "Payload hash"), "bad")

    def test_tampered_header_is_bad(self):
        k = helpers.Key()
        data = bytearray(k.sign_image(helpers.make(), 4))
        data[8] ^= 0x01                                    # a byte of the signed header (the chip id)
        rep = self.inspect(bytes(data), k)
        self.assertEqual(self.status(rep, "Signature"), "bad")

    def test_wrong_role_is_bad(self):
        k = helpers.Key()
        kernel = helpers.make(name="kernel", subtype=1)    # a kernel: boot role only
        rep = self.inspect(k.sign_image(kernel, 4), k, role="developer")
        self.assertEqual(self.status(rep, "Signature"), "bad")
        self.assertIn("may not sign", [c for c in rep["checks"] if c["name"] == "Signature"][0]["text"])

    def test_unknown_key_is_information(self):
        k = helpers.Key()
        rep = self.inspect(k.sign_image(helpers.make(), 9))
        self.assertEqual(self.status(rep, "Signature"), "info")

    def test_flash_image_parts(self):
        import struct
        k = helpers.Key()
        flash = bytearray(b"\xff" * 0x40000)
        flash[0] = 0xE9
        entry = lambda name, t, st, off, size: b"\xaa\x50" + struct.pack("<BBII", t, st, off, size) + name.encode().ljust(16, b"\0") + b"\0" * 4
        table = entry("nvs", 1, 2, 0xB000, 0x4000) + entry("bootpkg", 1, 0x40, 0x12000, 0xE000)
        flash[0xA000:0xA000 + len(table)] = table
        pkg = k.sign_image(helpers.make(name="bootpkg", subtype=6), 1)
        flash[0x12000:0x12000 + len(pkg)] = pkg
        d = helpers.tmpdir()
        path = os.path.join(d, "flash.bin")
        open(path, "wb").write(bytes(flash))
        rep = inspector.inspect_path(path, [helpers.keybag_dir("boot", 1, k)])
        self.assertEqual(rep["kind"], "flash-image")
        names = [p["title"] for p in rep["parts"]]
        self.assertEqual(names, ["bootloader", "table", "nvs", "bootpkg"])
        boot = next(p for p in rep["parts"] if p["title"] == "bootpkg")
        self.assertEqual(self.status(boot, "Signature"), "ok")


class Brand(unittest.TestCase):
    def test_defaults_are_the_stock_boot_screens(self):
        c = brand.DEFAULT["colours"]
        self.assertEqual([brand.rgb565(c[k]) for k in ("accent", "text", "dim", "ok")], [0xFDA0, 0xFFFF, 0x94B2, 0x564F])
        self.assertEqual(brand.check(brand.merge({})), [])

    def test_problems_are_said_plainly(self):
        b = brand.merge({"brand": {"logo": 'A"B', "product": "x" * 40}, "colours": {"accent": "red"}, "shell": {"accent": "Chartreuse"}})
        fields = {i["field"] for i in brand.check(b)}
        self.assertTrue({"brand.logo", "brand.product", "colours.accent", "shell.accent"} <= fields)
        with self.assertRaises(ValueError):
            brand.header(b)

    def test_header(self):
        b = brand.merge({"brand": {"product": "Moon OS", "logo": "MOON OS!", "logo_accent_from": -1}, "shell": {"accent": "Teal"}})
        h = brand.header(b)
        self.assertIn('#define PURR_BRAND_PRODUCT "Moon OS"', h)
        self.assertIn("#define PURR_BRAND_ACCENT_INDEX 3", h)
        self.assertIn("#define PKG_BRAND_LOGO_ACCENT_FROM 1000", h)


class Configs(unittest.TestCase):
    def test_setting_values(self):
        self.assertIsNone(configs.validate_setting("display.brightness", "128"))
        self.assertIn("1 to 255", configs.validate_setting("display.brightness", "999"))
        self.assertIsNone(configs.validate_setting("power.screen_timeout", "never"))
        self.assertIsNone(configs.validate_setting("power.screen_timeout", "60"))
        self.assertIsNotNone(configs.validate_setting("theme.base", "purple"))
        self.assertIsNone(configs.validate_setting("theme.accent", "Teal"))
        self.assertIn("do not know", configs.validate_setting("no.such", "1"))

    def test_apps_conf_round_trip(self):
        c = configs.parse_apps_conf("general:max_apps = 4\nrule = background, name:sync\nexec-boot = sync\nnonsense\n")
        self.assertEqual(c["general"], {"max_apps": 4})
        self.assertEqual(len(c["errors"]), 1)
        again = configs.parse_apps_conf(configs.dump_apps_conf(c))
        self.assertEqual((again["general"], again["rules"], again["exec_boot"]), (c["general"], c["rules"], c["exec_boot"]))


class KeyBag(unittest.TestCase):
    def test_roles(self):
        from purrsdk import formats
        img = formats.image()
        self.assertEqual(keybag.allowed_roles(img.IMG_MODULE, img.MOD_KERNEL), ["boot"])
        self.assertIn("developer", keybag.allowed_roles(img.IMG_APP, 0))


if __name__ == "__main__":
    unittest.main()
