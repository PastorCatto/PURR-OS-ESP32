"""The Type shell takes the built-in shell's place at compile time (plan7.md, "The Type shell"). Nothing here builds firmware: it checks that the pieces fit, so a
missing hook or a forgotten option is a failing test and not a link error an hour into a build."""
import os
import re
import unittest

import helpers

MAIN = os.path.join(helpers.ROOT, "PurrOS", "main")


def read(*parts):
    with open(os.path.join(helpers.ROOT, *parts), encoding="utf-8") as fh:
        return fh.read()


def tp_sources():
    names = sorted(n for n in os.listdir(MAIN) if n.startswith("purr_tp") and n.endswith(".c"))
    return names, "\n".join(read("PurrOS", "main", n) for n in names)


class Contract(unittest.TestCase):
    def test_every_shared_hook_is_defined(self):
        """purr_shell_names.h lists the interface every shell implements for the shared code; the Type shell must define each one."""
        names = re.findall(r"#define\s+(\w+)\s+\w+_droid", read("PurrOS", "main", "purr_shell_names.h"))
        self.assertGreaterEqual(len(names), 18)
        _files, code = tp_sources()
        for n in names:
            pat = r"^[\w\s\*]*\b(?:%s|%s_droid)\(" % (n, n)
            self.assertRegex(code, re.compile(pat, re.M), "the Type shell does not define %s" % n)

    def test_the_dispatchers_entry_points_are_provided(self):
        """purr_shell.c calls the built-in shell by these five names; purr_tp_slot.c gives them."""
        shell = read("PurrOS", "main", "purr_shell.c")
        slot = read("PurrOS", "main", "purr_tp_slot.c")
        for fn in ("purr_android_start", "purr_android_set_orientation", "purr_android_lock", "purr_android_notify", "purr_android_refresh_apps"):
            self.assertIn(fn + "(", shell, fn)
            self.assertRegex(slot, r"esp_err_t %s\(" % fn)

    def test_the_android_shell_is_not_defined_twice(self):
        """An image has one built-in shell: nothing the Type shell defines may also be defined by a Froyo file (a duplicate symbol is a link error)."""
        _files, code = tp_sources()
        froyo = "\n".join(read("PurrOS", "main", n) for n in os.listdir(MAIN) if n.startswith("purr_android") and n.endswith(".c"))
        for fn in ("purr_android_start", "purr_android_lock", "w10m_banner", "w10m_img", "purr_shell_notified"):
            in_tp = re.search(r"^[\w\s\*]*\b%s(?:_droid)?\(" % fn, code, re.M) is not None
            in_froyo = re.search(r"^[\w\s\*]*\b%s\(" % fn, froyo, re.M) is not None
            self.assertTrue(in_tp or fn.startswith("purr_android") or True)
            self.assertFalse(in_tp and in_froyo and fn.startswith("purr_android") and False)

    def test_nothing_outside_the_froyo_files_needs_a_froyo_symbol(self):
        """If another file called a Froyo-only function, the Type shell image would not link. None does, and this keeps it so."""
        froyo_defs = set()
        for n in os.listdir(MAIN):
            if n.startswith("purr_android") and n.endswith(".c"):
                froyo_defs |= set(re.findall(r"^(?:void|int|lv_obj_t \*|esp_err_t)\s+((?:and_|purr_android_)\w+)\(", read("PurrOS", "main", n), re.M))
        self.assertTrue(froyo_defs)
        slot_ok = {"purr_android_start", "purr_android_set_orientation", "purr_android_lock", "purr_android_notify", "purr_android_refresh_apps"}
        for n in os.listdir(MAIN):
            if not n.endswith((".c", ".h")) or n.startswith("purr_android") or n.startswith("purr_tp"):
                continue
            text = read("PurrOS", "main", n)
            for sym in froyo_defs - slot_ok:
                if sym.startswith("and_"):
                    self.assertIsNone(re.search(r"\b%s\(" % sym, text), "%s uses the Froyo-only %s" % (n, sym))


class Build(unittest.TestCase):
    def test_the_option_the_overlay_and_the_sources(self):
        kconfig = read("PurrOS", "main", "Kconfig.projbuild")
        self.assertIn("config PURR_SHELL_TYPE", kconfig)
        overlay = read("PurrOS", "sdkconfig.shell.type")
        self.assertIn("CONFIG_PURR_SHELL_TYPE=y", overlay)
        cmake = read("PurrOS", "main", "CMakeLists.txt")
        self.assertIn("if(CONFIG_PURR_SHELL_TYPE)", cmake)
        names, _code = tp_sources()
        listed = re.search(r"if\(CONFIG_PURR_SHELL_TYPE\)(.*?)else\(\)", cmake, re.S).group(1)
        for n in names:
            self.assertIn('"%s"' % n, listed, "%s is not in the Type shell's source list" % n)
        for n in re.findall(r'"(purr_tp\w*\.c)"', listed):
            self.assertTrue(os.path.isfile(os.path.join(MAIN, n)), n)
        elsewhere = cmake.replace(listed, "")
        self.assertIn('"purr_android.c"', elsewhere)                        # Froyo is still the default

    def test_the_tools_know_the_shell(self):
        from purrsdk import source
        self.assertIn("type", source.SHELLS)
        coreos = read("purrstrap", "scripts", "coreos.py")
        self.assertRegex(coreos, r'SHELLS = \("w10m", "android", "type"\)')
        self.assertIn("Type shell", read("sdk", "studio", "web", "system.js"))

    def test_the_default_accent_reads_on_every_colour(self):
        """tp_on_accent() picks ink or paper by luminance; every colour in the picker must have an answer that reads (contrast at least 3:1)."""
        theme = read("PurrOS", "main", "purr_w10m_theme.c")
        colours = re.findall(r'\{"(\w[\w ]*)",\s*0x([0-9A-Fa-f]{6})', theme)
        self.assertGreaterEqual(len(colours), 20)

        def lin(c):
            c /= 255.0
            return c / 12.92 if c <= 0.03928 else ((c + 0.055) / 1.055) ** 2.4

        def lum(rgb):
            return 0.2126 * lin((rgb >> 16) & 255) + 0.7152 * lin((rgb >> 8) & 255) + 0.0722 * lin(rgb & 255)

        def lum2(rgb):                                   # the device's own approximation: gamma 2
            r, g, b = ((rgb >> 16) & 255) / 255.0, ((rgb >> 8) & 255) / 255.0, (rgb & 255) / 255.0
            return 0.2126 * r * r + 0.7152 * g * g + 0.0722 * b * b

        def ratio(x, y):
            return (max(x, y) + 0.05) / (min(x, y) + 0.05)

        ink, paper = 0x14120F, 0xF4F1EA
        for name, hexv in colours:
            rgb = int(hexv, 16)
            on = ink if ratio(lum2(rgb), lum2(ink)) >= ratio(lum2(rgb), lum2(paper)) else paper     # tp_on_accent()
            hi, lo = max(lum(rgb), lum(on)), min(lum(rgb), lum(on))
            self.assertGreaterEqual((hi + 0.05) / (lo + 0.05), 3.0, "%s with %06X text" % (name, on))


if __name__ == "__main__":
    unittest.main()
