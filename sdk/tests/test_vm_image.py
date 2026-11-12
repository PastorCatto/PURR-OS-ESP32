"""Putting a built image where Run and the emulator look for it: the kit's images/vm-flash.bin."""
import os
import shutil
import tempfile
import time
import unittest
from unittest import mock

import helpers  # noqa: F401  (puts the sdk folder on sys.path)

from purrsdk import emulator, workspace


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="purrsdk-test-")
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.kit = os.path.join(self.tmp, "kit")
        os.makedirs(self.kit)
        with open(os.path.join(self.kit, "SDK-MANIFEST.json"), "w") as fh:
            fh.write("{}")
        patch = mock.patch.object(workspace, "kit_root", lambda: self.kit)
        patch.start()
        self.addCleanup(patch.stop)
        self.ws = workspace.Workspace(os.path.join(self.tmp, "w")).ensure()
        self.dst = os.path.join(self.kit, "images", "vm-flash.bin")

    def image(self, system, data, mtime=None):
        folder = os.path.join(self.ws.builds, "system-" + system)
        os.makedirs(folder, exist_ok=True)
        path = os.path.join(folder, "%s-tdeck_plus.img" % system)
        with open(path, "wb") as fh:
            fh.write(data)
        if mtime:
            os.utime(path, (mtime, mtime))
        return path


class Latest(Base):
    def test_none_when_nothing_was_built(self):
        self.assertIsNone(emulator.latest_built_image(self.ws))

    def test_the_newest_image_wins(self):
        now = time.time()
        self.image("Old", b"o", now - 500)
        new = self.image("New", b"n", now)
        os.makedirs(os.path.join(self.ws.builds, "not-a-system"))          # other folders in builds/ are ignored
        self.assertEqual(emulator.latest_built_image(self.ws), new)


class Adopt(Base):
    def test_places_a_copy_and_the_emulator_finds_it(self):
        src = self.image("A", b"image-A")
        self.assertEqual(emulator.adopt_image(src)["status"], "placed")
        with open(self.dst, "rb") as fh:
            self.assertEqual(fh.read(), b"image-A")
        self.assertTrue(os.path.isfile(src))                                # a copy: the build's own image stays
        with mock.patch.dict(os.environ, {}, clear=False):
            os.environ.pop("PURR_VM_IMAGE", None)
            self.assertEqual(emulator.vm_image(), self.dst)

    def test_a_newer_build_replaces_our_own_earlier_copy(self):
        emulator.adopt_image(self.image("A", b"one"))
        self.assertEqual(emulator.adopt_image(self.image("B", b"two"))["status"], "placed")
        with open(self.dst, "rb") as fh:
            self.assertEqual(fh.read(), b"two")

    def test_an_image_we_did_not_place_is_kept_unless_forced(self):
        os.makedirs(os.path.dirname(self.dst))
        with open(self.dst, "wb") as fh:
            fh.write(b"shipped")
        src = self.image("A", b"mine")
        res = emulator.adopt_image(src)
        self.assertEqual(res["status"], "kept")
        with open(self.dst, "rb") as fh:
            self.assertEqual(fh.read(), b"shipped")
        self.assertEqual(emulator.adopt_image(src, force=True)["status"], "placed")
        with open(self.dst, "rb") as fh:
            self.assertEqual(fh.read(), b"mine")

    def test_not_a_kit_and_missing_source_are_reported_not_raised(self):
        os.remove(os.path.join(self.kit, "SDK-MANIFEST.json"))
        res = emulator.adopt_image(self.image("A", b"x"))
        self.assertEqual(res["status"], "unavailable")
        self.assertFalse(os.path.exists(self.dst))
        os.makedirs(os.path.dirname(self.dst), exist_ok=True)
        with open(os.path.join(self.kit, "SDK-MANIFEST.json"), "w") as fh:
            fh.write("{}")
        self.assertEqual(emulator.adopt_image(os.path.join(self.tmp, "nope.img"))["status"], "unavailable")


if __name__ == "__main__":
    unittest.main()
