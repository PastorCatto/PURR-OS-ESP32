import json
import os
import unittest

import helpers
from purrsdk import keybag, repo


@unittest.skipUnless(helpers.HAVE_CRYPTO, "needs the cryptography package")
class Repository(unittest.TestCase):
    def make(self):
        d = helpers.tmpdir()
        folder = os.path.join(d, "r")
        info = repo.init(folder, "Test Apps", "me", "https://example.org/x", "secretpw", "for tests")
        k = helpers.Key()
        pkg = os.path.join(d, "clock.cat")
        with open(pkg, "wb") as fh:
            fh.write(k.sign_image(helpers.make(name="clock", version="1.2.0", image_type=4, subtype=0), 4))
        repo.add(folder, pkg, title="Clock", desc="Tells the time", category="tools", source="https://example.org/clock", perms=["keep-awake"])
        asset = os.path.join(d, "mono.bin")
        with open(asset, "wb") as fh:
            fh.write(b"font data" * 20)
        repo.add(folder, asset, name="mono-font", version="1.0", type_="asset", dest="fonts/mono.bin", target="any", title="Mono font")
        return d, folder, info

    def test_build_flat_and_verify(self):
        d, folder, info = self.make()
        out = os.path.join(d, "out")
        r = repo.build(folder, out, "secretpw")
        self.assertEqual(r["apps"], 2)
        self.assertIn("index.json.sig", r["files"])
        self.assertIn("clock-1.2.0.cat", r["files"])
        v = repo.verify(out)
        self.assertTrue(v["ok"], v["problems"])
        self.assertEqual((v["tier"], v["layout"], v["id"]), ("community", "flat", info["id"]))
        idx = json.load(open(os.path.join(out, "index.json")))
        self.assertEqual(idx["format"], 2)
        clock = next(a for a in idx["apps"] if a["name"] == "clock")
        self.assertEqual((clock["type"], clock["signed"], clock["key_id"], clock["perms"]), ("app", True, 4, ["keep-awake"]))
        font = next(a for a in idx["apps"] if a["name"] == "mono-font")
        self.assertEqual((font["type"], font["dest"], font["target"]), ("asset", "fonts/mono.bin", "any"))

    def test_tree_layout(self):
        d, folder, _ = self.make()
        out = os.path.join(d, "tree")
        repo.build(folder, out, "secretpw", "tree")
        self.assertTrue(os.path.isfile(os.path.join(out, "purr", "v1", "apps", "clock", "1.2.0", "package.cat")))
        v = repo.verify(out)
        self.assertTrue(v["ok"], v["problems"])
        self.assertEqual(v["layout"], "tree")

    def test_tampering_is_found(self):
        d, folder, _ = self.make()
        out = os.path.join(d, "out")
        repo.build(folder, out, "secretpw")
        with open(os.path.join(out, "clock-1.2.0.cat"), "r+b") as fh:
            fh.seek(-1, 2)
            b = fh.read(1)
            fh.seek(-1, 2)
            fh.write(bytes([b[0] ^ 1]))
        self.assertIn("does not match", " ".join(repo.verify(out)["problems"]))
        d2, folder2, _ = self.make()
        out2 = os.path.join(d2, "out")
        repo.build(folder2, out2, "secretpw")
        idx = open(os.path.join(out2, "index.json")).read().replace("Tells the time", "Tells the tyme")
        open(os.path.join(out2, "index.json"), "w").write(idx)
        self.assertIn("index.json is not signed", " ".join(repo.verify(out2)["problems"]))

    def test_endorsement_makes_it_official(self):
        d, folder, info = self.make()
        sysdir = os.path.join(d, "sys")
        os.makedirs(sysdir)
        sysk = helpers.Key()
        from cryptography.hazmat.primitives import serialization
        with open(os.path.join(sysdir, "system.key"), "wb") as fh:
            fh.write(sysk.priv.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.BestAvailableEncryption(b"syspw1")))
        e = repo.endorse(folder, os.path.join(sysdir, "system.key"), 2, "syspw1")
        self.assertEqual(e["repo_id"], info["id"])
        out = os.path.join(d, "out")
        repo.build(folder, out, "secretpw")
        v = repo.verify(out, [helpers.keybag_dir("system", 2, sysk)])
        self.assertEqual(v["tier"], "official", v["problems"])
        v2 = repo.verify(out, [])                                   # a device that does not know that key
        self.assertFalse(v2["ok"])
        self.assertIn("endorsement", " ".join(v2["problems"]))

    def test_a_signed_shell_is_an_item_and_nothing_else_is(self):
        d, folder, _ = self.make()
        k = helpers.Key()
        shell = os.path.join(d, "w10m.cat")
        with open(shell, "wb") as fh:
            fh.write(k.sign_image(helpers.make(name="w10m", version="1.0.32", image_type=3, subtype=repo.MOD_SHELL), 1))
        item = repo.add(folder, shell, type_="shell", title="Windows 10 Mobile", category="w10m", target="flash")
        self.assertEqual((item["type"], item["dest"]), ("shell", "shell.cat"))            # the destination is fixed
        out = os.path.join(d, "tree")
        repo.build(folder, out, "secretpw", "tree")
        self.assertTrue(os.path.isfile(os.path.join(out, "purr", "v1", "shells", "w10m", "1.0.32", "shell.cat")))
        v = repo.verify(out)
        self.assertTrue(v["ok"], v["problems"])
        idx = json.load(open(os.path.join(out, "purr", "v1", "index.json")))
        w = next(a for a in idx["apps"] if a["name"] == "w10m")
        self.assertEqual((w["type"], w["dest"], w["category"]), ("shell", "shell.cat", "w10m"))
        unsigned = os.path.join(d, "u.cat")
        with open(unsigned, "wb") as fh:
            fh.write(helpers.make(name="u", version="1", image_type=3, subtype=repo.MOD_SHELL))
        with self.assertRaises(repo.RepoError):
            repo.add(folder, unsigned, type_="shell")                                       # a device would never load it
        driver = os.path.join(d, "drv.cat")
        with open(driver, "wb") as fh:
            fh.write(k.sign_image(helpers.make(name="drv", version="1", image_type=3, subtype=5), 1))
        with self.assertRaises(repo.RepoError):
            repo.add(folder, driver, type_="shell")                                         # a driver is not a shell
        with self.assertRaises(repo.RepoError):
            repo.add(folder, shell, type_="shell", dest="somewhere/else.cat", name="w2", version="1")
        with self.assertRaises(repo.RepoError):
            repo.add(folder, driver)                                                        # and a module still cannot be added as an ordinary item

    def test_bad_input_is_explained(self):
        d, folder, _ = self.make()
        with self.assertRaises(repo.RepoError):
            repo.add(folder, os.path.join(d, "mono.bin"), type_="asset", dest="../etc/passwd", version="1")
        with self.assertRaises(repo.RepoError):
            repo.build(folder, os.path.join(d, "o"), "wrongpassword")


@unittest.skipUnless(helpers.HAVE_CRYPTO, "needs the cryptography package")
class MovingTheAddress(unittest.TestCase):
    OLD = "https://github.com/PastorCatto/PURR-OS-ESP32/releases/download/JumpingJaguar1/recovery.manifest"
    NEW_R = "https://example.org/hub/os/stable/recovery.manifest"
    NEW_M = "https://example.org/hub/os/stable/modules.manifest"

    def key(self):
        d = helpers.tmpdir()
        k = helpers.Key()
        from cryptography.hazmat.primitives import serialization
        path = os.path.join(d, "boot.key")
        with open(path, "wb") as fh:
            fh.write(k.priv.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.BestAvailableEncryption(b"bootpw1")))
        return d, k, path

    def test_the_file_is_written_and_signed(self):
        d, k, path = self.key()
        text = repo.move(os.path.join(d, "out"), self.OLD, self.NEW_R, self.NEW_M, path, 1, "bootpw1", 3, "Updates now come from the PURR hub.")
        self.assertEqual(open(os.path.join(d, "out", "moved.txt")).read(), text)
        self.assertEqual(text.splitlines()[:3], ["moved=1", "seq=3", "key_id=1"])
        sig = bytes.fromhex(open(os.path.join(d, "out", "moved.txt.sig")).read())
        self.assertEqual(len(sig), 64)
        from cryptography.hazmat.primitives import hashes
        from cryptography.hazmat.primitives.asymmetric import ec, utils
        r, s = int.from_bytes(sig[:32], "big"), int.from_bytes(sig[32:], "big")
        k.priv.public_key().verify(utils.encode_dss_signature(r, s), text.encode(), ec.ECDSA(hashes.SHA256()))

    def test_bad_input_is_refused(self):
        d, k, path = self.key()
        out = os.path.join(d, "out")
        for args in (("http://example.org/x/recovery.manifest", self.NEW_M), (self.NEW_R, "https://example.org/hub/os/stable/index.json"),
                     ("https://example.org/a b/recovery.manifest", self.NEW_M)):
            with self.assertRaises(repo.RepoError):
                repo.move(out, self.OLD, args[0], args[1], path, 1, "bootpw1", 1)
        with self.assertRaises(repo.RepoError):
            repo.move(out, self.OLD, self.NEW_R, self.NEW_M, path, 1, "bootpw1", 0)
        with self.assertRaises(repo.RepoError):
            repo.move(out, self.OLD, self.NEW_R, self.NEW_M, path, 1, "bootpw1", 1, "no\nnewlines")
        with self.assertRaises(repo.RepoError):
            repo.move(out, self.OLD, self.NEW_R, self.NEW_M, path, 1, "wrong password", 1)
        self.assertFalse(os.path.exists(os.path.join(out, "moved.txt")))


if __name__ == "__main__":
    unittest.main()
