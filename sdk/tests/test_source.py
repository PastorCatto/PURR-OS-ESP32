"""Compile from source: pulling only the folders a build needs, noticing something newer, keeping the build folders, the order of the compile and packaging
commands, signing every part, and the system road in source mode (the compiler itself is a stand-in)."""
import io
import os
import shutil
import subprocess
import tarfile
import unittest
from unittest import mock

import helpers
import test_system
from purrsdk import source, system, workspace


def git(*a, cwd):
    subprocess.check_call(["git"] + list(a), cwd=cwd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                          env=dict(os.environ, GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@t", GIT_COMMITTER_NAME="t", GIT_COMMITTER_EMAIL="t@t"))


def make_repo(path):
    for rel, text in (("PurrOS/CMakeLists.txt", "project\n"), ("UI/android-assets/a.png", "png"), ("purrstrap/purrstrap.py", "x"), ("archive/huge.bin", "big"), ("bootpkg/src/a.c", "c"), ("bootloader/CMakeLists.txt", "b")):
        p = os.path.join(path, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        open(p, "w").write(text)
    git("init", "-q", "-b", "main", cwd=path)
    git("add", "-A", cwd=path)
    git("commit", "-q", "-m", "one", cwd=path)


@unittest.skipUnless(shutil.which("git"), "needs git")
class Pulling(unittest.TestCase):
    def setUp(self):
        self.tmp = helpers.tmpdir()
        self.ws = workspace.Workspace(os.path.join(self.tmp, "ws")).ensure()
        self.repo = os.path.join(self.tmp, "repo")
        os.makedirs(self.repo)
        make_repo(self.repo)
        self.url = "file://" + self.repo

    def test_pull_takes_only_the_folders_a_build_needs(self):
        r = source.pull(self.ws, self.url, "main", on_line=lambda l: None)
        self.assertTrue(r["changed"])
        t = source.tree(self.ws)
        for d in ("PurrOS", "UI", "purrstrap", "bootloader", "bootpkg"):
            self.assertTrue(os.path.isdir(os.path.join(t, d)), d)
        self.assertFalse(os.path.exists(os.path.join(t, "archive")))
        self.assertFalse(os.path.exists(os.path.join(t, ".git")))

    def test_unchanged_source_is_not_fetched_again_and_newer_is_noticed(self):
        source.pull(self.ws, self.url, "main", on_line=lambda l: None)
        c = source.check(self.ws, self.url, "main")
        self.assertTrue(c["pulled"])
        self.assertFalse(c["newer"])
        self.assertFalse(source.pull(self.ws, self.url, "main", on_line=lambda l: None)["changed"])
        open(os.path.join(self.repo, "PurrOS", "new.c"), "w").write("n")
        git("add", "-A", cwd=self.repo)
        git("commit", "-q", "-m", "two", cwd=self.repo)
        self.assertTrue(source.check(self.ws, self.url, "main")["newer"])
        self.assertTrue(source.pull(self.ws, self.url, "main", on_line=lambda l: None)["changed"])
        self.assertTrue(os.path.isfile(os.path.join(source.tree(self.ws), "PurrOS", "new.c")))

    def test_the_build_folder_survives_a_newer_pull(self):
        source.pull(self.ws, self.url, "main", on_line=lambda l: None)
        b = os.path.join(source.tree(self.ws), "PurrOS", "build", "x")
        os.makedirs(b)
        open(os.path.join(b, "kept"), "w").write("1")
        open(os.path.join(self.repo, "PurrOS", "new.c"), "w").write("n")
        git("add", "-A", cwd=self.repo)
        git("commit", "-q", "-m", "two", cwd=self.repo)
        source.pull(self.ws, self.url, "main", on_line=lambda l: None)
        self.assertTrue(os.path.isfile(os.path.join(b, "kept")))

    def test_something_that_is_not_the_source_is_refused_and_leaves_nothing(self):
        other = os.path.join(self.tmp, "other")
        os.makedirs(other)
        open(os.path.join(other, "a.txt"), "w").write("a")
        git("init", "-q", "-b", "main", cwd=other)
        git("add", "-A", cwd=other)
        git("commit", "-q", "-m", "x", cwd=other)
        with self.assertRaises(source.SourceError):
            source.pull(self.ws, "file://" + other, "main", on_line=lambda l: None)
        self.assertFalse(os.path.exists(os.path.join(source.folder(self.ws), "tree.new")))

    def test_an_unreachable_source_says_so(self):
        c = source.check(self.ws, "file://" + os.path.join(self.tmp, "nowhere"), "main")
        self.assertTrue(c["error"])
        self.assertFalse(c["newer"])


class Helpers(unittest.TestCase):
    def test_github_addresses(self):
        self.assertEqual(source.github("https://github.com/A/B"), ("A", "B"))
        self.assertEqual(source.github("https://github.com/A/B.git/"), ("A", "B"))
        self.assertEqual(source.github("git@github.com:A/B.git"), ("A", "B"))
        self.assertIsNone(source.github("https://example.com/A/B"))

    def test_tarball_unpacks_only_the_wanted_folders_and_not_outside(self):
        buf = io.BytesIO()
        with tarfile.open(fileobj=buf, mode="w:gz") as tf:
            for name, data in (("repo-main/PurrOS/CMakeLists.txt", b"p"), ("repo-main/archive/x", b"a"), ("repo-main/UI/a.png", b"i"), ("repo-main/PurrOS/../../escape.txt", b"bad"), ("repo-main/purrstrap/purrstrap.py", b"#!x")):
                ti = tarfile.TarInfo(name)
                ti.size = len(data)
                ti.mode = 0o755 if name.endswith(".py") else 0o644
                tf.addfile(ti, io.BytesIO(data))
        tmp = helpers.tmpdir()
        dest = os.path.join(tmp, "t")
        source._extract_tar(buf.getvalue(), dest)
        self.assertTrue(os.path.isfile(os.path.join(dest, "PurrOS", "CMakeLists.txt")))
        self.assertTrue(os.path.isfile(os.path.join(dest, "UI", "a.png")))
        self.assertFalse(os.path.exists(os.path.join(dest, "archive")))
        self.assertFalse(os.path.exists(os.path.join(tmp, "escape.txt")))
        self.assertTrue(os.access(os.path.join(dest, "purrstrap", "purrstrap.py"), os.X_OK))

    def test_the_part_version_fits_a_container_header(self):
        self.assertEqual(source.short_version("0123456789abcdef"), "src-0123456")
        self.assertLessEqual(len(source.short_version("f" * 40)), 12)

    def test_compiler_report(self):
        idf = mock.Mock(version="5.3.5", path="/x")
        self.assertEqual(source.compiler(find=lambda: idf), {"found": True, "version": "5.3.5", "path": "/x", "right": True})
        self.assertFalse(source.compiler(find=lambda: mock.Mock(version="5.1.0", path="/y"))["right"])
        self.assertFalse(source.compiler(find=lambda: None)["found"])


class Compiling(unittest.TestCase):
    def setUp(self):
        self.tmp = helpers.tmpdir()
        self.ws = workspace.Workspace(os.path.join(self.tmp, "ws")).ensure()
        os.makedirs(os.path.join(source.tree(self.ws), "PurrOS"))
        source._save_state(self.ws, {"url": "u", "ref": "main", "commit": "abcdef0123456789", "when": "now"})
        self.calls = []
        self.fail_on = None

    def fake(self, args, extra_env=None, password=None):
        self.calls.append((list(args), dict(extra_env or {}), password))
        if self.fail_on and args[:2] == self.fail_on:
            return {"rc": 2, "output": "boom\n"}
        if args[:2] == ["coreos", "build"]:
            prof = args[args.index("--profile") + 1]
            shell = args[args.index("--shell") + 1]
            b = source.build_dir(self.ws, args[args.index("--board") + 1], prof, shell)
            os.makedirs(os.path.join(b, "bootloader"), exist_ok=True)
            for f in ("purros.bin", os.path.join("bootloader", "bootloader.bin")):
                open(os.path.join(b, f), "wb").write(b"\xe9" * 16)
        for flag in ("--out",):
            if flag in args:
                o = args[args.index(flag) + 1]
                if args[0] == "bootpkg":
                    os.makedirs(o, exist_ok=True)
                    open(os.path.join(o, "bootpkg.bin"), "wb").write(b"p")
                else:
                    os.makedirs(os.path.dirname(o), exist_ok=True)
                    open(o, "wb").write(b"c")
        return {"rc": 0, "output": ""}

    def go(self, **kw):
        steps = []
        r = source.compile_and_package(self.ws, kw.pop("board", "tdeck_plus"), kw.pop("shell", "android"), kw.pop("key", "/k/boot.key"), 4, "pw", out=os.path.join(self.tmp, "out"),
                                       on_step=lambda i, s, n: steps.append((i, s)), on_line=lambda l: None, run=self.fake, **kw)
        return r, steps

    def test_three_profiles_compiled_in_the_pulled_tree_then_five_parts_signed(self):
        r, steps = self.go()
        self.assertTrue(r["ok"], r)
        builds = [c for c in self.calls if c[0][:2] == ["coreos", "build"]]
        self.assertEqual([c[0][c[0].index("--profile") + 1] for c in builds], ["full", "minimal", "recovery"])
        self.assertTrue(all(c[0][c[0].index("--shell") + 1] == "android" for c in builds))
        self.assertTrue(all(c[1].get("PURR_REPO_ROOT") == source.tree(self.ws) for c in builds))
        self.assertEqual(sorted(r["parts"]), sorted(["kernel-tdeck_plus.cat", "loader-tdeck_plus.cat", "bootloader-tdeck_plus.cat", "kittenos-tdeck_plus.kitt", "bootpkg-tdeck_plus.cat"]))
        pk = [c for c in self.calls if c[0][:2] == ["coreos", "package"]]
        self.assertEqual(len(pk), 1)
        self.assertEqual(pk[0][1].get("PURR_REPO_ROOT"), source.tree(self.ws))      # it reads the build the pulled tree made, not a checkout's
        signs = [c for c in self.calls if c[0][:2] == ["keys", "sign"]]
        self.assertEqual(len(signs), 5)
        self.assertTrue(all(c[2] == "pw" for c in signs))
        self.assertEqual(r["version"], "src-abcdef0")
        for want in ("compile-full", "compile-minimal", "compile-recovery", "package", "sign"):
            self.assertIn((want, "done"), steps)

    def test_a_failed_compile_stops_before_packaging_and_names_the_part(self):
        self.fail_on = ["coreos", "build"]
        r, steps = self.go()
        self.assertFalse(r["ok"])
        self.assertIn("kernel", r["problem"])
        self.assertIn(("compile-full", "fail"), steps)
        self.assertFalse([c for c in self.calls if c[0][0] == "keys"])

    def test_a_signing_failure_is_reported(self):
        self.fail_on = ["keys", "sign"]
        r, steps = self.go()
        self.assertFalse(r["ok"])
        self.assertIn(("sign", "fail"), steps)

    def test_no_key_no_pull_and_unknown_things_are_refused_first(self):
        self.assertIn("key", self.go(key=None)[0]["problem"])
        self.assertIn("shell", self.go(shell="ios")[0]["problem"])
        self.assertIn("board", self.go(board="cyd_24c")[0]["problem"])
        os.remove(source._state_path(self.ws))
        self.assertIn("Pull", self.go()[0]["problem"])
        self.assertFalse(self.calls)


class TheRoadInSourceMode(test_system.Setup):
    def test_source_mode_compiles_then_makes_the_image_from_what_it_built(self):
        s = system.new_system(self.ws, "road", "tdeck_plus")
        s.update(parts_from="source", key="boot.key", key_id=4)
        system.save(self.ws, "road", s)
        self.assertEqual(system.load(self.ws, "road")["parts_from"], "source")
        self.assertEqual(system.load(self.ws, "road")["shell"], "android")
        built = os.path.join(self.tmp, "built")

        def fake_compile(ws, board, shell, key, key_id, password, out, on_step, on_line):
            test_system.make_parts(out, board)
            for i in ("compile-full", "package", "sign"):
                on_step(i, "done", "")
            return {"ok": True, "out": out, "version": "src-1234567", "parts": []}
        seen = []
        with mock.patch.object(source, "pull", return_value={"commit": "1234567" + "0" * 33, "changed": True}), \
                mock.patch.object(source, "compiler", return_value={"found": True, "version": "5.3.5", "path": "/idf", "right": True}), \
                mock.patch.object(source, "compile_and_package", side_effect=fake_compile):
            r = system.build(self.ws, "road", password="pw", on_step=lambda i, st, n: seen.append(i), on_line=lambda l: None)
        self.assertTrue(r["ok"], r["steps"])
        self.assertEqual(seen[:3], ["source", "source", "compiler"])
        self.assertEqual(r["steps"]["image"]["state"], "done")
        self.assertTrue(os.path.isdir(os.path.join(r["image"] and os.path.dirname(r["image"]), "parts")))

    def test_source_mode_needs_a_key_a_compiler_and_a_reachable_source(self):
        s = system.new_system(self.ws, "road", "tdeck_plus")
        s.update(parts_from="source")
        system.save(self.ws, "road", s)
        r = system.build(self.ws, "road", on_line=lambda l: None)
        self.assertFalse(r["ok"])
        self.assertEqual(r["steps"]["source"]["state"], "fail")
        s.update(key="boot.key")
        system.save(self.ws, "road", s)
        with mock.patch.object(source, "pull", side_effect=source.SourceError("no network")):
            self.assertIn("no network", system.build(self.ws, "road", on_line=lambda l: None)["steps"]["source"]["note"])
        with mock.patch.object(source, "pull", return_value={"commit": "a" * 40, "changed": False}), mock.patch.object(source, "compiler", return_value={"found": False, "version": "", "path": "", "right": False}):
            r = system.build(self.ws, "road", on_line=lambda l: None)
        self.assertEqual(r["steps"]["compiler"]["state"], "fail")
        self.assertIn("5.3.5", r["steps"]["compiler"]["note"])


if __name__ == "__main__":
    unittest.main()
