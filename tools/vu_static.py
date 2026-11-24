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

An executable built with the DVP linker lists its uploads itself: its
.DVP.ovlytab section (12 bytes an entry: the name's offset in .DVP.ovlystrtab,
the LMA - where the packet's pairs lie in the loaded program, its MPG VIFcode
just before -, the VU address). Those packets are taken as listed, without the
E / D / T test: a big program goes up as 256-pair packets, most of them without
an E bit of their own (God of War II: three VU1 programs of 6, 5 and 7 packets,
one with no E bit at all - P1 2026-11-24), and real code can carry a D or T
bit. The table's .DVP.overlay.* sections are not used: they lie outside the
load segment and differ from the loaded bytes. The content scan still runs for
the rest of the file (and for files without the table), past the listed
packets' bytes - so it adds no pieces of them.

    python engine/tools/vu_static.py FILE...      # prints the images found
"""

from __future__ import annotations

import struct
import sys

MPG = 0x4A
GAP = 64
VU0_SIZE = 0x1000
VU1_SIZE = 0x4000


def _sections(data: bytes) -> dict:
    """An ELF's sections by name: {name: (offset, size)}; {} for anything else."""
    if len(data) < 0x34 or data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:     # 32-bit little-endian only
        return {}
    shoff = struct.unpack_from("<I", data, 0x20)[0]
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x2E)
    if not shoff or shentsize < 40 or shstrndx >= shnum or shoff + shnum * shentsize > len(data):
        return {}
    heads = [struct.unpack_from("<IIIIII", data, shoff + i * shentsize) for i in range(shnum)]
    names = heads[shstrndx][4]
    out = {}
    for name, _, _, _, offset, size in heads:
        end = data.find(b"\0", names + name)
        if end > 0:
            out[data[names + name:end].decode("latin-1")] = (offset, size)
    return out


def _file_offset(data: bytes, address: int) -> int:
    """Where a loaded address's byte is in the ELF file (its PT_LOAD segments), or -1."""
    phoff = struct.unpack_from("<I", data, 0x1C)[0]
    phentsize, phnum = struct.unpack_from("<HH", data, 0x2A)
    for i in range(phnum):
        kind, offset, vaddr, _, filesz = struct.unpack_from("<IIIII", data, phoff + i * phentsize)
        if kind == 1 and vaddr <= address < vaddr + filesz:
            return offset + address - vaddr
    return -1


def listed_packets(data: bytes) -> list:
    """The MPG packets an ELF's .DVP.ovlytab lists: [(offset of the VIFcode, load address, pairs)]; [] without one.
    An entry whose bytes are not an MPG packet loading at its VU address is left out."""
    table = _sections(data).get(".DVP.ovlytab")
    if not table:
        return []
    found = []
    offset, size = table
    for at in range(offset, offset + size - 11, 12):
        _, lma, vu_address = struct.unpack_from("<III", data, at)
        start = _file_offset(data, lma)
        if start < 4 or start % 8:
            continue
        word = struct.unpack_from("<I", data, start - 4)[0]
        num = (word >> 16) & 0xFF or 256
        addr = (word & 0xFFFF) * 8
        if (word >> 24) & 0x7F != MPG or addr != vu_address or addr + num * 8 > VU1_SIZE or \
                start + num * 8 > len(data):
            continue
        found.append((start - 4, addr, [struct.unpack_from("<II", data, start + 8 * i) for i in range(num)]))
    return found


def packets(data: bytes) -> list:
    """[(offset of the VIFcode, load address, [(lower, upper), ...])] in file order: the packets the ELF's
    .DVP.ovlytab lists, and the ones the content scan finds elsewhere."""
    listed = listed_packets(data)
    taken = set()
    for off, _, pairs in listed:
        taken.update(range(off, off + 4 + 8 * len(pairs), 4))      # the VIFcode and its pairs (8-aligned after it)
    found = list(listed)
    for off in range(0, len(data) - 4, 4):
        if off in taken:
            continue
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
    return sorted(found, key=lambda packet: packet[0])


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
