import importlib.util
import json
import os
import tempfile
import unittest
from unittest import mock

from helpers import TOOL_DIR, make_ctx

spec = importlib.util.spec_from_file_location(
    "coreos_under_test", os.path.join(TOOL_DIR, "scripts", "coreos.py"))
coreos = importlib.util.module_from_spec(spec)
spec.loader.exec_module(coreos)


def touch(root, rel, text=""):
    path = os.path.join(root, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(text)
    return path


def full_project(root):
    for name in ("CMakeLists.txt", "sdkconfig.defaults"):
        touch(root, f"PurrOS/{name}")
    for b in coreos.BOARDS:
        touch(root, f"PurrOS/sdkconfig.board.{b}")
        touch(root, f"PurrOS/partitions/{b}.csv")
    for p in coreos.PROFILES:
        touch(root, f"PurrOS/sdkconfig.profile.{p}")


class FindIdfTests(unittest.TestCase):
    def setUp(self):
        self.home = tempfile.mkdtemp()

    def manifest(self, path, script, selected="b"):
        touch(os.path.dirname(path), os.path.basename(path), json.dumps({
            "idfSelectedId": selected,
            "idfInstalled": [
                {"id": "a", "path": "/a", "activationScript": script, "name": "v5.1.0"},
                {"id": "b", "path": "/b", "activationScript": script, "name": "v5.3.5"}]}))

    def test_active_shell_wins(self):
        idf = coreos.find_idf(env={"IDF_PATH": self.home}, home=self.home,
                              windows=False, which=lambda n: "/x/idf.py")
        self.assertEqual(idf.kind, "active")

    def test_idf_path_without_idf_py_is_not_active(self):
        idf = coreos.find_idf(env={"IDF_PATH": self.home}, home=self.home,
                              windows=False, which=lambda n: None)
        self.assertIsNone(idf)

    def test_manifest_selects_chosen_install(self):
        script = touch(self.home, "act.sh")
        self.manifest(os.path.join(self.home, ".espressif", "eim_idf.json"), script)
        idf = coreos.find_idf(env={}, home=self.home, windows=False,
                              which=lambda n: None)
        self.assertEqual((idf.kind, idf.version, idf.path), ("manifest", "5.3.5", "/b"))

    def test_manifest_with_missing_activation_script_is_skipped(self):
        self.manifest(os.path.join(self.home, ".espressif", "eim_idf.json"),
                      os.path.join(self.home, "gone.sh"))
        self.assertIsNone(coreos.find_idf(env={}, home=self.home, windows=False,
                                          which=lambda n: None))

    def test_export_sh_fallback(self):
        touch(self.home, "esp/v5.3.5/esp-idf/export.sh")
        idf = coreos.find_idf(env={}, home=self.home, windows=False,
                              which=lambda n: None)
        self.assertEqual((idf.kind, idf.version), ("export", "5.3.5"))

    def test_nothing_found(self):
        self.assertIsNone(coreos.find_idf(env={}, home=self.home, windows=False,
                                          which=lambda n: None))


class CommandTests(unittest.TestCase):
    def test_idf_args(self):
        args = coreos.idf_args("cyd_24c", "recovery")
        self.assertEqual(args[:3], ["idf.py", "-B", "build/cyd_24c-recovery"])
        self.assertIn("IDF_TARGET=esp32", args)                    # the board decides the chip
        self.assertIn("SDKCONFIG=build/cyd_24c-recovery/sdkconfig", args)
        self.assertIn("SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.board.cyd_24c;"
                      "sdkconfig.profile.recovery", args)
        self.assertIn("IDF_TARGET=esp32s3", coreos.idf_args("tdeck_plus", "full"))

    def test_wrap_active_is_unchanged(self):
        idf = coreos.Idf("active")
        self.assertEqual(coreos.wrap(idf, ["idf.py", "build"]), ["idf.py", "build"])

    def test_wrap_powershell_quotes_arguments(self):
        idf = coreos.Idf("manifest", activation=r"C:\it's\act.ps1")
        cmd = coreos.wrap(idf, ["idf.py", "-D", "A=b;c"], windows=True)
        self.assertEqual(cmd[0], "powershell")
        script = cmd[-1]
        self.assertIn(r". 'C:\it''s\act.ps1' *> $null; idf.py '-D' 'A=b;c'", script)
        self.assertTrue(script.endswith("exit $LASTEXITCODE"))

    def test_wrap_bash(self):
        idf = coreos.Idf("export", activation="/home/me/my esp/export.sh")
        cmd = coreos.wrap(idf, ["idf.py", "-D", "A=b;c"], windows=False)
        self.assertEqual(cmd[:2], ["bash", "-c"])
        self.assertIn(". '/home/me/my esp/export.sh'", cmd[2])
        self.assertIn("exec idf.py -D 'A=b;c'", cmd[2])


class ChildEnvTests(unittest.TestCase):
    def test_msys_variables_are_removed(self):
        env = coreos.child_env({"MSYSTEM": "MINGW64", "mingw_prefix": "x",
                                "PATH": "p", "IDF_PATH": "i"})
        self.assertEqual(env, {"PATH": "p", "IDF_PATH": "i"})


class ActionTests(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp()
        self.ctx, self.out = make_ctx(self.root)
        self.idf = coreos.Idf("active", "/idf", "", "5.3.5")

    def test_project_problems_lists_missing_files(self):
        touch(self.root, "PurrOS/CMakeLists.txt")
        problems = coreos.project_problems(os.path.join(self.root, "PurrOS"),
                                           "tdeck_plus", "recovery")
        self.assertIn("PurrOS/partitions/tdeck_plus.csv is missing", problems)
        self.assertIn("PurrOS/sdkconfig.profile.recovery is missing", problems)
        self.assertNotIn("PurrOS/CMakeLists.txt is missing", problems)

    def test_build_refuses_when_project_incomplete(self):
        with mock.patch.object(self.ctx, "run") as run:
            code = coreos.build(self.ctx, "tdeck_plus", "recovery", False)
        self.assertEqual(code, 1)
        run.assert_not_called()
        self.assertIn("is missing", self.out.getvalue())

    def test_build_runs_expected_command(self):
        full_project(self.root)
        with mock.patch.object(coreos, "find_idf", return_value=self.idf), \
                mock.patch.object(self.ctx, "run", return_value=0) as run:
            code = coreos.build(self.ctx, "tdeck_plus", "full", False)
        self.assertEqual(code, 0)
        argv = run.call_args.args[0]
        self.assertEqual(argv[-1], "build")
        self.assertIn("IDF_TARGET=esp32s3", argv)
        self.assertEqual(run.call_args.kwargs["cwd"], os.path.join(self.root, "PurrOS"))
        self.assertNotIn("MSYSTEM", run.call_args.kwargs["env"])

    def test_build_failure_is_reported(self):
        full_project(self.root)
        with mock.patch.object(coreos, "find_idf", return_value=self.idf), \
                mock.patch.object(self.ctx, "run", return_value=3):
            self.assertEqual(coreos.build(self.ctx, "tdeck_plus", "recovery", False), 3)
        self.assertIn("build failed", self.out.getvalue())

    def test_build_without_idf_fails_cleanly(self):
        full_project(self.root)
        with mock.patch.object(coreos, "find_idf", return_value=None):
            self.assertEqual(coreos.build(self.ctx, "tdeck_plus", "recovery", False), 1)
        self.assertIn("ESP-IDF not found", self.out.getvalue())

    def test_clean_flag_removes_old_build_dir(self):
        full_project(self.root)
        stale = touch(self.root, "PurrOS/build/tdeck_plus-recovery/old.txt")
        with mock.patch.object(coreos, "find_idf", return_value=self.idf), \
                mock.patch.object(self.ctx, "run", return_value=0):
            coreos.build(self.ctx, "tdeck_plus", "recovery", True)
        self.assertFalse(os.path.exists(stale))

    def test_clean_action(self):
        touch(self.root, "PurrOS/build/tdeck_plus-recovery/x")
        self.assertEqual(coreos.clean(self.ctx, "tdeck_plus", "recovery"), 0)
        self.assertFalse(os.path.isdir(os.path.join(self.root, "PurrOS/build/tdeck_plus-recovery")))
        self.assertEqual(coreos.clean(self.ctx, "tdeck_plus", "recovery"), 0)

    def test_size_needs_a_build(self):
        self.assertEqual(coreos.size(self.ctx, "tdeck_plus", "recovery"), 1)

    def test_check_reports_ready_and_missing(self):
        full_project(self.root)
        with mock.patch.object(coreos, "find_idf", return_value=self.idf):
            self.assertEqual(coreos.check(self.ctx), 0)
        os.remove(os.path.join(self.root, "PurrOS/sdkconfig.profile.full"))
        with mock.patch.object(coreos, "find_idf", return_value=self.idf):
            self.assertEqual(coreos.check(self.ctx), 1)

    def test_check_without_project_dir(self):
        with mock.patch.object(coreos, "find_idf", return_value=self.idf):
            self.assertEqual(coreos.check(self.ctx), 1)
        self.assertIn("PurrOS/ does not exist", self.out.getvalue())


if __name__ == "__main__":
    unittest.main()
