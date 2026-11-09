"""`purr system` and `purr source`: make and change a system, see what is wrong with the keys, build (with the road stood in) and pull from a local repository."""
import contextlib
import io
import os
import shutil
import unittest
from unittest import mock

import helpers
import test_source
from purrsdk import cli, source, system


def run(ws, *argv, stdin_tty=False):
    out = io.StringIO()
    with contextlib.redirect_stdout(out), mock.patch.dict(os.environ, {"PURRSTRAP_KEY_PASSWORD": "pw"}):
        rc = cli.main(["--workspace", ws] + list(argv))
    return rc, out.getvalue()


class SystemCommand(unittest.TestCase):
    def setUp(self):
        self.ws = os.path.join(helpers.tmpdir(), "ws")

    def test_new_set_show_list(self):
        self.assertEqual(run(self.ws, "system", "new", "froyo", "--board", "tdeck_plus")[0], 0)
        rc, out = run(self.ws, "system", "set", "froyo", "--parts-from", "source", "--source-ref", "PURR2.0", "--boot-key", "b.key", "--boot-key-id", "7",
                      "--key", "d.key", "--key-id", "6", "--keep-ours", "off", "--version", "2.0.1", "--apps", "mine,src:notes", "--drivers", "bme280")
        self.assertEqual(rc, 0, out)
        self.assertIn("taken out", out)
        from purrsdk import workspace
        s = system.load(workspace.Workspace(self.ws), "froyo")
        self.assertEqual((s["parts_from"], s["source_ref"], s["boot_key_id"], s["key_id"], s["keep_ours"], s["version"]), ("source", "PURR2.0", 7, 6, False, "2.0.1"))
        self.assertEqual((s["apps"], s["drivers"], s["drivers_set"]), (["mine", "src:notes"], ["bme280"], True))
        self.assertIn("froyo", run(self.ws, "system", "list")[1])
        self.assertIn("compiled from source", run(self.ws, "system", "show", "froyo")[1])

    def test_the_key_problems_show_up_in_show(self):
        run(self.ws, "system", "new", "x", "--board", "tdeck_plus")
        run(self.ws, "system", "set", "x", "--parts-from", "source", "--boot-key", "b.key", "--boot-key-id", "3", "--key", "d.key", "--key-id", "6")
        rc, out = run(self.ws, "system", "show", "x")
        self.assertIn("one of PURR's", out)

    def test_unknown_names_and_bad_choices_are_said_plainly(self):
        rc, out = run(self.ws, "system", "show", "ghost")
        self.assertEqual(rc, 1)
        self.assertIn("I do not know a system called ghost", out)
        run(self.ws, "system", "new", "x", "--board", "tdeck_plus")
        with self.assertRaises(SystemExit):
            run(self.ws, "system", "set", "x", "--shell", "ios")

    def test_build_prints_the_steps_and_the_image_and_a_failure_says_which_step(self):
        run(self.ws, "system", "new", "x", "--board", "tdeck_plus")
        good = {"ok": True, "image": "/b/x.img", "delivery": "/b", "steps": {}, "bytes": 1, "versions": {}, "fit": [], "apps": []}

        def fake(ws, name, pw, on_step=None, on_line=None, boot_password=None):
            on_step("apps", "skip", "none")
            on_step("image", "done", "16 MB")
            return good
        with mock.patch.object(system, "build", side_effect=fake):
            rc, out = run(self.ws, "system", "build", "x")
        self.assertEqual(rc, 0)
        self.assertIn("image: 16 MB", out)
        self.assertIn("/b/x.img", out)
        bad = {"ok": False, "steps": {"trust": {"state": "fail", "note": "no public key"}}, "image": "", "delivery": ""}
        with mock.patch.object(system, "build", return_value=bad):
            rc, out = run(self.ws, "system", "build", "x")
        self.assertEqual(rc, 1)
        self.assertIn("trust: no public key", out)

    def test_test_needs_a_built_image(self):
        run(self.ws, "system", "new", "x", "--board", "tdeck_plus")
        rc, out = run(self.ws, "system", "test", "x")
        self.assertEqual(rc, 1)
        self.assertIn("no built image", out)


@unittest.skipUnless(shutil.which("git"), "needs git")
class SourceCommand(unittest.TestCase):
    def test_check_pull_and_compiler(self):
        tmp = helpers.tmpdir()
        ws = os.path.join(tmp, "ws")
        repo = os.path.join(tmp, "repo")
        os.makedirs(repo)
        test_source.make_repo(repo)
        url = "file://" + repo
        rc, out = run(ws, "source", "check", "--url", url, "--ref", "main")
        self.assertEqual(rc, 0)
        self.assertIn("newer than what you have", out)
        self.assertEqual(run(ws, "source", "pull", "--url", url, "--ref", "main")[0], 0)
        self.assertIn("what you have", run(ws, "source", "check", "--url", url, "--ref", "main")[1])
        rc, out = run(ws, "source", "pull", "--url", url, "--ref", "nope")
        self.assertEqual(rc, 1)
        self.assertIn("no branch or tag called nope", out)
        with mock.patch.object(source, "compiler", return_value={"found": False, "version": "", "path": "", "right": False}):
            rc, out = run(ws, "source", "compiler")
        self.assertEqual(rc, 1)
        self.assertIn("not installed", out)


if __name__ == "__main__":
    unittest.main()
