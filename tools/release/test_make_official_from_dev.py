"""make_official_from_dev.py on a small fake dev-server folder (offline, nothing signed)."""
import hashlib
import os
import subprocess
import sys
import tempfile
import unittest

TOOL = os.path.join(os.path.dirname(os.path.abspath(__file__)), "make_official_from_dev.py")


def stanza(name, version, data, board="tdeck_plus", typ=None):
    s = "component=%s\n" % name
    if typ:
        s += "type=%s\n" % typ
    return s + "version=%s\nboard=%s\nfile=%s-%s.cat\nsize=%d\nsha256=%s\nkey=boot\n" % (
        version, board, name, board, len(data), hashlib.sha256(data).hexdigest())


class Build(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.src = os.path.join(self.tmp, "recovery")
        os.makedirs(self.src)
        self.files = {"kernel": b"K" * 100, "about": b"A" * 10, "bme280": b"B" * 20}
        for n, d in self.files.items():
            with open(os.path.join(self.src, "%s-tdeck_plus.cat" % n), "wb") as fh:
                fh.write(d)
        head = "release=dev-1\nreleased=2026-01-01\nwithdrawn_versions=*@1.0.24\nwithdrawn_reason=a reason\n\n"
        self.write("recovery.manifest", head + stanza("kernel", "1.0.27", self.files["kernel"]))
        self.write("modules.manifest", head + stanza("about", "0.1.2", self.files["about"], typ="module"))
        self.write("drivers.manifest", "release=drivers-1\nreleased=2026-01-01\n\n" + stanza("bme280", "1.0.0", self.files["bme280"], typ="driver"))
        self.write("legal.txt", "legal")

    def write(self, name, text):
        with open(os.path.join(self.src, name), "w") as fh:
            fh.write(text)

    def run_tool(self, out):
        return subprocess.run([sys.executable, TOOL, "--release", "1.0.27", "--out", out, "--src", self.src, "--offline"],
                              capture_output=True, text=True)

    def test_builds_both_folders_and_keeps_the_withdrawn_lines(self):
        out = os.path.join(self.tmp, "out")
        r = self.run_tool(out)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        rec = open(os.path.join(out, "JumpingJaguar1", "recovery.manifest")).read()
        self.assertTrue(rec.startswith("release=1.0.27+tdeck_plus\n"))
        self.assertIn("withdrawn_versions=*@1.0.24", rec)
        self.assertTrue(os.path.exists(os.path.join(out, "JumpingJaguar1", "kernel-tdeck_plus.cat")))
        self.assertTrue(os.path.exists(os.path.join(out, "JumpingJaguar2", "about-tdeck_plus.cat")))
        drv = open(os.path.join(out, "JumpingJaguar2", "drivers.manifest")).read()
        self.assertTrue(drv.startswith("release=drivers-1\n"))
        self.assertTrue(os.path.exists(os.path.join(out, "SHA256SUMS.txt")))

    def test_a_changed_file_is_refused(self):
        with open(os.path.join(self.src, "kernel-tdeck_plus.cat"), "ab") as fh:
            fh.write(b"x")
        r = self.run_tool(os.path.join(self.tmp, "out2"))
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("does not match", r.stdout + r.stderr)

    def test_an_existing_output_folder_is_refused(self):
        os.makedirs(os.path.join(self.tmp, "out3"))
        self.assertNotEqual(self.run_tool(os.path.join(self.tmp, "out3")).returncode, 0)


if __name__ == "__main__":
    unittest.main()
