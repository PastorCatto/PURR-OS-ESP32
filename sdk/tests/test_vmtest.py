"""Testing a built image in the emulator (emulator.test_image), against a fake headless runner that prints a chosen serial log and draws a chosen screen."""
import os
import stat
import sys
import unittest
from unittest import mock

import helpers
import test_studio
import test_system
from purrsdk import emulator

FAKE = r'''#!%(python)s
import os, sys, time, zlib, struct
flash, secs, out = sys.argv[1], int(sys.argv[2]), sys.argv[3]
cmd = os.environ["HEADLESS_CMD"]
stem = out[:-4]
done = 0
def png(path, bright):
    w = h = 16
    raw = b"".join(b"\x00" + (bytes([200, 200, 200, 255]) if bright else bytes([0, 0, 0, 255])) * w for _ in range(h))
    def chunk(t, d): return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))
t0 = time.time()
while time.time() - t0 < secs:
    lines = open(cmd).read().splitlines()
    while done < len(lines):
        verb, _, rest = lines[done].strip().partition(" "); done += 1
        if verb == "shot":
            png("%%s-%%s.png" %% (stem, rest), os.environ.get("FAKE_SCREEN", "bright") == "bright")
        elif verb == "dump":
            open(stem + "-serial.txt", "w").write(open(os.environ["FAKE_SERIAL"]).read())
        elif verb == "quit":
            sys.exit(0)
    time.sleep(0.05)
'''

def log(extra=""):
    """A serial dump as the emulator writes it: the UART0 section, then the same boot again on the USB Serial/JTAG port."""
    return ("--- UART0 ---\nESP-ROM:esp32s3-20210327\n\x1b[0;32mI (24) purr_boot: PURR OS bootloader\x1b[0m\nI (46) purr_boot: bootpkg 0.3.1 loaded (key id 1, ok): code\n"
            "I (2241) purros: PURR OS starting\nI (14242) wifi_init: rx ba win: 6\n" + extra + "--- USB Serial/JTAG ---\nESP-ROM:esp32s3-20210327\nI (24) purr_boot: PURR OS bootloader\n")


GOOD = ("--- UART0 ---\nESP-ROM:esp32s3-20210327\n\x1b[0;32mI (24) purr_boot: PURR OS bootloader\x1b[0m\nI (46) purr_boot: bootpkg 0.3.1 loaded (key id 1, ok): code\n"
        "I (2241) purros: PURR OS starting\nI (14242) wifi_init: rx ba win: 6\n--- USB Serial/JTAG ---\nESP-ROM:esp32s3-20210327\nI (24) purr_boot: PURR OS bootloader\n")


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = helpers.tmpdir()
        self.root = os.path.join(self.tmp, "emu")
        os.makedirs(os.path.join(self.root, "boards"))
        open(os.path.join(self.root, "boards", "t-deck-plus.toml"), "w").write("# a board\n")
        exe = os.path.join(self.root, "headless_tap")
        with open(exe, "w") as fh:
            fh.write(FAKE % {"python": sys.executable})
        os.chmod(exe, os.stat(exe).st_mode | stat.S_IXUSR)
        self.image = os.path.join(self.tmp, "img.bin")
        with open(self.image, "wb") as fh:
            fh.write(b"\xe9" * 4096)
        self.serial = os.path.join(self.tmp, "serial.txt")
        self._env = dict(os.environ)
        os.environ["PURR_EMULATOR_DIR"] = self.root
        os.environ["FAKE_SERIAL"] = self.serial
        os.environ.pop("PURR_EMULATOR", None)
        for p in (mock.patch.object(emulator, "_dev_checkout", lambda: None), mock.patch.object(emulator.workspace, "kit_path", lambda *a: os.path.join(self.tmp, "no-kit", *a))):
            p.start()
            self.addCleanup(p.stop)

    def tearDown(self):
        os.environ.clear()
        os.environ.update(self._env)

    def run_test(self, serial, screen="bright", seconds=6):
        open(self.serial, "w").write(serial)
        os.environ["FAKE_SCREEN"] = screen
        return emulator.test_image(self.image, "tdeck_plus", seconds=seconds, workdir=os.path.join(self.tmp, "w"), poll=0.3, on_line=lambda s: None)


@unittest.skipIf(os.name == "nt", "no emulator on Windows")
class Testing(Base):
    def status(self, r):
        return {c["id"]: c["status"] for c in r["checks"]}

    def test_a_good_boot_passes_and_names_the_stage(self):
        r = self.run_test(log())
        self.assertTrue(r["ok"], r["checks"])
        self.assertEqual(self.status(r), {"start": "ok", "bootpkg": "ok", "crash": "ok", "system": "ok", "screen": "ok", "stage": "ok"})
        self.assertIn("recovery loader", r["stage"])
        self.assertTrue(r["shots"] and os.path.isfile(r["shots"][0]))

    def test_the_usb_copy_of_the_log_is_not_counted_as_a_second_boot(self):
        r = self.run_test(log())
        self.assertEqual(self.status(r)["crash"], "ok")

    def test_a_crash_fails(self):
        r = self.run_test(log("Guru Meditation Error: Core 1 panic'ed (IllegalInstruction)\n"))
        self.assertFalse(r["ok"])
        self.assertEqual(self.status(r)["crash"], "bad")

    def test_a_boot_loop_fails(self):
        r = self.run_test(log("I (24) purr_boot: PURR OS bootloader\n"), seconds=4)
        self.assertEqual(self.status(r)["crash"], "bad")
        self.assertFalse(r["ok"])

    def test_a_boot_package_that_does_not_verify_fails(self):
        r = self.run_test(log().replace("(key id 1, ok)", "(key id 9, bad signature)"))
        self.assertEqual(self.status(r)["bootpkg"], "bad")
        self.assertFalse(r["ok"])

    def test_nothing_in_the_log_fails_start(self):
        r = self.run_test("--- UART0 ---\n", seconds=3)
        self.assertEqual(self.status(r)["start"], "bad")
        self.assertFalse(r["ok"])

    def test_a_black_screen_fails(self):
        r = self.run_test(log(), screen="black")
        self.assertEqual(self.status(r)["screen"], "bad")
        self.assertFalse(r["ok"])

    def test_the_home_screen_is_the_furthest_stage(self):
        r = self.run_test(log("I (30000) android-home: 4 apps\n"))
        self.assertEqual(r["stage"], "the home screen")

    def test_started_but_stuck_is_a_warning_not_a_failure(self):
        r = self.run_test(log().replace("wifi_init", "xx"), seconds=5)
        self.assertEqual(self.status(r)["stage"], "warn")
        self.assertTrue(r["ok"])

    def test_the_image_is_never_changed(self):
        before = open(self.image, "rb").read()
        self.run_test(log())
        self.assertEqual(open(self.image, "rb").read(), before)


class Helpers(unittest.TestCase):
    def test_clean_serial_drops_colours_and_the_usb_copy(self):
        self.assertEqual(emulator.clean_serial(log()).count("PURR OS bootloader"), 1)
        self.assertNotIn("\x1b", emulator.clean_serial(log()))

    def test_board_mapping_and_availability(self):
        self.assertEqual(emulator.emulator_board_for("tdeck"), "t-deck-plus")
        self.assertIsNone(emulator.emulator_board_for("waveshare154"))
        ok, why = emulator.available("waveshare154")
        self.assertFalse(ok)
        self.assertIn("not emulated", why)

    def test_no_emulator_says_so(self):
        with mock.patch.object(emulator, "find", lambda: None):
            ok, why = emulator.available("tdeck_plus")
        self.assertFalse(ok)
        self.assertIn("no emulator", why.lower())


@unittest.skipIf(os.name == "nt", "no emulator on Windows")
class Routes(test_studio.Studio, Base):
    """The Studio routes behind the deliver page's test button."""
    def setUp(self):
        Base.setUp(self)
        parts = os.path.join(self.tmp, "parts")
        test_system.make_parts(parts)
        os.environ["PURR_IMAGES"] = parts
        os.environ["PURR_DRIVERS"] = os.path.join(self.tmp, "drivers")
        test_system.make_drivers(os.environ["PURR_DRIVERS"])
        open(self.serial, "w").write(log())
        os.environ["FAKE_SCREEN"] = "bright"
        self.state.emu.update({"busy": False, "proc": None, "shots_dir": None})

    def wait_job(self, jid):
        import time
        for _ in range(900):
            st, j = self.call("GET", "/api/job?id=%s&since=0" % jid)
            if j["done"]:
                return j
            time.sleep(0.1)
        self.fail("the job did not finish")

    def test_testable_says_build_first_then_yes(self):
        self.call("POST", "/api/system/new", {"name": "vt1", "board": "tdeck_plus"})
        st, t = self.call("GET", "/api/system/testable?name=vt1")
        self.assertFalse(t["available"])
        self.assertIn("Build the system first", t["why"])
        self.assertEqual(self.call("POST", "/api/system/test", {"name": "vt1"})[0], 400)
        st, mk = self.call("POST", "/api/system/build", {"name": "vt1"})
        self.assertTrue(self.wait_job(mk["job"])["result"]["ok"])
        st, t = self.call("GET", "/api/system/testable?name=vt1")
        self.assertTrue(t["available"], t)

    def test_the_test_runs_and_its_pictures_can_be_fetched(self):
        self.call("POST", "/api/system/new", {"name": "vt2", "board": "tdeck_plus"})
        st, mk = self.call("POST", "/api/system/build", {"name": "vt2"})
        self.wait_job(mk["job"])
        st, tj = self.call("POST", "/api/system/test", {"name": "vt2", "seconds": 30})
        self.assertEqual(st, 200, tj)
        j = self.wait_job(tj["job"])
        self.assertTrue(j["result"]["ok"], j["result"])
        marks = [l for l in j["lines"] if l.startswith("@@test|")]
        self.assertTrue(any(m.startswith("@@test|bootpkg|ok|") for m in marks), marks)
        self.assertFalse(self.state.emu["busy"])
        st, shot = self.call("GET", "/api/emulator/shot?name=" + j["result"]["shots"][0])
        self.assertEqual(st, 200)
        self.assertTrue(shot["data"].startswith("data:image/png;base64,"))

    def test_the_e_paper_board_is_refused_plainly(self):
        self.call("POST", "/api/system/new", {"name": "vt3", "board": "waveshare154"})
        st, t = self.call("GET", "/api/system/testable?name=vt3")
        self.assertFalse(t["available"])
        self.assertIn("not emulated", t["why"])


if __name__ == "__main__":
    unittest.main()
