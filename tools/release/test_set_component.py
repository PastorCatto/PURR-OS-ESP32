"""set_component.py: one component's file and manifest line are swapped, nothing else changes."""
import hashlib
import os
import subprocess
import sys
import tempfile
import unittest

TOOL = os.path.join(os.path.dirname(os.path.abspath(__file__)), "set_component.py")


def stanza(name, board, fn, data, version="1.0"):
    return "component=%s\nversion=%s\nboard=%s\nfile=%s\nsize=%d\nsha256=%s\nkey=boot\n" % (name, version, board, fn, len(data), hashlib.sha256(data).hexdigest())


class Swap(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.mkdtemp()
        self.old = b"old bootloader"
        self.k = b"kernel"
        for fn, data in (("bootloader-tdeck.aaaaaaaaaa.cat", self.old), ("kernel-tdeck.bbbbbbbbbb.cat", self.k)):
            open(os.path.join(self.d, fn), "wb").write(data)
        open(os.path.join(self.d, "recovery.manifest"), "w").write("release=1.1\nreleased=2026-01-01\nwithdrawn_versions=*@1.0.24\n\n" + stanza("bootloader", "tdeck", "bootloader-tdeck.aaaaaaaaaa.cat", self.old, "0.4.0") + "\n"
                                                                    + stanza("kernel", "tdeck", "kernel-tdeck.bbbbbbbbbb.cat", self.k, "1.1"))
        self.new = os.path.join(tempfile.mkdtemp(), "new.cat")
        open(self.new, "wb").write(b"a newer bootloader")

    def run_tool(self, board="tdeck", comp="bootloader"):
        return subprocess.run([sys.executable, TOOL, "--folder", self.d, "--component", comp, "--board", board, "--file", self.new, "--version", "0.5.0"],
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True)

    def test_the_line_the_name_and_the_file_follow(self):
        r = self.run_tool()
        self.assertEqual(r.returncode, 0, r.stdout)
        digest = hashlib.sha256(b"a newer bootloader").hexdigest()
        name = "bootloader-tdeck.%s.cat" % digest[:10]
        self.assertTrue(os.path.isfile(os.path.join(self.d, name)))
        self.assertFalse(os.path.exists(os.path.join(self.d, "bootloader-tdeck.aaaaaaaaaa.cat")))
        text = open(os.path.join(self.d, "recovery.manifest")).read()
        self.assertIn("version=0.5.0", text)
        self.assertIn("file=%s" % name, text)
        self.assertIn("sha256=%s" % digest, text)
        self.assertIn("size=%d" % len(b"a newer bootloader"), text)
        self.assertIn("withdrawn_versions=*@1.0.24", text)
        self.assertTrue(os.path.isfile(os.path.join(self.d, "kernel-tdeck.bbbbbbbbbb.cat")))       # the others are untouched
        self.assertIn("version=1.1", text)

    def test_an_unknown_component_or_board_is_refused(self):
        self.assertNotEqual(self.run_tool(board="tdeck_plus").returncode, 0)
        self.assertNotEqual(self.run_tool(comp="loader").returncode, 0)


if __name__ == "__main__":
    unittest.main()
