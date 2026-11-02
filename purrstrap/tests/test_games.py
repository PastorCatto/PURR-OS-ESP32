import importlib.util
import os
import tempfile
import unittest

from helpers import TOOL_DIR, make_ctx
from lib import image as pimg

spec = importlib.util.spec_from_file_location("games_under_test", os.path.join(TOOL_DIR, "scripts", "games.py"))
games = importlib.util.module_from_spec(spec)
spec.loader.exec_module(games)

try:
    import tomllib
except ImportError:                                   # the tool's own tests run on 3.9 too; the TOML-reading ones need 3.11
    tomllib = None

REPO = os.path.dirname(TOOL_DIR)
SHIPPED = os.path.join(REPO, "Partner-Emulation", "upstream", "retro-go", "components", "retro-go", "targets", "t-deck-plus", "config.h")

GOOD = {
    "name": "retro-core", "version": "1.0.0", "kind": "game", "payload": "image", "board": "tdeck_plus",
    "description": "NES, PC Engine, Lynx and more",
    "game": {"systems": ["nes", "pce"], "extensions": [".nes", ".pce"], "image": "retro-core.bin",
             "needs": {"buttons": ["a", "b", "start", "select"], "audio": True, "display_min": [240, 160]}},
}


class ManifestTests(unittest.TestCase):
    def test_a_good_manifest_has_no_problems(self):
        self.assertEqual(games.check_manifest(GOOD), [])

    def test_a_launcher_plays_no_systems(self):
        m = dict(GOOD, game=dict(GOOD["game"], systems=[], extensions=[]))
        self.assertEqual(games.check_manifest(m), [])

    def test_bad_fields_are_reported(self):
        m = dict(GOOD, name="bad name!", version="1.0", kind="ui", payload="zip")
        m["game"] = dict(GOOD["game"], extensions=["nes"], needs={"buttons": ["jump"], "display_min": [0, 1]})
        problems = " | ".join(games.check_manifest(m))
        for word in ("name", "version", "kind", "payload", "extensions", "buttons", "display_min"):
            self.assertIn(word, problems)

    def test_an_image_package_needs_an_image_and_a_board(self):
        m = dict(GOOD)
        m["game"] = {k: v for k, v in GOOD["game"].items() if k != "image"}
        del m["board"]
        problems = " | ".join(games.check_manifest(m))
        self.assertIn("image", problems)
        self.assertIn("board", problems)


CHAIN = """#if defined(RG_TARGET_A)
#include "targets/a/config.h"
#elif defined(RG_TARGET_B)
#include "targets/b/config.h"
#else
#warning "No target defined."
#include "targets/a/config.h"
#endif
"""


class RegisterTests(unittest.TestCase):
    def test_the_new_branch_goes_before_the_final_else(self):
        out = games.add_target_include(CHAIN, "purr-board-x")
        self.assertIn('#elif defined(RG_TARGET_PURR_BOARD_X)\n#include "targets/purr-board-x/config.h"\n#else', out)
        self.assertLess(out.index("RG_TARGET_PURR_BOARD_X"), out.index("#else"))

    def test_registering_twice_changes_nothing_more(self):
        once = games.add_target_include(CHAIN, "purr-board-x")
        self.assertEqual(games.add_target_include(once, "purr-board-x"), once)

    def test_a_file_without_an_else_is_refused(self):
        with self.assertRaises(ValueError):
            games.add_target_include("#if 1\n#endif\n", "x")


class PayloadTests(unittest.TestCase):
    def test_round_trip(self):
        meta = games.flatten(GOOD)
        image = b"\xE9" + bytes(range(200))
        got_meta, got_image = games.unpack_payload(games.pack_payload(meta, image))
        self.assertEqual(got_meta, meta)
        self.assertEqual(got_image, image)

    def test_damage_is_refused(self):
        blob = games.pack_payload(games.flatten(GOOD), b"\xE9" * 40)
        for bad in (blob[:-1], blob + b"x", b"XXXX" + blob[4:]):
            with self.assertRaises(ValueError):
                games.unpack_payload(bad)


@unittest.skipIf(tomllib is None, "needs Python 3.11 (tomllib)")
class PackageTests(unittest.TestCase):
    def test_package_then_inspect(self):
        with tempfile.TemporaryDirectory() as d:
            with open(os.path.join(d, "retro-core.bin"), "wb") as fh:
                fh.write(b"\xE9" + b"\x01" * 500)
            with open(os.path.join(d, "manifest.toml"), "w") as fh:
                fh.write('name = "retro-core"\nversion = "1.0.0"\nkind = "game"\npayload = "image"\nboard = "tdeck_plus"\n'
                         '[game]\nsystems = ["nes"]\nextensions = [".nes"]\nimage = "retro-core.bin"\n[game.needs]\nbuttons = ["a"]\n')
            ctx, out = make_ctx()
            self.assertEqual(games.package(ctx, os.path.join(d, "manifest.toml"), ""), 0)
            cat = os.path.join(d, "retro-core-tdeck_plus.cat")
            data = open(cat, "rb").read()
            f = pimg.header_fields(pimg.parse_header(data))
            self.assertEqual((f["image_type"], f["flags"] & 0xFF), (pimg.IMG_APP, games.APPF_GAME_IMAGE))
            ctx2, out2 = make_ctx()
            self.assertEqual(games.inspect(ctx2, cat), 0)
            self.assertIn("package is consistent", out2.getvalue())

    def test_a_file_that_is_not_an_app_image_is_refused(self):
        with tempfile.TemporaryDirectory() as d:
            open(os.path.join(d, "x.bin"), "wb").write(b"not an image")
            with open(os.path.join(d, "manifest.toml"), "w") as fh:
                fh.write('name = "x"\nversion = "1.0.0"\nkind = "game"\npayload = "image"\nboard = "tdeck_plus"\n[game]\nsystems = []\nimage = "x.bin"\n')
            ctx, out = make_ctx()
            self.assertEqual(games.package(ctx, os.path.join(d, "manifest.toml"), ""), 1)
            self.assertIn("0xE9", out.getvalue())


@unittest.skipIf(tomllib is None, "needs Python 3.11 (tomllib)")
class TargetTests(unittest.TestCase):
    def _defines(self, text):
        out = {}
        for line in text.replace("\\\n", " ").splitlines():
            parts = line.split(None, 2)
            if len(parts) >= 2 and parts[0] == "#define" and "(" not in parts[1]:
                out[parts[1]] = (parts[2].split("//")[0].strip() if len(parts) > 2 else "")
        return out

    @unittest.skipUnless(os.path.isfile(SHIPPED), "Retro-Go checkout not present")
    def test_the_generated_t_deck_plus_target_matches_the_hand_written_one(self):
        with open(os.path.join(REPO, "sdk", "boards", "tdeck_plus.toml"), "rb") as fh:
            board = tomllib.load(fh)
        made = self._defines(games.generate_config(board))
        shipped = self._defines(open(SHIPPED).read())
        wanted = [k for k in shipped if k.startswith(("RG_GPIO_", "RG_STORAGE_", "RG_AUDIO_", "RG_SCREEN_", "RG_BATTERY_"))
                  and k not in ("RG_SCREEN_SPEED",)]
        self.assertGreater(len(wanted), 20)
        for k in wanted:
            self.assertEqual(made.get(k), shipped[k], k)

    def test_a_board_without_a_game_section_says_so(self):
        with self.assertRaises(ValueError):
            games.generate_config({"board": {"id": "x"}})


if __name__ == "__main__":
    unittest.main()
