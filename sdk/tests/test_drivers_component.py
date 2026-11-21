"""The `drivers` component of a release (OTA/SPEC.md) and the compatibility rule that makes updating it safe: a newer kernel serves every older driver."""
import os
import re
import sys
import tempfile
import unittest

import helpers  # noqa: F401  (puts the SDK on the path)

from purrsdk import drivers, release, runner

sys.path.insert(0, os.path.join(helpers.ROOT, "purrstrap", "scripts"))
sys.path.insert(0, os.path.join(helpers.ROOT, "purrstrap"))
import drvpart  # noqa: E402
from lib import image as pimg  # noqa: E402

BUS_H = os.path.join(helpers.ROOT, "PurrOS", "components", "kernel", "include", "purr_driver_bus.h")


def cat(name, version):
    return pimg.make_image(name, version, pimg.IMG_MODULE, pimg.MOD_DRIVER, b"\xe9" * 40, pimg.CHIP_ESP32S3)


def blob_file(folder, name="drivers_tdeck_plus.bin", versions=("1.0.0", "1.4.2")):
    path = os.path.join(folder, name)
    with open(path, "wb") as fh:
        fh.write(drvpart.pack([("st7789.cat", cat("st7789", versions[0])), ("gt911.cat", cat("gt911", versions[1]))]))
    return path


class Compatibility(unittest.TestCase):
    """The rule: the bus table only grows at the end, so the kernel serves every version from ABI_MIN up to its own. Raising ABI_MIN, or lowering the version, would
    leave drivers in the field (and in every released drivers image) without a display. This is the line that stops it."""

    def numbers(self):
        text = open(BUS_H, encoding="utf-8").read()
        return (int(re.search(r"PURR_DRIVER_BUS_ABI_MIN\s+(\d+)u", text).group(1)), int(re.search(r"PURR_DRIVER_BUS_ABI_VERSION\s+(\d+)u", text).group(1)))

    def test_the_floor_never_rises_and_the_version_never_falls(self):
        floor, version = self.numbers()
        self.assertLessEqual(floor, 4, "PURR_DRIVER_BUS_ABI_MIN was 4 in every released kernel: raising it breaks the drivers already on devices")
        self.assertGreaterEqual(version, 9, "PURR_DRIVER_BUS_ABI_VERSION only ever goes up")
        self.assertLessEqual(floor, version)

    def test_the_loader_refuses_only_outside_the_range(self):
        src = open(os.path.join(helpers.ROOT, "PurrOS", "main", "commands.c"), encoding="utf-8").read()
        self.assertIn("drv->bus_abi < PURR_DRIVER_BUS_ABI_MIN || drv->bus_abi > PURR_DRIVER_BUS_ABI_VERSION", src)

    def test_every_essential_driver_is_built_against_the_current_table(self):
        for f, (_name, rel, _entry) in drivers.ESSENTIAL.items():
            self.assertIn(".bus_abi = PURR_DRIVER_BUS_ABI_VERSION", open(os.path.join(helpers.ROOT, rel), encoding="utf-8").read(), f)

    def test_the_release_tools_take_the_abi_from_the_same_header(self):
        from scripts import dev
        self.assertEqual(dev._tree_bus_abi(), self.numbers()[1])


class Component(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def test_describe_image(self):
        info = drivers.describe_image(blob_file(self.tmp))
        self.assertEqual(info["files"], ["st7789.cat", "gt911.cat"])
        self.assertEqual(info["version"], "1.4.2")
        bad = os.path.join(self.tmp, "bad.bin")
        raw = bytearray(open(blob_file(self.tmp), "rb").read())
        raw[100] ^= 1
        open(bad, "wb").write(raw)
        with self.assertRaises(drivers.DriverError):
            drivers.describe_image(bad)

    def test_the_release_finds_the_image_under_either_name(self):
        folder = os.path.join(self.tmp, "kit")
        os.makedirs(folder)
        os.environ["PURR_IMAGES"] = folder
        try:
            self.assertNotIn("drivers", release.prebuilt("tdeck_plus"))
            blob_file(folder, "drivers-tdeck_plus.bin")                                   # the image maker's name
            self.assertIn("drivers", release.prebuilt("tdeck_plus"))
            os.remove(os.path.join(folder, "drivers-tdeck_plus.bin"))
            blob_file(folder, "drivers_tdeck_plus.bin")                                   # the release folder's name
            self.assertIn("drivers", release.prebuilt("tdeck_plus"))
            row = next(r for r in release.describe(release.prebuilt("tdeck_plus")) if r["component"] == "drivers")
            self.assertEqual(row["version"], "1.4.2")
            self.assertIn("2 driver(s)", row["signature_text"])
        finally:
            del os.environ["PURR_IMAGES"]

    def test_assemble_hosts_it_without_signing_the_image(self):
        calls = []

        def fake(args, password=None, on_line=None, **kw):
            calls.append([str(a) for a in args])
            return {"rc": 0, "output": ""}

        real = runner.purrstrap
        runner.purrstrap = fake
        try:
            res = release.assemble(os.path.join(self.tmp, "out"), {"drivers": blob_file(self.tmp)}, key="/k/boot.key", key_id=3)
        finally:
            runner.purrstrap = real
        self.assertTrue(res["ok"], res["log"])
        self.assertFalse([c for c in calls if c[:2] == ["keys", "sign"]], "a raw partition image is not a signable container")
        rec = next(c for c in calls if c[0] == "dev")
        self.assertIn("--drivers", rec)


if __name__ == "__main__":
    unittest.main()
