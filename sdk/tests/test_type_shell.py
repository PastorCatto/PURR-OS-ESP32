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

    def test_every_froyo_symbol_the_rest_of_the_firmware_uses_is_defined_by_the_type_shell(self):
        """An image has either Froyo or the Type shell. If a file that is in both images (the shared window code, say) calls a function only a Froyo file defines,
        the Type shell image does not link: found for real, 2026-11-12, with and_app_icon(). Every such function must be defined by the Type shell too."""
        froyo_defs = set()
        for n in os.listdir(MAIN):
            if n.startswith("purr_android") and n.endswith(".c"):
                froyo_defs |= set(re.findall(r"^[A-Za-z_][\w\s\*]*?\b((?:and_|purr_android_)\w+)\(", read("PurrOS", "main", n), re.M))
        froyo_defs -= {m for m in froyo_defs if re.search(r"^static\b[^\n]*\b%s\(" % m, "\n".join(read("PurrOS", "main", n) for n in os.listdir(MAIN) if n.startswith("purr_android") and n.endswith(".c")), re.M)}
        self.assertIn("and_app_icon", froyo_defs)
        _files, code = tp_sources()
        for n in sorted(os.listdir(MAIN)):
            if not n.endswith(".c") or n.startswith("purr_android") or n.startswith("purr_tp"):
                continue
            text = read("PurrOS", "main", n)
            for sym in sorted(froyo_defs):
                if re.search(r"(?<![\w])%s\(" % sym, re.sub(r"/\*.*?\*/", "", text, flags=re.S)):
                    defined = re.search(r"^[A-Za-z_][\w\s\*]*?\b%s\(" % sym, code, re.M) is not None
                    self.assertTrue(defined, "%s calls %s, which only the Froyo files define: the Type shell image will not link" % (n, sym))


class SharedScreens(unittest.TestCase):
    def test_the_permission_prompt_can_be_answered_without_touch(self):
        """On a board with a gamepad the permission prompt (modal) was impossible to answer, which locked the whole UI behind it (found in the first emulator run)."""
        perm = read("PurrOS", "main", "purr_w10m_perm.c")
        self.assertIn("int w10m_perm_key(uint32_t key)", perm)
        self.assertIn("perm_focus(2);", perm)                                   # it starts on "Don't allow": a stray Enter never grants anything
        self.assertIn("LV_KEY_ESC", perm)
        hook = read("PurrOS", "main", "purr_tp.c")
        self.assertRegex(hook, r"static int key_hook\(uint32_t key\)\s*\{\s*if \(w10m_perm_key\(key\)\)")   # first, before the lock screen and everything else

    def test_the_settings_search_bar_is_removed_in_the_type_build_only(self):
        text = read("PurrOS", "main", "purr_w10m_settings.c")
        self.assertIn("#if !CONFIG_PURR_SHELL_TYPE", text)
        i = text.index("static void page_main(void)")
        self.assertRegex(text[i:i + 400], r"#if !CONFIG_PURR_SHELL_TYPE\s+s_search = cm_input\(s_body, \"Find a setting\", 0\);")      # still there for every other build


class TheTheme(unittest.TestCase):
    """The user's rule (2026-11-12): the theme handles the user interface directly; a layout change is made in the theme and not in an app. So the d-pad and the look
    of every sheet come from the shell and the theme, and the shared screens only learned to name their roots and to ask for a warning colour."""

    def test_the_d_pad_reaches_every_sheet_through_the_shell(self):
        hook = read("PurrOS", "main", "purr_tp.c")
        i = hook.index("static int key_hook(uint32_t key)")
        body = hook[i:i + 2400]
        self.assertRegex(body, r"if \(tp_sheet_open\(\)\) \{\s*if \(tp_nav_key\(key\)\)")      # before the shell's own Back and Home handling
        self.assertRegex(hook, r"void tp_go_home\(void\)\s*\{[^}]*\}\s*tp_nav_reset\(\);")           # the keyboard and the highlight do not outlive the sheet
        self.assertIn("tp_nav_init();", hook)
        self.assertIn("tp_nav_layout();", hook)

    def test_every_sheet_names_its_root(self):
        priv = read("PurrOS", "main", "purr_w10m_priv.h")
        nav = read("PurrOS", "main", "purr_tp_nav.c")
        for fn, src in (("w10m_settings_root", "purr_w10m_settings.c"), ("w10m_devportal_root", "purr_w10m_devportal.c"), ("w10m_tasks_root", "purr_w10m_tasks.c")):
            self.assertIn("lv_obj_t *%s(void);" % fn, priv)
            self.assertRegex(read("PurrOS", "main", src), r"lv_obj_t \*%s\(void\)" % fn)
            self.assertIn(fn + "()", nav)
        self.assertIn("purr_catrt_ui_front()", nav)
        self.assertIn("lv_obj_t *purr_catrt_ui_front(void);", read("PurrOS", "main", "purr_catrt_ui.h"))

    def test_an_activation_is_what_a_finger_does(self):
        """A switch flips on the release and a button hears the click; both must be sent, in that order, and a handler that deletes the control stops the rest."""
        nav = read("PurrOS", "main", "purr_tp_nav.c")
        i = nav.index("static int activate(void)")
        body = nav[i:i + 1800]
        order = [body.index(c) for c in ("LV_EVENT_PRESSED", "LV_EVENT_RELEASED", "LV_EVENT_CLICKED")]
        self.assertEqual(order, sorted(order))
        self.assertEqual(body.count("!= LV_RESULT_OK"), 2)

    def test_the_type_look_is_switched_on_by_the_shell_only(self):
        theme = read("PurrOS", "main", "purr_w10m_theme.c")
        self.assertIn("void purr_theme_set_type_look(int on)", theme)
        self.assertIn("void purr_theme_set_type_look(int on);", read("PurrOS", "main", "purr_w10m_theme.h"))
        users = [n for n in os.listdir(MAIN) if n.endswith(".c") and "purr_theme_set_type_look(" in read("PurrOS", "main", n) and n != "purr_w10m_theme.c"]
        self.assertEqual(users, ["purr_tp.c"])
        tp = read("PurrOS", "main", "purr_tp.c")
        self.assertLess(tp.index("purr_theme_set_skin(PURR_SKIN_W10M);"), tp.index("purr_theme_set_type_look(1);"))
        self.assertLess(tp.index("purr_theme_set_type_look(1);"), tp.index("purr_theme_init();"))
        # a look and not a skin: the layout tests in the shared screens (purr_theme_skin() == PURR_SKIN_ANDROID) still see W10M
        self.assertNotIn("PURR_SKIN_TYPE", theme)

    def test_the_theme_and_the_shell_agree_on_what_reads_on_the_accent(self):
        theme = read("PurrOS", "main", "purr_w10m_theme.c")
        shell = read("PurrOS", "main", "purr_tp_kbd.c")                # tp_on_accent() lives with the key grid and the focus manager, which the shell-less images use too
        for text in (theme, shell):
            self.assertIn("0.2126f", text)
            self.assertIn("0.7152f", text)
            self.assertIn("0.0722f", text)
            self.assertIn("0.05f", text)
        self.assertIn("0x14120F", theme)
        self.assertIn("0xF4F1EA", theme)
        self.assertEqual(re.search(r"#define TP_INK\s+(0x\w+)", read("PurrOS", "main", "purr_tp_priv.h")).group(1).upper(), "0X14120F")

    def test_settings_has_a_type_layout_decided_by_the_theme(self):
        """The layout lives in the shared primitives and one branch of the main page, chosen by the theme at run time, so no other build changes."""
        widgets = read("PurrOS", "main", "purr_w10m_widgets.c")
        self.assertGreaterEqual(widgets.count("purr_theme_type_look()"), 3)           # title, switch row, tappable row
        settings = read("PurrOS", "main", "purr_w10m_settings.c")
        i = settings.index("static void page_main(void)")
        main = settings[i:i + 4200]
        branch = main[main.index("if (purr_theme_type_look())"):]
        self.assertLess(branch.index("continue;"), branch.index("w10m_img(sec->icon)"))     # no Windows-style pictures in the Type layout
        self.assertIn("purr_theme_type_look() ? \"SETTINGS\"", settings)
        theme = read("PurrOS", "main", "purr_w10m_theme.c")
        self.assertRegex(theme, r"void purr_theme_row_divider\(lv_obj_t \*obj\)\s*\{\s*if \(s_type\)")   # no effect in any other look
        nav = read("PurrOS", "main", "purr_tp_nav.c")
        self.assertEqual(int(re.search(r"#define HEAD_H\s+(\d+)", nav).group(1)), int(re.search(r"#define HEAD_H (\d+)", settings).group(1)))   # a control whose centre is in the title bar is skipped, so the first row (centre 23 px under it) is the start

    def test_the_images_without_a_shell_have_the_d_pad_too(self):
        """The owner could not enable developer mode on Playtime: KittenOS's recovery app, the internet loader, the setup wizard and the boot menu had no way to take
        a gamepad. A default key hook (purr_dpad_key) is installed by purr_lvgl_init() on that board, the recovery app names its root and its Back, and the boot
        package reads the gamepad module over its bit-banged bus."""
        lvgl = read("PurrOS", "main", "purr_lvgl.c")
        self.assertRegex(lvgl, r"#if CONFIG_PURR_BOARD_PLAYTIME[^#]*purr_lvgl_set_key_hook\(purr_dpad_key\);")
        rec = read("PurrOS", "main", "recovery_app.c")
        for needle in ("tp_nav_set_root(recovery_root, 0);", "tp_nav_set_back(recovery_back);", "tp_nav_init();"):
            self.assertIn(needle, rec)
        shim = read("PurrOS", "main", "purr_tp_shim.c")
        for fn in ("tp_signed_in", "tp_modal_up", "tp_pad_hint", "w10m_perm_is_open", "w10m_settings_root", "w10m_devportal_root", "w10m_tasks_root", "purr_catrt_ui_front"):
            self.assertRegex(shim, r"__attribute__\(\(weak\)\)[^\n]*\b%s\(" % fn, fn)      # weak: a shell's own definition wins
        hw = read("bootpkg", "src", "pkg_hw.c")
        self.assertIn("#ifdef BOARD_GAMEPAD_ADDR", hw)
        self.assertIn("BOARD_GAMEPAD_ADDR", read("bootpkg", "boards", "playtime.h"))
        self.assertNotIn("BOARD_GAMEPAD_ADDR", read("bootpkg", "boards", "tdeck_plus.h"))      # the T-Deck's keyboard path is untouched
        # the scan is the driver's: rows GPB0-1, columns GPB2-5, FUNC GPB6 (mask 0x7C pulled up), the lights left off
        self.assertIn("PAD_COLS   0x3C", hw)
        self.assertIn("PAD_FUNC   0x40", hw)
        self.assertIn("COL_MASK   0x3Cu", read("Modules", "drivers", "gamepad_mcp23017_module.c"))
        self.assertIn("FUNC_BIT   0x40u", read("Modules", "drivers", "gamepad_mcp23017_module.c"))

    def test_every_declared_font_exists_and_carries_its_licence(self):
        header = read("PurrOS", "main", "purr_tp_fonts.h")
        declared = re.findall(r"extern const lv_font_t (tp_font_\w+);", header)
        self.assertGreaterEqual(len(declared), 9)
        for name in declared:
            text = read("PurrOS", "main", "tpfonts", name + ".c")
            self.assertIn("const lv_font_t %s = {" % name, text)
            self.assertIn("SIL Open Font License", text)                       # the licence travels with the rendering
        self.assertIn("tpfonts/*.c", read("PurrOS", "main", "CMakeLists.txt"))
        third = read("THIRD_PARTY.md")
        self.assertIn("Space Grotesk", third)
        self.assertIn("Space Mono", third)
        for f in ("OFL-SpaceGrotesk.txt", "OFL-SpaceMono.txt", "README.md"):
            self.assertTrue(os.path.isfile(os.path.join(helpers.ROOT, "UI", "fonts", f)), f)
        # the images with no Type shell have the key grid and the navigation but not the fonts, so those two files must not use them
        for n in ("purr_tp_kbd.c", "purr_tp_nav.c", "purr_tp_shim.c"):
            self.assertNotIn("tp_font_", read("PurrOS", "main", n), n)

    def test_warnings_use_the_theme_colour(self):
        """Amber on paper does not read; the screens ask the theme."""
        for n in ("purr_w10m_settings.c", "purr_w10m_devportal.c"):
            self.assertNotIn("0xFFB900", read("PurrOS", "main", n), n)
        self.assertIn("0xFFB900", read("PurrOS", "main", "purr_w10m_theme.c"))      # the dark sheets keep their amber


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
        common = re.search(r"if\(CONFIG_PURR_SHELL_TYPE OR CONFIG_PURR_BOARD_PLAYTIME\)(.*?)endif\(\)", cmake, re.S).group(1)      # the d-pad files: the Type shell and every Playtime image
        for n in names:
            self.assertTrue('"%s"' % n in listed or '"%s"' % n in common, "%s is not in any source list" % n)
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
