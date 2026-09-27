import tempfile
import unittest

from helpers import GOOD, write_script

from lib import registry


class RegistryTests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()

    def load(self):
        return {e.name: e for e in registry.discover(self.dir)}

    def test_good_script_loads(self):
        write_script(self.dir, "good", GOOD.format(name="good"))
        e = self.load()["good"]
        self.assertEqual(e.status, registry.OK)
        self.assertEqual(e.script.actions[0].name, "hello")

    def test_broken_import_is_isolated(self):
        write_script(self.dir, "good", GOOD.format(name="good"))
        write_script(self.dir, "bad", "raise RuntimeError('boom')\n")
        entries = self.load()
        self.assertEqual(entries["bad"].status, registry.BROKEN)
        self.assertIn("boom", entries["bad"].error)
        self.assertEqual(entries["good"].status, registry.OK)

    def test_missing_script_object_is_broken(self):
        write_script(self.dir, "nothing", "x = 1\n")
        self.assertEqual(self.load()["nothing"].status, registry.BROKEN)

    def test_name_must_match_file(self):
        write_script(self.dir, "other", GOOD.format(name="wrong"))
        e = self.load()["other"]
        self.assertEqual(e.status, registry.BROKEN)
        self.assertIn("file name", e.error)

    def test_duplicate_action_is_broken(self):
        write_script(self.dir, "dup", """
            from lib.model import Script, Action
            f = lambda ctx: 0
            SCRIPT = Script("dup", "T", "d", (Action("a", "A", f), Action("a", "B", f)))
        """)
        self.assertIn("duplicate", self.load()["dup"].error)

    def test_choice_without_choices_is_broken(self):
        write_script(self.dir, "ch", """
            from lib.model import Script, Action, Param
            f = lambda ctx, x: 0
            SCRIPT = Script("ch", "T", "d", (Action("a", "A", f, params=(Param("x", "choice"),)),))
        """)
        self.assertIn("no choices", self.load()["ch"].error)

    def test_bad_default_is_broken(self):
        write_script(self.dir, "bd", """
            from lib.model import Script, Action, Param
            f = lambda ctx, x: 0
            SCRIPT = Script("bd", "T", "d", (Action("a", "A", f, params=(Param("x", "int", default="zz"),)),))
        """)
        self.assertIn("bad default", self.load()["bd"].error)

    def test_missing_requirement_is_unavailable_and_not_imported(self):
        write_script(self.dir, "needs", """
            raise RuntimeError("must not be imported")
        """)
        write_script(self.dir, "needs2", """
            from lib.model import Script
            SCRIPT = Script("needs2", "T", "d", (), requires=("no_such_pkg_xyz",))
        """)
        e = self.load()
        self.assertEqual(e["needs2"].status, registry.UNAVAILABLE)
        self.assertIn("no_such_pkg_xyz", e["needs2"].error)

    def test_underscore_files_ignored(self):
        write_script(self.dir, "_private", "raise RuntimeError('x')\n")
        self.assertEqual(self.load(), {})

    def test_missing_dir_is_empty(self):
        self.assertEqual(registry.discover(self.dir + "/nope"), [])


if __name__ == "__main__":
    unittest.main()
