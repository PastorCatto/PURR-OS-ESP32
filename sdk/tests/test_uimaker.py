"""The UI maker (purrsdk/uimaker.py): the starter template builds a complete valid theme for both shells, your own pictures win and are resized, and a
SquareLine Studio export (LVGL 8 and 9 image arrays, project assets, a zip of either) is imported with a guess at each slot. The exports are made here in the
layouts SquareLine writes; they are not files from a real export."""
import os
import shutil
import unittest
import zipfile

import helpers
from purrsdk import ui, uimaker


def rgb565(r, g, b):
    return (r >> 3) << 11 | (g >> 2) << 5 | b >> 3


def sample(w, h, alpha=False):
    """RGBA bytes whose colours survive RGB565 exactly."""
    out = bytearray()
    for i in range(w * h):
        r, g, b = ((i * 8) % 248) & 0xF8, ((i * 4) % 252) & 0xFC, ((i * 16) % 248) & 0xF8
        r, g, b = r | r >> 5, g | g >> 6, b | b >> 5
        out += bytes([r, g, b, (i * 5) % 256 if alpha else 255])
    return bytes(out)


def c_array(name, w, h, rgba, v9=True, alpha=False):
    px = [(rgb565(rgba[i * 4], rgba[i * 4 + 1], rgba[i * 4 + 2]), rgba[i * 4 + 3]) for i in range(w * h)]
    if alpha and v9:
        data = b"".join(bytes([c & 255, c >> 8]) for c, _a in px) + bytes(a for _c, a in px)
        cf = "LV_COLOR_FORMAT_RGB565A8"
    elif alpha:
        data = b"".join(bytes([c & 255, c >> 8, a]) for c, a in px)
        cf = "LV_IMG_CF_TRUE_COLOR_ALPHA"
    else:
        data = b"".join(bytes([c & 255, c >> 8]) for c, _a in px)
        cf = "LV_COLOR_FORMAT_NATIVE" if v9 else "LV_IMG_CF_TRUE_COLOR"
    hexes = ", ".join("0x%02x" % b for b in data)
    if v9:
        return ("#include \"../ui.h\"\nconst LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST uint8_t ui_img_%s_png_data[] = { %s };\n"
                "const lv_image_dsc_t ui_img_%s_png = {\n  .header.w = %d,\n  .header.h = %d,\n  .data_size = sizeof(ui_img_%s_png_data),\n  .header.cf = %s,\n"
                "  .header.magic = LV_IMAGE_HEADER_MAGIC,\n  .data = ui_img_%s_png_data };\n") % (name, hexes, name, w, h, name, cf, name)
    return ("#include \"../ui.h\"\nconst LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST LV_ATTRIBUTE_IMG_UI_IMG_%s_PNG uint8_t ui_img_%s_png_data[] = {\n"
            "#if LV_COLOR_DEPTH == 1 || LV_COLOR_DEPTH == 8\n  0x00, 0x00,\n#endif\n#if LV_COLOR_DEPTH == 16 && LV_COLOR_16_SWAP == 0\n  %s\n#endif\n"
            "#if LV_COLOR_DEPTH == 16 && LV_COLOR_16_SWAP != 0\n  0xff, 0xff,\n#endif\n#if LV_COLOR_DEPTH == 32\n  0xff, 0xff,\n#endif\n};\n"
            "const lv_img_dsc_t ui_img_%s_png = {\n  .header.always_zero = 0,\n  .header.w = %d,\n  .header.h = %d,\n  .data_size = %d,\n  .header.cf = %s,\n"
            "  .data = ui_img_%s_png_data};\n") % (name.upper(), name, hexes, name, w, h, len(data), cf, name)


class Reading(unittest.TestCase):
    def check(self, v9, alpha):
        rgba = sample(6, 5, alpha)
        ident, w, h, got = uimaker.parse_squareline_c(c_array("home", 6, 5, rgba, v9, alpha))
        self.assertEqual((ident, w, h), ("home", 6, 5))
        self.assertEqual(got, rgba)

    def test_lvgl9_rgb565(self):
        self.check(True, False)

    def test_lvgl9_rgb565_with_alpha(self):
        self.check(True, True)

    def test_lvgl8_rgb565(self):
        self.check(False, False)

    def test_lvgl8_with_alpha(self):
        self.check(False, True)

    def test_32_bit_colour_is_refused_plainly(self):
        text = "const uint8_t ui_img_x_png_data[] = { 0x00, 0x01 };\n.header.w = 2,\n.header.h = 2,\n.header.cf = LV_COLOR_FORMAT_ARGB8888,\n"
        with self.assertRaises(ValueError) as cm:
            uimaker.parse_squareline_c(text)
        self.assertIn("16-bit", str(cm.exception))


class Making(unittest.TestCase):
    def setUp(self):
        self.tmp = helpers.tmpdir()

    def project(self, shell="android", extra=""):
        d = os.path.join(self.tmp, "p_" + shell)
        os.makedirs(d)
        with open(os.path.join(d, "ui.toml"), "w") as fh:
            fh.write(uimaker.starter("moon", shell) + extra)
        return d

    def test_starter_template_is_valid(self):
        for shell in ("android", "w10m"):
            cfg, problems = uimaker.load_template(uimaker.starter("moon", shell))
            self.assertEqual(problems, [], shell)
            self.assertEqual(cfg["shell"], shell)

    def test_bad_template_says_what_is_wrong(self):
        cfg, problems = uimaker.load_template(uimaker.starter("not ok!", "android").replace('shell = "android"', 'shell = "palm"'))
        self.assertTrue(any("name" in p for p in problems))
        self.assertTrue(any("shell" in p for p in problems))
        cfg, problems = uimaker.load_template("[ui\nname=")
        self.assertIsNone(cfg)

    def test_both_shells_make_a_complete_valid_theme(self):
        for shell, required in (("android", 31), ("w10m", 21)):
            r = uimaker.compose(self.project(shell), on_line=lambda s: None)
            self.assertTrue(r["ok"], (shell, r["problems"]))
            self.assertEqual(r["yours"], [])
            self.assertGreaterEqual(len(r["generated"]), required)
            rep = ui.check_pack_file(r["pack"], shell)
            self.assertTrue(rep["ok"], rep)
            self.assertTrue(os.path.isfile(os.path.join(r["out"], "theme.conf")))

    def test_your_picture_wins_and_is_resized_to_the_slot(self):
        d = self.project("android")
        os.makedirs(os.path.join(d, "images"))
        uimaker.write_png(os.path.join(d, "images", "fr_app.png"), 96, 96, sample(96, 96, True))
        r = uimaker.compose(d, on_line=lambda s: None)
        self.assertTrue(r["ok"], r["problems"])
        self.assertIn("fr_app", r["yours"])
        self.assertNotIn("fr_app", r["generated"])
        self.assertTrue(any(x.startswith("fr_app 96x96 -> 48x48") for x in r["resized"]))
        w, h, _ = uimaker.read_png(os.path.join(d, "build", "pack-src", "fr_app.png"))
        self.assertEqual((w, h), (48, 48))

    def test_mapping_in_ui_toml_points_a_slot_at_a_file(self):
        d = self.project("android", "")
        os.makedirs(os.path.join(d, "mine"))
        uimaker.write_png(os.path.join(d, "mine", "x.png"), 48, 48, sample(48, 48))
        with open(os.path.join(d, "ui.toml"), "a") as fh:
            fh.write('fr_settings = "mine/x.png"\nfr_gallery = "mine/missing.png"\n')
        r = uimaker.compose(d, on_line=lambda s: None)
        self.assertIn("fr_settings", r["yours"])
        self.assertIn("fr_gallery", r["generated"])
        self.assertTrue(any("missing.png" in n for n in r["notes"]))

    def test_scale_changes_sizes(self):
        d = self.project("android")
        text = open(os.path.join(d, "ui.toml")).read().replace("scale = 1.0", "scale = 2.0")
        open(os.path.join(d, "ui.toml"), "w").write(text)
        cfg, _ = uimaker.load_template(os.path.join(d, "ui.toml"))
        self.assertEqual(uimaker.wanted(cfg)["fr_app"], (96, 96))

    def test_wallpapers_count(self):
        d = self.project("w10m")
        text = open(os.path.join(d, "ui.toml")).read().replace("wallpapers = 1", "wallpapers = 3")
        open(os.path.join(d, "ui.toml"), "w").write(text)
        cfg, _ = uimaker.load_template(os.path.join(d, "ui.toml"))
        self.assertEqual([n for n in uimaker.wanted(cfg) if n.startswith("img")], ["img0", "img1", "img2"])


class Importing(unittest.TestCase):
    def setUp(self):
        self.tmp = helpers.tmpdir()
        self.export = os.path.join(self.tmp, "export")
        os.makedirs(os.path.join(self.export, "images"))
        os.makedirs(os.path.join(self.export, "assets"))
        with open(os.path.join(self.export, "images", "ui_img_fr_app_png.c"), "w") as fh:
            fh.write(c_array("fr_app", 48, 48, sample(48, 48), True))
        with open(os.path.join(self.export, "images", "ui_img_wallpaper_png.c"), "w") as fh:
            fh.write(c_array("wallpaper", 8, 8, sample(8, 8, True), False, True))
        uimaker.write_png(os.path.join(self.export, "assets", "fr_settings.png"), 40, 40, sample(40, 40, True))
        uimaker.write_png(os.path.join(self.export, "assets", "fr_app.png"), 48, 48, sample(48, 48))      # the same picture as an array: the PNG wins

    def test_folder_import_guesses_slots(self):
        dest = os.path.join(self.tmp, "proj")
        os.makedirs(dest)
        r = uimaker.import_squareline(self.export, dest, "android", on_line=lambda s: None)
        by = {p["id"]: p for p in r["pictures"]}
        self.assertEqual(sorted(by), ["fr_app", "fr_settings", "wallpaper"])
        self.assertEqual(by["fr_app"]["slot"], "fr_app")
        self.assertEqual(by["fr_app"]["from"], "assets/fr_app.png")
        self.assertEqual(by["fr_settings"]["slot"], "fr_settings")
        self.assertIsNone(by["wallpaper"]["slot"])
        self.assertTrue(os.path.isfile(os.path.join(dest, "imported", "fr_settings.png")))

    def test_zip_import(self):
        z = os.path.join(self.tmp, "export.zip")
        with zipfile.ZipFile(z, "w") as zf:
            for dp, _d, fns in os.walk(self.export):
                for f in fns:
                    full = os.path.join(dp, f)
                    zf.write(full, os.path.relpath(full, self.export))
        dest = os.path.join(self.tmp, "proj2")
        os.makedirs(dest)
        r = uimaker.import_squareline(z, dest, "android", on_line=lambda s: None)
        self.assertEqual(len(r["pictures"]), 3)

    def test_zip_that_leaves_its_folder_is_refused(self):
        z = os.path.join(self.tmp, "evil.zip")
        with zipfile.ZipFile(z, "w") as zf:
            zf.writestr("../escape.txt", "x")
        with self.assertRaises(ValueError):
            uimaker.import_squareline(z, os.path.join(self.tmp, "d"), "android", on_line=lambda s: None)

    def test_nothing_to_import(self):
        empty = os.path.join(self.tmp, "empty")
        os.makedirs(empty)
        with self.assertRaises(ValueError) as cm:
            uimaker.import_squareline(empty, os.path.join(self.tmp, "d2"), "android", on_line=lambda s: None)
        self.assertIn("no pictures", str(cm.exception))

    def test_import_then_make(self):
        ws = type("W", (), {})()
        ws.projects = os.path.join(self.tmp, "projects")
        os.makedirs(ws.projects)
        r = uimaker.new_project(ws, "sq", "android", "tdeck_plus", squareline=self.export, on_line=lambda s: None)
        text = open(os.path.join(r["path"], "ui.toml")).read()
        self.assertIn('fr_app = "imported/fr_app.png"', text)
        self.assertIn('fr_settings = "imported/fr_settings.png"', text)
        made = uimaker.compose(r["path"], on_line=lambda s: None)
        self.assertTrue(made["ok"], made["problems"])
        self.assertIn("fr_settings", made["yours"])
        self.assertTrue(any(x.startswith("fr_settings 40x40 -> 48x48") for x in made["resized"]))


if __name__ == "__main__":
    unittest.main()


class Mapping(unittest.TestCase):
    base = uimaker.starter("moon", "android")

    @staticmethod
    def active(text):
        return [l.strip() for l in text.split("\n") if l.strip() and not l.strip().startswith("#")]

    def test_adds_replaces_and_removes(self):
        t = uimaker.set_mapping(self.base, "fr_app", "imported/a.png")
        self.assertIn('fr_app = "imported/a.png"', self.active(t))
        t = uimaker.set_mapping(t, "fr_app", "imported/b.png")
        self.assertEqual([l for l in self.active(t) if l.startswith("fr_app")], ['fr_app = "imported/b.png"'])
        t = uimaker.set_mapping(t, "fr_app", None)
        self.assertEqual([l for l in self.active(t) if l.startswith("fr_app")], [])

    def test_moving_a_picture_to_another_slot_clears_the_old_one(self):
        t = uimaker.set_mapping(self.base, "fr_app", "imported/a.png")
        t = uimaker.set_mapping(t, "fr_gallery", "imported/a.png", old_slot="fr_app")
        self.assertIn('fr_gallery = "imported/a.png"', self.active(t))
        self.assertNotIn('fr_app = "imported/a.png"', self.active(t))

    def test_the_result_still_reads(self):
        t = uimaker.set_mapping(self.base, "fr_settings", "imported/x.png")
        cfg, problems = uimaker.load_template(t)
        self.assertEqual(problems, [])
        self.assertEqual(cfg["pictures"], {"fr_settings": "imported/x.png"})

    def test_refuses_odd_paths(self):
        with self.assertRaises(ValueError):
            uimaker.set_mapping(self.base, "fr_app", "../../etc/passwd; rm")
