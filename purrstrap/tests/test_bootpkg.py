import hashlib
import importlib.util
import os
import struct
import unittest

from helpers import TOOL_DIR

spec = importlib.util.spec_from_file_location(
    "bootpkg_under_test", os.path.join(TOOL_DIR, "scripts", "bootpkg.py"))
bootpkg = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bootpkg)


def parse(image):
    h = struct.unpack(bootpkg.HEADER_FORMAT, image[:bootpkg.HEADER_SIZE])
    return {"magic": h[0], "version": h[1], "header_size": h[2], "chip": h[3], "type": h[4],
            "key_id": h[5], "flags": h[6], "name": h[7].rstrip(b"\0"),
            "pkg_version": h[8].rstrip(b"\0"), "payload_offset": h[10],
            "payload_size": h[11], "sha": h[12], "signature": h[13]}


class MakeImageTests(unittest.TestCase):
    def test_header_size_matches_the_abi(self):
        self.assertEqual(bootpkg.HEADER_SIZE, 173)   # purr_image_header_t

    def test_header_fields(self):
        img = bootpkg.make_image(b"\x01" * 8, b"\x02" * 4, 16, 0x40378010, 9)
        h = parse(img)
        self.assertEqual(h["magic"], 0x50555252)
        self.assertEqual(h["chip"], 9)
        self.assertEqual(h["type"], 3)                      # module
        self.assertEqual(h["flags"] & 0xFF, 6)              # bootpkg
        self.assertEqual(h["name"], b"bootpkg")
        self.assertEqual(h["payload_offset"], 173)
        self.assertEqual(h["signature"], b"\0" * 64)        # unsigned for now

    def test_payload_is_preamble_text_data(self):
        img = bootpkg.make_image(b"AAAA", b"BBBB", 12, 0x40378020, 9)
        payload = img[173:]
        text_size, data_size, bss, entry = struct.unpack("<IIII", payload[:16])
        self.assertEqual((text_size, data_size, bss, entry), (4, 4, 12, 0x40378020))
        self.assertEqual(payload[16:20], b"AAAA")
        self.assertEqual(payload[20:24], b"BBBB")

    def test_hash_covers_the_payload(self):
        img = bootpkg.make_image(b"AAAA", b"BBBB", 0, 0x40378000, 9)
        h = parse(img)
        self.assertEqual(h["payload_size"], len(img) - 173)
        self.assertEqual(h["sha"], hashlib.sha256(img[173:]).digest())

    def test_payload_is_a_multiple_of_four(self):
        # The bootloader's hardware SHA only takes whole words.
        for t in range(0, 9):
            for d in range(0, 9):
                img = bootpkg.make_image(b"x" * t, b"y" * d, 0, 0x40378000, 9)
                self.assertEqual(parse(img)["payload_size"] % 4, 0, (t, d))

    def test_padding_is_counted_in_the_preamble(self):
        img = bootpkg.make_image(b"x" * 5, b"y" * 6, 0, 0x40378000, 9)
        text_size, data_size = struct.unpack("<II", img[173:181])
        self.assertEqual((text_size, data_size), (8, 8))

    def test_too_big_is_refused(self):
        with self.assertRaises(ValueError):
            bootpkg.make_image(b"x" * (bootpkg.TEXT_MAX + 1), b"", 0, 0, 9)
        with self.assertRaises(ValueError):
            bootpkg.make_image(b"", b"x" * 100, bootpkg.DATA_MAX, 0, 9)
        # Exactly full is fine.
        bootpkg.make_image(b"x" * bootpkg.TEXT_MAX, b"y" * 4, bootpkg.DATA_MAX - 4, 0, 9)


class SymbolTests(unittest.TestCase):
    def test_parses_nm_output(self):
        out = "40378000 T purr_pkg_entry\n3fc90800 B __bss_start\n         U foo\n"
        syms = bootpkg._symbols(out)
        self.assertEqual(syms["purr_pkg_entry"], 0x40378000)
        self.assertEqual(syms["__bss_start"], 0x3fc90800)
        self.assertNotIn("foo", syms)


if __name__ == "__main__":
    unittest.main()
