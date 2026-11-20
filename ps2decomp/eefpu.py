"""The Emotion Engine's single-precision FPU (COP1) on bit patterns - the one Python model.

emu.py (the interpreter) and symexec.py (the symbolic model, whose expressions are checked against the
interpreter) both compute COP1 results here; runtime/src/eefpu.cpp and native/emu_core.c are the C copies
and must give the same bits.

The EE's FPU is not IEEE-754. What real hardware does (ps2autotests cpu/ee_fpu, run on a console; M8 gate
2026-11-14):

- No infinities and no NaNs: an exponent of 255 is an ordinary number, twice the size of exponent 254's
  (0x7F800000 = 2**128, 0x7FFFFFFF = (2 - 2**-23) * 2**128, the largest magnitude).
- A result beyond the largest magnitude saturates to +-0x7FFFFFFF (not 0x7F7FFFFF).
- Division by zero (also 0/0) gives +-0x7FFFFFFF, the sign the two operands' signs XORed; rsqrt by zero
  gives +-0x7FFFFFFF with the numerator's sign.
- sqrt and rsqrt take the magnitude: sqrt(-4) = 2.
- abs, neg and mov are bit operations (clear, flip, keep the sign); nothing is flushed or clamped.
- min and max compare the patterns as sign-magnitude integers (-0 < +0) and return one of them unchanged.
- c.eq/c.lt/c.le compare the values (an exponent of 255 included); +0 == -0.

Rounding: the console truncates (toward zero) add, sub, mul and the accumulate forms, and flushes denormal
operands and results to zero. div and sqrt round to nearest (19/19 and 15/15 console lines; truncation
misses 1 and 6). rsqrt is three steps: the root rounded to nearest, its reciprocal rounded to nearest, then
the numerator times it truncated (39/39 console lines; a plain quotient, truncated, misses 1/sqrt(25)). The
last step's rounding is not pinned by the console lines (nearest also fits). This module rounds add/sub/mul to
nearest and keeps denormals, as the interpreter always has; the runtime's default is the console's way and
PS2RT_EE_ROUND_NEAREST=1 selects this one (what the compiled-code check compares against). div, sqrt and
rsqrt round the same way in both. Known gaps (not modelled): the FCR31 flag bits (O/U/D/I, sticky and cause), and the multiplier's
last-bit quirks (1 * 0x3FFFFFFF gives 0x3FFFFFFE on the console).
"""

from __future__ import annotations

import math
import struct

MASK32 = 0xFFFFFFFF
SIGN = 0x80000000
LARGEST = 0x7FFFFFFF
# The value of 0x7FFFFFFF: anything at or beyond it saturates.
LARGEST_VALUE = math.ldexp(2.0 - 2.0 ** -23, 128)


def decode(bits: int) -> float:
    """A pattern's value; exponent 255 is a number, a denormal keeps its value (round-to-nearest model)."""
    bits &= MASK32
    exponent = (bits >> 23) & 0xFF
    mantissa = bits & 0x7FFFFF
    if exponent == 0:
        magnitude = math.ldexp(float(mantissa), -149)
    else:
        magnitude = math.ldexp(float(mantissa | 0x800000), exponent - 150)
    return -magnitude if bits & SIGN else magnitude


def encode(value: float, nearest: bool = True) -> int:
    """A double result rounded into a pattern - to nearest (half to even), or toward zero - saturating at
    +-0x7FFFFFFF."""
    if value != value:
        return 0
    sign = SIGN if math.copysign(1.0, value) < 0 else 0
    magnitude = abs(value)
    if magnitude >= LARGEST_VALUE:
        return sign | LARGEST
    if magnitude < math.ldexp(1.0, -126):
        # The denormal range: IEEE single precision rounds it the same way; truncated, the steps of 2**-149.
        if not nearest:
            return sign | int(math.ldexp(magnitude, 149))
        return sign | struct.unpack("<I", struct.pack("<f", magnitude))[0]
    fraction, exponent = math.frexp(magnitude)      # magnitude = fraction * 2**exponent, 0.5 <= fraction < 1
    biased = exponent + 126
    scaled = (fraction * 2.0 - 1.0) * 8388608.0
    mantissa = round(scaled) if nearest else math.floor(scaled)    # half to even, as IEEE
    if mantissa == 0x800000:
        mantissa = 0
        biased += 1
    if biased > 255:
        return sign | LARGEST
    return sign | (biased << 23) | mantissa


def add(a: int, b: int) -> int:
    return encode(decode(a) + decode(b))


def sub(a: int, b: int) -> int:
    return encode(decode(a) - decode(b))


def mul(a: int, b: int) -> int:
    return encode(decode(a) * decode(b))


def mul_add(acc: int, a: int, b: int, subtract: bool) -> int:
    product = decode(a) * decode(b)
    return encode(decode(acc) - product if subtract else decode(acc) + product)


def div(a: int, b: int) -> int:
    divisor = decode(b)
    if divisor == 0.0:
        return ((a ^ b) & SIGN) | LARGEST
    return encode(decode(a) / divisor)


def sqrt(a: int) -> int:
    return encode(math.sqrt(abs(decode(a))))


def rsqrt(a: int, b: int) -> int:
    """The console's three steps: the root rounded to nearest (as sqrt), its reciprocal rounded to nearest,
    then a times the reciprocal truncated."""
    root = decode(sqrt(b))
    if root == 0.0:
        return (a & SIGN) | LARGEST
    reciprocal = decode(encode(1.0 / root))
    return encode(decode(a) * reciprocal, nearest=False)


def _order(bits: int) -> int:
    bits &= MASK32
    return -(bits & LARGEST) - 1 if bits & SIGN else bits


def fmax(a: int, b: int) -> int:
    return (b if _order(b) > _order(a) else a) & MASK32


def fmin(a: int, b: int) -> int:
    return (b if _order(b) < _order(a) else a) & MASK32


def neg(a: int) -> int:
    return (a ^ SIGN) & MASK32


def fabs(a: int) -> int:
    return a & LARGEST


def move(a: int) -> int:
    return a & MASK32


def from_int(word: int) -> int:
    word &= MASK32
    return encode(float(word - (1 << 32) if word & SIGN else word))


def compare(op: str, a: int, b: int) -> bool:
    x, y = decode(a), decode(b)
    return {"eq": x == y, "lt": x < y, "le": x <= y, "false": False}[op]


# FCR0 reads as the implementation/revision number; FCR1-15 read as FCR0, FCR16-30 as FCR31 (console).
FCR0 = 0x2E30
# FCR31: bits 0 and 24 read as one; the condition (bit 23) acts; the other writable bits - 3-6 and 14-17, the
# console keeps 0x0183C079 of a write of all ones (ps2autotests cpu/ee_fpu/transfer) - are only kept and read back.
FCR31_FIXED = 0x01000001
FCR31_CONDITION = 1 << 23
FCR31_WRITABLE = 0x0003C078


def read_fcr(index: int, condition: bool, kept: int = 0) -> int:
    if index < 16:
        return FCR0
    return FCR31_FIXED | (FCR31_CONDITION if condition else 0) | (kept & FCR31_WRITABLE)
