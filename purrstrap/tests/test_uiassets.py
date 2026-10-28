"""The UI asset pack: packing with both colour formats, alignment, and importing SquareLine arrays."""
import os
import struct
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))

import importlib.util


def _load():
    spec = importlib.util.spec_from_file_location(
        "uiassets_under_test", os.path.join(HERE, "..", "scripts", "uiassets.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


ua = _load()

ARRAY = """// generated
#include "../ui.h"
const LV_ATTRIBUTE_MEM_ALIGN uint8_t ui_img_{name}_png_data[] = {{
{data}
}}; // LVGL_9 compatible
const lv_img_dsc_t ui_img_{name}_png = {{
    .header.magic = LV_IMAGE_HEADER_MAGIC,
    .header.w = {w},
    .header.h = {h},
    .data_size = sizeof(ui_img_{name}_png_data),
    .header.cf = {cf},
    .data = ui_img_{name}_png_data
}};
"""


def array_text(name, w, h, cf, data):
    body = ",".join(f"0x{b:02X}" for b in data)
    return ARRAY.format(name=name, w=w, h=h, cf=cf, data=body)


class PackTests(unittest.TestCase):
    def entries(self, blob):
        magic, count, _crc = struct.unpack(ua.HEADER_FMT, blob[:12])
        self.assertEqual(magic, ua.MAGIC)
        rows = []
        for i in range(count):
            at = 12 + i * ua.ENTRY_SIZE
            rows.append(struct.unpack(ua.ENTRY_FMT, blob[at:at + ua.ENTRY_SIZE]))
        return rows

    def test_both_colour_formats_and_their_sizes(self):
        blob = ua.pack([("a", 2, 2, b"\x01" * 8),
                        ("b", 2, 2, b"\x02" * 12, ua.CF_RGB565A8)])
        a, b = self.entries(blob)
        self.assertEqual((a[2], a[5]), (8, ua.CF_RGB565))
        self.assertEqual((b[2], b[5]), (12, ua.CF_RGB565A8))

    def test_every_entry_starts_on_a_four_byte_boundary(self):
        blob = ua.pack([("a", 1, 1, b"\x01\x01"), ("b", 1, 1, b"\x02\x02"),
                        ("c", 3, 1, b"\x03" * 6)])
        for row in self.entries(blob):
            self.assertEqual(row[1] % 4, 0)

    def test_data_of_the_wrong_size_is_refused(self):
        with self.assertRaises(ValueError):
            ua.pack([("a", 2, 2, b"\x01" * 7)])
        with self.assertRaises(ValueError):
            ua.pack([("a", 2, 2, b"\x01" * 8, ua.CF_RGB565A8)])

    def test_a_long_name_is_refused(self):
        with self.assertRaises(ValueError):
            ua.pack([("x" * 24, 1, 1, b"\x00\x00")])


class ImportTests(unittest.TestCase):
    def write(self, text):
        d = tempfile.mkdtemp()
        p = os.path.join(d, "ui_img_icon_png.c")
        with open(p, "w") as fh:
            fh.write(text)
        return p

    def test_an_opaque_image(self):
        data = bytes(range(8))
        p = self.write(array_text("icon", 2, 2, "LV_COLOR_FORMAT_NATIVE", data))
        self.assertEqual(ua.parse_lvgl_array(p), ("icon", 2, 2, data, ua.CF_RGB565))

    def test_an_image_with_alpha_keeps_both_planes_as_exported(self):
        data = bytes(range(12))
        p = self.write(array_text("icon", 2, 2, "LV_COLOR_FORMAT_NATIVE_WITH_ALPHA", data))
        self.assertEqual(ua.parse_lvgl_array(p), ("icon", 2, 2, data, ua.CF_RGB565A8))

    def test_an_unsupported_format_is_refused(self):
        p = self.write(array_text("icon", 2, 2, "LV_COLOR_FORMAT_ARGB8888", bytes(16)))
        with self.assertRaises(ValueError):
            ua.parse_lvgl_array(p)

    def test_a_file_that_is_not_an_image_array_is_refused(self):
        with self.assertRaises(ValueError):
            ua.parse_lvgl_array(self.write("int x;\n"))

    def test_imported_data_round_trips_through_pack(self):
        data = bytes(range(12))
        p = self.write(array_text("icon", 2, 2, "LV_COLOR_FORMAT_NATIVE_WITH_ALPHA", data))
        blob = ua.pack([ua.parse_lvgl_array(p)])
        (row,) = PackTests().entries(blob)
        start = 12 + ua.ENTRY_SIZE + row[1]
        self.assertEqual(blob[start:start + row[2]], data)


if __name__ == "__main__":
    unittest.main()
