#!/usr/bin/env python3
"""Find the vector units' microcode in a game's files, for compiling it ahead of time.

Microcode reaches a VU's instruction memory by the VIF's MPG command: a VIFcode
(command 0x4A in bits 24-30, NUM pairs in bits 16-23 - 0 meaning 256 -, the
load address in 8-byte units in bits 0-15) followed by the pairs, 64-bit
aligned. A game keeps those packets in its executable or its overlays and DMAs
them as they are, so the code a run will hold is on the disc (design 7d: code
from the disc, matched by its content - not only what a run happened to start).

What is taken for an MPG packet: the load fits the instruction memory, the
pairs have an E bit somewhere (a program ends) and no D or T bit (debug
breaks, which no shipped program has; random data has them at once). Packets
one after another (at most GAP bytes between - DMA tags, NOPs, the next
VIFcode) are one upload, an image, until one would load over pairs the upload
has loaded already (then a new upload starts: ZOE's VU0 packet follows its VU1
ones). An image that fits VU0's 4 KB could be for either unit, and is given for
both; the content check at run time (Vu::RunCompiled, Holds) picks.

    python engine/tools/vu_static.py FILE...      # prints the images found
"""

from __future__ import annotations

import struct
import sys

MPG = 0x4A
GAP = 64
VU0_SIZE = 0x1000
VU1_SIZE = 0x4000


def packets(data: bytes) -> list:
    """[(offset of the VIFcode, load address, [(lower, upper), ...])] in file order."""
    found = []
    for off in range(0, len(data) - 4, 4):
        word = struct.unpack_from("<I", data, off)[0]
        if (word >> 24) & 0x7F != MPG:
            continue
        num = (word >> 16) & 0xFF or 256
        addr = (word & 0xFFFF) * 8
        if addr + num * 8 > VU1_SIZE:
            continue
        start = off + 4
        while start % 8:
            start += 4
        if start + num * 8 > len(data):
            continue
        pairs = [struct.unpack_from("<II", data, start + 8 * i) for i in range(num)]
        if not any(upper & 0x40000000 for _, upper in pairs):
            continue
        if any(upper & 0x18000000 for _, upper in pairs):
            continue
        found.append((off, addr, pairs))
    return found


def uploads(data: bytes) -> list:
    """The images: [{pc: (lower, upper)}], each the packets of one upload."""
    images = []
    current, end = None, None
    for off, addr, pairs in packets(data):
        span = {addr + 8 * i for i in range(len(pairs))}
        if current is None or off - end > GAP or span & current.keys():
            current = {}
            images.append(current)
        for i, pair in enumerate(pairs):
            current[addr + 8 * i] = pair
        start = off + 4
        while start % 8:
            start += 4
        end = start + 8 * len(pairs)
    return images


def images_of_data(data: bytes, seen: set = None) -> list:
    """[(unit, {pc: (lower, upper)})] for every upload in one file's bytes (none already in `seen`)."""
    seen = set() if seen is None else seen
    out = []
    for image in uploads(data):
        units = (0, 1) if max(image) + 8 <= VU0_SIZE else (1,)
        for unit in units:
            key = (unit, tuple(sorted(image.items())))
            if key not in seen:
                seen.add(key)
                out.append((unit, image))
    return out


def images_of(paths: list) -> list:
    """[(unit, {pc: (lower, upper)})] for every upload in the files, without repeats."""
    out, seen = [], set()
    for path in paths:
        with open(path, "rb") as handle:
            out += images_of_data(handle.read(), seen)
    return out


def main(argv=None) -> int:
    paths = sys.argv[1:] if argv is None else argv
    for unit, image in images_of(paths):
        print("vu%d: %d pairs, 0x%04X-0x%04X" % (unit, len(image), min(image), max(image) + 8))
    return 0


if __name__ == "__main__":
    sys.exit(main())
