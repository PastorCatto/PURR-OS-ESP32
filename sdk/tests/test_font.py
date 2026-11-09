import contextlib
import io
import json
import os
import struct
import unittest

import helpers
from purrsdk import cli, font

FONTS = os.path.join(helpers.ROOT, "UI", "fonts")
ROBOTO = os.path.join(FONTS, "roboto", "Roboto-Regular-ui.ttf")
ROBOTO_BOLD = os.path.join(FONTS, "roboto", "Roboto-Bold-ui.ttf")
EMOJI = os.path.join(FONTS, "noto-emoji", "NotoEmoji-core.ttf")


def u16(v):
    return struct.pack(">H", v)


def u32(v):
    return struct.pack(">I", v)


def cmap4(mapping):
    """A cmap table with one format 4 subtable (3,1) mapping each code to the given glyph id; one segment per code."""
    codes = sorted(mapping)
    segs = [(c, c, (mapping[c] - c) & 0xFFFF) for c in codes] + [(0xFFFF, 0xFFFF, 1)]
    n = len(segs)
    sub = u16(4) + u16(0) + u16(0) + u16(n * 2) + u16(0) + u16(0) + u16(0)
    sub += b"".join(u16(e) for _, e, _ in segs) + u16(0) + b"".join(u16(s) for s, _, _ in segs)
    sub += b"".join(u16(d) for _, _, d in segs) + b"".join(u16(0) for _ in segs)
    sub = sub[:2] + u16(len(sub)) + sub[4:]
    return u16(0) + u16(1) + u16(3) + u16(1) + u32(12) + sub


def cmap12(groups):
    """A cmap table with one format 12 subtable (3,10); groups are (start, end, glyph)."""
    sub = u16(12) + u16(0) + u32(16 + 12 * len(groups)) + u32(0) + u32(len(groups)) + b"".join(u32(a) + u32(b) + u32(g) for a, b, g in groups)
    return u16(0) + u16(1) + u16(3) + u16(10) + u32(12) + sub


def name_table(strings):
    """A format 0 name table, Windows platform, English, UTF-16BE."""
    recs, store = b"", b""
    for nid, text in sorted(strings.items()):
        raw = text.encode("utf-16-be")
        recs += u16(3) + u16(1) + u16(0x409) + u16(nid) + u16(len(raw)) + u16(len(store))
        store += raw
    return u16(0) + u16(len(strings)) + u16(6 + len(recs)) + recs + store


GOOD_NAMES = {0: "Copyright 2026 Test", 1: "Tiny", 2: "Regular", 5: "Version 1.0", 13: "Test licence text", 14: "https://example.invalid/licence"}


def make_font(glyphs=3, cmap=None, drop=(), extra=None, magic=0x5F0F3CF5, flavor=0x00010000, names=None, loca_last=None, upem=1000):
    """Build a tiny sfnt in memory. `extra` adds tables ({tag: bytes}); `drop` removes some."""
    cmap = cmap if cmap is not None else cmap4({0x41: 1, 0x42: 2})
    glyf = b"\0" * 24
    loca_vals = [0] + [12 * min(i, 2) for i in range(1, glyphs + 1)]
    if loca_last is not None:
        loca_vals[-1] = loca_last
    head = u32(0x00010000) + u32(0) + u32(0) + u32(magic) + u16(0) + u16(upem) + b"\0" * 16 + u16(0) * 4 + u16(0) + u16(8) + u16(2) + struct.pack(">h", 0) + u16(0)
    assert len(head) == 54
    hhea = u32(0x00010000) + b"\0" * 28 + u16(0) + u16(1)
    assert len(hhea) == 36
    maxp = u32(0x00010000) + u16(glyphs) + b"\0" * 26
    tables = {
        "head": head, "hhea": hhea, "maxp": maxp, "cmap": cmap,
        "hmtx": (u16(500) + u16(0)) + u16(0) * (glyphs - 1),
        "loca": b"".join(u16(v // 2) for v in loca_vals), "glyf": glyf,
        "name": name_table(GOOD_NAMES if names is None else names),
    }
    tables.update(extra or {})
    for t in drop:
        tables.pop(t, None)
    tags = sorted(tables)
    out = u32(flavor) + u16(len(tags)) + u16(0) + u16(0) + u16(0)
    off = 12 + 16 * len(tags)
    body = b""
    for t in tags:
        out += t.encode("latin-1").ljust(4)[:4] + u32(0) + u32(off + len(body)) + u32(len(tables[t]))
        body += tables[t] + b"\0" * (-len(tables[t]) % 4)
    return out + body


def check(data, **kw):
    return font.check_bytes(data, **kw)


def bad_text(rep):
    return " ".join(c["text"] for c in rep["checks"] if c["status"] == "bad")


def statuses(rep, name):
    return [c["status"] for c in rep["checks"] if c["name"] == name]


class TinyFont(unittest.TestCase):
    def test_a_good_font_passes(self):
        rep = check(make_font())
        self.assertTrue(rep["ok"], rep)
        self.assertEqual(rep["facts"]["glyphs"], 3)
        self.assertEqual(rep["facts"]["codepoints"], 2)
        self.assertEqual(rep["facts"]["ranges"], [(0x41, 0x42)])
        self.assertEqual(rep["facts"]["outlines"], "TrueType (glyf)")
        self.assertEqual(rep["facts"]["names"]["copyright"], "Copyright 2026 Test")
        self.assertEqual(rep["facts"]["names"]["licence"], "Test licence text")
        self.assertEqual(rep["facts"]["latin1"], 2)
        self.assertEqual(rep["facts"]["shell_symbols"], 0)
        self.assertIn("outline glyphs only", [c["text"] for c in rep["checks"] if c["name"] == "colour"][0])

    def test_format_12_and_glyph_clamp(self):
        rep = check(make_font(cmap=cmap12([(0x41, 0x42, 1), (0x1F600, 0x1F6FF, 2)])))
        self.assertTrue(rep["ok"], rep)
        # glyph 2 is the last glyph, so only one code point of the long group has a glyph
        self.assertEqual(rep["facts"]["ranges"], [(0x41, 0x42), (0x1F600, 0x1F600)])

    def test_glyph_zero_is_not_a_character(self):
        rep = check(make_font(cmap=cmap4({0x41: 0, 0x42: 2})))
        self.assertEqual(rep["facts"]["ranges"], [(0x42, 0x42)])

    def test_missing_licence_is_a_warning_not_a_failure(self):
        rep = check(make_font(names={1: "Tiny"}))
        self.assertTrue(rep["ok"])
        self.assertEqual(statuses(rep, "licence"), ["warn"])
        self.assertEqual(statuses(rep, "copyright"), ["warn"])
        rep = check(make_font(drop=("name",)))
        self.assertTrue(rep["ok"])

    def test_size_cap(self):
        data = make_font()
        self.assertFalse(check(data, max_bytes=len(data) - 1)["ok"])
        self.assertTrue(check(data, max_bytes=len(data))["ok"])
        self.assertIn("over the cap", bad_text(check(data, max_bytes=10)))

    def test_not_a_font(self):
        for data, word in ((b"", "too short"), (b"hello world, not a font at all", "does not start"),
                           (b"ttcf" + b"\0" * 20, "collection"), (b"wOFF" + b"\0" * 20, "WOFF"),
                           (b"\0\1\0\0" + u16(0) + b"\0" * 8, "0 tables")):
            rep = check(data)
            self.assertFalse(rep["ok"])
            self.assertIn(word, bad_text(rep))

    def test_directory_out_of_bounds(self):
        good = make_font()
        self.assertFalse(check(good[:-8])["ok"])                      # last table runs past the end
        self.assertFalse(check(good[:20])["ok"])                      # directory does not fit
        n = struct.unpack_from(">H", good, 4)[0]
        d = bytearray(good)
        struct.pack_into(">I", d, 12 + 8, 0xFFFFFFF0)                 # first table: absurd offset
        self.assertIn("past the end", bad_text(check(bytes(d))))
        d = bytearray(good)
        struct.pack_into(">I", d, 12 + 8, 4)                          # first table starts inside the directory
        self.assertIn("inside the directory", bad_text(check(bytes(d))))
        d = bytearray(good)
        struct.pack_into(">H", d, 4, 500)
        self.assertIn("500 tables", bad_text(check(bytes(d))))
        self.assertGreater(n, 5)

    def test_overlapping_and_duplicate_tables(self):
        good = make_font()
        d = bytearray(good)
        tags = [good[12 + 16 * i:16 + 16 * i] for i in range(struct.unpack_from(">H", good, 4)[0])]
        i, j = tags.index(b"glyf"), tags.index(b"head")
        struct.pack_into(">I", d, 12 + 16 * i + 8, struct.unpack_from(">I", d, 12 + 16 * j + 8)[0] + 4)   # glyf starts inside head
        self.assertIn("overlap", bad_text(check(bytes(d))))
        d = bytearray(good)
        d[12 + 16 * j:16 + 16 * j] = b"glyf"
        self.assertIn("twice", bad_text(check(bytes(d))))

    def test_required_tables(self):
        for tag in font.REQUIRED + ("loca",):
            rep = check(make_font(drop=(tag,)))
            self.assertFalse(rep["ok"], tag)
            self.assertIn(tag, bad_text(rep))
        rep = check(make_font(drop=("glyf",)))
        self.assertIn("glyf or CFF", bad_text(rep))

    def test_cff_flavour(self):
        rep = check(make_font(flavor=0x4F54544F, drop=("glyf", "loca"), extra={"CFF ": b"\1\0\4\1" + b"\0" * 20}))
        self.assertTrue(rep["ok"], rep)
        self.assertEqual(rep["facts"]["outlines"], "CFF")
        self.assertIn("CFF2", bad_text(check(make_font(drop=("glyf", "loca"), extra={"CFF2": b"\2\0\5\0" + b"\0" * 20}))))

    def test_damaged_core_tables(self):
        self.assertIn("magic", bad_text(check(make_font(magic=0x12345678))))
        self.assertIn("units per em", bad_text(check(make_font(upem=2))))
        self.assertIn("past the end of the glyf", bad_text(check(make_font(loca_last=999))))
        self.assertIn("no glyphs", bad_text(check(make_font(glyphs=0, extra={"hmtx": b"\0" * 4}))))
        self.assertIn("backwards", bad_text(check(make_font(glyphs=3, loca_last=0))))
        self.assertIn("hmtx", bad_text(check(make_font(extra={"hmtx": b"\0\1"}))))

    def test_cmap_problems(self):
        self.assertIn("cmap", bad_text(check(make_font(cmap=b"\0\0"))))
        self.assertIn("do not fit", bad_text(check(make_font(cmap=u16(0) + u16(9) + b"\0" * 8))))
        only_fmt6 = u16(0) + u16(1) + u16(3) + u16(1) + u32(12) + u16(6) + u16(10) + u16(0) + u16(0x41) + u16(1) + u16(1)
        self.assertIn("format 4 or 12", bad_text(check(make_font(cmap=only_fmt6))))
        self.assertIn("out of order", bad_text(check(make_font(cmap=cmap12([(0x50, 0x60, 1), (0x41, 0x42, 1)])))))
        self.assertIn("bad range", bad_text(check(make_font(cmap=cmap12([(0x60, 0x50, 1)])))))
        liar = cmap12([(0x41, 0x42, 1)])
        liar = liar[:12 + 12] + u32(10 ** 6) + liar[12 + 16:]        # claims a million groups
        self.assertFalse(check(make_font(cmap=liar))["ok"])
        self.assertIn("no code point", bad_text(check(make_font(cmap=cmap4({0x41: 0})))))

    def test_colour_and_variable_fonts_are_warned_about(self):
        rep = check(make_font(extra={"CBDT": b"\0" * 8, "CBLC": b"\0" * 8, "COLR": b"\0" * 8, "SVG ": b"\0" * 8}))
        self.assertTrue(rep["ok"])
        self.assertEqual(statuses(rep, "colour"), ["warn"])
        self.assertEqual(rep["facts"]["colour_tables"], ["CBDT", "CBLC", "COLR", "SVG"])
        text = [c["text"] for c in rep["checks"] if c["name"] == "colour"][0]
        self.assertIn("does not support", text)
        rep = check(make_font(extra={"fvar": b"\0" * 16}))
        self.assertTrue(rep["ok"])
        self.assertEqual(statuses(rep, "variable font"), ["warn"])

    def test_shell_symbols_share(self):
        rep = check(make_font(cmap=cmap4({0x2190: 1, 0x2713: 2})))
        self.assertEqual(rep["facts"]["shell_symbols"], 2)
        self.assertEqual(rep["facts"]["latin1"], 0)

    def test_never_raises_on_damage(self):
        good = make_font()
        for cut in range(0, len(good), 7):
            check(good[:cut])
        for i in range(0, len(good), 5):
            d = bytearray(good)
            d[i] ^= 0xFF
            self.assertIsInstance(check(bytes(d))["ok"], bool)


@unittest.skipUnless(os.path.exists(ROBOTO) and os.path.exists(EMOJI), "the repository's UI/fonts folder is not here")
class RealFonts(unittest.TestCase):
    def test_roboto_subset(self):
        for path in (ROBOTO, ROBOTO_BOLD):
            rep = font.check_file(path)
            self.assertTrue(rep["ok"], rep)
            f = rep["facts"]
            self.assertLess(f["bytes"], 300 * 1024)
            self.assertEqual((f["latin1"], f["latin1_of"]), (191, 191))
            self.assertEqual(f["codepoints"], 1193)
            self.assertEqual(f["colour_tables"], [])
            self.assertIn("Roboto", f["names"]["family"])
            self.assertIn("Roboto Project Authors", f["names"]["copyright"])
            self.assertIn("Open Font License", f["names"]["licence"])
            self.assertEqual(sorted(f["tables"]), ["OS/2", "cmap", "glyf", "head", "hhea", "hmtx", "loca", "maxp", "name", "post"])

    def test_noto_emoji_subset(self):
        rep = font.check_file(EMOJI)
        self.assertTrue(rep["ok"], rep)
        f = rep["facts"]
        self.assertLess(f["bytes"], 400 * 1024)
        self.assertEqual(f["codepoints"], 414)
        self.assertEqual(f["glyphs"], 415)
        cps = {c for a, b in f["ranges"] for c in range(a, b + 1)}
        for c in (0x1F600, 0x2764, 0x1F44D, 0x1F431, 0x1F525, 0x2705, 0x1F4F1):
            self.assertIn(c, cps)
        self.assertNotIn(0x1F1E6, cps)                                  # regional indicators are left out
        self.assertEqual(f["colour_tables"], [])
        self.assertIn("Open Font License", f["names"]["licence"])
        self.assertEqual(f["names"]["copyright"], "Copyright 2013 Google LLC")

    def test_real_fonts_survive_truncation_and_bit_flips(self):
        with open(ROBOTO, "rb") as fh:
            data = fh.read()
        for cut in (0, 5, 12, 100, 160, 1000, len(data) // 2, len(data) - 1):
            self.assertFalse(check(data[:cut])["ok"], cut)
        for i in range(0, 400, 3):                                       # damage the directory and the head of the tables
            d = bytearray(data)
            d[i] ^= 0xA5
            self.assertIsInstance(check(bytes(d))["ok"], bool)


class Command(unittest.TestCase):
    def run_cli(self, *argv):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = cli.main(["font"] + list(argv))
        return code, out.getvalue()

    def write(self, data, name="x.ttf"):
        path = os.path.join(helpers.tmpdir(), name)
        with open(path, "wb") as fh:
            fh.write(data)
        return path

    def test_exit_codes_and_messages(self):
        code, out = self.run_cli("check", self.write(make_font()))
        self.assertEqual(code, 0)
        self.assertIn("passes the checks", out)
        code, out = self.run_cli("check", self.write(b"nope nope nope nope"))
        self.assertEqual(code, 1)
        self.assertIn("does not start like a TrueType", out)
        self.assertIn("fails the checks", out)
        code, out = self.run_cli("check", self.write(make_font()), "--max-size", "50")
        self.assertEqual(code, 1)
        self.assertIn("over the cap of 50 bytes", out)

    def test_missing_file_is_a_plain_message(self):
        code, out = self.run_cli("check", os.path.join(helpers.tmpdir(), "absent.ttf"))
        self.assertEqual(code, 1)
        self.assertIn("Not this time", out)

    def test_json(self):
        code, out = self.run_cli("check", self.write(make_font()), "--json")
        self.assertEqual(code, 0)
        rep = json.loads(out)
        self.assertTrue(rep["ok"])
        self.assertEqual(rep["facts"]["glyphs"], 3)

    def test_oversize_file_is_refused_without_being_read(self):
        path = self.write(b"\0\1\0\0" + b"\0" * 5000)
        rep = font.check_file(path, max_bytes=1000)
        self.assertFalse(rep["ok"])
        self.assertEqual(rep["facts"]["bytes"], 5004)


if __name__ == "__main__":
    unittest.main()
