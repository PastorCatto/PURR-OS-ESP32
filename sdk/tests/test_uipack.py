"""UI packs and themes: `purr ui pack`, `purr ui check`, `purr new theme`, using small generated PNGs."""
import contextlib
import io
import json
import os
import re
import struct
import unittest
import zlib

import helpers
from purrsdk import cli, formats, ui


def png(path, w, h, rgba=None, alpha=255, colour=(200, 40, 40)):
    """A true-colour 8-bit PNG: RGBA when `alpha` is not None, else RGB."""
    ch = 4 if alpha is not None else 3
    px = bytes(colour + ((alpha,) if ch == 4 else ())) * (w * h) if rgba is None else rgba

    def chunk(tag, data):
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)
    raw = b"".join(b"\x00" + px[y * w * ch:(y + 1) * w * ch] for y in range(h))
    with open(path, "wb") as fh:
        fh.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6 if ch == 4 else 2, 0, 0, 0)) +
                 chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def folder_with(shell, names=None, skip=()):
    """A folder of tiny PNGs named for the shell's required names, each at the size the shell draws."""
    d = os.path.join(helpers.tmpdir(), "images")
    os.makedirs(d)
    for n, (w, h) in ui.REQUIRED[shell].items():
        if n not in skip and (names is None or n in names):
            png(os.path.join(d, n + ".png"), w, h, alpha=128)
    return d


def run(*argv):
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        code = cli.main(list(argv))
    return code, out.getvalue()


class Packing(unittest.TestCase):
    def setUp(self):
        self.ua = formats.uiassets()
        self.dir = helpers.tmpdir()

    def test_a_folder_of_pngs_becomes_a_pack_with_the_kits_packer_format(self):
        src = os.path.join(self.dir, "src")
        os.makedirs(src)
        png(os.path.join(src, "icon_a.png"), 4, 3, alpha=100)                  # real transparency: RGB565A8
        png(os.path.join(src, "wall.png"), 2, 2, alpha=None)                   # no alpha channel: plain RGB565
        png(os.path.join(src, "solid.png"), 2, 2, alpha=255)                   # an alpha channel that is all opaque: plain RGB565
        r = ui.pack_folder(src, os.path.join(self.dir, "out", "pack.bin"))
        self.assertEqual((r["count"], r["names"]), (3, ["icon_a", "solid", "wall"]))
        data = open(r["path"], "rb").read()
        self.assertEqual(len(data), r["bytes"])
        magic, count, crc = struct.unpack_from(self.ua.HEADER_FMT, data, 0)
        self.assertEqual((magic, count), (self.ua.MAGIC, 3))
        rows = {}
        for i in range(count):
            n, off, ln, w, h, cf = struct.unpack_from(self.ua.ENTRY_FMT, data, 12 + i * self.ua.ENTRY_SIZE)
            rows[n.rstrip(b"\0").decode()] = (ln, w, h, cf)
            self.assertEqual(off % 4, 0)
        self.assertEqual(rows["icon_a"], (4 * 3 * 3, 4, 3, self.ua.CF_RGB565A8))
        self.assertEqual(rows["wall"], (2 * 2 * 2, 2, 2, self.ua.CF_RGB565))
        self.assertEqual(rows["solid"][3], self.ua.CF_RGB565)
        self.assertEqual(crc, zlib.crc32(data[12:]) & 0xFFFFFFFF)

    def test_the_pack_is_the_one_purrstrap_would_write(self):
        src = os.path.join(self.dir, "src")
        os.makedirs(src)
        png(os.path.join(src, "a.png"), 3, 3, alpha=90)
        png(os.path.join(src, "b.png"), 5, 2, alpha=None)
        ui.pack_folder(src, os.path.join(self.dir, "p.bin"))
        w, h, ch, px = self.ua.decode_png(os.path.join(src, "a.png"))
        w2, h2, ch2, px2 = self.ua.decode_png(os.path.join(src, "b.png"))
        want = self.ua.pack([("a", w, h, self.ua.to_rgb565a8(w, h, ch, px), self.ua.CF_RGB565A8), ("b", w2, h2, self.ua.to_rgb565(w2, h2, ch2, px2))])
        self.assertEqual(open(os.path.join(self.dir, "p.bin"), "rb").read(), want)

    def test_problems_are_messages_not_crashes(self):
        empty = os.path.join(self.dir, "empty")
        os.makedirs(empty)
        with self.assertRaises(ValueError):
            ui.pack_folder(empty, os.path.join(self.dir, "x.bin"))
        with self.assertRaises(ValueError):
            ui.pack_folder(os.path.join(self.dir, "nope"), os.path.join(self.dir, "x.bin"))
        longname = os.path.join(self.dir, "long")
        os.makedirs(longname)
        png(os.path.join(longname, "x" * 24 + ".png"), 1, 1)
        with self.assertRaises(ValueError) as cm:
            ui.pack_folder(longname, os.path.join(self.dir, "x.bin"))
        self.assertIn("1 to 23", str(cm.exception))
        bad = os.path.join(self.dir, "bad")
        os.makedirs(bad)
        with open(os.path.join(bad, "junk.png"), "wb") as fh:
            fh.write(b"not a png at all")
        with self.assertRaises(ValueError):
            ui.pack_folder(bad, os.path.join(self.dir, "x.bin"))

    def test_more_than_the_devices_128_pictures_is_refused(self):
        many = os.path.join(self.dir, "many")
        os.makedirs(many)
        for i in range(129):
            png(os.path.join(many, "p%03d.png" % i), 1, 1)
        with self.assertRaises(ValueError) as cm:
            ui.pack_folder(many, os.path.join(self.dir, "x.bin"))
        self.assertIn("128", str(cm.exception))

    def test_a_pack_over_5_mb_is_refused(self):
        big = os.path.join(self.dir, "big")
        os.makedirs(big)
        for i in range(3):
            png(os.path.join(big, "p%d.png" % i), 1024, 1024, alpha=None)      # 2 MB each as RGB565
        with self.assertRaises(ValueError) as cm:
            ui.pack_folder(big, os.path.join(self.dir, "x.bin"))
        self.assertIn("cap", str(cm.exception))
        self.assertFalse(os.path.exists(os.path.join(self.dir, "x.bin")))


class CheckingAPack(unittest.TestCase):
    def setUp(self):
        self.dir = helpers.tmpdir()
        self.ua = formats.uiassets()

    def pack(self, images=None):
        images = images or [("a", 2, 2, b"\x01" * 8), ("b", 2, 2, b"\x02" * 12, self.ua.CF_RGB565A8)]
        return self.ua.pack(images)

    def check(self, data, shell=None):
        return ui.check_pack_bytes(data, shell)

    def bad_text(self, rep):
        return " | ".join(c["text"] for c in rep["checks"] if c["status"] == "bad")

    def test_a_good_pack_passes(self):
        rep = self.check(self.pack())
        self.assertTrue(rep["ok"], rep)
        self.assertEqual(rep["facts"]["pictures"], ["a", "b"])

    def test_bad_magic_short_file_and_truncation(self):
        self.assertIn("magic", self.bad_text(self.check(b"\x00" * 64)))
        self.assertIn("too short", self.bad_text(self.check(b"\x01\x02")))
        data = self.pack()
        self.assertIn("past the end", self.bad_text(self.check(data[:-3])))
        self.assertIn("directory", self.bad_text(self.check(data[:20])))

    def test_the_crc_catches_a_flipped_pixel_and_a_flipped_name(self):
        data = bytearray(self.pack())
        data[-1] ^= 0x40
        self.assertIn("CRC mismatch", self.bad_text(self.check(bytes(data))))
        data = bytearray(self.pack())
        data[12] ^= 0x01                                                         # first letter of the first name
        self.assertFalse(self.check(bytes(data))["ok"])

    def resign(self, data):
        data = bytearray(data)
        struct.pack_into("<I", data, 8, zlib.crc32(bytes(data[12:])) & 0xFFFFFFFF)
        return bytes(data)

    def test_entry_problems_the_device_refuses(self):
        base = self.pack()
        entry = 12                                                               # the first directory entry
        # a length that does not match width x height x bytes per pixel
        d = bytearray(base)
        struct.pack_into("<I", d, entry + 28, 7)
        self.assertIn("is not what", self.bad_text(self.check(self.resign(d))))
        # pixels that start at an odd offset
        d = bytearray(base)
        struct.pack_into("<I", d, entry + 24, 1)
        self.assertIn("odd offset", self.bad_text(self.check(self.resign(d))))
        # pixels past the end of the file
        d = bytearray(base)
        struct.pack_into("<I", d, entry + 24, 4096)
        self.assertIn("past the end", self.bad_text(self.check(self.resign(d))))
        # an unknown colour format
        d = bytearray(base)
        d[entry + 36] = 9
        self.assertIn("colour format 9", self.bad_text(self.check(self.resign(d))))
        # a name with no end mark
        d = bytearray(base)
        d[entry:entry + 24] = b"n" * 24
        self.assertIn("no end", self.bad_text(self.check(self.resign(d))))

    def test_duplicate_names_and_too_many_pictures(self):
        rep = self.check(self.pack([("a", 1, 1, b"\x01\x01"), ("a", 1, 1, b"\x02\x02")]))
        self.assertIn("more than once", self.bad_text(rep))
        data = bytearray(self.pack())
        struct.pack_into("<I", data, 4, 129)
        self.assertIn("at most 128", self.bad_text(self.check(bytes(data))))

    def test_over_5_mb_fails_on_size(self):
        rep = self.check(self.pack() + b"\xff" * ui.MAX_PACK_BYTES)
        self.assertEqual(rep["checks"][0]["name"], "size")
        self.assertEqual(rep["checks"][0]["status"], "bad")

    def test_padding_after_the_last_picture_is_a_warning(self):
        rep = self.check(self.pack() + b"\xff" * 16)
        self.assertTrue(rep["ok"])
        self.assertIn("padding", [c["name"] for c in rep["checks"] if c["status"] == "warn"])

    def test_every_required_name_for_the_shell(self):
        src = folder_with("w10m")
        ui.pack_folder(src, os.path.join(self.dir, "w.bin"))
        rep = ui.check(os.path.join(self.dir, "w.bin"), "w10m")
        self.assertTrue(rep["ok"], rep)
        self.assertIn("all 21 required", " ".join(c["text"] for c in rep["checks"]))
        # the same file is not an Android pack
        rep = ui.check(os.path.join(self.dir, "w.bin"), "android")
        self.assertFalse(rep["ok"])
        self.assertIn("31 of 31", self.bad_text(rep))

    def test_a_missing_required_name_is_named(self):
        src = folder_with("android", skip=("st_w3", "fr_app"))
        ui.pack_folder(src, os.path.join(self.dir, "a.bin"))
        rep = ui.check(os.path.join(self.dir, "a.bin"), "android")
        self.assertFalse(rep["ok"])
        text = self.bad_text(rep)
        self.assertIn("2 of 31", text)
        self.assertIn("st_w3", text)
        self.assertIn("fr_app", text)

    def test_a_wrong_size_is_a_warning_and_wp_names_in_an_android_pack_are_flagged(self):
        src = folder_with("android")
        png(os.path.join(src, "fr_app.png"), 40, 40, alpha=128)                  # the shell draws 48
        png(os.path.join(src, "wp_system.png"), 32, 32, alpha=128)
        png(os.path.join(src, "mystery_pic.png"), 8, 8, alpha=128)
        ui.pack_folder(src, os.path.join(self.dir, "a.bin"))
        rep = ui.check(os.path.join(self.dir, "a.bin"), "android")
        self.assertTrue(rep["ok"], rep)
        warns = {c["name"]: c["text"] for c in rep["checks"] if c["status"] == "warn"}
        self.assertIn("fr_app is 40x40", warns["sizes"])
        self.assertIn("wp_ names", warns)
        other = next(c for c in rep["checks"] if c["name"] == "other names")
        self.assertIn("mystery_pic", other["text"])

    def test_the_wallpapers_are_known_to_both_shells(self):
        for shell in ui.SHELLS:
            for i in range(10):
                self.assertEqual(ui.known(shell)["img%d" % i], (320, 569))

    def test_an_unknown_shell_is_refused(self):
        with self.assertRaises(ValueError):
            ui.check(os.path.join(self.dir, "x"), "ios")


class ThemeConf(unittest.TestCase):
    GOOD = "name=Dusk\nauthor=A. Person\nlicence=CC-BY-4.0\nversion=1.0.0\nshell=w10m\naccent=#336699\ndark=1\n"

    def statuses(self, text, size=None):
        checks, values = ui.check_conf_text(text, size)
        return {c["name"]: c["status"] for c in checks}, values

    def test_a_good_theme_conf(self):
        st, values = self.statuses(self.GOOD)
        self.assertNotIn("bad", st.values())
        self.assertEqual(values["accent"], "#336699")

    def test_each_required_key_is_needed(self):
        for key in ui.THEME_REQUIRED:
            text = "".join(l for l in self.GOOD.splitlines(True) if not l.startswith(key + "="))
            st, _ = self.statuses(text)
            self.assertEqual(st[key], "bad", key)

    def test_values_are_checked(self):
        for line, key in (("shell=ios", "shell"), ("accent=20", "accent"), ("accent=#12345", "accent"), ("accent=red", "accent"),
                          ("dark=yes", "dark"), ("version=v1", "version"), ("name=" + "n" * 32, "name"), ("author=" + "a" * 32, "author"),
                          ("version=1.0.0.0.0.0.0", "version")):
            text = "".join(l for l in self.GOOD.splitlines(True) if not l.startswith(key + "=")) + line + "\n"
            st, _ = self.statuses(text)
            self.assertEqual(st[key], "bad", line)
        for line, key in (("shell=any", "shell"), ("shell=android", "shell"), ("accent=0", "accent"), ("accent=19", "accent"), ("dark=0", "dark")):
            text = "".join(l for l in self.GOOD.splitlines(True) if not l.startswith(key + "=")) + line + "\n"
            st, _ = self.statuses(text)
            self.assertEqual(st[key], "ok", line)

    def test_optional_keys_may_be_left_out_and_comments_are_fine(self):
        st, _ = self.statuses("# a theme\nname=Dusk\nauthor=A\nlicence=MIT\nversion=1\nshell=any\n")
        self.assertNotIn("bad", st.values())

    def test_unknown_keys_and_malformed_lines_warn(self):
        st, _ = self.statuses(self.GOOD + "colour=blue\nname = Dusk\nplain text\n")
        self.assertEqual(st["colour"], "warn")
        self.assertEqual(st["line"], "warn")

    def test_the_device_reads_under_512_bytes(self):
        st, _ = self.statuses(self.GOOD + "# " + "x" * 600 + "\n")
        self.assertEqual(st["size"], "bad")


class ThemeFolders(unittest.TestCase):
    CONF = "name=Dusk\nauthor=A. Person\nlicence=CC-BY-4.0\nversion=1.0.0\nshell=w10m\n"

    def theme(self, conf=CONF, with_pack=True, names=("wp_system", "wifi_icon")):
        root = os.path.join(helpers.tmpdir(), "dusk")
        os.makedirs(os.path.join(root, "images"))
        if conf is not None:
            with open(os.path.join(root, "theme.conf"), "w") as fh:
                fh.write(conf)
        for n in names:
            w, h = ui.REQUIRED["w10m"][n]
            png(os.path.join(root, "images", n + ".png"), w, h, alpha=128)
        if with_pack:
            ui.pack_folder(os.path.join(root, "images"), os.path.join(root, "pack.bin"))
        return root

    def test_a_theme_with_a_partial_pack_passes_and_lists_what_falls_back(self):
        rep = ui.check(self.theme())
        self.assertTrue(rep["ok"], rep)
        text = " ".join(c["text"] for c in rep["checks"])
        self.assertIn("19 of 21 names are not in this theme", text)                # the shell is read from theme.conf
        self.assertEqual([c["status"] for c in rep["checks"] if c["name"] == "w10m names"], ["warn"])

    def test_a_theme_for_another_shell_than_asked(self):
        rep = ui.check(self.theme(), "android")
        self.assertFalse(rep["ok"])

    def test_a_theme_for_any_shell_needs_a_shell_to_check_names(self):
        conf = self.CONF.replace("shell=w10m", "shell=any")
        rep = ui.check(self.theme(conf=conf))
        self.assertTrue(rep["ok"])
        self.assertTrue(any("--shell" in c["text"] for c in rep["checks"]))
        rep = ui.check(self.theme(conf=conf), "w10m")
        self.assertTrue(any(c["name"] == "w10m names" for c in rep["checks"]))

    def test_missing_conf_or_pictures(self):
        rep = ui.check(self.theme(conf=None))
        self.assertFalse(rep["ok"])
        self.assertIn("theme.conf", [c["name"] for c in rep["checks"] if c["status"] == "bad"])
        root = self.theme(with_pack=False, names=())
        rep = ui.check(root)
        self.assertFalse(rep["ok"])                                                # an images folder with no pictures

    def test_an_unbuilt_theme_is_checked_from_its_images(self):
        rep = ui.check(self.theme(with_pack=False))
        self.assertTrue(rep["ok"], rep)
        self.assertTrue(any("not built yet" in c["text"] for c in rep["checks"]))

    def test_a_folder_name_the_device_will_not_list_warns(self):
        root = self.theme()
        odd = os.path.join(os.path.dirname(root), "my theme!")
        os.rename(root, odd)
        rep = ui.check(odd)
        self.assertEqual([c["status"] for c in rep["checks"] if c["name"] == "id"], ["warn"])


class Commands(unittest.TestCase):
    def test_pack_then_check_then_the_exit_codes(self):
        src = folder_with("android")
        out = os.path.join(helpers.tmpdir(), "pack.bin")
        code, text = run("ui", "pack", src, "--out", out)
        self.assertEqual(code, 0)
        self.assertIn("31 picture(s)", text)
        code, text = run("ui", "check", out, "--shell", "android")
        self.assertEqual(code, 0)
        self.assertIn("passes the checks", text)
        code, text = run("ui", "check", out, "--shell", "w10m")
        self.assertEqual(code, 1)
        self.assertIn("fails the checks", text)
        code, text = run("ui", "check", out, "--shell", "android", "--json")
        self.assertTrue(json.loads(text)["ok"])

    def test_messages_for_a_missing_file_and_a_bad_folder(self):
        tmp = helpers.tmpdir()
        code, text = run("ui", "check", os.path.join(tmp, "absent.bin"))
        self.assertEqual(code, 1)
        self.assertIn("Not this time", text)
        code, text = run("ui", "pack", os.path.join(tmp, "absent"), "--out", os.path.join(tmp, "x.bin"))
        self.assertEqual(code, 1)
        self.assertIn("Not this time", text)

    def test_new_theme_makes_a_starter_that_needs_filling_in(self):
        ws = helpers.tmpdir()
        code, text = run("--workspace", ws, "new", "theme", "dusk")
        self.assertEqual(code, 0, text)
        root = os.path.join(ws, "projects", "dusk")
        self.assertTrue(os.path.isfile(os.path.join(root, "theme.conf")))
        self.assertTrue(os.path.isdir(os.path.join(root, "images")))
        readme = open(os.path.join(root, "README.txt")).read()
        self.assertEqual(len(readme.strip().splitlines()), 2)
        conf = open(os.path.join(root, "theme.conf")).read()
        self.assertIn("name=dusk", conf)
        # author and licence are empty on purpose: the check says so until they are filled in
        code, text = run("ui", "check", root)
        self.assertEqual(code, 1)
        self.assertIn("author", text)
        self.assertIn("licence", text)
        # a second one with the same name, and a name the device would not list
        code, text = run("--workspace", ws, "new", "theme", "dusk")
        self.assertEqual(code, 1)
        code, text = run("--workspace", ws, "new", "theme", "my theme")
        self.assertEqual(code, 1)

    def test_a_filled_in_theme_builds_into_something_that_checks(self):
        ws = helpers.tmpdir()
        run("--workspace", ws, "new", "theme", "dusk")
        root = os.path.join(ws, "projects", "dusk")
        with open(os.path.join(root, "theme.conf"), "w") as fh:
            fh.write("name=Dusk\nauthor=A\nlicence=MIT\nversion=1.0.0\nshell=w10m\n")
        png(os.path.join(root, "images", "wp_system.png"), 32, 32, alpha=128)
        code, text = run("--workspace", ws, "build", root)
        self.assertEqual(code, 0, text)
        built = os.path.join(root, "build", "dusk")
        self.assertTrue(os.path.isfile(os.path.join(built, "pack.bin")))
        self.assertTrue(os.path.isfile(os.path.join(built, "theme.conf")))
        code, text = run("ui", "check", built, "--shell", "w10m")
        self.assertEqual(code, 0, text)


class TheListsAgreeWithTheRepository(unittest.TestCase):
    """The names here, UI/PACK-NAMES.md and the tools that make the packs are one list; if one changes, this fails."""

    def path(self, *p):
        return os.path.join(helpers.ROOT, *p)

    def test_pack_names_md_lists_every_required_name(self):
        f = self.path("UI", "PACK-NAMES.md")
        if not os.path.isfile(f):
            self.skipTest("UI/PACK-NAMES.md is not in the kit")
        text = open(f, encoding="utf-8").read()
        ranges = {"st_bc": "`st_bc0` to `st_bc5`", "st_s": "`st_s0` to `st_s4`", "st_w": "`st_w0` to `st_w4`"}
        for shell, names in ui.REQUIRED.items():
            for n in names:
                if re.match(r"^st_(bc|s|w)[0-9]$", n):
                    continue
                self.assertIn("`%s`" % n, text, n)
        for rng in ranges.values():
            self.assertIn(rng, text)
        self.assertIn("%d required" % len(ui.REQUIRED["w10m"]), text)
        self.assertIn("%d required" % len(ui.REQUIRED["android"]), text)

    def test_the_windows_pack_tool_makes_exactly_the_required_windows_names(self):
        f = self.path("tools", "ui", "w10m_pack.py")
        if not os.path.isfile(f):
            self.skipTest("tools/ui is not in the kit")
        import importlib.util
        spec = importlib.util.spec_from_file_location("w10m_pack_under_test", f)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        self.assertEqual({n: v[:2] for n, v in mod.NAMES.items() if v[2]}, ui.REQUIRED["w10m"])
        self.assertEqual({n: v[:2] for n, v in mod.NAMES.items() if not v[2]}, ui.FALLBACK["w10m"])

    def test_the_froyo_tool_makes_the_required_android_names_at_their_sizes(self):
        f = self.path("tools", "ui", "froyo_pack.py")
        if not os.path.isfile(f):
            self.skipTest("tools/ui is not in the kit")
        import importlib.util
        spec = importlib.util.spec_from_file_location("froyo_pack_under_test", f)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        made = {n: (s, s) for n, (_f, s) in mod.LAUNCHER.items()}
        made.update({n: (mod.STATUS_SIZE, mod.STATUS_SIZE) for n in mod.STATUS})
        made.update({n: (mod.GLYPH_SIZE, mod.GLYPH_SIZE) for n in mod.GLYPHS})
        for n, size in ui.known("android").items():
            if n.startswith("img"):
                continue
            self.assertEqual(made.get(n), size, n)


if __name__ == "__main__":
    unittest.main()
