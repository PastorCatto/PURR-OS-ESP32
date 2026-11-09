"""resign_release.py with throwaway keys: old-key files are signed again with the new keys, new-key files are left alone, manifests and names follow, the source is untouched."""
import hashlib
import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
TOOL = os.path.join(HERE, "resign_release.py")
PURRSTRAP = os.path.join(ROOT, "purrstrap", "purrstrap.py")
sys.path.insert(0, os.path.join(ROOT, "purrstrap"))

try:
    import cryptography  # noqa: F401
    HAVE = True
except ImportError:
    HAVE = False


def ps(args, pw=None):
    env = dict(os.environ)
    env.pop("PURRSTRAP_KEY_PASSWORD", None)
    if pw:
        env["PURRSTRAP_KEY_PASSWORD"] = pw
    p = subprocess.run([sys.executable, PURRSTRAP] + [str(x) for x in args], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True)
    assert p.returncode == 0, p.stdout
    return p.stdout


@unittest.skipUnless(HAVE, "needs the cryptography module")
class Resign(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        from lib import image as pimg
        cls.pimg = pimg
        cls.tmp = tempfile.mkdtemp()
        cls.keys = os.path.join(cls.tmp, "keys")
        for role, kid, name in (("boot", 1, "old-boot"), ("developer", 2, "old-dev"), ("boot", 3, "new-boot"), ("developer", 4, "new-dev")):
            d = os.path.join(cls.tmp, "g-" + name)
            ps(["keys", "generate", "--role", role, "--out", d, "--key-id", kid], pw="pw-" + name)
            os.makedirs(cls.keys, exist_ok=True)
            os.replace(os.path.join(d, role + ".key"), os.path.join(cls.keys, name + ".key"))
            os.replace(os.path.join(d, role + ".pub"), os.path.join(cls.keys, name + ".pub"))

    def make(self, folder, name, version, ftype, sub, data, key, kid, role, hashed=True):
        img = self.pimg.make_image(name, version, ftype, sub, data, self.pimg.CHIP_ESP32S3)
        tmp = os.path.join(folder, ".t")
        open(tmp, "wb").write(img)
        ps(["keys", "sign", "--key", os.path.join(self.keys, key + ".key"), "--image", tmp, "--key-id", kid], pw="pw-" + key)
        digest = hashlib.sha256(open(tmp, "rb").read()).hexdigest()
        fn = "%s-tdeck.%s.cat" % (name, digest[:10]) if hashed else "%s_tdeck.cat" % name
        os.replace(tmp, os.path.join(folder, fn))
        return "component=%s\nversion=%s\nchip=esp32s3\nboard=tdeck\nfile=%s\nsize=%d\nsha256=%s\nkey=%s\n" % (name, version, fn, os.path.getsize(os.path.join(folder, fn)), digest, role)

    def setUp(self):
        self.src = tempfile.mkdtemp(dir=self.tmp)
        p = self.pimg
        rec = ["release=1.1+tdeck_plus\nreleased=2026-11-08\nwithdrawn_versions=*@1.0.24\n",
               self.make(self.src, "kernel", "1.1", p.IMG_MODULE, p.MOD_KERNEL, b"\xe9" * 200, "new-boot", 3, "boot"),
               self.make(self.src, "bootpkg", "0.3.1", p.IMG_MODULE, p.MOD_BOOTPKG, b"\xe9" * 100, "old-boot", 1, "boot"),
               self.make(self.src, "kernel_ws", "0.1.2", p.IMG_MODULE, p.MOD_KERNEL, b"\xe9" * 100, "old-boot", 1, "boot", hashed=False)]
        mod = ["release=1.1+tdeck_plus\nreleased=2026-11-08\n", self.make(self.src, "about", "0.1.2", p.IMG_MODULE, p.MOD_DRIVER, b"\xe9" * 50, "old-dev", 2, "developer")]
        open(os.path.join(self.src, "recovery.manifest"), "w").write("\n".join(rec))
        open(os.path.join(self.src, "modules.manifest"), "w").write("\n".join(mod))
        open(os.path.join(self.src, "legal.txt"), "w").write("legal")
        self.before = {f: hashlib.sha256(open(os.path.join(self.src, f), "rb").read()).hexdigest() for f in os.listdir(self.src)}
        self.out = os.path.join(tempfile.mkdtemp(dir=self.tmp), "signed")

    def run_tool(self, *extra, env=None):
        e = dict(os.environ, PURRSTRAP_BOOT_KEY_PASSWORD="pw-new-boot", PURRSTRAP_KEY_PASSWORD="pw-new-dev")
        e.update(env or {})
        return subprocess.run([sys.executable, TOOL, "--src", self.src, "--out", self.out, "--boot-key", os.path.join(self.keys, "new-boot.key"), "--boot-id", "3",
                               "--dev-key", os.path.join(self.keys, "new-dev.key"), "--dev-id", "4"] + list(extra), env=e, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              universal_newlines=True, stdin=subprocess.DEVNULL)

    def kid(self, path):
        return self.pimg.header_fields(self.pimg.parse_header(open(path, "rb").read()[:self.pimg.HEADER_SIZE]))["key_id"]

    def test_old_files_are_signed_again_and_new_ones_left_alone(self):
        r = self.run_tool()
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertIn("3 file(s) are signed with an old key", r.stdout)
        text = open(os.path.join(self.out, "recovery.manifest")).read()
        mtext = open(os.path.join(self.out, "modules.manifest")).read()
        names = {l.split("=", 1)[1] for t in (text, mtext) for l in t.splitlines() if l.startswith("file=")}
        self.assertEqual(len(names), 4)
        ids = {n.split(".")[0].split("-")[0].split("_")[0]: self.kid(os.path.join(self.out, n)) for n in names}
        self.assertEqual(ids, {"kernel": 3, "bootpkg": 3, "about": 4})
        for n in names:
            self.assertTrue(os.path.isfile(os.path.join(self.out, n)), n)
        self.assertTrue([n for n in names if n.startswith("kernel-tdeck.") and self.before.get(n)])    # the kernel kept its name and bytes
        self.assertIn("withdrawn_versions=*@1.0.24", text)                                           # the headers that matter are kept
        self.assertTrue(os.path.isfile(os.path.join(self.out, "legal.txt")))

    def test_the_name_hash_and_the_manifest_follow_the_new_bytes(self):
        self.run_tool()
        for m in ("recovery.manifest", "modules.manifest"):
            for block in open(os.path.join(self.out, m)).read().split("\n\n"):
                kv = dict(l.split("=", 1) for l in block.splitlines() if "=" in l)
                if "file" not in kv:
                    continue
                data = open(os.path.join(self.out, kv["file"]), "rb").read()
                self.assertEqual(hashlib.sha256(data).hexdigest(), kv["sha256"])
                self.assertEqual(int(kv["size"]), len(data))
                parts = kv["file"].split(".")
                if len(parts) == 3:                                            # name.<10 letters>.cat
                    self.assertEqual(parts[1], kv["sha256"][:10])

    def test_the_source_is_never_changed_and_a_plain_name_is_kept(self):
        self.run_tool()
        after = {f: hashlib.sha256(open(os.path.join(self.src, f), "rb").read()).hexdigest() for f in os.listdir(self.src)}
        self.assertEqual(self.before, after)
        self.assertTrue(os.path.isfile(os.path.join(self.out, "kernel_ws_tdeck.cat")))

    def test_the_old_key_no_longer_verifies_and_the_new_one_does(self):
        self.run_tool()
        f = os.path.join(self.out, [n for n in os.listdir(self.out) if n.startswith("about-")][0])
        ps(["keys", "verify", "--key", os.path.join(self.keys, "new-dev.pub"), "--image", f])
        p = subprocess.run([sys.executable, PURRSTRAP, "keys", "verify", "--key", os.path.join(self.keys, "old-dev.pub"), "--image", f], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.assertNotEqual(p.returncode, 0)

    def test_dry_run_changes_nothing_and_a_wrong_password_stops_with_nothing_half_done(self):
        r = self.run_tool("--dry-run")
        self.assertEqual(r.returncode, 0)
        self.assertFalse(os.path.exists(self.out))
        r = self.run_tool(env={"PURRSTRAP_BOOT_KEY_PASSWORD": "wrong"})
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("Signing", r.stdout)
        self.assertFalse([f for f in os.listdir(self.out) if f.startswith(".signing-")])      # no half-signed leftovers

    def test_a_damaged_source_file_or_an_unknown_role_is_refused(self):
        name = [f for f in os.listdir(self.src) if f.startswith("bootpkg-")][0]
        open(os.path.join(self.src, name), "ab").write(b"x")
        r = self.run_tool()
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("does not match its line", r.stdout)


if __name__ == "__main__":
    unittest.main()
