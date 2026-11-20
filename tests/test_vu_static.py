"""tools/vu_static.py: microcode found in a game's files by its MPG packets."""

import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
import vu_static  # noqa: E402

NOP = 0x8000033C            # lower NOP
E_NOP = 0x400002FF          # upper NOP with the E bit
UPPER_NOP = 0x000002FF


def mpg(addr: int, pairs: list) -> bytes:
    """An MPG packet as a DMA chain holds it: NOP, MPG VIFcode (the pairs 64-bit aligned after it), pairs."""
    words = [0, (vu_static.MPG << 24) | ((len(pairs) & 0xFF) << 16) | (addr // 8)]
    out = struct.pack("<II", *words)
    for lower, upper in pairs:
        out += struct.pack("<II", lower, upper)
    return out


def program(count: int) -> list:
    return [(NOP, UPPER_NOP)] * (count - 2) + [(NOP, E_NOP), (NOP, UPPER_NOP)]


class VuStatic(unittest.TestCase):
    def test_one_upload_of_two_packets(self):
        data = b"\0" * 64 + mpg(0x1000, program(4)) + mpg(0x1020, program(3)) + b"\0" * 64
        images = vu_static.images_of_data(data)
        self.assertEqual([(unit, sorted(image)) for unit, image in images],
                         [(1, [0x1000 + 8 * i for i in range(7)])])

    def test_an_upload_over_its_own_pairs_starts_another(self):
        # ZOE: the VU0 packet at 0x800 follows the VU1 uploads directly.
        data = mpg(0x0, program(4)) + mpg(0x1800, program(4)) + mpg(0x8, program(3))
        images = vu_static.images_of_data(data)
        self.assertEqual(len([i for u, i in images if u == 1]), 2)
        # The second fits VU0's memory: given for both units.
        self.assertIn((0, {0x8 + 8 * i: p for i, p in enumerate(program(3))}), images)

    def test_far_apart_packets_are_separate_uploads(self):
        data = mpg(0x0, program(4)) + b"\0" * 512 + mpg(0x1800, program(4))
        self.assertEqual(len([i for u, i in vu_static.images_of_data(data) if u == 1]), 2)

    def test_no_end_or_debug_bits_is_no_code(self):
        no_end = [(NOP, UPPER_NOP)] * 4
        debug = program(4)
        debug[0] = (NOP, UPPER_NOP | 0x10000000)        # D bit
        self.assertEqual(vu_static.images_of_data(mpg(0x0, no_end)), [])
        self.assertEqual(vu_static.images_of_data(mpg(0x0, debug)), [])

    def test_a_load_past_the_memory_is_no_packet(self):
        self.assertEqual(vu_static.images_of_data(mpg(0x3FF8, program(4))), [])


if __name__ == "__main__":
    unittest.main()
