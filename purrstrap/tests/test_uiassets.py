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


class AlphaTests(unittest.TestCase):
    def test_rgba_pixels_become_a_colour_plane_then_an_alpha_plane(self):
        px = bytes([255, 0, 0, 255,   0, 255, 0, 128,   0, 0, 255, 0,   255, 255, 255, 64])
        data = ua.to_rgb565a8(2, 2, 4, px)
        self.assertEqual(len(data), 2 * 2 * 3)
        self.assertEqual(data[:2], bytes([0x00, 0xF8]))          # red, little-endian RGB565
        self.assertEqual(data[8:], bytes([255, 128, 0, 64]))     # the alpha plane, in pixel order

    def test_rgb_pixels_get_an_opaque_alpha_plane(self):
        data = ua.to_rgb565a8(1, 2, 3, bytes([0, 0, 0, 255, 255, 255]))
        self.assertEqual(data[4:], bytes([255, 255]))


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


class PackageTests(unittest.TestCase):
    """`uiassets package`: the used part of a pack, wrapped as a PURR_MOD_UIASSETS image for stage 2."""

    def built(self, padded_to=4096):
        blob = ua.pack([("a", 2, 2, b"\x01" * 8), ("b", 2, 2, b"\x02" * 12, ua.CF_RGB565A8)])
        return blob, blob + b"\xff" * (padded_to - len(blob))

    def test_used_length_leaves_out_the_padding_to_the_partition(self):
        blob, padded = self.built()
        n = ua.used_length(padded)
        self.assertEqual(padded[:n].rstrip(b"\xff")[:12], blob[:12])
        self.assertLess(n, len(padded))
        self.assertEqual(n % ua.ALIGN, 0)
        self.assertGreaterEqual(n, len(blob) - ua.ALIGN)

    def test_a_pack_whose_pixels_run_past_the_file_is_refused(self):
        blob, _ = self.built()
        with self.assertRaises(ValueError):
            ua.used_length(blob[:-8])
        with self.assertRaises(ValueError):
            ua.used_length(b"\x00" * 64)                         # bad magic
        with self.assertRaises(ValueError):
            ua.used_length(b"\x01")

    def test_the_package_is_a_module_image_with_the_uiassets_subtype_and_the_trimmed_payload(self):
        from lib import image as pimg
        from helpers import make_ctx
        blob, padded = self.built(8192)
        tmp = tempfile.mkdtemp()
        src, dest = os.path.join(tmp, "p.bin"), os.path.join(tmp, "u.cat")
        with open(src, "wb") as fh:
            fh.write(padded)
        ctx, _ = make_ctx(tmp)
        self.assertEqual(ua.package(ctx, "tdeck_plus", src, "1.0.17", dest), 0)
        data = open(dest, "rb").read()
        self.assertTrue(pimg.is_purr_image(data))
        f = pimg.header_fields(pimg.parse_header(data))
        self.assertEqual(f["image_type"], pimg.IMG_MODULE)
        self.assertEqual(f["flags"] & 0xFF, pimg.MOD_UIASSETS)
        self.assertEqual(f["name"], "uiassets")
        self.assertEqual(f["version"], "1.0.17")
        payload = data[pimg.HEADER_SIZE:]
        self.assertEqual(f["payload_size"], len(payload))
        self.assertEqual(len(payload), ua.used_length(padded))
        self.assertEqual(payload, padded[:len(payload)])

    def test_a_missing_file_or_an_unknown_board_is_refused(self):
        from helpers import make_ctx
        tmp = tempfile.mkdtemp()
        ctx, _ = make_ctx(tmp)
        self.assertEqual(ua.package(ctx, "tdeck_plus", os.path.join(tmp, "nope.bin"), "1.0.0", ""), 1)
        self.assertEqual(ua.package(ctx, "no_such_board", os.path.join(tmp, "nope.bin"), "1.0.0", ""), 1)


if __name__ == "__main__":
    unittest.main()
