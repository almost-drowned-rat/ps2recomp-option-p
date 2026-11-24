#!/usr/bin/env python3
"""Compile a game's IOP modules (IRX files) to C++ ahead of time.

The IOP interpreter (engine/host/src/iop_vm.cpp, Execute) runs a module's R3000
code an instruction at a time. This turns every code word of a module into C++
that does exactly what the interpreter does there - the same register and memory
effects, the same count of instructions against the same limit, the same
library calls - so the module's threads, the scheduler's quanta and the
interrupts between them come out as they did (the tests compare the two).

* Every code word is an entry: a function covers a run of up to RUN words, a
  `switch` enters it at any of them (a thread resumes where its quantum ended,
  a delay slot included: the address after it is the context's npc).
* The module is compiled from its file, linked at 0; it runs at the base the
  loader gives it. Words the loader rewrites are read from memory as they run:
  relocated immediates and jump targets (R_MIPS_26, HI16, LO16) and the import
  stubs (a `syscall` into a host library, or a `j` into another module).
* Import and export tables are data and get no entry; neither does a word with
  a whole-word relocation (R_MIPS_32: a pointer, not code).
* A module is taken at run time only if its file is byte for byte the one
  compiled (size + FNV-1a 64, LoadModule): code from the disc, by content.

    python engine/tools/ioprecomp.py --out-dir build/port/generated FILE.IRX...
    python engine/tools/ioprecomp.py --corpus --out host/tests/iop_corpus.cpp
"""

from __future__ import annotations

import argparse
import os
import random
import struct
import sys

RUN = 64                    # words a function covers
LINES_PER_FILE = 6000

R_MIPS_32, R_MIPS_26, R_MIPS_HI16, R_MIPS_LO16 = 2, 4, 5, 6
IMPORT_MAGIC, EXPORT_MAGIC = 0x41E00000, 0x41C00000
JR_RA = 0x03E00008


def u32(data: bytes, off: int) -> int:
    return struct.unpack_from("<I", data, off)[0]


def u16(data: bytes, off: int) -> int:
    return struct.unpack_from("<H", data, off)[0]


def fnv1a64(data: bytes) -> int:
    h = 0xCBF29CE484222325
    for b in data:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


# ---------------------------------------------------------------- the module

def parse_irx(data: bytes) -> dict:
    """The loader's view (iop_vm.cpp LoadModule): the loadable segment, the code
    sections, the relocated words by type."""
    if len(data) < 0x34 or u32(data, 0) != 0x464C457F:
        raise ValueError("not an ELF file")
    phoff, shoff = u32(data, 0x1C), u32(data, 0x20)
    phentsize, phnum = u16(data, 0x2A), u16(data, 0x2C)
    shentsize, shnum = u16(data, 0x2E), u16(data, 0x30)
    segment = None
    for i in range(phnum):
        ph = phoff + i * phentsize
        if u32(data, ph) == 1:
            segment = (u32(data, ph + 4), u32(data, ph + 16), u32(data, ph + 20))
            break
    if segment is None:
        raise ValueError("no loadable segment")
    offset, filesz, memsz = segment
    image = data[offset:offset + filesz]
    code = []
    relocs = {}
    for i in range(shnum):
        sh = shoff + i * shentsize
        kind, flags, addr, size = u32(data, sh + 4), u32(data, sh + 8), u32(data, sh + 12), u32(data, sh + 20)
        if kind == 1 and flags & 4 and size:
            code.append((addr, addr + size))
        if kind == 9:
            rel = u32(data, sh + 16)
            for k in range(0, size - size % 8, 8):
                where, typ = u32(data, rel + k), u32(data, rel + k + 4) & 0xFF
                if typ:
                    relocs[where] = typ
    return {"image": image, "filesz": filesz, "memsz": memsz, "code": code, "relocs": relocs}


def tables(image: bytes) -> tuple:
    """(data words of import/export tables, import stub words) - as the loader scans for them."""
    data, stubs = set(), set()
    for a in range(0, len(image) - 20 + 1, 4):
        word = u32(image, a)
        if word == IMPORT_MAGIC:
            data.update(range(a, a + 20, 4))
            stub = a + 20
            while stub + 8 <= len(image) and u32(image, stub) == JR_RA and u32(image, stub + 4) >> 16 == 0x2400:
                stubs.add(stub)
                stub += 8
            if stub + 4 <= len(image) and u32(image, stub) == 0:
                data.add(stub)                  # the table's closing 0
        elif word == EXPORT_MAGIC:
            end = a + 20
            while end + 4 <= len(image) and u32(image, end) != 0:
                end += 4
            data.update(range(a, end + 4, 4))
    return data, stubs


# ---------------------------------------------------------------- one instruction

def sx16(v: int) -> int:
    return v - 0x10000 if v & 0x8000 else v


def body(off: int, w: int, relocated: bool, stub: bool) -> list:
    """C++ for the word at `off`, as Execute runs it. In scope: vm, c, r (c.r),
    pc (base + off), at (where the next instruction is), npc (where the one after
    goes; a branch sets it), out. Ends the function itself where Execute would
    return or the interpreter's run would leave (a library call)."""
    if stub:
        return ["const u32 w = vm.Read(pc, 4);",
                "if ((w >> 26) == 0 && (w & 0x3F) == 0x0C) {",
                "    c.pc = at; c.npc = npc;",
                "    if (LibraryCall(vm, c, pc, w, &out)) return true;",
                "    return false;",
                "}",
                "if ((w >> 26) == 2) npc = (pc & 0xF0000000u) | ((w & 0x03FFFFFFu) << 2);",
                "else if (w == 0x03E00008u) npc = r[31];",
                "else IopStop(\"an import stub changed at \" + std::to_string(pc));"]
    if relocated:
        # The loader's relocation changed the immediate (or the jump target): read as it is now.
        return ["const u32 w = vm.Read(pc, 4), imm = w & 0xFFFF; const s32 simm = Sext16(imm);",
                "(void)w; (void)imm; (void)simm;"] + _body(w, "imm", "u32(simm)", True)
    imm = w & 0xFFFF
    return _body(w, "0x%Xu" % imm, "0x%Xu" % (sx16(imm) & 0xFFFFFFFF), False)


def _body(w: int, IMM: str, SIMM: str, relocated: bool) -> list:
    op, rs, rt, rd = w >> 26, (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
    sa = (w >> 6) & 31

    def set_(reg, value):
        return ["r[%d] = %s;" % (reg, value)] if reg else ["(void)(%s);" % value]

    def stop(what):
        return ['IopStop("%s at " + std::to_string(pc));' % what]

    if op == 0x00:
        f = w & 0x3F
        if f == 0x00: return set_(rd, "r[%d] << %d" % (rt, sa))
        if f == 0x02: return set_(rd, "r[%d] >> %d" % (rt, sa))
        if f == 0x03: return set_(rd, "u32(s32(r[%d]) >> %d)" % (rt, sa))
        if f == 0x04: return set_(rd, "r[%d] << (r[%d] & 31)" % (rt, rs))
        if f == 0x06: return set_(rd, "r[%d] >> (r[%d] & 31)" % (rt, rs))
        if f == 0x07: return set_(rd, "u32(s32(r[%d]) >> (r[%d] & 31))" % (rt, rs))
        if f == 0x08: return ["npc = r[%d];" % rs]
        if f == 0x09:
            return ["{ const u32 t = r[%d];" % rs] + ["  " + x for x in set_(rd, "pc + 8")] + \
                   ["  npc = t;", "  if (vm.trace) { auto& e = vm.calls[t]; ++e.first; e.second = pc; } }"]
        if f == 0x0C:
            return ["c.pc = at; c.npc = npc;",
                    "if (LibraryCall(vm, c, pc, 0x%08Xu, &out)) return true;" % w,
                    "return false;"]
        if f == 0x0D: return stop("break")
        if f == 0x10: return set_(rd, "c.hi")
        if f == 0x11: return ["c.hi = r[%d];" % rs]
        if f == 0x12: return set_(rd, "c.lo")
        if f == 0x13: return ["c.lo = r[%d];" % rs]
        if f == 0x18:
            return ["{ const s64 p = s64(s32(r[%d])) * s64(s32(r[%d])); c.lo = u32(p); c.hi = u32(u64(p) >> 32); }"
                    % (rs, rt)]
        if f == 0x19:
            return ["{ const u64 p = u64(r[%d]) * u64(r[%d]); c.lo = u32(p); c.hi = u32(p >> 32); }" % (rs, rt)]
        if f == 0x1A:
            return ["{ const s32 a = s32(r[%d]), b = s32(r[%d]);" % (rs, rt),
                    "  if (b == 0) { c.lo = a < 0 ? 1u : 0xFFFFFFFFu; c.hi = u32(a); }",
                    "  else if (a == INT32_MIN && b == -1) { c.lo = u32(a); c.hi = 0; }",
                    "  else { c.lo = u32(a / b); c.hi = u32(a % b); } }"]
        if f == 0x1B:
            return ["{ const u32 a = r[%d], b = r[%d];" % (rs, rt),
                    "  if (b == 0) { c.lo = 0xFFFFFFFFu; c.hi = a; } else { c.lo = a / b; c.hi = a % b; } }"]
        if f in (0x20, 0x21): return set_(rd, "r[%d] + r[%d]" % (rs, rt))
        if f in (0x22, 0x23): return set_(rd, "r[%d] - r[%d]" % (rs, rt))
        if f == 0x24: return set_(rd, "r[%d] & r[%d]" % (rs, rt))
        if f == 0x25: return set_(rd, "r[%d] | r[%d]" % (rs, rt))
        if f == 0x26: return set_(rd, "r[%d] ^ r[%d]" % (rs, rt))
        if f == 0x27: return set_(rd, "~(r[%d] | r[%d])" % (rs, rt))
        if f == 0x2A: return set_(rd, "s32(r[%d]) < s32(r[%d]) ? 1u : 0u" % (rs, rt))
        if f == 0x2B: return set_(rd, "r[%d] < r[%d] ? 1u : 0u" % (rs, rt))
        return stop("instruction 0x%08X" % w)
    if op == 0x01:
        link = (rt & 0x1E) == 0x10
        cond = ("s32(r[%d]) >= 0" if rt & 1 else "s32(r[%d]) < 0") % rs
        # The condition reads rs before the link writes $ra (as the interpreter: taken first).
        lines = ["{ const bool taken = %s;" % cond]
        if link:
            lines.append("  r[31] = pc + 8;")
        lines.append("  if (taken) npc = pc + 4 + (%s << 2); }" % SIMM)
        return lines
    if op in (0x02, 0x03):
        target = "(pc & 0xF0000000u) | ((%s & 0x03FFFFFFu) << 2)" % ("w" if relocated else "0x%08Xu" % w)
        if op == 0x02:
            return ["npc = %s;" % target]
        return ["r[31] = pc + 8;", "npc = %s;" % target,
                "if (vm.trace) { auto& e = vm.calls[npc]; ++e.first; e.second = pc; }"]
    if op in (0x04, 0x05, 0x06, 0x07):
        cond = {4: "r[%d] == r[%d]" % (rs, rt), 5: "r[%d] != r[%d]" % (rs, rt),
                6: "s32(r[%d]) <= 0" % rs, 7: "s32(r[%d]) > 0" % rs}[op]
        return ["if (%s) npc = pc + 4 + (%s << 2);" % (cond, SIMM)]
    if op in (0x08, 0x09): return set_(rt, "r[%d] + %s" % (rs, SIMM))
    if op == 0x0A: return set_(rt, "s32(r[%d]) < s32(%s) ? 1u : 0u" % (rs, SIMM))
    if op == 0x0B: return set_(rt, "r[%d] < %s ? 1u : 0u" % (rs, SIMM))
    if op == 0x0C: return set_(rt, "r[%d] & %s" % (rs, IMM))
    if op == 0x0D: return set_(rt, "r[%d] | %s" % (rs, IMM))
    if op == 0x0E: return set_(rt, "r[%d] ^ %s" % (rs, IMM))
    if op == 0x0F: return set_(rt, "%s << 16" % IMM)
    if op == 0x10:
        if rs == 0x00: return set_(rt, "0x401u" if rd == 12 else "0u")
        if rs == 0x04: return []
        if rs == 0x10 and (w & 0x3F) == 0x10: return []
        return stop("COP0 instruction 0x%08X" % w)
    addr = "r[%d] + %s" % (rs, SIMM)
    if op == 0x20: return set_(rt, "u32(s32(s8(vm.Read(%s, 1))))" % addr)
    if op == 0x21: return set_(rt, "u32(Sext16(vm.Read(%s, 2)))" % addr)
    if op in (0x22, 0x26):
        lines = ["{ const u32 a = %s, word = vm.Read(a & ~3u, 4), k = a & 3; (void)word; (void)k;" % addr]
        if op == 0x22:
            value = "(r[%d] & (0x00FFFFFFu >> (8 * k))) | (word << (24 - 8 * k))" % rt
        else:
            value = "(r[%d] & ~(0xFFFFFFFFu >> (8 * k))) | (word >> (8 * k))" % rt
        return lines + ["  " + x for x in set_(rt, value)] + ["}"]
    if op == 0x23: return set_(rt, "vm.Read(%s, 4)" % addr)
    if op == 0x24: return set_(rt, "vm.Read(%s, 1)" % addr)
    if op == 0x25: return set_(rt, "vm.Read(%s, 2)" % addr)
    if op == 0x28: return ["vm.Write(%s, r[%d], 1);" % (addr, rt)]
    if op == 0x29: return ["vm.Write(%s, r[%d], 2);" % (addr, rt)]
    if op in (0x2A, 0x2E):
        if op == 0x2A:
            value = "(word & ~(0xFFFFFFFFu >> (24 - 8 * k))) | (r[%d] >> (24 - 8 * k))" % rt
        else:
            value = "(word & ~(0xFFFFFFFFu << (8 * k))) | (r[%d] << (8 * k))" % rt
        return ["{ const u32 a = %s, word = vm.Read(a & ~3u, 4), k = a & 3;" % addr,
                "  vm.Write(a & ~3u, %s, 4); }" % value]
    if op == 0x2B: return ["vm.Write(%s, r[%d], 4);" % (addr, rt)]
    return stop("instruction 0x%08X" % w)


# ---------------------------------------------------------------- functions

def runs(words: list) -> list:
    """Consecutive entry offsets, cut into functions of at most RUN words."""
    out = []
    for off in sorted(words):
        if out and out[-1][-1] + 4 == off and len(out[-1]) < RUN:
            out[-1].append(off)
        else:
            out.append([off])
    return out


def emit_function(name: str, offsets: list, image: bytes, relocated: set, stubs: set) -> list:
    first = offsets[0]
    lines = ["bool %s(Vm& vm, Context& c, u32 base, u64 limit, u64& n, Outcome& out) {" % name,
             "    u32* r = c.r;",
             "    u32 npc = c.npc;",
             "    switch ((c.pc - base - 0x%Xu) >> 2) {" % first]
    for i, off in enumerate(offsets):
        lines.append("        case %d: goto L%X;" % (i, off))
    lines += ["        default: return false;", "    }"]
    for off in offsets:
        w = u32(image, off)
        lines += ["L%X: {" % off,
                  "    if (n >= limit) { c.pc = base + 0x%Xu; c.npc = npc; out = Outcome::Return; return true; }" % off,
                  "    ++n; ++vm.instructions;",
                  "    const u32 pc = base + 0x%Xu; const u32 at = npc; npc = at + 4; vm.executing = pc;" % off]
        lines += ["    " + x for x in body(off, w, off in relocated, off in stubs)]
        lines += ["    if (at != pc + 4) { c.pc = at; c.npc = npc; return false; }", "}"]
    last = offsets[-1]
    lines += ["    c.pc = base + 0x%Xu; c.npc = npc;" % (last + 4), "    return false;", "}"]
    return lines


HEADER = """/* Generated by engine/tools/ioprecomp.py: %s. Do not edit. */

#include <climits>
#include <cstdint>
#include <string>

#include "iop_vm_internal.h"

namespace iop_generated {

using namespace ps2host;
using namespace ps2host::iop;
"""

FOOTER = """
}  // namespace iop_generated
"""


def module_symbol(name: str) -> str:
    # A C++ name must not start with a digit: God of War II's 989NOMID.IRX becomes m989NOMID_IRX (names that start
    # with a letter, every module before it, stay as they were).
    symbol = "".join(ch if ch.isalnum() else "_" for ch in name)
    return "m" + symbol if symbol[:1].isdigit() else symbol


def generate(modules: list, what: str, single: str = None) -> dict:
    """modules: [(name, file bytes, code offsets, image, relocated word offsets, stub word offsets)]
    -> {file name: C++}."""
    chunks = [[]]
    tables = []
    declarations = []
    registrations = []
    for name, data, offsets, image, relocated, stubs in modules:
        symbol = module_symbol(name)
        entries = []
        for run in runs(offsets):
            fn = "%s_%X" % (symbol, run[0])
            code = emit_function(fn, run, image, relocated, stubs)
            if single is None and len(chunks[-1]) + len(code) > LINES_PER_FILE and chunks[-1]:
                chunks.append([])
            chunks[-1] += code + [""]
            declarations.append("bool %s(Vm& vm, Context& c, u32 base, u64 limit, u64& n, Outcome& out);" % fn)
            entries += [(off, fn) for off in run]
        tables.append("const u32 k%sOffsets[] = {" % symbol)
        for i in range(0, len(entries), 10):
            tables.append("    " + ", ".join("0x%Xu" % off for off, _ in entries[i:i + 10]) + ",")
        tables.append("};")
        tables.append("const CompiledStep k%sSteps[] = {" % symbol)
        for i in range(0, len(entries), 6):
            tables.append("    " + ", ".join(fn for _, fn in entries[i:i + 6]) + ",")
        tables.append("};")
        registrations.append('    {"%s", %du, 0x%016Xull, %du, k%sOffsets, k%sSteps},' %
                             (name, len(data), fnv1a64(data), len(entries), symbol, symbol))
    main = [HEADER % what] + declarations + [""]
    if single is not None:
        main += chunks[0]
    main += tables
    main.append("const CompiledModule kModules[] = {")
    main += registrations or ['    {"", 0u, 0ull, 0u, nullptr, nullptr},']
    main.append("};")
    main += ["", "struct Registration {", "    Registration() {",
             "        for (const CompiledModule& m : kModules)",
             "            if (m.count) RegisterIopModule(&m);", "    }", "} registration;", FOOTER,
             "/* The modules, for the tests. */",
             "const ps2host::iop::CompiledModule* IopGeneratedModules(std::size_t* count) {",
             "    *count = sizeof iop_generated::kModules / sizeof iop_generated::kModules[0];",
             "    return iop_generated::kModules;", "}"]
    if single is not None:
        # The tests' corpus: the code itself too, for the interpreter's side.
        for k, (_, data, _, _, _, _) in enumerate(modules):
            words = struct.unpack("<%dI" % (len(data) // 4), data[:len(data) // 4 * 4])
            main.append("static const std::uint32_t kImage%d[] = {%s};" % (k, ", ".join("0x%08Xu" % x for x in words)))
        main.append("const std::uint32_t* IopGeneratedImage(std::size_t i, std::size_t* words) {")
        main.append("    static const std::uint32_t* const images[] = {%s};" %
                    ", ".join("kImage%d" % k for k in range(len(modules))))
        main.append("    static const std::size_t sizes[] = {%s};" %
                    ", ".join("%du" % (len(m[1]) // 4) for m in modules))
        main.append("    *words = sizes[i];")
        main.append("    return images[i];")
        main.append("}")
    files = {single or "iop_modules.cpp": "\n".join(main) + "\n"}
    if single is None:
        for i, chunk in enumerate(chunks):
            if chunk:
                files["iop_code_%d.cpp" % i] = "\n".join([HEADER % what] + chunk + [FOOTER])
    return files


def module_of(name: str, data: bytes) -> tuple:
    m = parse_irx(data)
    image = m["image"]
    data_words, stubs = tables(image)
    # Words a whole-word relocation rewrites hold a pointer: data, no entry (the interpreter, if ever run).
    pointers = {off for off, typ in m["relocs"].items() if typ == R_MIPS_32}
    relocated = {off for off, typ in m["relocs"].items() if typ in (R_MIPS_26, R_MIPS_HI16, R_MIPS_LO16)}
    offsets = []
    for lo, hi in m["code"]:
        for off in range(lo & ~3, min(hi, len(image)), 4):
            if off in data_words or off in pointers:
                continue
            offsets.append(off)
    return (name, data, offsets, image, relocated, stubs)


# ---------------------------------------------------------------- the test corpus

def random_word(rng: random.Random, i: int, count: int) -> int:
    """Random MIPS I code over $8-$15 and a memory window at $9 (the tests point it there): ALU, shifts, multiply
    and divide, HI/LO, loads/stores, branches within the program (forward mostly)."""
    rs, rt, rd = rng.randrange(8, 16), rng.randrange(8, 16), rng.choice([0] + list(range(10, 16)))
    roll = rng.random()
    if roll < 0.35:
        f = rng.choice([0x00, 0x02, 0x03, 0x04, 0x06, 0x07, 0x10, 0x12, 0x11, 0x13, 0x18, 0x19, 0x1A, 0x1B, 0x21,
                        0x23, 0x24, 0x25, 0x26, 0x27, 0x2A, 0x2B])
        return (rs << 21) | (rt << 16) | (rd << 11) | (rng.randrange(32) << 6) | f
    if roll < 0.55:
        op = rng.choice([0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F])
        return (op << 26) | (rs << 21) | (rng.choice([0, 10, 11, 12, 13]) << 16) | rng.randrange(0x10000)
    if roll < 0.8:
        op = rng.choice([0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x28, 0x29, 0x2A, 0x2B, 0x2E])
        size = {0x21: 2, 0x25: 2, 0x29: 2, 0x23: 4, 0x2B: 4}.get(op, 1)
        off = rng.randrange(0, 60 // size) * size
        return (op << 26) | (9 << 21) | (rng.choice([10, 11, 12, 13, 14]) << 16) | off
    # A branch within the program (its delay slot inside it): forward mostly, back sometimes.
    to = rng.randrange(0, count) if rng.random() < 0.2 or i + 2 > count else rng.randrange(i + 2, count + 1)
    rel = (to - i - 1) & 0xFFFF
    op = rng.choice([0x04, 0x05, 0x06, 0x07, 0x01])
    if op == 0x01:
        return (1 << 26) | (rs << 21) | (rng.choice([0, 1]) << 16) | rel
    return (op << 26) | (rs << 21) | (rt << 16) | rel


def corpus(count: int, seed: int, name: str) -> dict:
    rng = random.Random(seed)
    modules = []
    for k in range(count):
        n = rng.randrange(8, 80)
        words = [random_word(rng, i, n) for i in range(n)]
        words += [JR_RA, 0]                     # back to the caller ($ra: the tests' return address)
        image = b"".join(struct.pack("<I", w) for w in words)
        # Some immediates and branches as if relocated: the code that reads them from memory as they run.
        relocated = {4 * i for i, w in enumerate(words) if (w >> 26) and rng.random() < 0.3}
        modules.append(("CORPUS%d" % k, image, list(range(0, len(image), 4)), image, relocated, set()))
    return generate(modules, "%d random programs (seed %d) for IopCompiledIsTheInterpreter" % (count, seed),
                    single=name)


def write_if_changed(path: str, text: str) -> None:
    old = open(path, encoding="utf-8").read() if os.path.exists(path) else None
    if text != old:
        with open(path, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(text)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("modules", nargs="*", help="IRX files")
    parser.add_argument("--out-dir", help="iop_modules.cpp and iop_code_N.cpp here")
    parser.add_argument("--corpus", action="store_true", help="random programs for the host tests")
    parser.add_argument("--count", type=int, default=60)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--out", help="the corpus: one file")
    args = parser.parse_args(argv)
    if args.corpus:
        if not args.out:
            parser.error("--corpus needs --out")
        for name, text in corpus(args.count, args.seed, os.path.basename(args.out)).items():
            write_if_changed(os.path.join(os.path.dirname(args.out) or ".", name), text)
        return 0
    if not args.out_dir:
        parser.error("--out-dir is needed")
    modules = []
    for path in args.modules:
        with open(path, "rb") as handle:
            modules.append(module_of(os.path.basename(path).upper(), handle.read()))
    files = generate(modules, "%d modules: %s" % (len(modules), ", ".join(m[0] for m in modules) or "none"))
    for name, text in files.items():
        write_if_changed(os.path.join(args.out_dir, name), text)
    for name in os.listdir(args.out_dir):
        if name.startswith("iop_code_") and name.endswith(".cpp") and name not in files:
            os.remove(os.path.join(args.out_dir, name))
    print("ioprecomp: %d modules, %d words, %d files" % (len(modules), sum(len(m[2]) for m in modules), len(files)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
