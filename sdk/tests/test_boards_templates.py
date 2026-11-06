import os
import subprocess
import unittest

import helpers
from purrsdk import boards, inspector, minitoml, templates

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


def read_csv(path):
    """A PURR partition CSV as [(name, type, subtype, offset, size)] with numbers."""
    out = []
    for ln in open(path):
        ln = ln.split("#", 1)[0].strip()
        if not ln:
            continue
        name, typ, sub, off, size = [c.strip() for c in ln.split(",")]
        sub = boards.PART_SUBTYPES.get((typ, sub)) if (typ, sub) in boards.PART_SUBTYPES else int(sub, 0)
        out.append((name, boards.PART_TYPES[typ], sub, int(off, 0), int(size, 0)))
    return sorted(out, key=lambda r: r[3])


class FlashLayout(unittest.TestCase):
    """Flash map v2 (1.0.29): the board profile, the build's partition tables and the inspector all say the same thing."""

    def layout(self):
        return boards.flash_layout(boards.load(REF))

    def test_board_profile_is_flash_map_v2(self):
        names = [p["name"] for p in self.layout()]
        self.assertEqual(names, ["nvs", "purrcfg", "phy_init", "bootpkg", "kittenos", "kernel", "loader", "netrec", "root"])
        self.assertNotIn("ui_assets", names)
        root = self.layout()[-1]
        self.assertEqual((root["offset"], root["size"]), (0x5C1000, 0xA3F000))
        slots = {p["name"]: (p["offset"], p["size"]) for p in self.layout()}
        self.assertEqual(slots["kittenos"], (0x20000, 0x1C0000))                 # 1.75 MB, each slot 256 KB larger than in 1.0.28
        self.assertEqual(slots["kernel"], (0x1E0000, 0x220000))                  # 2.125 MB
        self.assertEqual(slots["loader"], (0x400000, 0x1C0000))                  # 1.75 MB
        self.assertEqual(slots["netrec"], (0x5C0000, 0x1000))
        self.assertEqual(root["offset"] + root["size"], 16 << 20)                # root runs to the end of the flash
        self.assertEqual([i for i in boards.check(boards.load(REF)) if i["where"].startswith("partition")], [])

    def test_board_profile_equals_the_build_tables(self):
        mine = [(p["name"], p["type"], p["subtype"], p["offset"], p["size"]) for p in self.layout()]
        for rel in (("PurrOS", "partitions", "tdeck_plus.csv"), ("bootloader", "partitions.csv"), ("Kernel", "partitions.csv")):
            path = os.path.join(helpers.ROOT, *rel)
            if os.path.isfile(path):
                self.assertEqual(read_csv(path), mine, "/".join(rel))

    def test_layout_mistakes_are_found(self):
        b = boards.load(REF)
        b["partition"][-1]["size"] = 0x5FF000                                    # root short of the flash end
        self.assertIn("end of the flash", " ".join(i["text"] for i in boards.check_layout(b) if i["level"] == "warn"))
        b = boards.load(REF)
        b["partition"][5]["size"] = 0x240000                                     # the kernel runs into the loader
        self.assertIn("Overlaps", " ".join(i["text"] for i in boards.check_layout(b)))
        b = boards.load(REF)
        b["partition"].insert(-1, {"name": "ui_assets", "type": "data", "subtype": "0x42", "offset": 0x501000, "size": 0x500000})
        self.assertIn("gone in flash map v2", " ".join(i["text"] for i in boards.check_layout(b)))
        b = boards.load(REF)
        b["partition"][-1]["offset"] += 0x800                                    # not a sector boundary
        self.assertIn("4 KB", " ".join(i["text"] for i in boards.check_layout(b)))

    def table_image(self, rows):
        import struct
        flash = bytearray(b"\xff" * (16 << 20))
        flash[0] = 0xE9
        table = b"".join(b"\xaa\x50" + struct.pack("<BBII", t, st, off, size) + name.encode().ljust(16, b"\0") + b"\0" * 4 for name, t, st, off, size in rows)
        flash[0xA000:0xA000 + len(table)] = table
        path = os.path.join(helpers.tmpdir(), "flash.bin")
        open(path, "wb").write(bytes(flash))
        return path

    def test_inspecting_an_image_checks_its_table_against_the_board(self):
        new = [(p["name"], p["type"], p["subtype"], p["offset"], p["size"]) for p in self.layout()]
        rep = inspector.inspect_path(self.table_image(new))
        lay = [c for c in rep["checks"] if c["name"] == "Layout"]
        self.assertEqual([c["status"] for c in lay], ["ok"])
        self.assertIn("flash map v2", lay[0]["text"])

    def test_an_image_in_the_old_layout_is_flagged(self):
        old = [r for r in read_old_rows()]
        rep = inspector.inspect_path(self.table_image(old))
        lay = [c for c in rep["checks"] if c["name"] == "Layout"]
        self.assertEqual([c["status"] for c in lay], ["warn"])
        self.assertIn("ui_assets", lay[0]["text"])
        self.assertIn("0x5C1000", lay[0]["text"])

    def test_a_table_that_differs_is_named(self):
        rows = [(p["name"], p["type"], p["subtype"], p["offset"], p["size"]) for p in self.layout()]
        rows[-1] = ("root", 1, 0x83, 0x5C1000, 0x5FF000)
        lay = [c for c in inspector.inspect_path(self.table_image(rows))["checks"] if c["name"] == "Layout"]
        self.assertEqual(lay[0]["status"], "warn")
        self.assertIn("root is at 0x5C1000 for 0x5FF000 bytes", lay[0]["text"])

    def test_flash_warns_about_a_table_that_is_not_the_boards(self):
        from purrsdk import flash
        old = flash.layout_note(self.table_image(read_old_rows()), "0x0")
        self.assertIn("older layout", old)
        self.assertIsNone(flash.layout_note(self.table_image(read_old_rows()), "0x20000"))       # a single slot: nothing to compare
        new = [(p["name"], p["type"], p["subtype"], p["offset"], p["size"]) for p in self.layout()]
        self.assertIsNone(flash.layout_note(self.table_image(new), "0x0"))
        self.assertIsNone(flash.layout_note("/nonexistent.bin", "0x0"))


def read_old_rows():
    """The T-Deck Plus table up to 1.0.28: 1.5 MB / 1.875 MB / 1.5 MB slots, a raw 5 MB `ui_assets` and a smaller `root`."""
    return [("nvs", 1, 2, 0xB000, 0x4000), ("purrcfg", 1, 0x40, 0xF000, 0x2000), ("phy_init", 1, 1, 0x11000, 0x1000), ("bootpkg", 1, 0x40, 0x12000, 0xE000),
            ("kittenos", 0, 0, 0x20000, 0x180000), ("kernel", 0, 0x10, 0x1A0000, 0x1E0000), ("loader", 0, 0x20, 0x380000, 0x180000),
            ("netrec", 1, 0x41, 0x500000, 0x1000), ("ui_assets", 1, 0x42, 0x501000, 0x500000), ("root", 1, 0x83, 0xA01000, 0x5FF000)]


class Templates(unittest.TestCase):
    def test_names_are_checked(self):
        with self.assertRaises(ValueError):
            templates.driver_source("Bad Name", "gnss")
        with self.assertRaises(ValueError):
            templates.driver_source("fine", "nonsense")


if __name__ == "__main__":
    unittest.main()
