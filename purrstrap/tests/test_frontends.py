import io
import tempfile
import unittest
from unittest import mock

from helpers import GOOD, make_ctx, write_script

from lib import cli, registry, ui
from lib.model import Param, coerce


class CoerceTests(unittest.TestCase):
    def test_types(self):
        self.assertEqual(coerce(Param("n", "int"), "0x10"), 16)
        self.assertIs(coerce(Param("b", "bool"), "yes"), True)
        self.assertIs(coerce(Param("b", "bool"), "off"), False)
        self.assertEqual(coerce(Param("c", "choice", choices=("a", "b")), "b"), "b")

    def test_errors(self):
        with self.assertRaises(ValueError):
            coerce(Param("n", "int"), "abc")
        with self.assertRaises(ValueError):
            coerce(Param("c", "choice", choices=("a",)), "z")
        with self.assertRaises(ValueError):
            coerce(Param("b", "bool"), "maybe")


class CliTests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        write_script(self.dir, "good", GOOD.format(name="good"))
        write_script(self.dir, "bad", "raise RuntimeError('boom')\n")
        self.entries = registry.discover(self.dir)
        self.ctx, self.out = make_ctx()

    def run_cli(self, *argv):
        return cli.main(list(argv), self.ctx, self.entries)

    def test_runs_action_with_params(self):
        self.assertEqual(self.run_cli("good", "hello", "--who", "b", "--loud"), 0)
        self.assertIn("HELLO b", self.out.getvalue())

    def test_bool_can_be_negated_and_defaults_apply(self):
        self.run_cli("good", "hello", "--no-loud")
        self.assertIn("hello a", self.out.getvalue())

    def test_bad_choice_is_rejected_by_argparse(self):
        with self.assertRaises(SystemExit):
            with mock.patch("sys.stderr", io.StringIO()):
                self.run_cli("good", "hello", "--who", "zzz")

    def test_unknown_script(self):
        self.assertEqual(self.run_cli("nope"), 2)

    def test_broken_script_reports_and_fails(self):
        self.assertEqual(self.run_cli("bad", "x"), 2)
        self.assertIn("boom", self.out.getvalue())

    def test_list_shows_everything(self):
        self.assertEqual(self.run_cli("list"), 0)
        text = self.out.getvalue()
        self.assertIn("good hello", text)
        self.assertIn("--who", text)
        self.assertIn("bad: broken", text)

    def test_crashing_action_is_contained(self):
        def boom(ctx):
            raise ValueError("kaput")
        from lib.model import Action
        self.assertEqual(cli.run_action(self.ctx, Action("x", "X", boom), {}), 1)
        self.assertIn("kaput", self.out.getvalue())

    def test_action_return_code_passes_through(self):
        from lib.model import Action
        self.assertEqual(cli.run_action(self.ctx, Action("x", "X", lambda ctx: 7), {}), 7)


def keys(*seq):
    it = iter(seq)
    return lambda: next(it)


class UiTests(unittest.TestCase):
    def setUp(self):
        self.out = io.StringIO()

    def test_menu_select(self):
        with mock.patch.object(ui, "read_key", keys("down", "down", "enter")):
            self.assertEqual(ui.menu("t", ["a", "b", "c"], self.out), 2)

    def test_menu_wraps_and_backs_out(self):
        with mock.patch.object(ui, "read_key", keys("up", "enter")):
            self.assertEqual(ui.menu("t", ["a", "b"], self.out), 1)
        with mock.patch.object(ui, "read_key", keys("esc")):
            self.assertIsNone(ui.menu("t", ["a"], self.out))

    def test_form_choice_bool_and_run(self):
        params = (Param("target", "choice", choices=("x", "y"), default="x"),
                  Param("clean", "bool", default=False))
        seq = keys("right", "down", "space", "down", "enter")
        with mock.patch.object(ui, "read_key", seq):
            result = ui.form("f", params, {}, self.out)
        self.assertEqual(result, {"target": "y", "clean": True})

    def test_form_prefills_and_text_edit(self):
        params = (Param("n", "int", default=1),)
        with mock.patch.object(ui, "read_key", keys("enter", "down", "enter")), \
                mock.patch.object(ui, "read_line", lambda p: "0x20"):
            self.assertEqual(ui.form("f", params, {}, self.out), {"n": 32})

    def test_form_rejects_bad_number_then_accepts_prefill(self):
        params = (Param("n", "int", default=5),)
        with mock.patch.object(ui, "read_key", keys("enter", "down", "enter")), \
                mock.patch.object(ui, "read_line", lambda p: "abc"):
            self.assertEqual(ui.form("f", params, {}, self.out), {"n": 5})
        self.assertIn("expected a number", self.out.getvalue())

    def test_form_blocks_run_when_required_missing(self):
        params = (Param("p", "str", required=True),)
        with mock.patch.object(ui, "read_key", keys("down", "enter", "esc")):
            self.assertIsNone(ui.form("f", params, {}, self.out))
        self.assertIn("still needed: p", self.out.getvalue())

    def test_form_cancel(self):
        with mock.patch.object(ui, "read_key", keys("esc")):
            self.assertIsNone(ui.form("f", (Param("a"),), {}, self.out))


if __name__ == "__main__":
    unittest.main()
