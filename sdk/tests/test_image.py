"""The image maker (purrsdk/image.py): the partition table is checked byte for byte against what ESP-IDF's tool wrote, and a whole image is made from synthetic parts."""
import os
import shutil
import unittest

import helpers
from purrsdk import formats, image

DATA = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data")


def esp_payload(n):
    return b"\xe9" + bytes(range(1, 256)) * (n // 255) + b"\0" * (n % 255 - 1 if n % 255 else 0)


class Table(unittest.TestCase):
    def test_matches_esp_idf_byte_for_byte(self):
        for board in ("tdeck_plus", "waveshare154"):
            with open(os.path.join(DATA, "partition-table-%s.bin" % board), "rb") as fh:
                golden = fh.read()
            self.assertEqual(image.table_bytes(image.layout_for(board)), golden, board)

    def test_read_table_round_trips(self):
        lay = image.layout_for("tdeck_plus")
        got = image.read_table(image.table_bytes(lay) + b"\xff" * 100)
        self.assertEqual([(p["name"], p["offset"], p["size"]) for p in got], [(p["name"], p["offset"], p["size"]) for p in lay])

    def test_unknown_board(self):
        with self.assertRaises(image.ImageError):
            image.layout_for("no-such-board")

    def test_flash_map_v2_shapes(self):
        for board, root_at, end in (("tdeck_plus", 0x5C1000, 0x1000000), ("tdeck", 0x5C1000, 0x1000000), ("waveshare154", 0x461000, 0x800000)):
            lay = {p["name"]: p for p in image.layout_for(board)}
            self.assertNotIn("ui_assets", lay)
            self.assertEqual(lay["root"]["offset"], root_at, board)
            self.assertEqual(lay["root"]["offset"] + lay["root"]["size"], end, board)


@unittest.skipUnless(helpers.HAVE_CRYPTO, "needs the cryptography package")
class Whole(unittest.TestCase):
    def setUp(self):
        self.tmp = helpers.tmpdir()
        img = formats.image()
        self.folder = os.path.join(self.tmp, "parts")
        os.makedirs(self.folder)

        def put(name, image_type, subtype, size, version="1.1"):
            with open(os.path.join(self.folder, name), "wb") as fh:
                fh.write(img.make_image(name, version, image_type, subtype, esp_payload(size), img.CHIP_ESP32S3))
        put("bootloader-tdeck_plus.cat", img.IMG_MODULE, img.MOD_BOOTLOADER, 30000, "0.4.0")
        put("bootpkg-tdeck_plus.cat", img.IMG_MODULE, img.MOD_BOOTPKG, 5000, "0.3.1")
        put("kittenos-tdeck_plus.kitt", img.IMG_RECOVERY, 0, 100000)
        put("kernel_tdeck_plus.cat", img.IMG_MODULE, img.MOD_KERNEL, 200000)          # the official release spells it with an underscore
        put("loader-tdeck_plus.cat", img.IMG_MODULE, img.MOD_LOADER, 100000)

    def test_finds_both_spellings(self):
        found = image.find_parts(self.folder, "tdeck_plus")
        self.assertEqual(sorted(found), ["bootloader", "bootpkg", "kernel", "kittenos", "loader"])

    def test_finds_the_files_a_release_names_even_with_a_hash_in_the_name(self):
        d = os.path.join(helpers.tmpdir(), "rel")
        os.makedirs(d)
        names = {"kernel": "kernel-tdeck.461e04c4de.cat", "kittenos": "kittenos-tdeck.5a0a2f0949.kitt", "loader": "loader-tdeck.16cdfbd517.cat", "bootpkg": "bootpkg-tdeck.389e8dcc72.cat",
                 "bootloader": "bootloader-tdeck.bdc8f7995a.cat"}
        stanzas = ["release=1.1+tdeck_plus", ""]
        for part, fn in names.items():
            shutil.copyfile(os.path.join(self.folder, "loader-tdeck_plus.cat"), os.path.join(d, fn))
            stanzas += ["component=%s" % part, "board=tdeck", "file=%s" % fn, ""]
        stanzas += ["component=kernel", "board=waveshare154", "file=kernel_waveshare154.cat", "", "component=kernel", "board=tdeck_plus", "file=missing.cat", ""]
        open(os.path.join(d, "recovery.manifest"), "w").write("\n".join(stanzas))
        found = image.find_parts(d, "tdeck")
        self.assertEqual({p: os.path.basename(f) for p, f in found.items()}, names)
        self.assertEqual(image.find_parts(d, "waveshare154"), {})                     # named but not there: not found, not guessed
        self.assertEqual(image.find_parts(d, "tdeck_plus"), {})

    def test_plan_reports_missing_and_too_big(self):
        os.remove(os.path.join(self.folder, "loader-tdeck_plus.cat"))
        p = image.plan("tdeck_plus", self.folder)
        self.assertFalse(p["ok"])
        self.assertTrue(any("loader" in t for t in p["problems"]))
        self.assertTrue(p["parts"][0]["found"])

    def test_a_damaged_container_is_refused(self):
        path = os.path.join(self.folder, "kernel_tdeck_plus.cat")
        with open(path, "rb") as fh:
            data = bytearray(fh.read())
        data[-5] ^= 0xFF
        with open(path, "wb") as fh:
            fh.write(bytes(data))
        p = image.plan("tdeck_plus", self.folder)
        self.assertFalse(p["ok"])
        self.assertTrue(any("hash" in t for t in p["problems"]))

    def test_builds_a_whole_image(self):
        out = os.path.join(self.tmp, "out", "tdeck_plus.img")
        lines = []
        r = image.build("tdeck_plus", self.folder, out, on_line=lines.append)
        self.assertTrue(r["ok"])
        with open(out, "rb") as fh:
            data = fh.read()
        self.assertEqual(len(data), 16 * 1024 * 1024)
        lay = {p["name"]: p for p in image.layout_for("tdeck_plus")}
        self.assertEqual(data[0], 0xE9)                                               # the bootloader's ESP image starts the flash
        self.assertEqual(image.read_table(data[0xA000:0xB000]), image.layout_for("tdeck_plus"))
        for slot in ("kernel", "kittenos", "loader"):
            at, size = lay[slot]["offset"], lay[slot]["size"]
            self.assertEqual(data[at], 0xE9, slot)                                    # the app image
            self.assertEqual(data[at + size - 4096:at + size - 4092], b"RRUP", slot)  # the signature trailer in the last sector ('PURR' little-endian)
        self.assertEqual(data[0xF000:0xF004], b"\xff" * 4)                            # no trust block given
        self.assertEqual(set(data[lay["root"]["offset"]:lay["root"]["offset"] + 4096]), {0xFF})
        self.assertTrue(any("No trust block" in n for n in r["notes"]))

    def test_a_trust_block_goes_to_purrcfg(self):
        tb = os.path.join(self.tmp, "purrcfg.bin")
        with open(tb, "wb") as fh:
            fh.write(b"PCFG" + b"\x01" * 100)
        out = os.path.join(self.tmp, "t.img")
        image.build("tdeck_plus", self.folder, out, trust_block=tb, on_line=lambda l: None)
        with open(out, "rb") as fh:
            data = fh.read()
        self.assertEqual(data[0xF000:0xF004], b"PCFG")


if __name__ == "__main__":
    unittest.main()
