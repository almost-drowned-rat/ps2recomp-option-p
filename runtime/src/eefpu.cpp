/*
 * The Emotion Engine's single-precision FPU, as the derived code relies on it.
 *
 * The derived functions in src/generated/ do float arithmetic through these
 * helpers rather than with `float` operators, because the EE's FPU is not
 * IEEE-754 and a port that uses the host's arithmetic directly will drift.
 * What the console does (ps2autotests cpu/ee_fpu, M8 gate 2026-11-14; the
 * Python model is ps2decomp/eefpu.py, which these must match bit for bit):
 *
 *  - There are no infinities or NaNs: an exponent of 255 is an ordinary number
 *    (0x7F800000 = 2**128, 0x7FFFFFFF = (2 - 2**-23) * 2**128, the largest).
 *  - A result beyond the largest magnitude saturates to +-0x7FFFFFFF.
 *  - Division by zero (0/0 too) gives +-0x7FFFFFFF, the operands' signs XORed.
 *  - sqrt takes the magnitude; abs, neg and mov are bit operations; min and max
 *    compare the patterns as sign-magnitude integers and return one unchanged.
 *
 * Each operation is computed in double precision and then rounded once to
 * single, which gives the correctly rounded single-precision result for +, -, *,
 * / and square root (double has more than twice the precision of float).
 *
 *  - add, sub, mul and the accumulate forms are rounded toward zero, and
 *    denormal operands and results are flushed to zero - as the console does.
 *    PS2RT_EE_ROUND_NEAREST=1 rounds those to nearest and keeps denormals, as
 *    ps2decomp/eefpu.py does (what cppcheck compares against).
 *  - div and sqrt round to nearest in both modes; rsqrt is the root rounded to
 *    nearest, its reciprocal rounded to nearest, then the product truncated
 *    (the console's results, M8 gate).
 *
 * Not modelled: the FCR31 flag bits, and the multiplier's last-bit quirks.
 */

#include "ps2rt/generated.h"

#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace generated {

namespace {

const bool kNearest = std::getenv("PS2RT_EE_ROUND_NEAREST") != nullptr;

const u32 kSign = 0x80000000u;
const u32 kLargest = 0x7FFFFFFFu;
const double kTwo128 = 3.4028236692093846e38;            // 2**128, the value of 0x7F800000
const double kLargestValue = 6.8056469327705772e38;      // (2 - 2**-23) * 2**128, the value of 0x7FFFFFFF

/* An operand's value: an exponent of 255 is a number (beyond float's range, hence double). */
double Decode(u32 bits) {
    const u32 exponent = (bits >> 23) & 0xFF;
    if (exponent == 0xFF) {
        const double magnitude = std::ldexp(double((bits & 0x7FFFFFu) | 0x800000u), 105);
        return (bits & kSign) ? -magnitude : magnitude;
    }
    if (!kNearest && exponent == 0) bits &= kSign;     // a denormal operand is zero
    float value;
    std::memcpy(&value, &bits, sizeof value);
    return value;
}

}  // namespace

u32 FloatNarrow(double value) { return FloatNarrowRounded(value, kNearest); }

/* Rounded as `nearest` says; denormal results flushed in the console mode (not PS2RT_EE_ROUND_NEAREST). */
u32 FloatNarrowRounded(double value, bool nearest) {
    if (value != value) return 0;
    const u32 sign = std::signbit(value) ? kSign : 0;
    const double magnitude = std::fabs(value);
    if (magnitude >= kTwo128) {
        // Exponent 255: the mantissa by hand, rounded as the mode says; past the largest, saturated.
        if (magnitude >= kLargestValue) return sign | kLargest;
        const double scaled = (magnitude / kTwo128 - 1.0) * 8388608.0;
        const double mantissa = nearest ? std::nearbyint(scaled) : std::floor(scaled);
        if (mantissa >= 8388608.0) return sign | kLargest;
        return sign | 0x7F800000u | u32(mantissa);
    }
    if (magnitude > FLT_MAX) {
        // Between 0x7F7FFFFF and 2**128: truncated, or to nearest (the tie goes up, to the even 0x7F800000).
        if (!nearest) return sign | 0x7F7FFFFFu;
        return sign | (magnitude >= FLT_MAX + 1.0141204801825835e31 ? 0x7F800000u : 0x7F7FFFFFu);
    }
    float narrowed = static_cast<float>(value);
    // Toward zero: a value rounded away from it steps back one.
    if (!nearest && std::fabs(double(narrowed)) > magnitude) narrowed = std::nextafter(narrowed, 0.0f);
    if (!kNearest && std::fabs(narrowed) < FLT_MIN) narrowed = std::copysign(0.0f, narrowed);
    u32 bits;
    std::memcpy(&bits, &narrowed, sizeof bits);
    return bits;
}

u32 FloatAdd(u32 a, u32 b) { return FloatNarrow(Decode(a) + Decode(b)); }
u32 FloatSub(u32 a, u32 b) { return FloatNarrow(Decode(a) - Decode(b)); }
u32 FloatMul(u32 a, u32 b) { return FloatNarrow(Decode(a) * Decode(b)); }

u32 FloatMulAdd(u32 acc, u32 a, u32 b) { return FloatNarrow(Decode(acc) + Decode(a) * Decode(b)); }
u32 FloatMulSub(u32 acc, u32 a, u32 b) { return FloatNarrow(Decode(acc) - Decode(a) * Decode(b)); }

u32 FloatDiv(u32 a, u32 b) {
    const double divisor = Decode(b);
    if (divisor == 0.0) return ((a ^ b) & kSign) | kLargest;
    return FloatNarrowRounded(Decode(a) / divisor, true);       // div rounds to nearest on the console
}

namespace {
/* The order min and max use: the pattern as a sign-magnitude integer, -0 below +0. */
inline long long Order(u32 bits) { return (bits & kSign) ? -(long long)(bits & kLargest) - 1 : (long long)bits; }
}  // namespace

u32 FloatMin(u32 a, u32 b) { return Order(b) < Order(a) ? b : a; }
u32 FloatMax(u32 a, u32 b) { return Order(b) > Order(a) ? b : a; }

u32 FloatNeg(u32 a) { return a ^ kSign; }
u32 FloatAbs(u32 a) { return a & kLargest; }

/* The root of the magnitude: sqrt(-4) is 2. */
u32 FloatSqrt(u32 a) { return FloatNarrowRounded(std::sqrt(std::fabs(Decode(a))), true); }   // to nearest

/* `mov.s` copies the bits. */
u32 FloatMove(u32 a) { return a; }

u32 FloatFromInt(u32 bits_as_int) {
    return FloatNarrow(static_cast<double>(static_cast<s32>(bits_as_int)));
}

/* Truncates toward zero below 2**31 in magnitude (an exponent up to 2**30's)
   and saturates beyond, by the sign alone: the EE's cvt.w.s (emu.py's
   float_to_word). */
u32 IntFromFloat(u32 float_bits) {
    if ((float_bits & 0x7F800000u) <= 0x4E800000u)
        return static_cast<u32>(static_cast<s32>(Decode(float_bits)));
    return (float_bits & kSign) ? 0x80000000u : 0x7FFFFFFFu;
}

/* For comparisons (c.eq/lt/le.s, the derived code's): the value, an exponent
   of 255 included - such patterns order by mantissa, above every other. PS2 code
   starts a minimum search at 0x7FFFFFFF (ZOE's 0x004481C8, 2026-11-07). */
double FloatValue(u32 bits) { return Decode(bits); }

/* An operand for arithmetic, as the operations above read it. */
double FloatOperand(u32 bits) { return Decode(bits); }

u32 FloatBits(float value) {
    u32 bits;
    std::memcpy(&bits, &value, sizeof bits);
    return bits;
}

}  // namespace generated
