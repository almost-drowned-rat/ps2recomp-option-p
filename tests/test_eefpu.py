"""ps2decomp/eefpu.py against results from a real console (ps2autotests cpu/ee_fpu *.expected, ISC licence,
ps2autotests @97469ff) - the cases whose result is exact, so the rounding
mode does not matter."""

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

from ps2decomp import eefpu

ONE, NEGONE, MAX, MIN = 0x3F800000, 0xBF800000, 0x7FFFFFFF, 0xFFFFFFFF


class TestConsoleValues(unittest.TestCase):
    def test_exponent_255_is_a_number(self):
        self.assertEqual(eefpu.add(0x00000000, 0x7F800000), 0x7F800000)
        self.assertEqual(eefpu.add(MAX, 0x00000000), MAX)
        self.assertEqual(eefpu.sub(ONE, 0x7F800001), 0xFF800001)
        self.assertEqual(eefpu.mul(0x7F800001, ONE), 0x7F800001)

    def test_saturation_at_7fffffff(self):
        self.assertEqual(eefpu.add(MAX, MAX), MAX)
        self.assertEqual(eefpu.add(MIN, MIN), MIN)
        self.assertEqual(eefpu.add(0x7F800000, 0x7F800000), MAX)
        self.assertEqual(eefpu.mul(0xFF800000, 0x7F800000), MIN)

    def test_division_by_zero(self):
        self.assertEqual(eefpu.div(0x00000000, 0x00000000), MAX)
        self.assertEqual(eefpu.div(0x00000000, 0x80000000), MIN)
        self.assertEqual(eefpu.div(0x80000000, 0x80000000), MAX)
        self.assertEqual(eefpu.div(ONE, 0x00000000), MAX)

    def test_rsqrt(self):
        self.assertEqual(eefpu.rsqrt(0x80000000, 0x00000000), MIN)
        self.assertEqual(eefpu.rsqrt(ONE, NEGONE), ONE)
        self.assertEqual(eefpu.rsqrt(0x7F800000, 0x7F800000), 0x5F800000)

    def test_sqrt_of_the_magnitude(self):
        self.assertEqual(eefpu.sqrt(NEGONE), ONE)
        self.assertEqual(eefpu.sqrt(0x80000000), 0x00000000)
        self.assertEqual(eefpu.sqrt(0xFF800000), 0x5F800000)

    def test_bit_operations(self):
        self.assertEqual(eefpu.fabs(0xFFFFFFFF), MAX)
        self.assertEqual(eefpu.fabs(0xFF800000), 0x7F800000)
        self.assertEqual(eefpu.neg(MAX), MIN)
        self.assertEqual(eefpu.move(0x00001337), 0x00001337)

    def test_min_max_sign_magnitude(self):
        self.assertEqual(eefpu.fmax(MAX, MIN), MAX)
        self.assertEqual(eefpu.fmin(MAX, MIN), MIN)
        self.assertEqual(eefpu.fmin(0x00000000, 0x80000000), 0x80000000)
        self.assertEqual(eefpu.fmax(0x00000000, 0x80000000), 0x00000000)
        self.assertEqual(eefpu.fmax(0x00001337, 0xDEADBEEF), 0x00001337)
        self.assertEqual(eefpu.fmax(0x7F800001, ONE), 0x7F800001)

    def test_compare(self):
        self.assertFalse(eefpu.compare("eq", 0x7F800000, 0x7F800001))
        self.assertTrue(eefpu.compare("lt", 0x7F800000, 0x7F800001))
        self.assertFalse(eefpu.compare("le", 0x7F800001, 0x7F800000))
        self.assertTrue(eefpu.compare("eq", 0x00000000, 0x80000000))

    def test_division_and_roots_round_as_the_console(self):
        # div and sqrt to nearest; rsqrt = the root to nearest, its reciprocal to nearest, the product truncated.
        self.assertEqual(eefpu.div(ONE, 0x3FFFFFFF), 0x3F000001)
        self.assertEqual(eefpu.sqrt(0x3FFFFFFF), 0x3FB504F3)
        self.assertEqual(eefpu.sqrt(MAX), 0x5FB504F3)
        self.assertEqual(eefpu.sqrt(0xDEADBEEF), 0x4F152108)
        self.assertEqual(eefpu.rsqrt(ONE, 0x3FFFFFFF), 0x3F3504F3)
        self.assertEqual(eefpu.rsqrt(MAX, MAX), 0x5FB504F2)
        self.assertEqual(eefpu.rsqrt(MIN, MIN), 0xDFB504F2)
        self.assertEqual(eefpu.rsqrt(0x40400000, 0x40400000), 0x3FDDB3D7)
        self.assertEqual(eefpu.rsqrt(ONE, 0x7F800001), 0x1F800000)
        self.assertEqual(eefpu.rsqrt(ONE, 0x41C80000), 0x3E4CCCCD)     # 1/sqrt(25): a quotient truncated misses
        self.assertEqual(eefpu.rsqrt(0x40A00000, 0x41C80000), ONE)

    def test_control_registers(self):
        self.assertEqual(eefpu.read_fcr(0, False), 0x2E30)
        self.assertEqual(eefpu.read_fcr(15, True), 0x2E30)
        self.assertEqual(eefpu.read_fcr(31, False), 0x01000001)
        self.assertEqual(eefpu.read_fcr(16, True), 0x01800001)


if __name__ == "__main__":
    unittest.main()
