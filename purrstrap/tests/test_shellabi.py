import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "scripts"))
import shellabi  # noqa: E402

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
        self.assertIn("a = 0x7F000000;", text)
        self.assertIn("b = 0x7F000004;", text)
        self.assertIn("c = 0x7F000008;", text)

    def test_the_c_header_agrees_on_the_base(self):
        h = open(os.path.join(os.path.dirname(DEF), "purr_shell_abi.h"), encoding="utf-8").read()
        self.assertIn("PURR_SHELL_IMPORT_BASE   0x%08Xu" % shellabi.IMPORT_BASE, h)


class ImportTests(unittest.TestCase):
    def blobs(self, words_a, words_b):
        return struct.pack("<%dI" % len(words_a), *words_a), struct.pack("<%dI" % len(words_b), *words_b)

    def test_placeholders_are_found_and_patched(self):
        a, b = self.blobs([0x1000, 0x7F000008, 5, 0x7F000000], [0x101000, 0x7F000008, 5, 0x7F000000])
        found = shellabi.find_imports(a, b, 4)
        self.assertEqual(found, [(4, 2), (12, 0)])
        out = shellabi.patch_imports(a, found, [0x42001000, 0x42002000, 0x3C000000, 0])
        self.assertEqual(struct.unpack("<4I", out), (0x1000, 0x3C000000, 5, 0x42001000))

    def test_a_placeholder_that_moved_with_the_base_is_refused(self):
        a, b = self.blobs([0x7F000004], [0x7F100004])
        with self.assertRaises(shellabi.ShellAbiError):
            shellabi.find_imports(a, b, 4)

    def test_a_word_in_range_but_not_a_placeholder_is_ignored(self):
        a, b = self.blobs([0x7F000003, 0x7F000100], [0x7F000003, 0x7F000100])
        self.assertEqual(shellabi.find_imports(a, b, 4), [])


if __name__ == "__main__":
    unittest.main()
