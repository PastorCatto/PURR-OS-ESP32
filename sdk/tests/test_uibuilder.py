"""The layout side of the UI builder: elements become the screen file the device parses, icon packs are imported and matched, the activity log orders itself."""
import os
import time
import unittest
import zipfile

import helpers
import test_studio
import test_uimaker
from purrsdk import activity, uibuilder, uimaker, ui


class Compile(unittest.TestCase):
    SIZE = (320, 240)

    def conf(self, start="android", assets=("kb_home", "kb_back", "kb_menu", "fr_dock_apps", "st_w4", "st_b100")):
        el = uibuilder.starter_layout(start, self.SIZE)
        return uibuilder.to_screen_conf(el, "home", self.SIZE, assets)

    def test_the_android_start_compiles_and_reads_back(self):
        text, notes, problems = self.conf("android")
        self.assertEqual(problems, [])
        scr = uibuilder.parse_screen_conf(text)
        self.assertEqual(scr["name"], "home")
        self.assertEqual(scr["background"], "#0b1c10")
        types = [w["type"] for w in scr["widgets"]]
        self.assertIn("app_grid", types)
        self.assertGreaterEqual(types.count("image"), 3)            # the three keys and the drawer, drawn from the pack's own pictures
        self.assertEqual(scr["ignored"], [])
        self.assertLessEqual(len(scr["widgets"]), uibuilder.MAX_WIDGETS)

    def test_actions_are_kept_and_reported_not_silently_dropped(self):
        _t, notes, _p = self.conf("android")
        self.assertTrue(any("go home" in n or "should home" in n or "tap should home" in n for n in notes))
        self.assertTrue(any("not wired" in n for n in notes))

    def test_the_metro_start_uses_labels_and_buttons(self):
        text, _n, problems = self.conf("metro")
        scr = uibuilder.parse_screen_conf(text)
        self.assertEqual(problems, [])
        self.assertIn("label", [w["type"] for w in scr["widgets"]])
        clock = [w for w in scr["widgets"] if w["type"] == "label"][0]
        self.assertEqual(clock["text"], "09:41")
        self.assertGreaterEqual(clock["scale"], 2)

    def test_more_than_sixteen_widgets_is_a_plain_problem(self):
        el = uibuilder.starter_layout("blank", self.SIZE)
        for i in range(20):
            el.append({"screen": "home", "kind": "box", "glyph": "tile", "name": "b%d" % i, "x": 0, "y": 0, "w": 20, "h": 20, "label": "x", "action": "nothing", "color": "#2b2b2b"})
        text, _n, problems = uibuilder.to_screen_conf(el, "home", self.SIZE, ())
        self.assertTrue(any("at most 16" in p for p in problems))
        self.assertEqual(len(uibuilder.parse_screen_conf(text)["widgets"]), 16)         # the device keeps the first 16 and ignores the rest

    def test_things_off_the_screen_are_noted(self):
        el = [{"screen": "home", "kind": "box", "glyph": "tile", "name": "wide", "x": 300, "y": 0, "w": 100, "h": 20, "label": "x", "action": "nothing", "color": "#2b2b2b"}]
        _t, notes, _p = uibuilder.to_screen_conf(el, "home", self.SIZE, ())
        self.assertTrue(any("cut off" in n for n in notes))

    def test_a_widget_the_device_lacks_is_drawn_as_a_button(self):
        el = [{"screen": "home", "kind": "glyph", "glyph": "slider", "name": "vol", "x": 10, "y": 10, "w": 100, "h": 20, "label": "", "action": "nothing", "color": "#2b2b2b"}]
        text, notes, _p = uibuilder.to_screen_conf(el, "home", self.SIZE, ())
        self.assertEqual(uibuilder.parse_screen_conf(text)["widgets"][0]["type"], "button")
        self.assertTrue(any("no widget of its own" in n for n in notes))

    def test_quotes_in_labels_cannot_break_the_file(self):
        el = [{"screen": "home", "kind": "text", "glyph": "text", "name": 'a"b', "x": 0, "y": 0, "w": 50, "h": 16, "label": 'say "hi"\nnow', "action": "nothing", "color": "#fff"}]
        text, _n, _p = uibuilder.to_screen_conf(el, "home", self.SIZE, ())
        w = uibuilder.parse_screen_conf(text)["widgets"][0]
        self.assertNotIn('"', w["text"])

    def test_parser_copy_refuses_what_the_device_refuses(self):
        for bad in ('screen "x" {\n  label "a" {\n', 'label "a" {\n}\n', 'screen "x" {\n  oops\n}\n'):
            with self.assertRaises(ValueError):
                uibuilder.parse_screen_conf(bad)

    def test_element_validation(self):
        with self.assertRaises(ValueError):
            uibuilder.element({"kind": "banana"})
        with self.assertRaises(ValueError):
            uibuilder.element({"kind": "box", "action": "explode"})


class Project(unittest.TestCase):
    def setUp(self):
        self.tmp = helpers.tmpdir()
        self.ws = type("W", (), {})()
        self.ws.projects = os.path.join(self.tmp, "projects")
        self.ws.path = self.tmp
        os.makedirs(self.ws.projects)

    def test_new_ui_has_a_layout_and_makes_screen_files(self):
        r = uimaker.new_project(self.ws, "lay", "android", "tdeck_plus", on_line=lambda s: None, start="android")
        cfg, problems = uimaker.load_template(os.path.join(r["path"], "ui.toml"))
        self.assertEqual(problems, [])
        self.assertGreaterEqual(len(cfg["elements"]), 9)
        made = uimaker.compose(r["path"], on_line=lambda s: None)
        self.assertTrue(made["ok"], made["problems"])
        self.assertEqual(made["screens"], ["home"])
        scr = uibuilder.parse_screen_conf(open(os.path.join(made["out"], "shell.conf")).read())
        self.assertGreater(len(scr["widgets"]), 3)
        self.assertTrue(os.path.isfile(os.path.join(made["out"], "screens", "home.conf")))

    def test_elements_round_trip_through_ui_toml(self):
        text = uimaker.starter("rt", "android", "tdeck_plus")
        els = uibuilder.starter_layout("android", (320, 240))
        text = uimaker.save_elements(text, els)
        cfg, problems = uimaker.load_template(text)
        self.assertEqual(problems, [])
        self.assertEqual(cfg["elements"], [uibuilder.element(e) for e in els])
        again = uimaker.save_elements(text, els[:3])
        self.assertEqual(len(uimaker.load_template(again)[0]["elements"]), 3)
        self.assertEqual(again.count("[[element]]"), 3)

    def test_the_e_paper_screen_is_200_square(self):
        r = uimaker.new_project(self.ws, "epd", "android", "waveshare154", on_line=lambda s: None, start="blank")
        cfg, _p = uimaker.load_template(os.path.join(r["path"], "ui.toml"))
        self.assertEqual(cfg["size"], (200, 200))


class IconPacks(unittest.TestCase):
    def setUp(self):
        self.tmp = helpers.tmpdir()
        self.ws = type("W", (), {})()
        self.ws.projects = os.path.join(self.tmp, "projects")
        self.ws.path = self.tmp
        os.makedirs(self.ws.projects)
        self.r = uimaker.new_project(self.ws, "ip", "android", "tdeck_plus", on_line=lambda s: None, start="blank")
        self.pack = os.path.join(self.tmp, "mypack")
        os.makedirs(os.path.join(self.pack, "deep", "er"))
        uimaker.write_png(os.path.join(self.pack, "fr_app.png"), 64, 64, test_uimaker.sample(64, 64, True))                # wrong size: resized
        uimaker.write_png(os.path.join(self.pack, "deep", "er", "fr_settings.png"), 48, 48, test_uimaker.sample(48, 48, True))  # right size, found in a subfolder
        uimaker.write_png(os.path.join(self.pack, "logo.png"), 16, 16, test_uimaker.sample(16, 16))                        # not a slot

    def test_import_folder_matches_and_reports(self):
        info = uimaker.import_icon_pack(self.pack, self.r["path"], "android", on_line=lambda s: None)
        by = {p["id"]: p for p in info["pictures"]}
        self.assertEqual(by["fr_app"]["slot"], "fr_app")
        self.assertIsNone(by["logo"]["slot"])
        rep = uimaker.icon_report(self.r["path"])
        st = {s["name"]: s["state"] for s in rep["slots"]}
        self.assertEqual(st["fr_app"], "resized")
        self.assertEqual(st["fr_settings"], "yours")
        self.assertEqual(st["fr_gallery"], "studio")
        self.assertEqual(rep["matched"], 2)
        self.assertEqual(rep["resized"], 1)

    def test_use_false_ignores_the_pack(self):
        uimaker.import_icon_pack(self.pack, self.r["path"], "android", on_line=lambda s: None)
        toml = os.path.join(self.r["path"], "ui.toml")
        with open(toml) as fh:
            text = uimaker.set_icons(fh.read(), use=False)
        with open(toml, "w") as fh:
            fh.write(text)
        rep = uimaker.icon_report(self.r["path"])
        self.assertEqual(rep["matched"], 0)

    def test_missing_slots_fail_when_studio_may_not_fill_them(self):
        uimaker.import_icon_pack(self.pack, self.r["path"], "android", on_line=lambda s: None)
        toml = os.path.join(self.r["path"], "ui.toml")
        with open(toml) as fh:
            text = uimaker.set_icons(fh.read(), pack="mypack", fill_missing=False)
        with open(toml, "w") as fh:
            fh.write(text)
        made = uimaker.compose(self.r["path"], on_line=lambda s: None)
        self.assertFalse(made["ok"])
        self.assertTrue(any("no picture for" in p for p in made["problems"]))

    def test_recolour_changes_colour_and_keeps_alpha(self):
        out = uimaker.tint(bytes([10, 20, 30, 128, 1, 2, 3, 0]), (96, 169, 23))
        self.assertEqual(out, bytes([96, 169, 23, 128, 96, 169, 23, 0]))

    def test_a_pack_bin_is_unpacked(self):
        folder = os.path.join(self.tmp, "src")
        os.makedirs(folder)
        uimaker.write_png(os.path.join(folder, "fr_app.png"), 48, 48, test_uimaker.sample(48, 48, True))
        ui.pack_folder(folder, os.path.join(self.tmp, "pack.bin"))
        info = uimaker.import_icon_pack(os.path.join(self.tmp, "pack.bin"), self.r["path"], "android", on_line=lambda s: None)
        self.assertEqual([p["slot"] for p in info["pictures"]], ["fr_app"])
        w, h, rgba = uimaker.read_png(os.path.join(self.r["path"], "imported", "fr_app.png"))
        self.assertEqual((w, h), (48, 48))
        self.assertEqual(rgba, test_uimaker.sample(48, 48, True))        # RGB565 and the alpha plane survive a round trip exactly


class ActivityLog(unittest.TestCase):
    def setUp(self):
        self.ws = type("W", (), {})()
        self.ws.path = helpers.tmpdir()

    def test_newest_first_and_no_repeats(self):
        activity.record(self.ws, "ui", "a", "builder", "a", when=100)
        activity.record(self.ws, "image", "b", "image", when=200)
        activity.record(self.ws, "ui", "a", "builder", "a", note="again", when=300)
        items = activity.load(self.ws)
        self.assertEqual([i["title"] for i in items], ["a", "b"])
        self.assertEqual(items[0]["note"], "again")

    def test_unknown_kind_becomes_other_and_corrupt_file_is_empty(self):
        activity.record(self.ws, "banana", "x", "home")
        self.assertEqual(activity.load(self.ws)[0]["kind"], "other")
        open(os.path.join(self.ws.path, "activity.json"), "w").write("{not json")
        self.assertEqual(activity.load(self.ws), [])

    def test_ago(self):
        self.assertEqual(activity.ago(1000, 1010), "just now")
        self.assertEqual(activity.ago(0, 600), "10 min ago")
        self.assertEqual(activity.ago(0, 3 * 3600), "3 h ago")
        self.assertEqual(activity.ago(0, 86400 + 5), "yesterday")


class Routes(test_studio.Studio):
    def wait_job(self, jid):
        for _ in range(300):
            st, j = self.call("GET", "/api/job?id=%s&since=0" % jid)
            if j["done"]:
                return j
            time.sleep(0.05)
        self.fail("the job did not finish")

    def test_builder_round_trip_and_activity(self):
        st, body = self.call("POST", "/api/ui/new", {"name": "bld", "shell": "android", "board": "tdeck_plus", "start": "android"})
        self.assertEqual(st, 200, body)
        st, b = self.call("GET", "/api/ui/builder?name=bld")
        self.assertEqual(st, 200)
        self.assertEqual((b["size"]["w"], b["size"]["h"]), (320, 240))
        self.assertGreaterEqual(len(b["elements"]), 9)
        els = b["elements"] + [{"screen": "home", "kind": "text", "glyph": "clock", "name": "clock", "x": 200, "y": 40, "w": 100, "h": 32, "label": "09:41", "action": "nothing", "color": "#ffffff"}]
        st, saved = self.call("POST", "/api/ui/builder/save", {"name": "bld", "elements": els})
        self.assertEqual(st, 200, saved)
        self.assertEqual(saved["count"], len(els))
        st, act = self.call("GET", "/api/activity")
        self.assertEqual(act["items"][0]["title"], "bld")
        self.assertEqual(act["items"][0]["kind"], "ui")
        st, mk = self.call("POST", "/api/ui/make", {"name": "bld"})
        j = self.wait_job(mk["job"])
        self.assertTrue(j["result"]["ok"], j["lines"])

    def test_bad_element_is_a_plain_error(self):
        self.call("POST", "/api/ui/new", {"name": "bad1", "shell": "android", "board": "tdeck_plus", "start": "blank"})
        st, body = self.call("POST", "/api/ui/builder/save", {"name": "bad1", "elements": [{"kind": "banana"}]})
        self.assertEqual(st, 400)

    def test_icon_pack_import_through_the_routes(self):
        self.call("POST", "/api/ui/new", {"name": "icp", "shell": "android", "board": "tdeck_plus", "start": "blank"})
        tmp = helpers.tmpdir()
        z = os.path.join(tmp, "pack.zip")
        buf = os.path.join(tmp, "fr_app.png")
        uimaker.write_png(buf, 64, 64, test_uimaker.sample(64, 64, True))
        with zipfile.ZipFile(z, "w") as zf:
            zf.write(buf, "fr_app.png")
        with open(z, "rb") as fh:
            st, up = self.call("POST", "/api/ui/upload?kind=zip", raw=fh.read())
        st, rep = self.call("POST", "/api/ui/icons/import", {"name": "icp", "src": up["path"]})
        self.assertEqual(st, 200, rep)
        self.assertEqual(rep["matched"], 1)
        self.assertEqual(rep["resized"], 1)
        st, rep = self.call("POST", "/api/ui/icons/options", {"name": "icp", "use": False})
        self.assertEqual(rep["matched"], 0)
        st, rep = self.call("POST", "/api/ui/icons/options", {"name": "icp", "use": True, "recolour": True})
        self.assertTrue(rep["options"]["recolour"])
        self.assertEqual(self.call("POST", "/api/ui/icons/import", {"name": "icp", "src": "/no/such"})[0], 400)

    def test_slot_picture_made_export_and_reveal_guard(self):
        self.call("POST", "/api/ui/new", {"name": "ex1", "shell": "android", "board": "tdeck_plus", "start": "android"})
        tmp = helpers.tmpdir()
        png = os.path.join(tmp, "x.png")
        uimaker.write_png(png, 48, 48, test_uimaker.sample(48, 48, True))
        with open(png, "rb") as fh:
            st, rep = self.call("POST", "/api/ui/icons/slot?name=ex1&slot=fr_app", raw=fh.read())
        self.assertEqual(st, 200, rep)
        self.assertEqual({s["name"]: s["state"] for s in rep["slots"]}["fr_app"], "yours")
        self.assertEqual(self.call("POST", "/api/ui/icons/slot?name=ex1&slot=nope", raw=b"x")[0], 400)
        self.assertEqual(self.call("POST", "/api/ui/icons/slot?name=ex1&slot=fr_app", raw=b"not a png")[0], 400)
        self.assertEqual(self.call("GET", "/api/ui/made?name=ex1")[1], {})
        self.assertEqual(self.call("POST", "/api/ui/export", {"name": "ex1"})[0], 400)          # not made yet
        st, mk = self.call("POST", "/api/ui/make", {"name": "ex1"})
        self.wait_job(mk["job"])
        st, made = self.call("GET", "/api/ui/made?name=ex1")
        self.assertGreater(made["count"], 30)
        self.assertEqual(made["screens"], ["home"])
        st, z = self.call("POST", "/api/ui/export", {"name": "ex1"})
        self.assertEqual(st, 200, z)
        with zipfile.ZipFile(z["zip"]) as zf:
            names = zf.namelist()
        self.assertIn("ex1/pack.bin", names)
        self.assertIn("ex1/shell.conf", names)
        self.assertEqual(self.call("POST", "/api/reveal", {"path": "/etc/passwd"})[0], 400)       # only files inside the workspace


if __name__ == "__main__":
    unittest.main()
