"""Option P: derive's emission budget (derive.MAX_EMIT_TREE).

The emitter prints every expression tree in full, so a few many-path functions grew
ZOE PAL's emission past 19 GB. render_body now refuses a derivation whose summed
tree size reaches the bound; emit() then leaves it out and calls it as the original.
"""

import os
import struct
import sys
import unittest

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
sys.path[:0] = [ROOT, os.path.join(ROOT, "tools")]

import derive  # noqa: E402
from test_path_caps import BASE, six_flags  # noqa: E402


def sixty_four_paths():
    words = six_flags()
    image = struct.pack("<%dI" % len(words), *words)
    func = {"addr": BASE, "end": BASE + 4 * len(words), "size": 4 * len(words)}
    return derive.derive_function(image, BASE, func, {}, None)


class TestEmitBudget(unittest.TestCase):
    def setUp(self):
        self.saved = derive.MAX_EMIT_TREE

    def tearDown(self):
        derive._set_max_emit_tree(self.saved)

    def test_size_counts_every_path(self):
        derivation = sixty_four_paths()
        size = derive.derivation_tree_size(derivation)
        self.assertGreater(size, len(derivation.paths))

    def test_below_the_bound_renders(self):
        derivation = sixty_four_paths()
        derive._set_max_emit_tree(derive.derivation_tree_size(derivation) + 1)
        self.assertTrue(derive.render_body(derivation))

    def test_at_the_bound_is_refused(self):
        derivation = sixty_four_paths()
        derive._set_max_emit_tree(derive.derivation_tree_size(derivation))
        with self.assertRaises(derive.Unsupported) as caught:
            derive.render_body(derivation)
        self.assertIn("too large to emit", str(caught.exception))

    def test_zero_means_no_bound(self):
        derivation = sixty_four_paths()
        derive._set_max_emit_tree(0)
        self.assertTrue(derive.render_body(derivation))


if __name__ == "__main__":
    unittest.main()
