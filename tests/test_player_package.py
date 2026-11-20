"""The player package's pieces that hold no game data: microprograms as references into the disc's files (and
read back), the strings read back by address, the release script's build.ninja reading."""
import os
import struct
import sys
import tempfile
import types
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))

import player_build  # noqa: E402
import player_package  # noqa: E402
import player_release  # noqa: E402


class TestPieces(unittest.TestCase):
    def test_whole_run_in_one_file(self):
        blob = b"\0" * 16 + bytes(range(64))
        self.assertEqual(player_package._find_pieces(bytes(range(8, 40)), {"a": blob}), [("a", 24, 4)])

    def test_run_split_by_a_tag(self):
        # the recorded run is the file's pairs 0-1 and 3-4: pair 2 (a VIF tag in the file) is not in it
        pairs = [struct.pack("<2I", i, 0x100 + i) for i in range(5)]
        blob = b"".join(pairs)
        data = pairs[0] + pairs[1] + pairs[3] + pairs[4]
        self.assertEqual(player_package._find_pieces(data, {"a": blob}), [("a", 0, 2), ("a", 24, 2)])

    def test_nowhere(self):
        self.assertIsNone(player_package._find_pieces(b"\1" * 8, {"a": b"\0" * 64}))


class TestRefsRoundTrip(unittest.TestCase):
    def test_manifest_from_references_is_the_recorded_one(self):
        with tempfile.TemporaryDirectory() as d:
            os.makedirs(os.path.join(d, "extracted"))
            os.makedirs(os.path.join(d, "build"))
            words = [0x1000 + i for i in range(12)]
            with open(os.path.join(d, "extracted", "GAME.ELF"), "wb") as f:
                f.write(b"\xEE" * 40 + struct.pack("<6I", *words[:6]) + b"\xAA" * 8 + struct.pack("<6I", *words[6:]))
            manifest = os.path.join(d, "build", "vu_programs.txt")
            line = "vu1 10 10:" + ",".join("%X" % w for w in words)
            with open(manifest, "w") as f:
                f.write(line + "\n")
            game = types.SimpleNamespace(directory=d, extracted="extracted", executable="GAME.ELF", build="build",
                                         path=lambda rel: os.path.normpath(os.path.join(d, rel)))
            refs = os.path.join(d, "build", "vu_refs.txt")
            self.assertEqual(player_package.vu_refs(game, manifest, refs), 1)
            text = open(refs).read()
            self.assertEqual(text.strip(), "vu1 10 10:extracted/GAME.ELF+28*3,extracted/GAME.ELF+48*3")
            os.remove(manifest)
            self.assertEqual(player_build.resolve_vu(game), 1)
            self.assertEqual(open(manifest).read().strip(), line)


class TestStrings(unittest.TestCase):
    def test_text_read_back_to_the_nul(self):
        sections = [(b"xx" + b"Hello\0World\0", 0x1000)]
        self.assertEqual(player_build._read_string(sections, 0x1002), "Hello")
        self.assertEqual(player_build._read_string(sections, 0x1008), "World")
        self.assertIsNone(player_build._read_string(sections, 0x2000))


class TestNinja(unittest.TestCase):
    def test_block_and_split(self):
        text = ("build a/generated/dispatch.cpp.obj: CXX x.cpp || dep\n"
                '  DEFINES = -DNAME="\\"Zone of\\"" -DS=\\"A.B\\" -DX=1\n'
                "  FLAGS = -O3 $\n    -mavx2\n"
                "  INCLUDES = -ID:/e/host -ID:/e/rt\n\n")
        block = player_release.ninja_block(text, "/generated/dispatch.cpp.obj")
        self.assertEqual(player_release.ninja_split(block["FLAGS"]), ["-O3", "-mavx2"])
        self.assertEqual(player_release.ninja_split(block["DEFINES"]), ['-DNAME="Zone of"', '-DS="A.B"', "-DX=1"])
        self.assertEqual(player_release.ninja_split(block["INCLUDES"]), ["-ID:/e/host", "-ID:/e/rt"])


if __name__ == "__main__":
    unittest.main()
