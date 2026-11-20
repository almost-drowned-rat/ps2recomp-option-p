"""VU0 in macro mode: the vector unit driven directly by COP2 instructions.

The game does its vector maths here - 62,000 COP2 instructions across about 700
functions. The state is 32 vector registers of four float lanes (VF0 is the
constant (0, 0, 0, 1)), 16 integer registers, an accumulator, and the Q, I and R
special registers.

VU arithmetic is not IEEE-754, and this module defines one model of it, which the
lifted code (runtime/src/vu0.cpp) follows bit for bit:

* An operand with a zero exponent is zero (denormals are flushed, sign kept); an
  operand with the maximum exponent - an infinity or NaN elsewhere - is the
  largest finite magnitude with its sign.
* An operation is computed in double precision and rounded to single, which is
  the correctly rounded single result; a result that overflows saturates to the
  largest finite magnitude, and one that is denormal becomes zero.
* Multiply-add rounds the product before the addition.
* MAX and MINI compare their operands as sign-and-magnitude integers and hand
  back one of them as it is, uncleaned: a denormal stays (a program builds
  integer constants with `loi` and `maxi` - Burnout 3's GIF tags), and an
  exponent of 255 is simply the largest.

That follows PCSX2's interpreter rather than the hardware, which truncates
rather than rounds in places, so a result can differ from the console's in the
last bit. A port that needs bit-exact physics must revisit this model.

The FMAC operations set the MAC and status flags (zero and sign, with their
sticky copies; not overflow or underflow), and VCLIPw the clip flags. TPC
(control register 26) is not modelled, and a `cfc2` of it is refused.
"""

from __future__ import annotations

import math
import struct

MASK32 = 0xFFFFFFFF
SIGN = 0x80000000
MAX = 0x7F7FFFFF
ONE = 0x3F800000

# Control registers: 0-15 are the integer registers VI0-VI15.
REG_STATUS, REG_MAC, REG_CLIP, REG_R, REG_I, REG_Q = 16, 17, 18, 20, 21, 22
# VPU-STAT: whether VU0 and VU1 are running. No microprogram runs in this model
# (vcallms is refused) and transfers to VU1 complete when they start, so both are
# always idle: it reads 0. Code that waits for VU1 polls it, so it is modelled.
REG_VPU_STAT = 29
UNREADABLE = {REG_STATUS, REG_MAC, REG_CLIP, 26}
# Of those, the ones this model and the runtime keep (execute: _set_flags after FMACs, VCLIPw's judgement in
# CLIP) - readable by the interpreter and lifted code; derived code (symexec) still refuses all of UNREADABLE.
MODELLED_FLAGS = {REG_STATUS, REG_MAC, REG_CLIP}


# The operations execute() implements (mnemonics without their mask suffix).
SUPPORTED = (
    {"v%s%s" % (op, suffix) for op in ("add", "sub", "mul", "madd", "msub", "max", "mini")
     for suffix in ("", "x", "y", "z", "w", "q", "i")}
    | {"v%sa%s" % (op, suffix) for op in ("add", "sub", "mul", "madd", "msub")
       for suffix in ("", "x", "y", "z", "w", "q", "i")}
    | {"vopmula", "vopmsub", "vmove", "vmr32", "vabs", "vnop", "vwaitq", "vclipw",
       "vdiv", "vsqrt", "vrsqrt", "vmtir", "vmfir", "viadd", "visub", "viand", "vior",
       "viaddi", "vrinit", "vrxor", "vrget", "vrnext",
       "vlqi", "vsqi", "vlqd", "vsqd", "vilwr", "viswr"}
    | {"vitof%d" % n for n in (0, 4, 12, 15)} | {"vftoi%d" % n for n in (0, 4, 12, 15)}
) - {"vmaxq", "vminiq", "vmaxa", "vminia"}


class Refused(Exception):
    """An instruction this model does not reproduce."""


def initial_vf():
    vf = [[0, 0, 0, 0] for _ in range(32)]
    vf[0] = [0, 0, 0, ONE]
    return vf


def initial_vc():
    vc = [0] * 32
    vc[REG_R] = ONE
    return vc


def clean(bits: int) -> int:
    exponent = bits & 0x7F800000
    if exponent == 0:
        return bits & SIGN
    if exponent == 0x7F800000:
        return (bits & SIGN) | MAX
    return bits & MASK32


def value(bits: int) -> float:
    return struct.unpack("<f", struct.pack("<I", clean(bits)))[0]


def result(number: float) -> int:
    """A double rounded to a VU single: saturated, with denormals flushed."""
    if number != number:
        return MAX
    try:
        bits = struct.unpack("<I", struct.pack("<f", number))[0]
    except OverflowError:
        return (SIGN if number < 0 else 0) | MAX
    if (bits & 0x7F800000) == 0x7F800000:
        return (bits & SIGN) | MAX
    if (bits & 0x7F800000) == 0:
        return bits & SIGN
    return bits


def vu_binary(op: str, a: int, b: int, acc: int = 0) -> int:
    """add, sub, mul, max, mini; madd and msub add to or subtract from `acc`."""
    return _binary(op, a, b, acc)


def vu_abs(x: int) -> int:
    return clean(x) & ~SIGN & MASK32


def vu_itof(x: int, shift: int) -> int:
    return result(_s32(x) / (1 << shift))


def vu_ftoi(x: int, shift: int) -> int:
    return _float_to_int(value(x) * (1 << shift))


def vu_div(num: int, den: int) -> int:
    if value(den) == 0.0:
        return ((num ^ den) & SIGN) | MAX
    return result(value(num) / value(den))


def vu_sqrt(x: int) -> int:
    return result(math.sqrt(abs(value(x))))


def vu_rsqrt(num: int, den: int) -> int:
    root = math.sqrt(abs(value(den)))
    if root == 0.0:
        return (num & SIGN) | MAX
    return result(value(num) / root)


def _fields(word: int):
    dest = (word >> 21) & 0xF
    return [lane for lane, bit in enumerate((8, 4, 2, 1)) if dest & bit]


def _binary(op: str, a: int, b: int, acc: int = 0) -> int:
    # (The runtime's adder drops an operand 25 or more exponents below the other, ps2rt/vu_math.h AlignOperands:
    # rounded to nearest, as this model is, that gives the same sum, so it is not done here.)
    x, y = value(a), value(b)
    if op == "add":
        return result(x + y)
    if op == "sub":
        return result(x - y)
    if op == "mul":
        return result(x * y)
    if op == "madd":
        return result(value(acc) + value(result(x * y)))
    if op == "msub":
        return result(value(acc) - value(result(x * y)))
    if op == "max":
        return (b if _order(a) < _order(b) else a) & MASK32
    if op == "mini":
        return (b if _order(b) < _order(a) else a) & MASK32
    raise Refused(op)


def _order(bits: int) -> int:
    """MAX's and MINI's key: the bits as a sign-and-magnitude integer."""
    magnitude = bits & 0x7FFFFFFF
    return -magnitude if bits & SIGN else magnitude


_FMAC = ("madd", "msub", "mini", "max", "add", "sub", "mul")


def _split(base: str):
    """'vmaddax' -> ('madd', True, 'x'); 'vmulq' -> ('mul', False, 'q')."""
    name = base[1:]
    for op in _FMAC:
        if name.startswith(op):
            rest = name[len(op):]
            to_acc = rest.startswith("a") and op not in ("max", "mini")
            if to_acc:
                rest = rest[1:]
            return op, to_acc, rest
    return None, False, ""


def clean(bits: int) -> int:
    """A float as the VU leaves it: a denormal to its signed zero, Inf or NaN to the signed maximum."""
    exponent = bits & 0x7F800000
    if exponent == 0:
        return bits & SIGN
    if exponent == 0x7F800000:
        return (bits & SIGN) | MAX
    return bits


def _set_flags(m, values, lanes) -> None:
    """MAC and STATUS after an FMAC (runtime MacroFlags, ps2host Vu::Upper): MAC the zero (bits 3-0) and sign
    (7-4) of each field dest names, x highest; STATUS Z and S from them and their sticky copies (bits 6-7)."""
    mac = 0
    for lane in lanes:
        bits = clean(values[lane] & MASK32)
        if bits & 0x7FFFFFFF == 0:
            mac |= 8 >> lane
        if bits & SIGN:
            mac |= 0x80 >> lane
    m.vc[REG_MAC] = mac
    status = (m.vc[REG_STATUS] & ~0x3 & MASK32) | (1 if mac & 0xF else 0) | (2 if mac & 0xF0 else 0)
    m.vc[REG_STATUS] = status | ((status & 3) << 6)


def execute(m, word: int, mnemonic: str) -> None:
    """Run one COP2 operation on machine `m` (its vf, vc and vacc state)."""
    base = mnemonic.split(".")[0]
    fd, fs, ft = (word >> 6) & 0x1F, (word >> 11) & 0x1F, (word >> 16) & 0x1F
    vf, vc = m.vf, m.vc
    lanes = _fields(word)

    def write(index, values):
        if index != 0:
            for lane in lanes:
                vf[index][lane] = values[lane] & MASK32

    op, to_acc, operand = _split(base)
    if op is not None and base not in ("vopmula", "vopmsub"):
        if operand in ("x", "y", "z", "w"):
            b = [vf[ft]["xyzw".index(operand)]] * 4
        elif operand == "q":
            b = [vc[REG_Q]] * 4
        elif operand == "i":
            b = [vc[REG_I]] * 4
        elif operand == "":
            b = vf[ft]
        else:
            raise Refused(base)
        a = vf[fs]
        out = [_binary(op, a[lane], b[lane], m.vacc[lane]) for lane in range(4)]
        if to_acc:
            for lane in lanes:
                m.vacc[lane] = out[lane]
        else:
            write(fd, out)
        if op not in ("max", "mini"):       # MAX and MINI leave the flags
            _set_flags(m, out, lanes)
        return

    if base == "vopmula":
        a, b = vf[fs], vf[ft]
        out = [result(value(a[1]) * value(b[2])), result(value(a[2]) * value(b[0])),
               result(value(a[0]) * value(b[1])), m.vacc[3]]
        for lane in lanes:
            if lane < 3:
                m.vacc[lane] = out[lane]
        _set_flags(m, m.vacc, lanes)        # ACC as it is now (w unchanged)
        return
    if base == "vopmsub":
        a, b, acc = vf[fs], vf[ft], m.vacc
        out = [_binary("msub", a[1], b[2], acc[0]), _binary("msub", a[2], b[0], acc[1]),
               _binary("msub", a[0], b[1], acc[2]), 0]
        if fd != 0:
            for lane in lanes:
                if lane < 3:
                    vf[fd][lane] = out[lane]
        _set_flags(m, vf[fd] if fd else out, lanes)     # what fd holds now (w as it was), or VF0's (w 0)
        return
    if base == "vmove":
        write(ft, list(vf[fs]))
        return
    if base == "vmr32":
        a = vf[fs]
        write(ft, [a[1], a[2], a[3], a[0]])
        return
    if base == "vabs":
        write(ft, [vu_abs(x) for x in vf[fs]])
        return
    if base.startswith("vitof"):
        write(ft, [vu_itof(x, int(base[5:])) for x in vf[fs]])
        return
    if base.startswith("vftoi"):
        write(ft, [vu_ftoi(x, int(base[5:])) for x in vf[fs]])
        return
    if base in ("vnop", "vwaitq"):
        return
    if base == "vclipw":
        # fs.xyz against |ft.w|: +x, -x, +y, -y, +z, -z in bits 0-5, the three earlier judgements above (ps2host
        # Vu::Upper's CLIP).
        w = abs(value(vf[ft][3]))
        judgement = 0
        for lane in range(3):
            x = value(vf[fs][lane])
            if x > w:
                judgement |= 1 << (2 * lane)
            if x < -w:
                judgement |= 2 << (2 * lane)
        vc[REG_CLIP] = ((vc[REG_CLIP] << 6) | judgement) & 0xFFFFFF
        return
    fsf, ftf = (word >> 21) & 3, (word >> 23) & 3
    if base == "vdiv":
        vc[REG_Q] = vu_div(vf[fs][fsf], vf[ft][ftf])
        return
    if base == "vsqrt":
        vc[REG_Q] = vu_sqrt(vf[ft][ftf])
        return
    if base == "vrsqrt":
        vc[REG_Q] = vu_rsqrt(vf[fs][fsf], vf[ft][ftf])
        return
    if base == "vmtir":
        if ft != 0 and ft < 16:
            vc[ft] = vf[fs][fsf] & 0xFFFF
        return
    if base == "vmfir":
        is_ = fs
        extended = vc[is_] & 0xFFFF
        extended = (extended - 0x10000) & MASK32 if extended & 0x8000 else extended
        write(ft, [extended] * 4)
        return
    if base in ("viadd", "visub", "viand", "vior"):
        a, b = vc[fs], vc[ft]
        out = {"viadd": a + b, "visub": a - b, "viand": a & b, "vior": a | b}[base]
        if 0 < fd < 16:
            vc[fd] = out & 0xFFFF
        return
    if base == "viaddi":
        imm = fd - 32 if fd & 0x10 else fd
        if 0 < ft < 16:
            vc[ft] = (vc[fs] + imm) & 0xFFFF
        return
    if base == "vrinit":
        vc[REG_R] = ONE | (vf[fs][fsf] & 0x7FFFFF)
        return
    if base == "vrxor":
        vc[REG_R] = ONE | ((vc[REG_R] ^ vf[fs][fsf]) & 0x7FFFFF)
        return
    if base == "vrget":
        write(ft, [vc[REG_R]] * 4)
        return
    if base == "vrnext":
        if ft == 0:
            return
        r = vc[REG_R]
        bit = ((r >> 4) & 1) ^ ((r >> 22) & 1)
        vc[REG_R] = ONE | ((((r << 1) & MASK32) ^ bit) & 0x7FFFFF)
        write(ft, [vc[REG_R]] * 4)
        return
    # VU0's data memory (m.vu0_data, 4 KB): a quadword at VI * 16, wrapping; VI 0 never changes (runtime vu0.cpp).
    data = getattr(m, "vu0_data", None)
    if base in ("vlqi", "vlqd", "vsqi", "vsqd", "vilwr", "viswr") and data is None:
        raise Refused(base)

    def word_at(at):
        return int.from_bytes(data[at: at + 4], "little")

    def put(at, value):
        data[at: at + 4] = (value & MASK32).to_bytes(4, "little")

    if base in ("vlqi", "vlqd"):
        if base == "vlqd" and fs != 0:
            vc[fs] = (vc[fs] - 1) & 0xFFFF
        at = (vc[fs] & 0xFF) * 16
        write(ft, [word_at(at + 4 * lane) for lane in range(4)])
        if base == "vlqi" and fs != 0:
            vc[fs] = (vc[fs] + 1) & 0xFFFF
        return
    if base in ("vsqi", "vsqd"):
        if base == "vsqd" and ft != 0:
            vc[ft] = (vc[ft] - 1) & 0xFFFF
        at = (vc[ft] & 0xFF) * 16
        for lane in lanes:
            put(at + 4 * lane, vf[fs][lane])
        if base == "vsqi" and ft != 0:
            vc[ft] = (vc[ft] + 1) & 0xFFFF
        return
    if base == "vilwr":
        lane = lanes[0] if lanes else 3
        loaded = word_at((vc[fs] & 0xFF) * 16 + 4 * lane)
        if 0 < ft < 16:
            vc[ft] = loaded & 0xFFFF
        return
    if base == "viswr":
        for lane in lanes:
            put((vc[fs] & 0xFF) * 16 + 4 * lane, vc[ft] & 0xFFFF)
        return
    raise Refused(base)


def _s32(x: int) -> int:
    x &= MASK32
    return x - (1 << 32) if x & SIGN else x


def _float_to_int(number: float) -> int:
    if number >= 2147483647.0:
        return 0x7FFFFFFF
    if number <= -2147483648.0:
        return 0x80000000
    return int(number) & MASK32


def move_to(m, word: int, mnemonic: str, rt_value: int, rt_high: int) -> None:
    """qmtc2 rt, vfd and ctc2 rt, id: write VU0 state from a general register."""
    index = (word >> 11) & 0x1F
    if mnemonic == "qmtc2":
        if index:
            m.vf[index] = [rt_value & MASK32, (rt_value >> 32) & MASK32,
                           rt_high & MASK32, (rt_high >> 32) & MASK32]
        return
    if index == 31:
        # CMSAR1: the write starts VU1 at it (the lifted code's StartVu1) - an outside effect, like vcallms.
        raise Refused("ctc2 to CMSAR1 (starts VU1)")
    # As the console takes them (ps2autotests cpu/vu0_macro/transfer; vu0.cpp's Cop2Move): STATUS its sticky
    # bits only, MAC, TPC, VPU-STAT and the reserved 19, 23-25, FBRST (28) not at all, CMSAR0 16 bits.
    if index == REG_STATUS:
        m.vc[REG_STATUS] = (m.vc[REG_STATUS] & 0x3F) | (rt_value & 0xFC0)
    elif index in (REG_MAC, 19, 23, 24, 25, 26, 28, REG_VPU_STAT):
        pass
    elif index == 27:
        m.vc[27] = rt_value & 0xFFFF
    elif index < 16:
        if index:
            m.vc[index] = rt_value & 0xFFFF
    elif index == REG_R:
        m.vc[REG_R] = ONE | (rt_value & 0x7FFFFF)
    elif index == REG_CLIP:
        m.vc[REG_CLIP] = rt_value & 0xFFFFFF
    else:
        m.vc[index] = rt_value & MASK32


def move_from(m, word: int, mnemonic: str):
    """qmfc2 rt, vfs and cfc2 rt, id: (low 64 bits, high 64 bits or None)."""
    index = (word >> 11) & 0x1F
    if mnemonic == "qmfc2":
        v = m.vf[index]
        return v[0] | (v[1] << 32), v[2] | (v[3] << 32)
    if index in UNREADABLE and index not in MODELLED_FLAGS:
        raise Refused("cfc2 of control register %d (flags are not modelled)" % index)
    if index == REG_VPU_STAT:
        return 0, None
    # R reads its 23 fraction bits; the reserved 19 and 24 read constants (cpu/vu0_macro/transfer).
    if index == REG_R:
        return m.vc[REG_R] & 0x7FFFFF, None
    if index == 19:
        return 0x2E30, None
    if index == 24:
        return 0x0C0C, None
    return m.vc[index], None
