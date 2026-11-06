import os
import subprocess
import unittest

import helpers
from purrsdk import boards, minitoml, templates

REF = os.path.join(helpers.SDK, "boards", "tdeck_plus.toml")


def compiles(path):
    cc = helpers.clang()
    cmd = [cc, "-fsyntax-only", "-Wall", "-Wextra", "-Werror", "-std=gnu11"] + ["-I" + i for i in helpers.INCLUDES] + [path]
    return subprocess.run(cmd, capture_output=True, text=True)


@unittest.skipUnless(helpers.clang(), "needs a C compiler")
class Compiles(unittest.TestCase):
    def test_every_driver_template_compiles(self):
        d = helpers.tmpdir()
        for cls in templates.CLASSES:
            src, entry, compat, bus = templates.driver_source("demo-" + cls.split("-")[0], cls)
            p = os.path.join(d, cls + ".c")
            open(p, "w").write(src)
            r = compiles(p)
            self.assertEqual(r.returncode, 0, cls + ": " + r.stderr)

    def test_generated_board_file_compiles(self):
        b = boards.load(REF)
        p = os.path.join(helpers.tmpdir(), "tdeck_plus.c")
        open(p, "w").write(boards.to_c(b))
        r = compiles(p)
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_generated_board_file_matches_the_real_one(self):
        """The core numbers of the generated file are the ones in the hand-written kernel/boards/tdeck_plus.c."""
        import re
        real = open(os.path.join(helpers.ROOT, "PurrOS", "components", "kernel", "boards", "tdeck_plus.c")).read()
        gen = boards.to_c(boards.load(REF))
        for needle in (".power_pin = 10", ".mosi = 41", ".miso = 38", ".sclk = 40", ".cs = 12", ".dc = 11", ".backlight = 42", ".width = 320", ".madctl = 0x70",
                       ".sda = 18", ".scl = 8", ".addr = 0x55", ".rst_pin = 17", ".native_x_max = 311", ".native_y_max = 235"):
            self.assertIn(needle, gen, needle)
            self.assertIn(needle, re.sub(r"\s+", " ", real) if False else real, needle)


class Boards(unittest.TestCase):
    def test_reference_board_is_clean(self):
        b = boards.load(REF)
        self.assertEqual([i for i in boards.check(b) if i["level"] == "bad"], [])
        cov = {c["class"]: c["status"] for c in boards.coverage(b)}
        self.assertEqual(cov["gnss"], "written")
        self.assertEqual(cov["lora"], "written")
        self.assertEqual(cov["display"], "works")

    def test_mistakes_are_found(self):
        b = boards.load(REF)
        b["device"][1]["tx"] = 26                          # a flash pin
        b["device"][2]["cs"] = 12                          # the display's chip select
        text = " ".join(i["text"] for i in boards.check(b) if i["level"] == "bad")
        self.assertIn("flash", text)
        self.assertIn("more than one part", text)

    def test_profile_has_the_devices_the_os_reads(self):
        text, n = boards.to_profile(boards.load(REF))
        self.assertIn("device=gps0\ncompatible=gnss-nmea", text)
        self.assertIn("bus=uart", text)
        self.assertIn("device=trackball\ncompatible=trackball-gpio", text)
        self.assertIn("device=sd0\ncompatible=sd-spi", text)
        self.assertIn("device=kbd-backlight\n", text)
        self.assertIn("device=kbdraw\n", text)
        self.assertNotIn("speaker", text)                # templates (no driver yet) are not written
        self.assertEqual(n, 6)


class Templates(unittest.TestCase):
    def test_names_are_checked(self):
        with self.assertRaises(ValueError):
            templates.driver_source("Bad Name", "gnss")
        with self.assertRaises(ValueError):
            templates.driver_source("fine", "nonsense")


if __name__ == "__main__":
    unittest.main()
