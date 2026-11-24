"""Tests for function discovery's "does this look like code" judgement.

The check has to reject data that merely decodes - runs of zeroes are valid
`nop`s - while accepting real functions, including the very short ones: a getter
is `jr $ra; lw $v0, 8($a0)`, and an empty function is just `jr $ra; nop`. Getting
the short ones wrong lost 424 call targets, one called from 866 sites.
"""

import os
import struct
import sys
import unittest

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
sys.path[:0] = [ROOT, os.path.join(ROOT, "tools")]

from ps2decomp.analysis import ImageAnalyzer

BASE = 0x00100000
JR_RA = 0x03E00008
NOP = 0
LW_V0_8_A0 = (0x23 << 26) | (4 << 21) | (2 << 16) | 8
J_ELSEWHERE = (2 << 26) | ((BASE + 0x100) >> 2)
JAL = (3 << 26) | ((BASE + 0x100) >> 2)
B_FORWARD = 0x10000004         # beq $zero, $zero, +4


def analyzer(words):
    words = list(words) + [NOP] * 64
    return ImageAnalyzer(struct.pack("<%dI" % len(words), *words), BASE, BASE)


class TestLooksLikeCode(unittest.TestCase):
    def test_getter_with_its_body_in_the_delay_slot(self):
        self.assertTrue(analyzer([JR_RA, LW_V0_8_A0])._looks_like_code(BASE))

    def test_empty_function_is_accepted_only_as_a_call_target(self):
        a = analyzer([JR_RA, NOP])
        self.assertTrue(a._looks_like_code(BASE, called=True))
        self.assertFalse(a._looks_like_code(BASE))

    def test_thunk_is_accepted_as_a_call_target(self):
        self.assertTrue(analyzer([J_ELSEWHERE, NOP])._looks_like_code(BASE, called=True))

    def test_zeroes_are_not_code(self):
        self.assertFalse(analyzer([NOP] * 8)._looks_like_code(BASE, called=True))

    def test_empty_delay_slot_does_not_count_as_padding(self):
        """`jal; nop; b; ...` is real code; counting its nop delay slot as padding
        tipped 37 pointer-table entries below the threshold."""
        self.assertTrue(analyzer([JAL, NOP, B_FORWARD, NOP])._looks_like_code(BASE))


class TestSwitches(unittest.TestCase):
    """A switch's cases belong to its function, not to functions of their own."""

    # switch (a0) { case 0: ...; case 1: ...; }, its table at BASE + 0x100.
    CODE = [
        0x2C810002,     # 00 sltiu $at, $a0, 2
        0x1020000A,     # 04 beq $at, $zero, 0x30
        NOP,            # 08
        0x3C030010,     # 0C lui $v1, 0x0010
        0x00042080,     # 10 sll $a0, $a0, 2
        0x24630100,     # 14 addiu $v1, $v1, 0x0100
        0x00832021,     # 18 addu $a0, $a0, $v1
        0x8C820000,     # 1C lw $v0, 0($a0)
        0x00400008,     # 20 jr $v0
        NOP,            # 24
        0x24420001,     # 28 case 0: addiu $v0, $v0, 1
        JR_RA, NOP,     # 2C
        0x24420002,     # 34 case 1: addiu $v0, $v0, 2
        JR_RA, NOP,     # 38
    ]

    def image(self):
        words = self.CODE + [NOP] * (0x40 - len(self.CODE))
        words += [BASE + 0x28, BASE + 0x34] + [NOP] * 32
        return ImageAnalyzer(struct.pack("<%dI" % len(words), *words), BASE, BASE)

    def test_the_table_is_read_from_the_code_before_the_jump(self):
        self.assertEqual(self.image().jump_table(BASE + 0x20, 2), [BASE + 0x28, BASE + 0x34])

    def test_the_low_half_may_be_in_the_load(self):
        # lui $v1, 0x0010; addu $v1, $v1, $a0; lw $v1, 0x0100($v1); jr $v1
        code = list(self.CODE)
        code[3:9] = [0x00042080, 0x3C030010, 0x00641821, NOP, 0x8C630100, 0x00600008]
        words = code + [NOP] * (0x40 - len(code)) + [BASE + 0x28, BASE + 0x34] + [NOP] * 32
        a = ImageAnalyzer(struct.pack("<%dI" % len(words), *words), BASE, BASE)
        self.assertEqual(a.jump_table(BASE + 0x20, 3), [BASE + 0x28, BASE + 0x34])

    def test_the_table_may_be_summed_into_another_register(self):
        # CodeWarrior: lui $v0, 0x0010; addiu $v1, $v0, 0x0100; sll $v0, $a0, 2;
        # addu $v0, $v0, $v1; lw $v0, 0($v0); jr $v0
        code = list(self.CODE)
        code[3:9] = [0x3C020010, 0x24430100, 0x00041080, 0x00431021, 0x8C420000, 0x00400008]
        words = code + [NOP] * (0x40 - len(code)) + [BASE + 0x28, BASE + 0x34] + [NOP] * 32
        a = ImageAnalyzer(struct.pack("<%dI" % len(words), *words), BASE, BASE)
        self.assertEqual(a.jump_table(BASE + 0x20, 2), [BASE + 0x28, BASE + 0x34])

    def test_cases_are_labels_of_the_switch(self):
        a = self.image()
        a.run()
        self.assertEqual(sorted(a.functions), [BASE])
        self.assertEqual(a.functions[BASE].end, BASE + 0x40)

    def test_the_table_may_be_in_a_data_section(self):
        """ZOE US keeps its switch tables in .rodata, apart from .text: unread, the
        cases of 0x00389488 were left out of it and a played session stopped at one
        (0x00389530, 2026-11-12)."""
        code = list(self.CODE)
        code[3], code[5] = 0x3C030011, 0x24630000       # the table at BASE + 0x10000
        words = code + [NOP] * (0x40 - len(code)) + [NOP] * 32
        image = struct.pack("<%dI" % len(words), *words)
        table = struct.pack("<2I", BASE + 0x28, BASE + 0x34)
        self.assertIsNone(ImageAnalyzer(image, BASE, BASE).jump_table(BASE + 0x20, 2))
        a = ImageAnalyzer(image, BASE, BASE, [(BASE + 0x10000, table)])
        self.assertEqual(a.jump_table(BASE + 0x20, 2), [BASE + 0x28, BASE + 0x34])
        a.run()
        self.assertEqual(sorted(a.functions), [BASE])


class TestSwitchBaseInSavedRegister(unittest.TestCase):
    """The table's `lui` in a saved register far above, a call between (God of
    War II's 0x002B85F0, P1 2026-11-24)."""

    CODE = [
        0x3C140010,     # 00 lui $s4, 0x0010
        JAL,            # 04 jal
        NOP,            # 08
        0x2C810002,     # 0C sltiu $at, $a0, 2
        0x1020000A,     # 10 beq $at, $zero, ...
        0x00042080,     # 14 sll $a0, $a0, 2
        0x26830100,     # 18 addiu $v1, $s4, 0x0100
        0x00832021,     # 1C addu $a0, $a0, $v1
        0x8C820000,     # 20 lw $v0, 0($a0)
        0x00400008,     # 24 jr $v0
        NOP,            # 28
        0x24420001, JR_RA, NOP,     # 2C case 0
        0x24420002, JR_RA, NOP,     # 38 case 1
    ]

    def analyzer(self, code):
        words = code + [NOP] * (0x40 - len(code)) + [BASE + 0x2C, BASE + 0x38] + [NOP] * 32
        return ImageAnalyzer(struct.pack("<%dI" % len(words), *words), BASE, BASE)

    def test_the_lui_is_found_past_the_call(self):
        self.assertEqual(self.analyzer(list(self.CODE)).jump_table(BASE + 0x24, 2), [BASE + 0x2C, BASE + 0x38])

    def test_no_table_when_the_register_is_set_otherwise_between(self):
        code = list(self.CODE)
        code[2] = 0x24140001        # addiu $s4, $zero, 1 in the call's delay slot
        self.assertIsNone(self.analyzer(code).jump_table(BASE + 0x24, 2))


class TestBuiltPointerAboveFrame(unittest.TestCase):
    """A callback whose address the code builds (`lui; addiu`), its entry an
    instruction above its frame setup and after no terminator (God of War II's
    0x00285408, 0x001C6918, P1 2026-11-24)."""

    def words(self, before):
        return [
            0x3C040010,     # 00 lui $a0, 0x0010
            0x24840018,     # 04 addiu $a0, $a0, 0x18: the callback's address
            JR_RA, NOP,     # 08
            0x30420020,     # 10 andi $v0, $v0, 0x20
            before,         # 14
            0x00C0182D,     # 18 the entry: daddu $v1, $a2, $zero
            0x27BDFFF0,     # 1C addiu $sp, $sp, -0x10
            JR_RA,          # 20
            0x27BD0010,     # 24 addiu $sp, $sp, 0x10
        ]

    def test_the_entry_is_the_built_address(self):
        a = analyzer(self.words(0x30420002))       # andi $v0, $v0, 2
        a.run()
        self.assertIn(BASE + 0x18, a.functions)
        self.assertNotIn(BASE + 0x1C, a.functions)

    def test_not_when_it_reads_what_the_instruction_before_set(self):
        a = analyzer(self.words(0x3C060001))       # lui $a2, 1: the "entry" is mid-sequence
        a.run()
        self.assertNotIn(BASE + 0x18, a.functions)


class TestPointerTablesInData(unittest.TestCase):
    """A function only a data section's word names (a callback table in .data)."""

    WORDS = [JR_RA, NOP,                        # BASE: the entry
             0x24420001, JR_RA, NOP]            # BASE + 8: addiu $v0, $v0, 1; jr $ra - no frame

    def test_found_through_the_data_section(self):
        image = struct.pack("<%dI" % (len(self.WORDS) + 64), *(self.WORDS + [NOP] * 64))
        plain = ImageAnalyzer(image, BASE, BASE)
        plain.run()
        self.assertNotIn(BASE + 8, plain.functions)
        a = ImageAnalyzer(image, BASE, BASE, [(BASE + 0x10000, struct.pack("<I", BASE + 8))])
        a.run()
        self.assertIn(BASE + 8, a.functions)



class TestScheduledEntry(unittest.TestCase):
    """An instruction scheduled above the frame setup belongs to the function.

        f:  jr $ra; nop
        g:  lui $v0, 0x21; addiu $sp, $sp, -0x10; lw $v1, 0($v0); ...; jr $ra
    """

    LUI_V0 = (0x0F << 26) | (2 << 16) | 0x21
    ADDIU_SP = (0x09 << 26) | (29 << 21) | (29 << 16) | 0xFFF0
    ADDIU_SP_BACK = (0x09 << 26) | (29 << 21) | (29 << 16) | 0x10
    LW_V1_V0 = (0x23 << 26) | (2 << 21) | (3 << 16)

    def run_on(self, words):
        a = analyzer(words)
        a.run()
        return sorted(a.functions)

    def test_the_function_starts_at_the_scheduled_instruction(self):
        words = [JR_RA, NOP, self.LUI_V0, self.ADDIU_SP, self.LW_V1_V0, JR_RA,
                 self.ADDIU_SP_BACK]
        self.assertIn(BASE + 8, self.run_on(words))
        self.assertNotIn(BASE + 12, self.run_on(words))

    def test_padding_may_come_between(self):
        words = [JR_RA, NOP, NOP, self.LUI_V0, self.ADDIU_SP, self.LW_V1_V0, JR_RA,
                 self.ADDIU_SP_BACK]
        self.assertIn(BASE + 12, self.run_on(words))
        self.assertNotIn(BASE + 16, self.run_on(words))

    def test_five_scheduled_instructions(self):
        # 0x001ADF00: sll, lui, addu, sll, sll, then the frame setup.
        SLL_V0 = (2 << 11) | (4 << 16) | (1 << 6)
        words = [JR_RA, NOP] + [SLL_V0] * 4 + [self.LUI_V0, self.ADDIU_SP, self.LW_V1_V0,
                                               JR_RA, self.ADDIU_SP_BACK]
        self.assertIn(BASE + 8, self.run_on(words))
        self.assertNotIn(BASE + 28, self.run_on(words))

    def test_a_branch_before_the_setup_is_not_taken_in(self):
        words = [JR_RA, NOP, B_FORWARD, self.ADDIU_SP, self.LW_V1_V0, JR_RA,
                 self.ADDIU_SP_BACK]
        self.assertNotIn(BASE + 8, self.run_on(words))


if __name__ == "__main__":
    unittest.main()
