"""The kit's prebuilt parts: fetching them from an over-the-air release folder, and finding them by the names that release uses."""
import hashlib
import importlib.util
import os
import shutil
import tempfile
import unittest

import helpers  # noqa: F401  (puts the sdk folder on sys.path)

from purrsdk import release

_spec = importlib.util.spec_from_file_location("build_kit", os.path.join(helpers.SDK, "build-kit.py"))
build_kit = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(build_kit)

BASE = "https://example.invalid/releases/download/JumpingJaguar1/"


def _release(files, bad=None):
    """({name: bytes} the server holds, the recovery.manifest text) for `files` {(component, board, name): bytes}."""
    served, blocks = {}, ["release=1.1+tdeck_plus\nreleased=2026-11-09\nwithdrawn_versions=*@1.0.24"]
    for (comp, board, name), data in files.items():
        served[name] = data
        blocks.append("component=%s\nversion=1.0\nchip=esp32s3\nboard=%s\nfile=%s\nsize=%d\nsha256=%s\nkey=boot" %
                      (comp, board, name, len(data), hashlib.sha256(data).hexdigest()))
    served["recovery.manifest"] = ("\n\n".join(blocks) + "\n").encode()
    served["legal.txt"] = b"credits\n"
    if bad:
        served[bad] = served[bad] + b"x"
    return served


class FetchTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="purrsdk-test-")
        self.addCleanup(shutil.rmtree, self.tmp, True)

    def _fetch(self, served):
        return build_kit.fetch_release_parts(BASE, self.tmp, opener=lambda url: served[url[len(BASE):]])

    def test_files_are_written_and_described(self):
        served = _release({("kernel", "tdeck_plus", "kernel-tdeck_plus.aa11.cat"): b"K" * 40, ("kittenos", "tdeck_plus", "kittenos-tdeck_plus.bb22.kitt"): b"O" * 10})
        info = self._fetch(served)
        self.assertEqual(sorted(os.listdir(self.tmp)), ["kernel-tdeck_plus.aa11.cat", "kittenos-tdeck_plus.bb22.kitt", "legal.txt", "recovery.manifest"])
        self.assertEqual((info["files"], info["bytes"], info["release"], info["released"]), (2, 50, "1.1+tdeck_plus", "2026-11-09"))
        with open(os.path.join(self.tmp, "recovery.manifest"), "rb") as fh:
            self.assertEqual(fh.read(), served["recovery.manifest"])             # kept exactly as the release gives it

    def test_a_wrong_hash_stops_the_build(self):
        served = _release({("kernel", "tdeck_plus", "kernel-tdeck_plus.aa11.cat"): b"K" * 40}, bad="kernel-tdeck_plus.aa11.cat")
        with self.assertRaises(SystemExit) as cm:
            self._fetch(served)
        self.assertIn("does not match", str(cm.exception))

    def test_a_file_name_with_a_path_is_refused(self):
        served = _release({("kernel", "tdeck_plus", "../evil.cat"): b"K"})
        with self.assertRaises(SystemExit):
            self._fetch(served)
        self.assertFalse(os.path.exists(os.path.join(os.path.dirname(self.tmp), "evil.cat")))

    def test_an_empty_manifest_is_refused(self):
        with self.assertRaises(SystemExit):
            self._fetch({"recovery.manifest": b"release=1.1\n", "legal.txt": b""})

    def test_a_missing_legal_text_does_not_stop_it(self):
        served = _release({("kernel", "tdeck_plus", "kernel-tdeck_plus.aa11.cat"): b"K" * 40})

        def opener(url):
            name = url[len(BASE):]
            if name == "legal.txt":
                raise OSError("404")
            return served[name]
        info = build_kit.fetch_release_parts(BASE, self.tmp, opener=opener)
        self.assertEqual(info["files"], 1)
        self.assertFalse(os.path.exists(os.path.join(self.tmp, "legal.txt")))


class PrebuiltTests(unittest.TestCase):
    def test_a_fetched_release_folder_is_found_by_its_manifest(self):
        tmp = tempfile.mkdtemp(prefix="purrsdk-test-")
        self.addCleanup(shutil.rmtree, tmp, True)
        files = {(c, "tdeck_plus", "%s-tdeck_plus.%s%s" % (c, "ab12", ".kitt" if c == "kittenos" else ".cat")): c.encode() * 4
                 for c in ("kernel", "kittenos", "loader", "bootpkg", "bootloader")}
        build_kit.fetch_release_parts(BASE, tmp, opener=lambda url: _release(files)[url[len(BASE):]])
        old = os.environ.get("PURR_IMAGES")
        os.environ["PURR_IMAGES"] = tmp
        self.addCleanup(lambda: os.environ.pop("PURR_IMAGES", None) if old is None else os.environ.__setitem__("PURR_IMAGES", old))
        found = release.prebuilt("tdeck_plus")
        self.assertEqual(sorted(found), ["bootloader", "bootpkg", "kernel", "kittenos", "loader"])
        self.assertEqual(release.prebuilt("tdeck"), {})                          # no parts for a board the folder does not carry


if __name__ == "__main__":
    unittest.main()
