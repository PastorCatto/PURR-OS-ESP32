import os
import struct
import sys
import unittest

from helpers import TOOL_DIR  # noqa: E402,F401  (puts the tool's folder on the path)
from lib import shellabi  # noqa: E402

DEF = os.path.join(os.path.dirname(__file__), "..", "..", "PurrOS", "main", "purr_shell_exports.def")


class ParseTests(unittest.TestCase):
    def test_the_real_list_parses_and_has_no_duplicates(self):
        entries = shellabi.parse_def(open(DEF, encoding="utf-8").read())
        names = [n for _k, n, _t in entries]
        self.assertEqual(len(names), len(set(names)))
        self.assertGreater(len(entries), 150)
        self.assertIn("lv_obj_create", names)
        self.assertIn(("data", "lv_font_montserrat_16", "lv_font_t"), entries)

    def test_order_is_kept_and_kinds_are_told_apart(self):
        e = shellabi.parse_def("/* c */\nSHELL_FN(b)\nSHELL_FN(a)\nSHELL_DATA(const x_t, d)\n")
        self.assertEqual(e, [("fn", "b", None), ("fn", "a", None), ("data", "d", "const x_t")])

    def test_bad_lines_are_refused(self):
        for text in ("SHELL_FN(a)\nSHELL_FN(a)\n", "SHELL_FN(t, a)\n", "SHELL_DATA(a)\n", "int x;\n", "/* nothing */\n"):
            with self.assertRaises(shellabi.ShellAbiError, msg=text):
                shellabi.parse_def(text)


class LinkerScriptTests(unittest.TestCase):
    def test_every_name_gets_its_own_placeholder(self):
        e = shellabi.parse_def("SHELL_FN(a)\nSHELL_FN(b)\nSHELL_DATA(t, c)\n")
        text = shellabi.linker_script(e)
        self.assertIn("a = 0x0F000000;", text)
        self.assertIn("b = 0x0F000004;", text)
        self.assertIn("c = 0x0F000008;", text)

    def test_the_c_header_agrees_on_the_base(self):
        h = open(os.path.join(os.path.dirname(DEF), "purr_shell_abi.h"), encoding="utf-8").read()
        self.assertIn("PURR_SHELL_IMPORT_BASE   0x%08Xu" % shellabi.IMPORT_BASE, h)


class PayloadTests(unittest.TestCase):
    def test_the_payload_round_trips(self):
        blob = shellabi.pack_payload(1, 183, 0x40, 0x1200, [4, 8], [12], [(16, 3), (20, 0)], b"CODE")
        p = shellabi.parse_payload(blob)
        self.assertEqual((p["abi_version"], p["export_count"], p["entry_offset"], p["bss_size"]), (1, 183, 0x40, 0x1200))
        self.assertEqual((p["data_relocs"], p["code_relocs"], p["imports"], p["code"]), ([4, 8], [12], [(16, 3), (20, 0)], b"CODE"))

    def test_a_wrong_or_short_payload_is_refused(self):
        with self.assertRaises(shellabi.ShellAbiError):
            shellabi.parse_payload(b"\0" * 40)
        with self.assertRaises(shellabi.ShellAbiError):
            shellabi.parse_payload(shellabi.pack_payload(1, 1, 0, 0, [4, 8, 12], [], [], b"")[:36])


class ImportTests(unittest.TestCase):
    def blobs(self, words_a, words_b):
        return struct.pack("<%dI" % len(words_a), *words_a), struct.pack("<%dI" % len(words_b), *words_b)

    def test_placeholders_are_found_and_patched(self):
        a, b = self.blobs([0x1000, 0x0F000008, 5, 0x0F000000], [0x101000, 0x0F000008, 5, 0x0F000000])
        found = shellabi.find_imports(a, b, 4)
        self.assertEqual(found, [(4, 2), (12, 0)])
        out = shellabi.patch_imports(a, found, [0x42001000, 0x42002000, 0x3C000000, 0])
        self.assertEqual(struct.unpack("<4I", out), (0x1000, 0x3C000000, 5, 0x42001000))

    def test_a_placeholder_that_moved_with_the_base_is_refused(self):
        a, b = self.blobs([0x0F000004], [0x0F100004])
        with self.assertRaises(shellabi.ShellAbiError):
            shellabi.find_imports(a, b, 4)

    def test_a_word_in_range_but_not_a_placeholder_is_ignored(self):
        a, b = self.blobs([0x0F000003, 0x0F000100], [0x0F000003, 0x0F000100])
        self.assertEqual(shellabi.find_imports(a, b, 4), [])


import importlib.util  # noqa: E402
import json  # noqa: E402
import tempfile  # noqa: E402

_spec = importlib.util.spec_from_file_location("shells_under_test", os.path.join(TOOL_DIR, "scripts", "shells.py"))
shells = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(shells)


class ShellBuildTests(unittest.TestCase):
    def test_the_kernels_include_paths_and_defines_are_taken_and_its_optimisation_is_not(self):
        cmd = ("xtensa-esp32s3-elf-gcc -DESP_PLATFORM -DFOO=1 -I/a/inc -I /b/inc -isystem /c/sys -include /d/sdkconfig.h -mlongcalls -Og -ggdb "
               "-fstrict-volatile-bitfields -mdisable-hardware-atomics -std=gnu17 -o x.obj -c /p/main/purr_w10m.c")
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "compile_commands.json")
            json.dump([{"file": "/p/main/other.c", "command": "gcc -I/no"}, {"file": "/p/main/purr_w10m.c", "command": cmd, "directory": d}], open(path, "w"))
            got = shells.kernel_flags(path)
        self.assertEqual(got, ["-DESP_PLATFORM", "-DFOO=1", "-I/a/inc", "-I", "/b/inc", "-isystem", "/c/sys", "-include", "/d/sdkconfig.h",
                               "-fstrict-volatile-bitfields", "-mdisable-hardware-atomics"])
        self.assertIn("-mlongcalls", shells.module_flags(got))
        self.assertNotIn("-Og", shells.module_flags(got))

    def test_a_build_without_the_shell_is_refused(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "compile_commands.json")
            json.dump([{"file": "/p/main/other.c", "command": "gcc -c other.c"}], open(path, "w"))
            with self.assertRaises(shellabi.ShellAbiError):
                shells.kernel_flags(path)

    def test_the_linker_script_has_the_layout_and_every_placeholder(self):
        e = shellabi.parse_def("SHELL_FN(a)\nSHELL_FN(b)\n")
        text = shells.linker_script(0x100000, e)
        self.assertIn("ENTRY(purr_shell_module_main)", text)
        self.assertIn(".module 0x100000", text)
        self.assertIn("__shell_bss_end", text)
        self.assertIn("b = 0x0F000004;", text)

    def test_missing_names_are_read_from_the_linker_message(self):
        err = "x.o:(.text+0x4): undefined reference to `lv_foo'\nx.o: undefined reference to `purr_bar'\nx.o: undefined reference to `lv_foo'\n"
        self.assertEqual(shells._missing(err), ["lv_foo", "purr_bar"])


if __name__ == "__main__":
    unittest.main()
