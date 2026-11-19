"""The driver partition's slot format on the Python side: it must write the bytes the C library (purr_drvpart.c) reads (the golden slot is in test_drvpart.c)."""
import os
import struct
import sys
import unittest
import zlib

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "scripts"))
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import drvpart as d  # noqa: E402


class Slots(unittest.TestCase):
    def test_the_golden_slot(self):
        """The same driver as test_drvpart.c's golden slot: the header, the .cat and the erased rest."""
        s = d.build_slot("x.cat", bytes([1, 2, 3, 4]))
        self.assertEqual(len(s), 8192)
        self.assertEqual(s[0:4], b"PDRV")
        self.assertEqual(struct.unpack_from("<HHII", s, 4), (1, 0, 4, 0xB63CFBCD))      # printed by the C test: crc32 of 01 02 03 04
        self.assertEqual(zlib.crc32(bytes([1, 2, 3, 4])) & 0xFFFFFFFF, 0xB63CFBCD)
        self.assertEqual(s[16:21], b"x.cat")
        self.assertEqual(s[21:48], b"\0" * 27)
        self.assertEqual(s[48:64], b"\xff" * 16)
        self.assertEqual(s[64:68], bytes([1, 2, 3, 4]))
        self.assertEqual(s[68:], b"\xff" * (8192 - 68))

    def test_pack_and_read(self):
        a, b = b"A" * 500, bytes(range(256)) * 8
        img = d.pack([("gt911.cat", a), ("st7789.cat", b)])
        self.assertEqual(len(img), 0x14000)
        rows = d.read(img)
        self.assertEqual([r["state"] for r in rows], ["ok", "ok"] + ["empty"] * 8)
        self.assertEqual(rows[0]["file"], "gt911.cat")
        self.assertEqual(rows[1]["length"], len(b))
        self.assertEqual(d.extract(img, 1), ("st7789.cat", b))

    def test_limits(self):
        with self.assertRaises(d.DrvpartError):
            d.pack([("d%d.cat" % i, b"x") for i in range(11)])                          # ten slots
        d.pack([("d%d.cat" % i, b"x") for i in range(10)])
        with self.assertRaises(d.DrvpartError):
            d.build_slot("big.cat", b"x" * (d.MAX_CAT + 1))
        d.build_slot("big.cat", b"x" * d.MAX_CAT)
        for bad in ("", "../a.cat", "a/b.cat", "a\\b.cat", "x" * 32):
            with self.assertRaises(d.DrvpartError):
                d.build_slot(bad, b"x")
        with self.assertRaises(d.DrvpartError):
            d.pack([("a.cat", b"1"), ("a.cat", b"2")])

    def test_damage_is_reported(self):
        img = bytearray(d.pack([("a.cat", b"abcdef"), ("b.cat", b"ghijkl")]))
        img[64 + 2] ^= 1                                                                 # a flipped bit in the first driver
        img[8192] = ord("X")                                                             # the second one's magic
        rows = d.read(bytes(img))
        self.assertEqual([r["state"] for r in rows[:3]], ["damaged", "damaged", "empty"])
        with self.assertRaises(d.DrvpartError):
            d.extract(bytes(img), 0)


if __name__ == "__main__":
    unittest.main()
