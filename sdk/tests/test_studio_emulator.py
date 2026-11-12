"""Studio's Run page and the run button: the routes that wrap purrsdk/emulator.py, tested against the fake emulator of test_emulator.py (no QEMU)."""
import os
import sys
import time
import unittest
from unittest import mock

import helpers
from test_emulator import make_emulator
import test_studio

sys.path.insert(0, os.path.join(helpers.SDK, "studio"))
from purrsdk import emulator  # noqa: E402


class EmulatorRoutes(test_studio.Studio):
    def setUp(self):
        self.tmp = helpers.tmpdir()
        self._env = dict(os.environ)
        os.environ.pop("PURR_EMULATOR", None)
        os.environ.pop("PURR_VM_IMAGE", None)
        os.environ.pop("PURR_EMULATOR_DIR", None)
        for p in (mock.patch.object(emulator, "_dev_checkout", lambda: None),
                  mock.patch.object(emulator, "_HEADLESS_NAMES", (helpers.RUNNER_NAME,)),
                  mock.patch.object(emulator.workspace, "kit_path", lambda *a: os.path.join(self.tmp, "no-kit", *a))):
            p.start()
            self.addCleanup(p.stop)
        self.state.emu.update({"busy": False, "proc": None, "shots_dir": None})

    def tearDown(self):
        os.environ.clear()
        os.environ.update(self._env)

    def have_emulator(self, with_image=True):
        root = os.path.join(self.tmp, "emu")
        os.makedirs(root)
        make_emulator(root)
        os.environ["PURR_EMULATOR_DIR"] = root
        if with_image:
            image = os.path.join(self.tmp, "vm.bin")
            with open(image, "wb") as fh:
                fh.write(b"\xe9" * 4096)
            os.environ["PURR_VM_IMAGE"] = image

    def wait_job(self, jid):
        for _ in range(100):
            st, j = self.call("GET", "/api/job?id=%s&since=0" % jid)
            if j["done"]:
                return j
            time.sleep(0.05)
        self.fail("the job did not finish")

    def test_no_emulator_says_why(self):
        st, body = self.call("GET", "/api/emulator")
        self.assertEqual(st, 200)
        self.assertFalse(body["available"])
        self.assertTrue(body["why"])

    def test_with_emulator_and_image(self):
        self.have_emulator()
        st, body = self.call("GET", "/api/emulator")
        self.assertTrue(body["available"], body["why"])
        self.assertTrue(body["headless"])
        self.assertFalse(body["window"])
        self.assertIn("t-deck-plus-vm", body["boards"])

    def test_no_image_is_not_available(self):
        self.have_emulator(with_image=False)
        st, body = self.call("GET", "/api/emulator")
        self.assertFalse(body["available"])
        self.assertIn("image", body["why"])

    def test_run_is_refused_without_an_emulator(self):
        st, body = self.call("POST", "/api/emulator/run", {"project": "anything"})
        self.assertEqual(st, 400)
        self.assertIn("error", body)

    def test_run_wiring_and_pictures(self):
        self.have_emulator()
        cat = os.path.join(self.tmp, "hello.cat")
        with open(cat, "wb") as fh:
            fh.write(b"x")
        seen = {}

        def fake_run_app(path, image, board, shots, port, headless=True, on_line=print, steps=None):
            seen.update(path=path, headless=headless, board=board)
            png = os.path.join(shots, "start.png")
            with open(png, "wb") as fh:
                fh.write(b"\x89PNG\r\n\x1a\n")
            on_line("pretend run")
            return {"ok": True, "shots": [png]}
        with mock.patch.object(emulator, "run_app", fake_run_app):
            st, body = self.call("POST", "/api/emulator/run", {"file": cat})
            self.assertEqual(st, 200, body)
            j = self.wait_job(body["job"])
        self.assertTrue(j["result"]["ok"])
        self.assertEqual(j["result"]["shots"], ["start.png"])
        self.assertEqual(seen["path"], cat)
        self.assertTrue(seen["headless"])
        self.assertFalse(self.state.emu["busy"])
        st, shot = self.call("GET", "/api/emulator/shot?name=start.png")
        self.assertEqual(st, 200)
        self.assertTrue(shot["data"].startswith("data:image/png;base64,"))
        self.assertEqual(self.call("GET", "/api/emulator/shot?name=../../etc/passwd")[0], 400)

    def test_a_second_run_is_refused_while_busy(self):
        self.have_emulator()
        cat = os.path.join(self.tmp, "hello.cat")
        with open(cat, "wb") as fh:
            fh.write(b"x")
        self.state.emu["busy"] = True
        st, body = self.call("POST", "/api/emulator/run", {"file": cat})
        self.assertEqual(st, 400)
        self.assertIn("busy", body["error"])


if __name__ == "__main__":
    unittest.main()
