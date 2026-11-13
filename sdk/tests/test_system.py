"""A system project: choices saved, drivers filtered by board, and the whole road (UI, drivers, parts, image, delivery folder) run on synthetic signed parts."""
import hashlib
import os
import time
import unittest
from unittest import mock

import helpers
import test_studio
from purrsdk import formats, system, uimaker, workspace


def make_parts(folder, board="tdeck_plus"):
    img = formats.image()
    os.makedirs(folder, exist_ok=True)

    def put(name, t, sub, size, version="1.1"):
        with open(os.path.join(folder, name), "wb") as fh:
            fh.write(img.make_image(name, version, t, sub, b"\xe9" + b"\x01" * (size - 1), img.CHIP_ESP32S3))
    put("bootloader-%s.cat" % board, img.IMG_MODULE, img.MOD_BOOTLOADER, 30000, "0.4.0")
    put("bootpkg-%s.cat" % board, img.IMG_MODULE, img.MOD_BOOTPKG, 5000, "0.3.1")
    put("kittenos-%s.kitt" % board, img.IMG_RECOVERY, 0, 100000)
    put("kernel-%s.cat" % board, img.IMG_MODULE, img.MOD_KERNEL, 200000)
    put("loader-%s.cat" % board, img.IMG_MODULE, img.MOD_LOADER, 100000)


def make_drivers(folder):
    os.makedirs(folder, exist_ok=True)
    stanzas = ["release=test", "released=2026-11-08", ""]
    for name, board, data in (("bme280", "any", b"a" * 40), ("kbd-raw", "tdeck_plus", b"b" * 41), ("kbd-raw", "tdeck", b"b" * 41), ("only-epd", "waveshare154", b"c" * 42)):
        fname = "%s-%s.cat" % (name, board)
        with open(os.path.join(folder, fname), "wb") as fh:
            fh.write(data)
        stanzas += ["component=%s" % name, "type=driver", "version=1.0.0", "chip=esp32s3", "board=%s" % board, "file=%s" % fname, "size=%d" % len(data),
                    "sha256=%s" % hashlib.sha256(data).hexdigest(), "key=developer", ""]
    with open(os.path.join(folder, "drivers.manifest"), "w") as fh:
        fh.write("\n".join(stanzas))


class Setup(unittest.TestCase):
    def setUp(self):
        self.tmp = helpers.tmpdir()
        self.ws = workspace.Workspace(os.path.join(self.tmp, "ws")).ensure()
        self.parts = os.path.join(self.tmp, "parts")
        self.drivers = os.path.join(self.tmp, "drivers")
        make_parts(self.parts)
        make_drivers(self.drivers)
        self._env = dict(os.environ)
        os.environ["PURR_IMAGES"] = self.parts
        os.environ["PURR_DRIVERS"] = self.drivers

    def tearDown(self):
        os.environ.clear()
        os.environ.update(self._env)


class Choices(Setup):
    def test_catalog_filters_by_board_and_prefers_the_boards_own_entry(self):
        names = {d["name"]: d for d in system.driver_catalog("tdeck_plus")}
        self.assertEqual(sorted(names), ["bme280", "kbd-raw"])
        self.assertEqual(names["kbd-raw"]["scope"], "tdeck_plus")
        self.assertEqual(names["bme280"]["scope"], "any")
        self.assertEqual(sorted(d["name"] for d in system.driver_catalog("waveshare154")), ["bme280", "only-epd"])

    def test_save_load_round_trip(self):
        s = system.new_system(self.ws, "lunar", "tdeck_plus")
        self.assertEqual(s["drivers"], [])
        s.update(drivers=["bme280"], drivers_set=True, apps=["hello"], key="developer.key", key_id=4, ui="moonlit")
        system.save(self.ws, "lunar", s)
        again = system.load(self.ws, "lunar")
        self.assertEqual((again["drivers"], again["apps"], again["ui"], again["key_id"]), (["bme280"], ["hello"], "moonlit", 4))
        self.assertEqual([x["name"] for x in system.list_systems(self.ws)], ["lunar"])

    def test_a_playtime_system_starts_from_source_with_the_type_shell(self):
        """There are no ready-made Playtime parts and the board has its own launcher, so a new Playtime system is set up for that; other boards are not."""
        p = system.new_system(self.ws, "play", "playtime")
        self.assertEqual((p["board"], p["parts_from"], p["shell"]), ("playtime", "source", "type"))
        t = system.new_system(self.ws, "deck", "tdeck_plus")
        self.assertEqual((t["parts_from"], t["shell"]), ("ready", "android"))

    def test_bad_names_and_boards_are_refused(self):
        for bad in ("", "../x", ".hidden", "a/b"):
            with self.assertRaises(system.SystemError_):
                system.new_system(self.ws, bad, "tdeck_plus")
        with self.assertRaises(system.SystemError_):
            system.new_system(self.ws, "x", "palm")
        system.new_system(self.ws, "dup", "tdeck")
        with self.assertRaises(system.SystemError_):
            system.new_system(self.ws, "dup", "tdeck")

    def test_overview_defaults_every_driver_on_until_you_choose(self):
        system.new_system(self.ws, "ov", "tdeck_plus")
        o = system.overview(self.ws, "ov")
        self.assertTrue(all(d["on"] for d in o["drivers"]))
        self.assertTrue(o["plan"]["ok"])
        self.assertEqual(o["folder"], self.parts)


@unittest.skipUnless(helpers.HAVE_CRYPTO, "needs the cryptography package")
class Road(Setup):
    def steps_of(self, r):
        return {k: v["state"] for k, v in r["steps"].items()}

    def test_the_whole_road_with_a_ui_and_drivers(self):
        uimaker.new_project(self.ws, "moonlit", "android", "tdeck_plus", on_line=lambda s: None, start="android")
        s = system.new_system(self.ws, "road", "tdeck_plus")
        s.update(drivers=["bme280", "kbd-raw"], drivers_set=True, ui="moonlit")
        system.save(self.ws, "road", s)
        seen = []
        r = system.build(self.ws, "road", on_step=lambda i, st, n: seen.append((i, st)), on_line=lambda l: None)
        self.assertTrue(r["ok"], r["steps"])
        self.assertEqual(self.steps_of(r), {"apps": "skip", "ui": "done", "drivers": "done", "parts": "done", "image": "done", "delivery": "done", "verify": "done"})
        self.assertIn(("image", "run"), seen)
        self.assertEqual(os.path.getsize(r["image"]), 16 * 1024 * 1024)
        rec = os.path.join(r["delivery"], "recovery")
        self.assertTrue(os.path.isfile(os.path.join(rec, "kernel-tdeck_plus.cat")))
        self.assertTrue(os.path.isfile(os.path.join(rec, "recovery.manifest")))
        text = open(os.path.join(rec, "drivers.manifest")).read()
        self.assertIn("component=bme280", text)
        self.assertIn("component=kbd-raw", text)
        self.assertTrue(os.path.isfile(os.path.join(rec, "kbd-raw-tdeck_plus.cat")))
        self.assertTrue(os.path.isfile(os.path.join(r["delivery"], "ui", "themes", "moonlit", "pack.bin")))

    def test_only_the_drivers_you_switched_on_are_gathered(self):
        s = system.new_system(self.ws, "few", "tdeck_plus")
        s.update(drivers=["bme280"], drivers_set=True)
        system.save(self.ws, "few", s)
        r = system.build(self.ws, "few", on_line=lambda l: None)
        text = open(os.path.join(r["delivery"], "recovery", "drivers.manifest")).read()
        self.assertIn("bme280", text)
        self.assertNotIn("kbd-raw", text)

    def test_a_damaged_driver_stops_the_road_with_its_name(self):
        with open(os.path.join(self.drivers, "bme280-any.cat"), "ab") as fh:
            fh.write(b"x")
        system.new_system(self.ws, "bad", "tdeck_plus")
        r = system.build(self.ws, "bad", on_line=lambda l: None)
        self.assertFalse(r["ok"])
        self.assertEqual(r["steps"]["drivers"]["state"], "fail")
        self.assertIn("bme280", r["steps"]["drivers"]["note"])

    def test_missing_parts_say_what_to_do(self):
        s = system.new_system(self.ws, "np", "tdeck_plus")
        with mock.patch.object(system, "parts_folders", lambda board: []):                 # as on a computer with no kit images and no dev server folder
            r = system.build(self.ws, "np", on_line=lambda l: None)
        self.assertFalse(r["ok"])
        self.assertIn("five signed system parts", r["steps"]["parts"]["note"])

    def test_apps_without_a_key_are_refused_plainly(self):
        s = system.new_system(self.ws, "ak", "tdeck_plus")
        s.update(apps=["hello"])
        system.save(self.ws, "ak", s)
        r = system.build(self.ws, "ak", on_line=lambda l: None)
        self.assertFalse(r["ok"])
        self.assertIn("signing key", r["steps"]["apps"]["note"])


class Routes(test_studio.Studio):
    def setUp(self):
        self.tmp = helpers.tmpdir()
        self.parts = os.path.join(self.tmp, "parts")
        self.drivers = os.path.join(self.tmp, "drivers")
        make_parts(self.parts)
        make_drivers(self.drivers)
        self._env = dict(os.environ)
        os.environ["PURR_IMAGES"] = self.parts
        os.environ["PURR_DRIVERS"] = self.drivers

    def tearDown(self):
        os.environ.clear()
        os.environ.update(self._env)

    def wait_job(self, jid):
        for _ in range(600):
            st, j = self.call("GET", "/api/job?id=%s&since=0" % jid)
            if j["done"]:
                return j
            time.sleep(0.05)
        self.fail("the job did not finish")

    def test_new_save_overview_build(self):
        st, s = self.call("POST", "/api/system/new", {"name": "rt1", "board": "tdeck_plus"})
        self.assertEqual(st, 200, s)
        st, o = self.call("GET", "/api/system?name=rt1")
        self.assertEqual(st, 200)
        self.assertEqual(sorted(d["name"] for d in o["drivers"]), ["bme280", "kbd-raw"])
        self.assertEqual(self.call("POST", "/api/system/save", {"name": "rt1", "drivers": ["bme280"]})[0], 200)
        st, o = self.call("GET", "/api/system?name=rt1")
        self.assertEqual([d["name"] for d in o["drivers"] if d["on"]], ["bme280"])
        self.assertEqual(self.call("POST", "/api/system/save", {"name": "rt1", "board": "palm"})[0], 400)
        st, mk = self.call("POST", "/api/system/build", {"name": "rt1"})
        j = self.wait_job(mk["job"])
        self.assertTrue(j["result"]["ok"], j["lines"])
        marks = [l for l in j["lines"] if l.startswith("@@step|")]
        self.assertIn("@@step|image|done|16 MB", marks)
        st, act = self.call("GET", "/api/activity")
        self.assertEqual(act["items"][0]["title"], "rt1")
        self.assertEqual(act["items"][0]["kind"], "image")

    def test_unknown_system_and_address(self):
        self.assertEqual(self.call("GET", "/api/system?name=nope")[0], 400)
        st, a = self.call("GET", "/api/system/address")
        self.assertEqual(st, 200)
        self.assertEqual(a["port"], 47801)
        self.assertRegex(a["address"], r"^\d+\.\d+\.\d+\.\d+$")


if __name__ == "__main__":
    unittest.main()
