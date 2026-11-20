"""tools/ioprecomp.py: what of an IOP module is code, what the loader rewrites."""

import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
import ioprecomp  # noqa: E402


def words(*values) -> bytes:
    return b"".join(struct.pack("<I", v) for v in values)


class IopRecomp(unittest.TestCase):
    def test_import_table_is_data_its_stubs_are_rewritten_words(self):
        # code, then an import table: magic, 0, version, name (8 bytes), two stubs, the closing 0.
        image = words(0x24020001, 0x03E00008, 0,
                      ioprecomp.IMPORT_MAGIC, 0, 0x0101, 0x6C647473, 0x00000000,   # "stdl"
                      ioprecomp.JR_RA, 0x24000004, ioprecomp.JR_RA, 0x24000007, 0)
        data, stubs = ioprecomp.tables(image)
        self.assertEqual(stubs, {32, 40})
        self.assertEqual(data, {12, 16, 20, 24, 28, 48})

    def test_export_table_is_data(self):
        image = words(0, ioprecomp.EXPORT_MAGIC, 0, 0x0101, 0x41414141, 0x41414141, 0x100, 0x200, 0, 0x24020001)
        data, stubs = ioprecomp.tables(image)
        self.assertEqual(stubs, set())
        self.assertEqual(data, {4, 8, 12, 16, 20, 24, 28, 32})

    def test_a_relocated_word_is_read_as_it_runs(self):
        lines = ioprecomp.body(0, 0x3C040000, True, False)       # lui $a0, %hi(...)
        self.assertIn("vm.Read(pc, 4)", " ".join(lines))
        self.assertIn("r[4] = imm << 16;", lines)
        lines = ioprecomp.body(0, 0x0C000040, True, False)       # jal (R_MIPS_26)
        self.assertIn("npc = (pc & 0xF0000000u) | ((w & 0x03FFFFFFu) << 2);", lines)
        lines = ioprecomp.body(0, 0x3C040012, False, False)
        self.assertNotIn("vm.Read(pc, 4)", " ".join(lines))
        self.assertIn("r[4] = 0x12u << 16;", lines)

    def test_functions_cover_runs_of_at_most_run_words(self):
        offsets = list(range(0, 4 * (ioprecomp.RUN + 3), 4)) + [0x1000]
        runs = ioprecomp.runs(offsets)
        self.assertEqual([len(r) for r in runs], [ioprecomp.RUN, 3, 1])

    def test_fnv1a64(self):
        self.assertEqual(ioprecomp.fnv1a64(b""), 0xCBF29CE484222325)
        self.assertEqual(ioprecomp.fnv1a64(b"a"), 0xAF63DC4C8601EC8C)


if __name__ == "__main__":
    unittest.main()
