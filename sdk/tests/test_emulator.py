"""The emulator layer (purrsdk/emulator.py), tested against a small fake of the emulator's headless program, so no QEMU is needed."""
import os
import socket
import stat
import sys
import time
import unittest
from unittest import mock

import helpers
from purrsdk import emulator

# The same protocol as headless_tap: HEADLESS_CMD names a file that gets one command per line; args are flash image, seconds, out.png.
FAKE = r'''#!%(python)s
import os, sys, time
flash, secs, out = sys.argv[1], int(sys.argv[2]), sys.argv[3]
cmd = os.environ["HEADLESS_CMD"]
stem = out[:-4]
done = 0
record = os.environ.get("FAKE_RECORD")
panic = os.environ.get("FAKE_PANIC")
if os.environ.get("FAKE_CHILD"):                      # like the real runner: QEMU is its child
    import subprocess
    kid = subprocess.Popen(["sleep", "300"])
    open(os.environ["FAKE_CHILD"], "w").write(str(kid.pid))
t0 = time.time()
PNG = bytes.fromhex("89504e470d0a1a0a0000000d49484452000000010000000108060000001f15c4890000000d49444154789c6360000002000001e221bc330000000049454e44ae426082")
while time.time() - t0 < secs:
    lines = open(cmd).read().splitlines()
    while done < len(lines):
        line = lines[done].strip(); done += 1
        verb, _, rest = line.partition(" ")
        if record:
            open(record, "a").write(line + "\n")
        if verb == "shot":
            open("%%s-%%s.png" %% (stem, rest), "wb").write(PNG)
        elif verb == "dump":
            text = "I (100) android-home: 4 apps, 4 pinned\n"
            if panic:
                text += "Guru Meditation Error: Core 1 panic'ed (IllegalInstruction)\n"
            open(stem + "-serial.txt", "w").write(text)
        elif verb == "quit":
            sys.exit(0)
    time.sleep(0.05)
'''


def make_emulator(root, record=None):
    os.makedirs(os.path.join(root, "boards"))
    with open(os.path.join(root, "boards", "t-deck-plus-vm.toml"), "w") as fh:
        fh.write("# a board\n")
    exe = os.path.join(root, "headless_tap")
    with open(exe, "w") as fh:
        fh.write(FAKE % {"python": sys.executable})
    os.chmod(exe, os.stat(exe).st_mode | stat.S_IXUSR)
    return exe


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = helpers.tmpdir()
        self.root = os.path.join(self.tmp, "emu")
        os.makedirs(self.root)
        self.exe = make_emulator(self.root)
        self.image = os.path.join(self.tmp, "vm.bin")
        with open(self.image, "wb") as fh:
            fh.write(b"\xe9" * 4096)
        self._env = dict(os.environ)
        os.environ["PURR_EMULATOR_DIR"] = self.root
        os.environ.pop("PURR_EMULATOR", None)
        os.environ.pop("PURR_VM_IMAGE", None)
        for p in (mock.patch.object(emulator, "_dev_checkout", lambda: None),                      # only the folder the test made counts, not this machine's
                  mock.patch.object(emulator.workspace, "kit_path", lambda *a: os.path.join(self.tmp, "no-kit", *a))):
            p.start()
            self.addCleanup(p.stop)

    def tearDown(self):
        os.environ.clear()
        os.environ.update(self._env)


@unittest.skipIf(os.name == "nt", "no emulator on Windows")
class Finding(Base):
    def test_finds_the_folder_named_in_the_environment(self):
        em = emulator.find()
        self.assertEqual(em["root"], self.root)
        self.assertEqual(em["headless"], self.exe)
        self.assertEqual(em["boards"], os.path.join(self.root, "boards"))
        self.assertIsNone(em["gui"])

    def test_the_vm_image_comes_from_the_environment(self):
        self.assertIsNone(emulator.vm_image())
        os.environ["PURR_VM_IMAGE"] = self.image
        self.assertEqual(emulator.vm_image(), self.image)

    def test_board_file_by_name_and_by_path(self):
        em = emulator.find()
        self.assertTrue(emulator.board_file(em).endswith("t-deck-plus-vm.toml"))
        p = os.path.join(self.tmp, "mine.toml")
        with open(p, "w") as fh:
            fh.write("x")
        self.assertEqual(emulator.board_file(em, p), p)
        with self.assertRaises(emulator.EmulatorError) as cm:
            emulator.board_file(em, "nope")
        self.assertIn("nope", str(cm.exception))

    def test_no_window_launcher_is_a_clear_error(self):
        with self.assertRaises(emulator.EmulatorError) as cm:
            emulator.open_window(self.image)
        self.assertIn("no emulator", str(cm.exception).lower())


@unittest.skipIf(os.name == "nt", "no emulator on Windows")
class Session(Base):
    def test_a_run_takes_pictures_and_reads_the_serial_log(self):
        work = os.path.join(self.tmp, "w")
        with emulator.Headless(self.image, workdir=work, seconds=60) as h:
            self.assertTrue(h.running())
            pic = h.shot("one")
            self.assertTrue(pic.endswith("s-one.png"))
            with open(pic, "rb") as fh:
                self.assertTrue(fh.read().startswith(b"\x89PNG"))
            self.assertIn("android-home", h.serial())
            self.assertTrue(h.wait_serial("4 pinned", timeout=5, poll=0.1))
            self.assertFalse(h.wait_serial("never printed", timeout=0.6, poll=0.2))
            self.assertFalse(h.panicked())
        self.assertFalse(h.running())

    def test_the_original_image_is_never_touched(self):
        with open(self.image, "rb") as fh:
            before = fh.read()
        with emulator.Headless(self.image, workdir=os.path.join(self.tmp, "w2"), seconds=30) as h:
            with open(os.path.join(h.workdir, "flash.bin"), "r+b") as fh:       # the emulator writes to its copy
                fh.write(b"scribble")
        with open(self.image, "rb") as fh:
            self.assertEqual(fh.read(), before)

    def test_a_panic_is_noticed(self):
        os.environ["FAKE_PANIC"] = "1"
        with emulator.Headless(self.image, workdir=os.path.join(self.tmp, "w3"), seconds=30) as h:
            self.assertTrue(h.panicked())

    def test_stop_ends_a_run_that_ignores_quit(self):
        h = emulator.Headless(self.image, workdir=os.path.join(self.tmp, "w4"), seconds=120).start()
        self.assertTrue(h.running())
        h.stop(grace=5)
        self.assertFalse(h.running())

    def test_a_child_left_behind_by_the_runner_is_stopped_too(self):
        pidfile = os.path.join(self.tmp, "kid.pid")
        os.environ["FAKE_CHILD"] = pidfile
        h = emulator.Headless(self.image, workdir=os.path.join(self.tmp, "w10"), seconds=60).start()
        for _ in range(50):
            if os.path.isfile(pidfile) and os.path.getsize(pidfile):
                break
            time.sleep(0.1)
        with open(pidfile) as fh:
            kid = int(fh.read())
        h.stop(grace=5)                                          # the runner quits on "quit"; its child would live on without the group stop
        time.sleep(0.5)
        with self.assertRaises(OSError):
            os.kill(kid, 0)

    def test_each_session_has_its_own_temp_folder_and_its_guest_flash(self):
        h1 = emulator.Headless(self.image, workdir=os.path.join(self.tmp, "w11"), seconds=30).start()
        h2 = emulator.Headless(self.image, workdir=os.path.join(self.tmp, "w12"), seconds=30).start()
        try:
            self.assertNotEqual(h1.tmpdir, h2.tmpdir)
            self.assertIsNone(h1.guest_flash())
            run_copy = os.path.join(h1.tmpdir, "esp32-emulator-headless-tap")
            os.makedirs(run_copy)
            with open(os.path.join(run_copy, "flash.bin"), "wb") as fh:
                fh.write(b"guest")
            self.assertEqual(h1.guest_flash(), os.path.join(run_copy, "flash.bin"))
            self.assertIsNone(h2.guest_flash())
        finally:
            h1.stop(grace=5)
            h2.stop(grace=5)

    def test_no_image_is_a_clear_error(self):
        with self.assertRaises(emulator.EmulatorError) as cm:
            emulator.Headless(None, workdir=os.path.join(self.tmp, "w5"))
        self.assertIn("flash image", str(cm.exception))

    def test_no_headless_runner_is_a_clear_error(self):
        os.remove(self.exe)
        os.environ["PURR_EMULATOR_DIR"] = self.root
        with self.assertRaises(emulator.EmulatorError):
            emulator.Headless(self.image, workdir=os.path.join(self.tmp, "w6"))


@unittest.skipIf(os.name == "nt", "no emulator on Windows")
class Serving(Base):
    def test_a_taken_port_is_refused_and_named(self):
        sock = socket.socket()
        sock.bind(("127.0.0.1", 0))
        sock.listen(1)
        self.addCleanup(sock.close)
        port = sock.getsockname()[1]
        with self.assertRaises(emulator.EmulatorError) as cm:
            emulator.serve_folder(os.path.join(self.tmp, "repo"), port=port)
        self.assertIn(str(port), str(cm.exception))
        self.assertIn("already in use", str(cm.exception))

    def test_a_free_port_is_not_taken(self):
        sock = socket.socket()
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
        sock.close()
        self.assertFalse(emulator._port_busy(port))


@unittest.skipIf(os.name == "nt", "no emulator on Windows")
class Steps(Base):
    def test_steps_run_in_order_and_pictures_come_back(self):
        rec = os.path.join(self.tmp, "rec.txt")
        os.environ["FAKE_RECORD"] = rec
        steps = [("wait_serial", "android-home:", 10), ("shot", "a"), ("tap", 160, 184, 0.3), ("drag", 1, 2, 3, 4, 0.3), ("pause", 0.1), ("shot", "b")]
        seen = []
        with emulator.Headless(self.image, workdir=os.path.join(self.tmp, "w7"), seconds=60) as h:
            pics = emulator.run_steps(h, steps, on_step=seen.append)
        self.assertEqual([os.path.basename(p) for p in pics], ["s-a.png", "s-b.png"])
        self.assertEqual(len(seen), len(steps))
        with open(rec) as fh:
            lines = fh.read().splitlines()
        self.assertEqual([l for l in lines if l.startswith(("tap", "drag"))], ["tap 160 184", "drag 1 2 3 4"])

    def test_a_wait_that_never_comes_is_an_error_that_names_the_text(self):
        with emulator.Headless(self.image, workdir=os.path.join(self.tmp, "w8"), seconds=30) as h:
            with self.assertRaises(emulator.EmulatorError) as cm:
                emulator.run_steps(h, [("wait_serial", "never", 0.5)])
        self.assertIn("never", str(cm.exception))

    def test_an_unknown_step_is_refused(self):
        with emulator.Headless(self.image, workdir=os.path.join(self.tmp, "w9"), seconds=30) as h:
            with self.assertRaises(emulator.EmulatorError):
                emulator.run_steps(h, [("fly", 1)])

    def test_the_built_in_steps_are_well_formed(self):
        for st in emulator.ANDROID_START + emulator.ANDROID_PORTAL:
            self.assertIn(st[0], ("tap", "drag", "shot", "wait_serial", "pause"))
            if st[0] == "tap":
                self.assertTrue(0 <= st[1] < emulator.SCREEN[0] and 0 <= st[2] < emulator.SCREEN[1])
            if st[0] == "drag":
                self.assertTrue(all(0 <= v < 320 for v in (st[1], st[3])) and all(0 <= v < 240 for v in (st[2], st[4])))


if __name__ == "__main__":
    unittest.main()
