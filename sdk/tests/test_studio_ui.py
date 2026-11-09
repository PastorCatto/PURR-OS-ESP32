"""Studio's make-a-UI routes over the real HTTP handler: start a project (fresh, and from a SquareLine zip), edit, map, preview and make it."""
import os
import time
import unittest
import zipfile

import helpers
import test_studio
import test_uimaker
from purrsdk import uimaker


class UiRoutes(test_studio.Studio):
    def wait_job(self, jid):
        for _ in range(300):
            st, j = self.call("GET", "/api/job?id=%s&since=0" % jid)
            if j["done"]:
                return j
            time.sleep(0.05)
        self.fail("the job did not finish")

    def test_starter_text_reads(self):
        st, body = self.call("GET", "/api/ui/starter?name=moon&shell=w10m&board=tdeck")
        self.assertEqual(st, 200)
        cfg, problems = uimaker.load_template(body["text"])
        self.assertEqual(problems, [])
        self.assertEqual(cfg["shell"], "w10m")

    def test_fresh_project_edit_preview_and_make(self):
        st, body = self.call("POST", "/api/ui/new", {"name": "freshui", "shell": "android", "board": "tdeck_plus"})
        self.assertEqual(st, 200, body)
        st, listing = self.call("GET", "/api/ui/projects")
        self.assertIn("freshui", [p["name"] for p in listing["projects"]])
        st, proj = self.call("GET", "/api/ui/project?name=freshui")
        self.assertEqual(proj["shell"], "android")
        self.assertIn("fr_app", proj["slots"])
        text = proj["text"].replace("#60a917", "#1ba1e2")
        st, saved = self.call("POST", "/api/ui/save", {"name": "freshui", "text": text})
        self.assertEqual(saved["problems"], [])
        st, prev = self.call("POST", "/api/ui/preview", {"text": text})
        self.assertEqual(st, 200)
        self.assertEqual(prev["colours"]["accent"], "#1ba1e2")
        self.assertTrue(prev["pictures"][0]["data"].startswith("data:image/png;base64,"))
        st, mk = self.call("POST", "/api/ui/make", {"name": "freshui"})
        j = self.wait_job(mk["job"])
        self.assertTrue(j["result"]["ok"], j["lines"])
        self.assertEqual(j["result"]["yours"], [])
        self.assertTrue(os.path.isfile(os.path.join(j["result"]["out"], "pack.bin")))

    def test_bad_template_is_a_plain_message_not_a_crash(self):
        st, body = self.call("POST", "/api/ui/preview", {"text": "[ui\nbroken"})
        self.assertEqual(st, 400)
        self.assertIn("error", body)

    def test_squareline_zip_upload_import_map_and_make(self):
        tmp = helpers.tmpdir()
        z = os.path.join(tmp, "export.zip")
        with zipfile.ZipFile(z, "w") as zf:
            zf.writestr("images/ui_img_fr_settings_png.c", test_uimaker.c_array("fr_settings", 40, 40, test_uimaker.sample(40, 40, True), False, True))
            zf.writestr("images/ui_img_hero_png.c", test_uimaker.c_array("hero", 8, 8, test_uimaker.sample(8, 8), True))
        with open(z, "rb") as fh:
            st, up = self.call("POST", "/api/ui/upload?kind=zip", raw=fh.read())
        self.assertEqual(st, 200)
        st, body = self.call("POST", "/api/ui/new", {"name": "squi", "shell": "android", "board": "tdeck_plus", "squareline": up["path"]})
        self.assertEqual(st, 200, body)
        st, proj = self.call("GET", "/api/ui/project?name=squi")
        by = {p["id"]: p for p in proj["pictures"]}
        self.assertEqual(by["fr_settings"]["slot"], "fr_settings")
        self.assertIsNone(by["hero"]["slot"])
        st, m = self.call("POST", "/api/ui/map", {"name": "squi", "slot": "fr_gallery", "file": "imported/hero.png"})
        self.assertIn('fr_gallery = "imported/hero.png"', m["text"])
        st, mk = self.call("POST", "/api/ui/make", {"name": "squi"})
        j = self.wait_job(mk["job"])
        self.assertTrue(j["result"]["ok"], j["lines"])
        self.assertIn("fr_settings", j["result"]["yours"])
        self.assertIn("fr_gallery", j["result"]["yours"])
        self.assertTrue(any(x.startswith("fr_gallery 8x8 -> 48x48") for x in j["result"]["resized"]))

    def test_project_names_cannot_escape(self):
        st, body = self.call("GET", "/api/ui/project?name=../x")
        self.assertEqual(st, 400)


if __name__ == "__main__":
    unittest.main()
