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
from purrsdk import formats, source, system, workspace


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
        self.assertNotIn("repository exists", c["error"])             # the line that says why, not git's closing advice
        self.assertIn("nowhere", c["error"])
        self.assertFalse(c["newer"])


class Helpers(unittest.TestCase):
    def test_the_reason_is_the_fatal_line_not_the_last_one(self):
        out = "fatal: '/x' does not appear to be a git repository\nfatal: Could not read from remote repository.\n\nPlease make sure you have the correct access rights\nand the repository exists.\n"
        self.assertEqual(source._why(out), "'/x' does not appear to be a git repository")
        self.assertEqual(source._why("something odd\nhappened\n"), "happened")
        self.assertEqual(source._why(""), "no reason given")

    def test_a_long_workspace_path_is_warned_about_on_windows_only(self):
        short = mock.Mock(path="C:\\p")
        long_ = mock.Mock(path="C:\\Users\\somebody\\Documents\\PURR-Projects")
        self.assertEqual(source.path_warning(short, windows=True), "")
        self.assertIn("260", source.path_warning(long_, windows=True))
        self.assertEqual(source.path_warning(long_, windows=False), "")

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
        self.assertEqual(source.short_version("0123456789abcdef"), "1.1.0+012345")
        self.assertEqual(source.short_version("0123456789abcdef", "2.0"), "2.0+01234567")
        for base in ("1", "1.2", "10.20.30", "12345.1.1"):
            self.assertLessEqual(len(source.short_version("f" * 40, base)), 12)

    def test_the_version_must_be_one_the_device_reads(self):
        self.assertEqual(source.version_problem("1.1.0"), "")
        self.assertEqual(source.version_problem("2"), "")
        for bad in ("src-1234", "1.1.0.0", "v1.1", "", "1.x", "1.1.0+abc", "12345.12345.12345"):
            self.assertTrue(source.version_problem(bad), bad)

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
        self.assertEqual(r["version"], "1.1.0+abcdef")
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


OURS = """/* Generated by purrstrap keys export-public. Do not edit by hand. */
#include "purr_keybag.h"

const purr_key_t purr_default_keys[] = {
%s};

const size_t purr_default_keys_count = sizeof(purr_default_keys) / sizeof(purr_default_keys[0]);
"""


def keybag_c(entries):
    body = "".join("    {.key_id = %d, .role = PURR_ROLE_%s,\n     .pub = {%s}},\n" % (i, r.upper(), ", ".join("0x%02x" % b for b in raw)) for i, r, raw in entries)
    return OURS % body


class Trust(unittest.TestCase):
    def setUp(self):
        self.tmp = helpers.tmpdir()
        self.ws = workspace.Workspace(os.path.join(self.tmp, "ws")).ensure()
        self.ours = [(1, "boot", b"\x01" * 64), (3, "boot", b"\x03" * 64), (4, "developer", b"\x04" * 64), (5, "system", b"\x05" * 64)]
        c = os.path.join(source.tree(self.ws), source.KEYS_C)
        os.makedirs(os.path.dirname(c))
        open(c, "w").write(keybag_c(self.ours))
        for n, b in (("boot", b"\xb0"), ("dev", b"\xd0")):
            open(os.path.join(self.tmp, n + ".pub"), "wb").write(b * 64)

    def fake_export(self, args, extra_env=None, password=None):
        """what `purrstrap keys export-public` does: one .pub.bin per key and the list regenerated from every one in the folder"""
        a = dict(zip(args[2::2], args[3::2]))
        out = a["--out"]
        open(os.path.join(out, "%s_%s.pub.bin" % (a["--role"], a["--key-id"])), "wb").write(open(a["--pub"], "rb").read())
        entries = []
        for f in sorted(os.listdir(out)):
            if f.endswith(".pub.bin"):
                role, _, i = f[:-8].rpartition("_")
                entries.append((int(i), role, open(os.path.join(out, f), "rb").read()))
        entries.sort()
        open(os.path.join(out, "purr_default_keys.c"), "w").write(keybag_c(entries))
        return {"rc": 0, "output": ""}

    def bag(self):
        return source.read_keybag(open(os.path.join(source.tree(self.ws), source.KEYS_C)).read())

    def own(self):
        return [(os.path.join(self.tmp, "boot.pub"), "boot", 7), (os.path.join(self.tmp, "dev.pub"), "developer", 6)]

    def test_keeping_ours_adds_yours_beside_them(self):
        ids = source.apply_trust(self.ws, True, self.own(), run=self.fake_export)
        self.assertEqual(ids, [1, 3, 4, 5, 6, 7])
        bag = {i: (r, k) for i, r, k in self.bag()}
        self.assertEqual(bag[7], ("boot", b"\xb0" * 64))
        self.assertEqual(bag[1], ("boot", b"\x01" * 64))

    def test_taking_ours_out_leaves_only_yours_and_the_choice_can_change_back(self):
        self.assertEqual(source.apply_trust(self.ws, False, self.own(), run=self.fake_export), [6, 7])
        self.assertEqual(sorted(r for _i, r, _k in self.bag()), ["boot", "developer"])
        self.assertEqual(source.apply_trust(self.ws, True, self.own(), run=self.fake_export), [1, 3, 4, 5, 6, 7])      # PURR's list was kept aside

    def test_a_missing_public_key_says_where_to_get_it(self):
        with self.assertRaises(source.SourceError) as e:
            source.apply_trust(self.ws, True, [(os.path.join(self.tmp, "none.pub"), "boot", 7)], run=self.fake_export)
        self.assertIn("public key", str(e.exception))

    def test_the_choice_of_keys_is_checked(self):
        ok = source.trust_problems(True, "b.key", 7, "d.key", 6)
        self.assertEqual(ok, [])
        self.assertTrue(source.trust_problems(True, "", 7, "d.key", 6))
        self.assertTrue(source.trust_problems(True, "b.key", 7, "", 6))
        self.assertTrue(source.trust_problems(True, "b.key", 3, "d.key", 6))                    # an id that is PURR's, while PURR's are kept
        self.assertEqual(source.trust_problems(False, "b.key", 3, "d.key", 4), [])             # fine once PURR's keys are out
        self.assertTrue(source.trust_problems(False, "b.key", 5, "d.key", 5))                  # the same id twice
        self.assertTrue(source.trust_problems(False, "b.key", 40, "d.key", 6))                 # beyond the 32-bit mask

    def test_the_public_key_sits_next_to_the_private_one(self):
        self.assertEqual(source.pub_path("/k/boot.key"), "/k/boot.pub")


class Modules(unittest.TestCase):
    def setUp(self):
        self.tmp = helpers.tmpdir()
        self.ws = workspace.Workspace(os.path.join(self.tmp, "ws")).ensure()
        t = source.tree(self.ws)
        source._save_state(self.ws, {"url": "u", "ref": "main", "commit": "abcdef0123456789", "when": "now"})
        for rel in [m[1] for m in source.SYSTEM_MODULES] + ["Modules/drivers/battery_module.c", "Modules/drivers/kbd_raw_module.c", "Modules/drivers/kbd_i2c_module.c", "Modules/drivers/bme280_module.c",
                                                           "Apps/notes/notes.c", "Apps/nomanifest/nomanifest.c"]:
            p = os.path.join(t, *rel.split("/"))
            os.makedirs(os.path.dirname(p), exist_ok=True)
            open(p, "w").write("c")
        open(os.path.join(t, "Apps", "notes", "manifest.toml"), "w").write('name = "notes"\nversion = "1.1.0"\n')
        self.calls = []
        self.fail = None

    def fake(self, args, extra_env=None, password=None):
        self.calls.append((list(args), dict(extra_env or {}), password))
        if self.fail and args[:2] == self.fail[0] and (len(self.fail) < 2 or self.fail[1] in args):
            return {"rc": 2, "output": "boom\n"}
        if args[:2] == ["modules", "build"]:
            o = args[args.index("--out") + 1]
            os.makedirs(os.path.dirname(o), exist_ok=True)
            open(o, "wb").write(b"cat" + args[args.index("--name") + 1].encode())
        return {"rc": 0, "output": ""}

    def go(self, drivers=(), apps=()):
        steps = []
        r = source.compile_modules(self.ws, "tdeck_plus", "/k/dev.key", 6, "pw", [{"name": n, "scope": sc} for n, sc in drivers], list(apps), os.path.join(self.tmp, "out"),
                                   on_step=lambda i, s, n: steps.append((i, s)), on_line=lambda l: None, run=self.fake)
        return r, steps

    def builds(self):
        return [c for c in self.calls if c[0][:2] == ["modules", "build"]]

    def test_drivers_are_found_by_their_stem_and_the_longest_stem_wins(self):
        self.assertTrue(source.driver_source(self.ws, "kbd-raw")[0].endswith("kbd_raw_module.c"))
        self.assertEqual(source.driver_source(self.ws, "kbd-raw")[1], "kbd_raw_entry")
        self.assertEqual(source.driver_source(self.ws, "battery-adc")[1], "battery_entry")           # the catalog name has a suffix the file does not
        self.assertIsNone(source.driver_source(self.ws, "nothing-here"))

    def test_apps_need_a_source_and_a_manifest(self):
        self.assertEqual(source.tree_apps(self.ws), [("notes", "1.1.0")])

    def test_all_eight_system_modules_the_drivers_and_the_apps_are_compiled_in_the_pulled_tree_and_signed(self):
        r, steps = self.go([("kbd-raw", "tdeck_plus"), ("bme280", "any"), ("ghost", "any")], ["notes"])
        self.assertTrue(r["ok"], r)
        b = self.builds()
        self.assertEqual(len(b), 8 + 2 + 1)
        self.assertTrue(all(c[1].get("PURR_REPO_ROOT") == source.tree(self.ws) for c in b))
        kinds = {c[0][c[0].index("--name") + 1]: c[0][c[0].index("--kind") + 1] for c in b}
        self.assertEqual(kinds["fs"], "driver")
        self.assertEqual(kinds["notes"], "app")
        self.assertEqual(r["missing"], ["ghost"])
        self.assertEqual(sorted(os.path.basename(p) for p in r["modules"]), sorted("%s-tdeck_plus.cat" % m[0] for m in source.SYSTEM_MODULES))
        self.assertEqual({d["file"] for d in r["drivers"]}, {"kbd-raw-tdeck_plus.cat", "bme280-any.cat"})
        self.assertEqual(r["drivers"][0]["version"], "1.1.0+abcdef")
        signs = [c for c in self.calls if c[0][:2] == ["keys", "sign"]]
        self.assertEqual(len(signs), 11)
        self.assertTrue(all(c[2] == "pw" and c[0][c[0].index("--key-id") + 1] == "6" for c in signs))
        notes = [c for c in b if c[0][c[0].index("--name") + 1] == "notes"][0]
        self.assertIn("notes_entry", notes[0])
        self.assertIn("1.1.0", notes[0])
        self.assertIn(("srcapps", "done"), steps)

    def test_the_delivery_check_catches_a_missing_a_short_and_an_altered_file(self):
        d = os.path.join(helpers.tmpdir(), "out")
        rec = os.path.join(d, "recovery")
        os.makedirs(rec)
        import hashlib
        files = {"a.cat": b"aaaa", "b.cat": b"bbbbbb", "c.cat": b"c"}
        stanzas = []
        for n, data in files.items():
            open(os.path.join(rec, n), "wb").write(data)
            stanzas.append("component=%s\nfile=%s\nsize=%d\nsha256=%s\n" % (n[0], n, len(data), hashlib.sha256(data).hexdigest()))
        open(os.path.join(rec, "recovery.manifest"), "w").write("release=x\n\n" + "\n".join(stanzas))
        ok = system.check_delivery(d)
        self.assertEqual((ok["ok"], ok["checked"]), (True, 3))
        os.remove(os.path.join(rec, "a.cat"))
        open(os.path.join(rec, "b.cat"), "wb").write(b"bb")
        open(os.path.join(rec, "c.cat"), "wb").write(b"X")
        bad = system.check_delivery(d)
        self.assertFalse(bad["ok"])
        self.assertEqual(len(bad["problems"]), 3)
        self.assertIn("not there", bad["problems"][0])
        self.assertFalse(system.check_delivery(os.path.join(d, "recovery", "nothing"))["ok"])
        # the app repository's index is checked as well
        app = os.path.join(d, "purr", "v1", "apps", "x", "1.0", "package.cat")
        os.makedirs(os.path.dirname(app))
        open(app, "wb").write(b"appdata")
        import json
        idx = os.path.join(d, "purr", "v1", "index.json")
        json.dump({"apps": [{"name": "x", "url": "apps/x/1.0/package.cat", "size": 7, "sha256": hashlib.sha256(b"appdata").hexdigest()}]}, open(idx, "w"))
        for n, data in files.items():
            open(os.path.join(rec, n), "wb").write(data)
        self.assertEqual(system.check_delivery(d)["checked"], 4)
        open(app, "wb").write(b"altered")
        self.assertIn("app index", system.check_delivery(d)["problems"][0])

    def test_the_manifest_entry_matches_the_file(self):
        r, _ = self.go([("bme280", "any")])
        d = r["drivers"][0]
        import hashlib
        data = open(d["path"], "rb").read()
        self.assertEqual((d["size"], d["sha256"]), (len(data), hashlib.sha256(data).hexdigest()))

    def test_a_module_that_does_not_compile_stops_the_road_and_names_it(self):
        self.fail = (["modules", "build"], "wifi")
        r, steps = self.go()
        self.assertFalse(r["ok"])
        self.assertIn("wifi", r["problem"])
        self.assertIn(("modules", "fail"), steps)

    def test_a_missing_module_source_is_named(self):
        os.remove(os.path.join(source.tree(self.ws), "Modules", "network", "wifi_module.c"))
        r, _ = self.go()
        self.assertFalse(r["ok"])
        self.assertIn("wifi", r["problem"])

    def test_an_app_that_is_not_in_the_source_is_refused(self):
        r, _ = self.go(apps=["nope"])
        self.assertFalse(r["ok"])
        self.assertIn("nope", r["problem"])


def container(name, flags=5, size=40, mark=0xe9):
    img = formats.image()
    return img.make_image(name, "1.0.0", img.IMG_MODULE, flags, bytes([mark]) * size, img.CHIP_ESP32S3)


def c_array(name, data):
    return "static const uint8_t %s[] = {%s\n};\n" % (name, source._format_array(data))


class Baked(unittest.TestCase):
    """the signed binaries inside commands.c are compiled again from source and rewritten with the user's signature"""

    def setUp(self):
        self.tmp = helpers.tmpdir()
        self.ws = workspace.Workspace(os.path.join(self.tmp, "ws")).ensure()
        t = source.tree(self.ws)
        source._save_state(self.ws, {"url": "u", "ref": "main", "commit": "abcdef0123456789", "when": "now"})
        self.old = {}
        text = "/* commands */\nint x;\n"
        for array, rel, _e in source.BAKED:
            name = array.replace("s_", "").replace("_driver", "").replace("_module", "").replace("_", "-")
            self.old[array] = container(name, flags=5 if "driver" in array else 5, mark=0x11)
            text += c_array(array, self.old[array]) + "int after_%s;\n" % array
            p = os.path.join(t, *rel.split("/"))
            os.makedirs(os.path.dirname(p), exist_ok=True)
            open(p, "w").write("c")
        os.makedirs(os.path.join(t, "PurrOS", "main"), exist_ok=True)
        open(os.path.join(t, source.COMMANDS_C), "w").write(text)
        for a in ("store", "storesvc"):
            os.makedirs(os.path.join(t, "Apps", a))
            open(os.path.join(t, "Apps", a, a + ".c"), "w").write("c")
            open(os.path.join(t, "Apps", a, "manifest.toml"), "w").write('version = "1.0.1"\n')
        os.makedirs(os.path.join(t, "tools", "release"))
        open(os.path.join(t, "tools", "release", "gen_builtin_apps.py"), "w").write("x")
        self.calls = []
        self.gen = []
        self.fail = None

    def fake(self, args, extra_env=None, password=None):
        self.calls.append((list(args), dict(extra_env or {}), password))
        if self.fail and args[:2] == self.fail[0] and self.fail[1] in " ".join(args):
            return {"rc": 2, "output": "boom\n"}
        if args[:2] == ["modules", "build"]:
            o = args[args.index("--out") + 1]
            open(o, "wb").write(container(args[args.index("--name") + 1], mark=0x77))
        return {"rc": 0, "output": ""}

    def go(self):
        steps = []
        r = source.rebake(self.ws, "tdeck_plus", "/k/dev.key", 6, "pw", on_step=lambda i, st, n: steps.append((i, st)), on_line=lambda l: None, run=self.fake,
                          gen=lambda a: (self.gen.append(a), {"rc": 0, "output": ""})[1])
        return r, steps

    def text(self):
        return open(os.path.join(source.tree(self.ws), source.COMMANDS_C)).read()

    def test_every_baked_binary_is_compiled_signed_and_written_back(self):
        r, steps = self.go()
        self.assertTrue(r["ok"], r)
        builds = [c for c in self.calls if c[0][:2] == ["modules", "build"]]
        self.assertEqual(len(builds), len(source.BAKED) + 2)
        self.assertTrue(all(c[1].get("PURR_REPO_ROOT") == source.tree(self.ws) for c in builds))
        signs = [c for c in self.calls if c[0][:2] == ["keys", "sign"]]
        self.assertEqual(len(signs), len(source.BAKED))                        # the Store's apps are unsigned: the kernel's hash vouches for them
        self.assertTrue(all(c[2] == "pw" and c[0][c[0].index("--key-id") + 1] == "6" for c in signs))
        t = self.text()
        for array, _r, _e in source.BAKED:
            self.assertEqual(source._array_bytes(t, array)[:4], self.old[array][:4])        # still a PURR container
            self.assertNotEqual(source._array_bytes(t, array), self.old[array])             # and the new one, not PURR's
        self.assertIn("int after_s_fs_module;", t)                             # the rest of the file is untouched
        self.assertIn(("baked", "done"), steps)
        self.assertIn(("builtin", "done"), steps)

    def test_the_name_and_kind_come_from_the_binary_that_was_there(self):
        self.go()
        b = {c[0][c[0].index("--name") + 1]: c[0][c[0].index("--kind") + 1] for c in self.calls if c[0][:2] == ["modules", "build"]}
        self.assertEqual(b["about"], "driver")
        self.assertEqual(b["st7789"], "driver")
        self.assertEqual(b["store"], "app")

    def test_the_store_is_written_by_the_generator_from_the_compiled_apps(self):
        self.go()
        self.assertEqual(len(self.gen), 1)
        self.assertTrue(self.gen[0][0].endswith("gen_builtin_apps.py"))
        self.assertEqual([os.path.basename(x) for x in self.gen[0][1:]], ["store.cat", "storesvc.cat"])

    def test_rebuilding_again_starts_from_the_originals(self):
        self.go()
        first = self.text()
        self.go()
        self.assertEqual(first, self.text())
        self.assertTrue(os.path.isfile(os.path.join(source.tree(self.ws), source.COMMANDS_C + ".orig")))

    def test_a_module_that_does_not_compile_stops_and_leaves_commands_c_alone(self):
        before = self.text()
        self.fail = (["modules", "build"], "gt911")
        r, steps = self.go()
        self.assertFalse(r["ok"])
        self.assertIn("gt911", r["problem"])
        self.assertIn(("baked", "fail"), steps)
        self.assertEqual(before, self.text())

    def test_a_missing_source_or_array_is_named(self):
        os.remove(os.path.join(source.tree(self.ws), "Modules", "drivers", "st7789_module.c"))
        r, _ = self.go()
        self.assertFalse(r["ok"])
        self.assertIn("s_st7789_driver_module", r["problem"])

    def test_the_tarball_keeps_the_tools_release_folder_but_not_the_rest_of_tools(self):
        buf = io.BytesIO()
        with tarfile.open(fileobj=buf, mode="w:gz") as tf:
            for name in ("r-main/tools/release/gen_builtin_apps.py", "r-main/tools/vm/x.sh", "r-main/Modules/a.c"):
                ti = tarfile.TarInfo(name)
                ti.size = 1
                tf.addfile(ti, io.BytesIO(b"x"))
        dest = os.path.join(helpers.tmpdir(), "t")
        source._extract_tar(buf.getvalue(), dest)
        self.assertTrue(os.path.isfile(os.path.join(dest, "tools", "release", "gen_builtin_apps.py")))
        self.assertTrue(os.path.isfile(os.path.join(dest, "Modules", "a.c")))
        self.assertFalse(os.path.exists(os.path.join(dest, "tools", "vm")))


class TheRoadInSourceMode(test_system.Setup):
    def test_source_mode_compiles_then_makes_the_image_from_what_it_built(self):
        s = system.new_system(self.ws, "road", "tdeck_plus")
        s.update(parts_from="source", key="developer.key", key_id=6, boot_key="boot.key", boot_key_id=7)
        system.save(self.ws, "road", s)
        self.assertEqual(system.load(self.ws, "road")["parts_from"], "source")
        self.assertEqual(system.load(self.ws, "road")["shell"], "android")
        built = os.path.join(self.tmp, "built")

        def fake_compile(ws, board, shell, key, key_id, password, out, on_step, on_line, base):
            test_system.make_parts(out, board)
            for i in ("compile-full", "package", "sign"):
                on_step(i, "done", "")
            return {"ok": True, "out": out, "version": "src-1234567", "parts": []}
        seen = []
        def fake_modules(ws, board, key, key_id, password, drivers, apps, out, on_step, on_line, base):
            os.makedirs(out)
            mods = []
            for m in system.MODULES:
                p = os.path.join(out, "%s-%s.cat" % (m, board))
                img = formats.image()
                open(p, "wb").write(img.make_image(m, "src-1234567", img.IMG_MODULE, img.MOD_DRIVER, b"\xe9" * 64, img.CHIP_ESP32S3))
                mods.append(p)
            ds = []
            for d in drivers:
                p = os.path.join(out, "%s-%s.cat" % (d["name"], d["scope"]))
                open(p, "wb").write(b"drv")
                ds.append({"name": d["name"], "scope": d["scope"], "version": "1.1.0+123456", "path": p, "file": os.path.basename(p), "size": 3, "sha256": __import__("hashlib").sha256(b"drv").hexdigest()})
            return {"ok": True, "modules": mods, "drivers": ds, "apps": [], "missing": []}
        with mock.patch.object(source, "pull", return_value={"commit": "1234567" + "0" * 33, "changed": True}), \
                mock.patch.object(source, "compiler", return_value={"found": True, "version": "5.3.5", "path": "/idf", "right": True}), \
                mock.patch.object(source, "apply_trust", return_value=[1, 2, 3, 4, 5, 6, 7]) as at, \
                mock.patch.object(source, "rebake", return_value={"ok": True, "count": 16, "problem": ""}) as rb, \
                mock.patch.object(source, "compile_modules", side_effect=fake_modules) as cm, \
                mock.patch.object(source, "compile_and_package", side_effect=fake_compile):
            r = system.build(self.ws, "road", password="pw", on_step=lambda i, st, n: seen.append(i), on_line=lambda l: None)
        self.assertTrue(r["ok"], r["steps"])
        self.assertEqual(seen[:5], ["source", "source", "compiler", "compiler", "trust"])
        own = at.call_args[0][2]
        self.assertEqual([(o[1], o[2]) for o in own], [("boot", 7), ("developer", 6)])
        self.assertTrue(at.call_args[0][1])                                        # PURR's keys are kept by default
        self.assertEqual(rb.call_args[0][2:5], (os.path.join(self.ws.keys, "developer.key"), 6, "pw"))      # the baked-in binaries are signed with the developer key
        rec = os.path.join(r["delivery"], "recovery")
        self.assertIn("component=bme280", open(os.path.join(rec, "drivers.manifest")).read())
        self.assertTrue(all(os.path.isfile(os.path.join(rec, "%s-tdeck_plus.cat" % m)) for m in system.MODULES))
        self.assertEqual(r["steps"]["image"]["state"], "done")
        self.assertTrue(os.path.isdir(os.path.join(r["image"] and os.path.dirname(r["image"]), "parts")))

    def test_a_version_the_device_cannot_read_stops_the_road_before_anything_is_fetched(self):
        s = system.new_system(self.ws, "road", "tdeck_plus")
        s.update(parts_from="source", key="developer.key", key_id=6, boot_key="boot.key", boot_key_id=7, version="src-1")
        system.save(self.ws, "road", s)
        with mock.patch.object(source, "pull") as pull:
            r = system.build(self.ws, "road", on_line=lambda l: None)
        self.assertFalse(r["ok"])
        self.assertIn("numbers", r["steps"]["source"]["note"])
        pull.assert_not_called()

    def test_source_mode_needs_a_key_a_compiler_and_a_reachable_source(self):
        s = system.new_system(self.ws, "road", "tdeck_plus")
        s.update(parts_from="source")
        system.save(self.ws, "road", s)
        r = system.build(self.ws, "road", on_line=lambda l: None)
        self.assertFalse(r["ok"])
        self.assertEqual(r["steps"]["source"]["state"], "fail")
        self.assertIn("boot key", r["steps"]["source"]["note"])
        s.update(key="developer.key", key_id=4, boot_key="boot.key", boot_key_id=7)
        system.save(self.ws, "road", s)
        self.assertIn("PURR's", system.build(self.ws, "road", on_line=lambda l: None)["steps"]["source"]["note"])      # id 4 is PURR's developer key
        s.update(key_id=6)
        system.save(self.ws, "road", s)
        with mock.patch.object(source, "pull", side_effect=source.SourceError("no network")):
            self.assertIn("no network", system.build(self.ws, "road", on_line=lambda l: None)["steps"]["source"]["note"])
        with mock.patch.object(source, "pull", return_value={"commit": "a" * 40, "changed": False}), mock.patch.object(source, "compiler", return_value={"found": False, "version": "", "path": "", "right": False}):
            r = system.build(self.ws, "road", on_line=lambda l: None)
        self.assertEqual(r["steps"]["compiler"]["state"], "fail")
        self.assertIn("5.3.5", r["steps"]["compiler"]["note"])


if __name__ == "__main__":
    unittest.main()
