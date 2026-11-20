"""The native verification core (ps2decomp/native/emu_core.c) against the Python interpreter, step by step.

Two machines get the same random state and the same random instruction (and delay slot): one runs emu.py's
step, the other the native core's. Where the core takes the step, every register, HI/LO pair, the pc, every
memory byte, the journal and the watch_zero result must be the same; where emu.py raises, the core must have
refused the step (it leaves those to emu.py). Plus whole runs of a small real function both ways.
Skipped where the core cannot be built (no Visual C++).
"""

import ctypes
import os
import random
import struct
import sys
import unittest

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
sys.path[:0] = [ROOT, os.path.join(ROOT, "tools")]

from ps2decomp import emu, emu_native

BASE = 0x00100000
CODE = 0x4000                   # code bytes from BASE
RAM = 0x40000                   # RAM bytes from BASE
PAGE = 0x20000000               # a page mapped for tests
UNMAPPED = 0x30000000


def available():
    try:
        return emu_native.library() is not None
    except Exception:           # no compiler here
        return False


def machine():
    m = emu.Machine(BASE, bytes(CODE), RAM)
    m.memory.page_source = lambda page: bytes((page >> 12) & 0xFF for _ in range(4096))
    m.memory.map_page(PAGE)
    m.memory.map_page(PAGE + 0x3000)
    return m


# Opcodes / functs to draw from: everything the core takes, and neighbours it must leave alone.
PRIMARY = [0x00] * 12 + [0x01] * 3 + [0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D,
                         0x0E, 0x0F, 0x10, 0x11, 0x12, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B] + \
          [0x1C] * 4 + [0x11] * 8 + [0x31, 0x39] * 2 + [0x12] * 10 + [0x36, 0x3E] * 2 + [0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x2B,
                        0x2C, 0x2D, 0x2E, 0x2F, 0x31, 0x33, 0x36, 0x37, 0x39, 0x3E, 0x3F] * 2
SPECIAL = list(range(64))
MMI = [0x00, 0x01, 0x04, 0x08, 0x10, 0x11, 0x12, 0x13, 0x18, 0x19, 0x1A, 0x1B, 0x20, 0x21, 0x28, 0x29]
COP1_RS = [0x00, 0x02, 0x04, 0x06, 0x08, 0x08, 0x10, 0x10, 0x10, 0x10, 0x10, 0x14, 0x01, 0x11]
COP1_S = [0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x16, 0x18, 0x19, 0x1A, 0x1C, 0x1D, 0x1E, 0x1F,
          0x24, 0x28, 0x29, 0x30, 0x32, 0x34, 0x36, 0x20, 0x31, 0x3C]
FLOATS = [0x00000000, 0x80000000, 0x3F800000, 0xBF800000, 0x7F7FFFFF, 0xFF7FFFFF, 0x7F800000, 0xFF800000,
          0x7FC00000, 0xFFFFFFFF, 0x00000001, 0x807FFFFF, 0x00800000, 0x4E800000, 0x4F000000, 0xCF000000,
          0x4EFFFFFF, 0x3F000001, 0x7F000000, 0x40490FDB]


def float_bits(rng):
    pick = rng.random()
    if pick < 0.4:
        return rng.choice(FLOATS)
    if pick < 0.7:
        import struct as _s
        return _s.unpack("<I", _s.pack("<f", rng.uniform(-1e6, 1e6)))[0]
    return rng.randrange(1 << 32)
REGIMM = [0x00, 0x01, 0x02, 0x03, 0x08, 0x10, 0x11, 0x12, 0x13, 0x18, 0x19]


def word(rng, plain=False):
    while True:
        op = rng.choice(PRIMARY)
        rs, rt, rd, sa = rng.randrange(32), rng.randrange(32), rng.randrange(32), rng.randrange(32)
        if rng.random() < 0.7:
            sa = 0
        if op == 0x00:
            w = (rs << 21) | (rt << 16) | (rd << 11) | (sa << 6) | rng.choice(SPECIAL)
        elif op == 0x1C:
            w = (0x1C << 26) | (rs << 21) | (rt << 16) | (rd << 11) | (sa << 6) | rng.choice(MMI)
        elif op == 0x11:
            crs = rng.choice(COP1_RS)
            if crs == 0x08:
                w = (0x11 << 26) | (8 << 21) | (rng.randrange(6) << 16) | (rng.randrange(-64, 64) & 0xFFFF)
            else:
                w = (0x11 << 26) | (crs << 21) | (rt << 16) | (rd << 11) | (rng.randrange(32) << 6) | \
                    (rng.choice(COP1_S) if rng.random() < 0.9 else rng.randrange(64))
        elif op == 0x12:
            if rng.random() < 0.75:     # macro mode: special1 (funct < 0x3C) or special2 (its code in bits 6-10, 0-1)
                dest = rng.choice([0xF, 0x8, 0x4, 0x2, 0x1, 0xE, 0x7, rng.randrange(16)])
                funct = rng.randrange(0x3C) if rng.random() < 0.55 else 0x3C | rng.randrange(4)
                w = (0x12 << 26) | (1 << 25) | (dest << 21) | (rt << 16) | (rs << 11) | (rd << 6) | funct
            else:                       # qmfc2 cfc2 qmtc2 ctc2 bc2, the interlock bit now and then
                crs = rng.choice([0x01, 0x02, 0x05, 0x06, 0x08, 0x00, 0x04])
                w = (0x12 << 26) | (crs << 21) | (rt << 16) | (rng.choice([rd, 16, 17, 18, 20, 21, 22, 26, 29]) << 11) | \
                    (1 if rng.random() < 0.1 else 0)
        elif op == 0x10:
            w = (0x10 << 26) | (rng.choice([0x08, 0x08, 0x00, 0x04, 0x10]) << 21) | (rng.randrange(6) << 16) | \
                (rng.randrange(-64, 64) & 0xFFFF)
        elif op == 0x01:
            w = (1 << 26) | (rs << 21) | (rng.choice(REGIMM) << 16) | rng.randrange(-64, 64) & 0xFFFF
        elif op in (0x02, 0x03):
            w = (op << 26) | ((BASE + 4 * rng.randrange(CODE // 4)) >> 2 & 0x03FFFFFF)
        elif op in (0x04, 0x05, 0x06, 0x07, 0x14, 0x15, 0x16, 0x17):
            w = (op << 26) | (rs << 21) | (rt << 16) | (rng.randrange(-64, 64) & 0xFFFF)
        else:
            imm = rng.choice([rng.randrange(0x10000), rng.randrange(-64, 64) & 0xFFFF, 0, 0xFFFF, 0x8000, 0x7FFF])
            w = (op << 26) | (rs << 21) | (rt << 16) | imm
        if not plain:
            return w
        # A delay slot: anything but a transfer (emu.py refuses those, so does the core).
        from ps2decomp.r5900 import DISASM
        if not DISASM.decode(w, BASE).has_delay_slot:
            return w


def value(rng):
    pick = rng.random()
    if pick < 0.06:
        return BASE + 4 * rng.randrange(CODE // 4)              # a code address (jr / jalr targets)
    if pick < 0.25:
        return BASE + rng.randrange(RAM)                        # an address in RAM
    if pick < 0.35:
        return 0x70000000 + rng.randrange(0x4000)               # the scratchpad window
    if pick < 0.42:
        return PAGE + rng.randrange(0x4000)                     # a mapped page (or the unmapped one between)
    if pick < 0.47:
        return UNMAPPED + rng.randrange(0x1000)
    if pick < 0.55:
        return rng.choice([0, 1, 0xFFFFFFFFFFFFFFFF, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF80000000, 31, 32, 63, 64])
    if pick < 0.7:
        return rng.randrange(1 << 32)
    return rng.randrange(1 << 64)


def state(m):
    mem = m.memory
    return (list(m.regs), list(m.regs_hi), m.hi, m.lo, m.hi1, m.lo1, m.pc, bytes(mem.bytes),
            list(mem._journal or []), getattr(mem, "_journal_bytes", 0), mem.last_zero_read,
            list(m.fpr), bool(m.fp_cond), m.fp_acc, [list(v) for v in m.vf], list(m.vc), list(m.vacc))


@unittest.skipUnless(available(), "the native core cannot be built here")
class TestNativeStep(unittest.TestCase):
    TRIALS = int(os.environ.get("EMU_NATIVE_TRIALS", "60000"))

    def test_every_step_the_core_takes_is_emu_pys(self):
        rng = random.Random(20261113)
        a, b = machine(), machine()
        bridge = emu_native.Bridge(b)
        taken = refused = python_raised = 0
        kinds = {}
        for trial in range(self.TRIALS):
            regs = [0] + [value(rng) for _ in range(31)]
            regs_hi = [rng.randrange(1 << 64) for _ in range(32)]
            hilo = [rng.choice([value(rng), rng.randrange(1 << 64)]) for _ in range(4)]
            fpr = [float_bits(rng) for _ in range(32)]
            fp_cond, fp_acc = rng.random() < 0.5, float_bits(rng)
            vf = [[0, 0, 0, 0x3F800000]] + [[float_bits(rng) for _ in range(4)] for _ in range(31)]
            vc = [0] + [rng.randrange(1 << 16) for _ in range(15)] + [rng.randrange(1 << 32) for _ in range(16)]
            vc[20] = 0x3F800000 | rng.randrange(1 << 23)
            vc[21], vc[22] = float_bits(rng), float_bits(rng)
            vacc = [float_bits(rng) for _ in range(4)]
            pc = BASE + 4 * rng.randrange(CODE // 4 - 2)
            w, slot = word(rng), word(rng, plain=True)
            watch = rng.random() < 0.3
            journal = rng.random() < 0.8
            near_limit = rng.random() < 0.05
            spent = emu.Memory.JOURNAL_LIMIT - rng.randrange(20)
            for m in (a, b):
                m.regs[:] = regs
                m.regs_hi[:] = regs_hi
                m.hi, m.lo, m.hi1, m.lo1 = hilo
                m.fpr[:] = fpr
                m.fp_cond, m.fp_acc = fp_cond, fp_acc
                m.vf = [list(v) for v in vf]
                m.vc = list(vc)
                m.vacc = list(vacc)
                m.pc = pc
                m.memory.write_int(pc, w, 4)
                m.memory.write_int(pc + 4, slot, 4)
                m.memory.watch_zero = watch
                m.memory.last_zero_read = None
                if journal:
                    m.memory.begin_journal()
                    if near_limit:
                        m.memory._journal_bytes = spent
                else:
                    m.memory.end_journal()
            before = state(b)
            try:
                a.step()
                error = None
            except Exception as exc:        # emu.py's own errors (faults, unsupported, limits)
                error = exc
            reason, _ = bridge.run(0, 0, 1 << 40, single=True)
            if reason == emu_native.NEED_PYTHON:
                refused += 1
                self.assertEqual(state(b), before, "refused, yet changed something: %08X / %08X" % (w, slot))
                python_raised += error is not None
            else:
                taken += 1
                from ps2decomp.r5900 import DISASM as _D
                kinds[_D.decode(w, pc).mnemonic] = kinds.get(_D.decode(w, pc).mnemonic, 0) + 1
                self.assertIsNone(error, "the core took %08X (slot %08X), emu.py raised %r" % (w, slot, error))
                sa, sb = state(a), state(b)
                for name, x, y in zip(("regs", "regs_hi", "hi", "lo", "hi1", "lo1", "pc", "memory", "journal",
                                       "journal bytes", "last zero read", "fpr", "fp_cond", "fp_acc", "vf", "vc",
                                       "vacc"), sa, sb):
                    if x != y:
                        detail = ""
                        if name in ("regs", "regs_hi", "fpr", "vf", "vc", "vacc"):
                            i = next(i for i in range(32) if x[i] != y[i])
                            detail = " ($%d: emu.py 0x%X, core 0x%X)" % (i, x[i], y[i])
                        elif name not in ("memory", "journal"):
                            detail = " (emu.py %r, core %r)" % (x, y)
                        self.fail("%08X (slot %08X): %s differs%s" % (w, slot, name, detail))
            for m in (a, b):
                if m.memory._journal is not None:
                    m.memory.rollback()
                    m.memory.end_journal()
            b.memory.bytes[:] = a.memory.bytes      # where the core refused, emu.py alone wrote (unjournalled)
        print("\n  native step: %d taken, %d refused (%d of those emu.py raised on)" % (taken, refused, python_raised))
        print("  taken: " + ", ".join("%s %d" % kv for kv in sorted(kinds.items(), key=lambda kv: -kv[1])))
        self.assertGreater(taken, self.TRIALS // 3)


@unittest.skipUnless(available(), "the native core cannot be built here")
class TestNativeRun(unittest.TestCase):
    def test_a_real_loop_runs_the_same(self):
        # sum = 0; while (n) { sum += *p++ * 3; n--; } return sum >> 3 (a loop, loads, a delay slot, jr $ra)
        code = [
            0x00001025,     # or    v0, zero, zero
            0x10A00008,     # beq   a1, zero, done
            0x00000000,     # nop
            0x90880000,     # loop: lbu t0, 0(a0)
            0x00084840,     # sll   t1, t0, 1
            0x01284821,     # addu  t1, t1, t0
            0x00491021,     # addu  v0, v0, t1
            0x24A5FFFF,     # addiu a1, a1, -1
            0x14A0FFFA,     # bne   a1, zero, loop
            0x24840001,     # addiu a0, a0, 1  (delay slot)
            0x000210C2,     # done: srl   v0, v0, 3
            0x03E00008,     # jr    ra
            0x00000000,     # nop
        ]
        image = b"".join(struct.pack("<I", w) for w in code) + bytes(CODE - 4 * len(code))
        results = []
        for native in ("0", "1"):
            os.environ["PS2_EMU_NATIVE"] = native
            m = emu.Machine(BASE, image, RAM)
            data = bytes(range(256)) * 4
            m.memory.write(BASE + 0x8000, data)
            m.memory.begin_journal()
            # Coverage (liftverify's guided cases): the same marks from both. n = 0 never enters the loop.
            marks = (ctypes.c_uint8 * len(code))()
            m.coverage = (BASE, marks)
            m.call(BASE, (BASE + 0x8000, 0))
            first = bytes(marks)
            v0 = m.call(BASE, (BASE + 0x8000, 1000))
            m.coverage = None
            results.append((v0, m.instructions_executed, list(m.regs), m.hi, m.lo, len(m.memory._journal),
                            first, bytes(marks)))
        os.environ["PS2_EMU_NATIVE"] = "0"
        self.assertEqual(results[0], results[1])
        self.assertEqual(results[0][0], (sum(data[:1000]) * 3) >> 3)
        # Steps start at the branch, not its delay slot: 0,1 then 10,11 when n = 0; then the loop too.
        self.assertEqual(results[0][6], bytes([1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0]))
        self.assertEqual(results[0][7], bytes([1, 1, 0, 1, 1, 1, 1, 1, 1, 0, 1, 1, 0]))


if __name__ == "__main__":
    unittest.main()
