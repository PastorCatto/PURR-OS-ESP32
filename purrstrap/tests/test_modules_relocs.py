import importlib.util
import os
import unittest

from helpers import TOOL_DIR, make_ctx

spec = importlib.util.spec_from_file_location("modules_under_test", os.path.join(TOOL_DIR, "scripts", "modules.py"))
modules = importlib.util.module_from_spec(spec)
spec.loader.exec_module(modules)

READELF = """\
Symbol table '.symtab' contains 6 entries:
   Num:    Value  Size Type    Bind   Vis      Ndx Name
    10: 00000044   300 FUNC    LOCAL  DEFAULT    7 on_job
    11: 00000170    16 FUNC    LOCAL  DEFAULT    8 tiny
    12: 00000200     8 OBJECT  LOCAL  DEFAULT    9 table
    13: 00000300     0 FUNC    GLOBAL DEFAULT   10 entry
"""


def words(*ws):
    return b"".join(w.to_bytes(4, "little") for w in ws)


class CodeAddressTests(unittest.TestCase):
    def test_every_byte_of_a_function_counts_as_code(self):
        code = modules._function_addresses(READELF)
        self.assertIn(0x44, code)                 # a function start (a function pointer)
        self.assertIn(0x44 + 0xAB, code)          # the middle: where a switch's jump table points
        self.assertIn(0x44 + 299, code)           # the last byte
        self.assertNotIn(0x180, code)             # the byte after the next function ends is not
        self.assertNotIn(0x20, code)
        self.assertNotIn(0x200, code)             # an OBJECT (a const table) is data
        self.assertIn(0x300, code)                # a size-0 FUNC still counts at its own address

    def test_a_jump_table_entry_is_relocated_against_the_code_alias(self):
        ctx, _ = make_ctx()
        delta = 0x1000
        code = modules._function_addresses(READELF)
        # word 0: a function pointer, word 1: a jump-table entry inside on_job, word 2: a pointer to a data object
        a = words(0x44, 0x44 + 0xAB, 0x200, 7)
        b = words(0x44 + delta, 0x44 + 0xAB + delta, 0x200 + delta, 7)
        data_relocs, code_relocs = modules.find_relocations(ctx, a, b, delta, code)
        self.assertEqual(code_relocs, [0, 4])
        self.assertEqual(data_relocs, [8])


if __name__ == "__main__":
    unittest.main()
