"""The essential drivers are built, signed and packed into the drivers partition (flash map v3), never baked into the kernel (except the W5500)."""
import os
import struct
import sys
import unittest

import helpers  # noqa: F401  (puts the SDK on the path)

from purrsdk import drivers, image, minitoml

sys.path.insert(0, os.path.join(helpers.ROOT, "purrstrap", "scripts"))
sys.path.insert(0, os.path.join(helpers.ROOT, "purrstrap"))
import drvpart  # noqa: E402


class Lists(unittest.TestCase):
    def test_defaults_and_the_board_files_agree(self):
        for board in ("tdeck_plus", "playtime"):
            toml = minitoml.load(os.path.join(helpers.ROOT, "sdk", "boards", board + ".toml"))
            self.assertEqual(drivers.board_files(board, toml), list(drivers.DEFAULTS[board]), board)
        self.assertEqual(drivers.board_files("waveshare154"), [])                         # the 8 MB e-ink board carries none

    def test_a_board_file_decides_and_a_bad_list_is_refused(self):
        self.assertEqual(drivers.board_files("x", {"board": {"drivers": ["st7789.cat"]}}), ["st7789.cat"])
        for bad in (["nope.cat"], ["st7789.cat", "st7789.cat"], ["gt911.cat"] * 3, [f for f in drivers.ESSENTIAL] + ["kbd_i2c.cat"] * 5):
            with self.assertRaises(drivers.DriverError):
                drivers.board_files("x", {"board": {"drivers": bad}})

    def test_every_driver_has_its_source(self):
        for f, (name, rel, entry) in drivers.ESSENTIAL.items():
            src = os.path.join(helpers.ROOT, *rel.split("/"))
            self.assertTrue(os.path.isfile(src), f)
            with open(src, encoding="utf-8") as fh:
                self.assertIn(entry, fh.read(), f)                                        # the entry symbol the build asks for is in the file
        self.assertTrue(drivers.MAX_DRIVERS == drvpart.SLOTS == 10)


def _read(*parts):
    with open(os.path.join(helpers.ROOT, *parts), encoding="utf-8") as fh:
        return fh.read()


class Kernel(unittest.TestCase):
    """What the kernel may and may not carry (owner, 2026-11-19: only the W5500 stays baked in; the rest ship as modules)."""

    def test_only_the_w5500_and_a_test_blob_are_baked_in(self):
        import re
        text = _read("PurrOS", "main", "commands.c")
        arrays = sorted(re.findall(r"static const uint8_t (s_\w*driver_module)\[\]", text))
        self.assertEqual(arrays, ["s_hello_driver_module", "s_w5500_driver_module"])
        table = text[text.index("static const temp_module_asset_t s_temp_drivers[]"):]
        table = table[:table.index("};")]
        self.assertEqual(re.findall(r'\{"([^"]+)"', table), ["w5500.cat"])

    def test_the_kernel_does_not_choose_drivers_by_the_boards_name(self):
        text = _read("PurrOS", "main", "commands.c")
        self.assertNotIn('"playtime"', text)                                                # the board's file lists its drivers (sdk/boards/<board>.toml)
        self.assertNotIn("gamepad.cat", text)

    def test_planting_and_the_loader_read_the_drivers_partition(self):
        text = _read("PurrOS", "main", "commands.c")
        self.assertIn("purr_drvpart_each(plant_slot_cb, &pd)", text)
        self.assertIn("purr_drvpart_each(load_slot_cb, &ld)", text)                         # the loader and KittenOS have no file system: straight from the slots
        self.assertIn("if (purr_drvpart_has_file(file)) {", text)                           # a planted essential driver is never held back or set aside
        self.assertIn('"purr_drvpart_plat.c"', _read("PurrOS", "main", "CMakeLists.txt"))
        self.assertIn('"src/purr_drvpart.c"', _read("PurrOS", "components", "coreos", "CMakeLists.txt"))

    def test_every_16_mb_table_ends_with_the_drivers_partition(self):
        for rel in (("PurrOS", "partitions", "tdeck_plus.csv"), ("PurrOS", "partitions", "tdeck.csv"), ("PurrOS", "partitions", "playtime.csv"), ("bootloader", "partitions.csv"), ("Kernel", "partitions.csv")):
            rows = [l.split(",") for l in _read(*rel).splitlines() if l.strip() and not l.startswith("#")]
            last, root = [c.strip() for c in rows[-1]], [c.strip() for c in rows[-2]]
            self.assertEqual(last[0], "drivers", rel)
            self.assertEqual((int(last[3], 0), int(last[4], 0)), (0xFEC000, 0x14000), rel)
            self.assertEqual(int(root[3], 0) + int(root[4], 0), int(last[3], 0), rel)         # root runs up to it
            self.assertEqual(int(last[4], 0), drvpart.SIZE, rel)

    def test_the_image_scripts_know_the_new_layout(self):
        for rel in (("tools", "hw", "make_hw_image.sh"), ("tools", "vm", "mkimage_dbg.sh")):
            text = _read(*rel)
            self.assertIn("0xA2B000", text, rel)
            self.assertNotIn("(0x5C1000, 0xA3F000)", text, rel)
            self.assertIn("0xFEC000", text, rel)
            self.assertIn("DRIVERS_FROM", text, rel)


class Build(unittest.TestCase):
    def fake(self, calls):
        def run(args, extra_env=None, password=None):
            calls.append((list(args), dict(extra_env or {}), password))
            if args[:2] == ["modules", "build"]:
                with open(args[args.index("--out") + 1], "wb") as fh:
                    fh.write(b"CAT:" + args[args.index("--name") + 1].encode())
            if args[:2] == ["drvpart", "pack"]:
                files = args[args.index("--files") + 1].split(",")
                items = []
                for f in files:
                    with open(f, "rb") as fh:
                        items.append((os.path.basename(f), fh.read()))
                with open(args[args.index("--out") + 1], "wb") as fh:
                    fh.write(drvpart.pack(items))
            return {"rc": 0, "output": ""}
        return run

    def test_each_driver_is_compiled_signed_and_packed_in_order(self):
        tmp = helpers.tmpdir()
        calls = []
        out = os.path.join(tmp, "drivers-playtime.bin")
        r = drivers.build("playtime", helpers.ROOT, "/k/dev.key", 6, "pw", out, "1.2.3", run=self.fake(calls), on_line=lambda l: None)
        self.assertTrue(r["ok"], r)
        kinds = [c[0][:2] for c in calls]
        self.assertEqual(kinds, [["modules", "build"], ["keys", "sign"], ["modules", "build"], ["keys", "sign"], ["drvpart", "pack"]])
        builds = [c for c in calls if c[0][:2] == ["modules", "build"]]
        self.assertTrue(all(c[1].get("PURR_REPO_ROOT") == helpers.ROOT for c in builds))
        self.assertTrue(all(c[0][c[0].index("--kind") + 1] == "driver" for c in builds))        # a driver is a module the developer key may sign (the boot key may not)
        self.assertEqual([c[2] for c in calls if c[0][:2] == ["keys", "sign"]], ["pw", "pw"])
        self.assertTrue(all(c[0][c[0].index("--key-id") + 1] == "6" for c in calls if c[0][:2] == ["keys", "sign"]))
        with open(out, "rb") as fh:
            rows = drvpart.read(fh.read())
        self.assertEqual([r_["file"] for r_ in rows if r_["state"] == "ok"], ["st7789.cat", "gamepad.cat"])

    def test_a_failing_compile_names_the_driver(self):
        def run(args, extra_env=None, password=None):
            if args[:2] == ["modules", "build"] and "gt911" in " ".join(args):
                return {"rc": 2, "output": "boom\n"}
            return self.fake([])(args, extra_env, password)
        r = drivers.build("tdeck_plus", helpers.ROOT, "/k/d.key", 4, "pw", os.path.join(helpers.tmpdir(), "d.bin"), "1", run=run, on_line=lambda l: None)
        self.assertFalse(r["ok"])
        self.assertIn("gt911.cat", r["problem"])


class ImageMaker(unittest.TestCase):
    def test_a_board_with_the_partition_needs_the_drivers_image(self):
        folder = helpers.tmpdir()
        p = image.plan("playtime", folder)
        self.assertFalse(p["ok"])
        self.assertTrue([x for x in p["problems"] if "drivers-playtime.bin" in x], p["problems"])
        with open(os.path.join(folder, "drivers-playtime.bin"), "wb") as fh:
            fh.write(drvpart.pack([("a.cat", b"x")]))
        row = [r for r in image.plan("playtime", folder)["parts"] if r["part"] == "drivers"][0]
        self.assertEqual((row["offset"], row["room"], row["found"]), (0xFEC000, 0x14000, True))

    def test_a_board_that_carries_no_driver_is_not_asked_for_an_image(self):
        """The 8 MB e-paper board has the drivers partition (flash map v3, 1.2) but no driver of its own: no image is demanded and the slots stay blank."""
        p = image.plan("waveshare154", helpers.tmpdir())
        self.assertFalse([x for x in p["problems"] if "drivers image" in x])
        row = [r for r in p["parts"] if r["part"] == "drivers"][0]
        self.assertEqual((row["offset"], row["room"], row["found"]), (0x7EC000, 0x14000, False))

    def test_a_16_mb_board_is_still_asked_for_its_drivers_image(self):
        p = image.plan("tdeck_plus", helpers.tmpdir())
        self.assertTrue([x for x in p["problems"] if "drivers image" in x])

    def test_the_image_has_the_drivers_at_the_end_of_the_flash(self):
        lay = {p["name"]: p for p in image.layout_for("playtime")}
        self.assertEqual(lay["drivers"]["offset"] + lay["drivers"]["size"], 16 * 1024 * 1024)
        self.assertEqual(lay["root"]["offset"] + lay["root"]["size"], lay["drivers"]["offset"])
        self.assertEqual(lay["drivers"]["size"], drvpart.SIZE)


if __name__ == "__main__":
    unittest.main()
