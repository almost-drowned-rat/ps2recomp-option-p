"""A game's run-time immediates (game.json "immediates", lift.use_immediates): the listed instructions' constants
read through lifted::Immediate(k, original), their functions kept plain."""

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))

from ps2decomp import lift  # noqa: E402
from ps2decomp.r5900 import DISASM  # noqa: E402
import lift as lift_tool  # noqa: E402  (tools/lift.py)


class TestImmediates(unittest.TestCase):
    def tearDown(self):
        lift.use_immediates([])

    def statement(self, word, address):
        return lift.statement(DISASM.decode(word, address))

    def test_unlisted_instructions_are_unchanged(self):
        lift.use_immediates([0x1000])
        self.assertEqual(self.statement(0x3C024248, 0x2000), "c.r[2] = 0x42480000ull;")

    def test_each_kind_reads_its_operand_through_immediate(self):
        lift.use_immediates([0x100, 0x104, 0x108])
        # lui v0, 0x4248 (50.0f's upper half): the register's value
        self.assertEqual(self.statement(0x3C024248, 0x100), "c.r[2] = lifted::Immediate(0, 0x42480000ull);")
        # ori v0, v0, 0x6666: zero-extended
        self.assertEqual(self.statement(0x34426666, 0x104), "c.r[2] = c.r[2] | lifted::Immediate(1, 0x6666ull);")
        # addiu a0, zero, 50: the 32-bit immediate
        self.assertEqual(self.statement(0x24040032, 0x108),
                         "c.r[4] = lifted::Sx32((u32)0ull + (u32)lifted::Immediate(2, 0x32u));")

    def test_other_instructions_are_refused_loudly(self):
        lift.use_immediates([0x100])
        with self.assertRaises(ValueError):
            self.statement(0x00851021, 0x100)          # addu v0, a0, a1: no immediate

    def test_their_functions_stay_plain(self):
        lift.use_immediates([0x108])
        self.assertTrue(lift_tool.holds_immediate({"addr": 0x100, "end": 0x110}))
        self.assertFalse(lift_tool.holds_immediate({"addr": 0x110, "end": 0x120}))
        self.assertTrue(lift_tool.holds_immediate({"addr": 0x200, "end": 0x210, "chunks": [(0x100, 0x10C)]}))


if __name__ == "__main__":
    unittest.main()
