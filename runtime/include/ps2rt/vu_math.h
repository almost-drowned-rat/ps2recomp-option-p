/*
 * The VU's arithmetic model (engine/ps2decomp/vu0.py documents it, and where it
 * departs from the hardware): clean operands, results narrowed to single
 * precision and saturated. Inline, so that the macro-mode COP2 (vu0.cpp) and
 * the micro-mode interpreter (engine/host/src/vu.cpp) compute each lane
 * without a call per step.
 */

#ifndef PS2RT_VU_MATH_H
#define PS2RT_VU_MATH_H

#include <cmath>
#include <cstring>

#include "ps2rt/types.h"

/* The model's steps, inlined into the interpreter and compiled microprograms. */
#if defined(_MSC_VER)
#define PS2RT_VU_INLINE __forceinline
#else
#define PS2RT_VU_INLINE inline __attribute__((always_inline))
#endif

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <emmintrin.h>
#define PS2RT_VU_SSE 1
#define PS2RT_VU_SSE2NEON 0
#elif (defined(__aarch64__) || defined(_M_ARM64)) && !defined(PS2RT_NO_SSE2NEON)
/* AArch64 (Android, agent L 2026-11-21): the same four-lane paths, their SSE intrinsics as NEON by sse2neon (MIT,
   v1.8.0, include/sse2neon - LICENSES/sse2neon-MIT.txt). The scalar forms were ~30 % of VU1's and the EE's time on
   an Odin 2 (Snapdragon 8 Gen 2). The rounding: the callers set it by ps2rt/fp_mode.h (FPCR: toward zero, FZ - FZ
   flushes operands and results, as DAZ + FTZ), not by sse2neon's _mm_setcsr (the rounding only). */
#include "sse2neon/sse2neon.h"
#define PS2RT_VU_SSE 1
#define PS2RT_VU_SSE2NEON 1
#else
#define PS2RT_VU_SSE 0
#define PS2RT_VU_SSE2NEON 0
#endif
/* SSE4.1 (pminud): every x86-64 processor a Vulkan port runs on has it; MSVC
   takes its intrinsics without /arch, and other compilers say so with
   __SSE4_1__. */
#if PS2RT_VU_SSE2NEON
#define PS2RT_VU_SSE41 1            /* (sse2neon has _mm_min_epu32 and the rest) */
#elif PS2RT_VU_SSE && (defined(__SSE4_1__) || defined(_M_X64))
#include <smmintrin.h>
#define PS2RT_VU_SSE41 1
#else
#define PS2RT_VU_SSE41 0
#endif



namespace lifted {

inline constexpr u32 kSign = 0x80000000u;
inline constexpr u32 kMax = 0x7F7FFFFFu;
inline constexpr u32 kOne = 0x3F800000u;
inline constexpr int kRegClip = 18, kRegR = 20, kRegI = 21, kRegQ = 22;

PS2RT_VU_INLINE u32 Clean(u32 bits) {
    const u32 exponent = bits & 0x7F800000u;
    if (exponent == 0) return bits & kSign;
    if (exponent == 0x7F800000u) return (bits & kSign) | kMax;
    return bits;
}

PS2RT_VU_INLINE double Value(u32 bits) {
    const u32 cleaned = Clean(bits);
    float value;
    std::memcpy(&value, &cleaned, sizeof value);
    return value;
}

PS2RT_VU_INLINE u32 Result(double number) {
    if (number != number) return kMax;
    const float narrowed = static_cast<float>(number);
    u32 bits;
    std::memcpy(&bits, &narrowed, sizeof bits);
    if ((bits & 0x7F800000u) == 0x7F800000u) return (bits & kSign) | kMax;
    if ((bits & 0x7F800000u) == 0) return bits & kSign;
    return bits;
}

enum Op { kAdd, kSub, kMul, kMadd, kMsub, kMax_, kMini };

/* One operation, chosen at compile time: RunFmac picks it once an
   instruction rather than once a lane. */
/* MAX's and MINI's key: the bits as a sign-and-magnitude integer. They
   compare so and hand back an operand as it is - no Clean: a denormal stays
   (programs build integer constants with LOI and MAXI). */
PS2RT_VU_INLINE s32 Order(u32 bits) {
    const s32 magnitude = s32(bits & 0x7FFFFFFFu);
    return (bits & kSign) ? -magnitude : magnitude;
}

/* The VU's adder lines the smaller operand up with the larger one's exponent and has no room for it 25 or more
   exponents down: there it is a zero of its sign, the sum the larger operand as it is - where round toward zero
   of the exact sum gives the larger less an ulp when the signs differ (ps2autotests vu/games/triace, Tri-Ace's
   games; PCSX2 keeps it as a game fix). Clean operands (bits, exponents 1-254 or zero). */
PS2RT_VU_INLINE void AlignOperands(u32& a, u32& b) {
    const int ea = int((a >> 23) & 0xFF), eb = int((b >> 23) & 0xFF);
    if (ea - eb >= 25) b &= kSign;
    else if (eb - ea >= 25) a &= kSign;
}

template <int Op>
inline u32 BinaryOf(u32 a, u32 b, u32 acc) {
    if constexpr (Op == kMax_) return Order(a) < Order(b) ? b : a;
    else if constexpr (Op == kMini) return Order(b) < Order(a) ? b : a;
    if constexpr (Op == kAdd || Op == kSub) {
        u32 ca = Clean(a), cb = Clean(b);
        AlignOperands(ca, cb);
        return Result(Op == kAdd ? Value(ca) + Value(cb) : Value(ca) - Value(cb));
    }
    const double x = Value(a), y = Value(b);
    if constexpr (Op == kMul) return Result(x * y);
    u32 total = Clean(acc), product = Result(x * y);
    AlignOperands(total, product);
    if constexpr (Op == kMadd) return Result(Value(total) + Value(product));
    else return Result(Value(total) - Value(product));
}

inline u32 Binary(int op, u32 a, u32 b, u32 acc) {
    switch (op) {
        case kAdd: return BinaryOf<kAdd>(a, b, acc);
        case kSub: return BinaryOf<kSub>(a, b, acc);
        case kMul: return BinaryOf<kMul>(a, b, acc);
        case kMadd: return BinaryOf<kMadd>(a, b, acc);
        case kMsub: return BinaryOf<kMsub>(a, b, acc);
        case kMax_: return BinaryOf<kMax_>(a, b, acc);
        case kMini: return BinaryOf<kMini>(a, b, acc);
    }
    return 0;
}

#if PS2RT_VU_SSE
/*
 * All four lanes at once in single precision. The model computes in double
 * and narrows (BinaryOf); that is the same number: the operands are clean -
 * no infinities, NaNs or denormals - and double's 53 bits are at least
 * 2 x 24 + 2, so rounding an add, a subtract or a multiply first to double
 * and then to single gives the single-precision result rounded once (and an
 * overflow the infinity Result saturates). Result is Clean's transform of
 * the bits. RunFmacScalar is the model; VuLanesAreTheModel (host tests)
 * compares the two.
 */
PS2RT_VU_INLINE __m128i CleanLanes(__m128i v) {
#if PS2RT_VU_SSE41
    // The magnitude clamped to the largest finite (an exponent of 255 is
    // above it, whatever the mantissa), zero below the smallest normal, and
    // the sign put back: Clean's three cases in six operations.
    const __m128i magnitude = _mm_and_si128(v, _mm_set1_epi32(0x7FFFFFFF));
    const __m128i sign = _mm_xor_si128(v, magnitude);
    const __m128i clamped = _mm_min_epu32(magnitude, _mm_set1_epi32(int(kMax)));
    const __m128i denormal = _mm_cmpgt_epi32(_mm_set1_epi32(0x00800000), magnitude);
    return _mm_or_si128(sign, _mm_andnot_si128(denormal, clamped));
#else
    const __m128i exponent = _mm_and_si128(v, _mm_set1_epi32(0x7F800000));
    const __m128i sign = _mm_and_si128(v, _mm_set1_epi32(int(kSign)));
    const __m128i zero = _mm_cmpeq_epi32(exponent, _mm_setzero_si128());
    const __m128i huge = _mm_cmpeq_epi32(exponent, _mm_set1_epi32(0x7F800000));
    const __m128i saturated = _mm_or_si128(sign, _mm_set1_epi32(int(kMax)));
    v = _mm_or_si128(_mm_andnot_si128(zero, v), _mm_and_si128(zero, sign));
    return _mm_or_si128(_mm_andnot_si128(huge, v), _mm_and_si128(huge, saturated));
#endif
}

/* Clean for code that runs with the SSE control word's DAZ and FTZ on (a VU
   run: VuRounding in vu.cpp): the processor takes a denormal operand as a
   zero of its sign and gives a zero of its sign for a result that would be
   one - Clean's two lower cases - so of Clean only the exponent of 255 is
   left to do: the magnitude clamped to the largest finite, the sign kept.
   For values going into arithmetic, or coming out of it; a denormal stays
   as it was. */
PS2RT_VU_INLINE __m128i ClampLanes(__m128i v) {
#if PS2RT_VU_SSE41
    const __m128i magnitude = _mm_and_si128(v, _mm_set1_epi32(0x7FFFFFFF));
    const __m128i sign = _mm_xor_si128(v, magnitude);
    return _mm_or_si128(sign, _mm_min_epu32(magnitude, _mm_set1_epi32(int(kMax))));
#else
    return CleanLanes(v);
#endif
}

inline __m128 AsFloats(__m128i v) { return _mm_castsi128_ps(v); }
inline __m128i AsBits(__m128 v) { return _mm_castps_si128(v); }

/* `Known` says which operands the caller knows to be clean already (bit 0
   a, bit 1 b, bit 2 acc) in the lanes it keeps: Clean would leave them as
   they are. Compiled VU code knows it of fields an FMAC wrote earlier in
   the same block (vurecomp.py, `clean`). */
PS2RT_VU_INLINE __m128i OrderLanes(__m128i v) {
    const __m128i negative = _mm_srai_epi32(v, 31);
    const __m128i magnitude = _mm_and_si128(v, _mm_set1_epi32(0x7FFFFFFF));
    return _mm_sub_epi32(_mm_xor_si128(magnitude, negative), negative);
}

/* AlignOperands, four lanes at once: an operand 25 or more exponents below the other one a zero of its sign
   (an exponent of 0 - a denormal under DAZ - counts as 0, which it is there). */
PS2RT_VU_INLINE void AlignLanes(__m128i& a, __m128i& b) {
    const __m128i ea = _mm_srli_epi32(_mm_slli_epi32(a, 1), 24), eb = _mm_srli_epi32(_mm_slli_epi32(b, 1), 24);
    const __m128i d = _mm_sub_epi32(ea, eb);
    const __m128i sign = _mm_set1_epi32(int(kSign));
    const __m128i b_gone = _mm_cmpgt_epi32(d, _mm_set1_epi32(24)), a_gone = _mm_cmplt_epi32(d, _mm_set1_epi32(-24));
    b = _mm_andnot_si128(_mm_andnot_si128(sign, b_gone), b);
    a = _mm_andnot_si128(_mm_andnot_si128(sign, a_gone), a);
}

/* `Daz`: the caller runs with DAZ and FTZ on (compiled VU code), so
   ClampLanes does what CleanLanes would. */
template <int Op, int Known = 0, bool Daz = false>
PS2RT_VU_INLINE __m128i LanesSse(__m128i a, __m128i b, __m128i acc) {
    if constexpr (Op == kMax_ || Op == kMini) {
        // MAX takes b where a < b, MINI where b < a; otherwise a - by Order,
        // the operands as they are.
        const __m128i take_b = Op == kMax_ ? _mm_cmplt_epi32(OrderLanes(a), OrderLanes(b))
                                           : _mm_cmplt_epi32(OrderLanes(b), OrderLanes(a));
        return _mm_or_si128(_mm_andnot_si128(take_b, a), _mm_and_si128(take_b, b));
    }
    const auto clean = [](__m128i v) { return Daz ? ClampLanes(v) : CleanLanes(v); };
    // A result under DAZ and FTZ needs no cleaning: a VU run rounds toward
    // zero too (VuRounding), and there the sum, difference or product of
    // finite operands that overflows is the largest finite of its sign -
    // what the clamp would give - never an infinity, and never a NaN; a
    // denormal is flushed to a zero of its sign. (The clamp was ~15% of the
    // race's VU1 time.)
    const auto result = [](__m128i v) { return Daz ? v : CleanLanes(v); };
    if constexpr ((Known & 1) == 0) a = clean(a);
    if constexpr ((Known & 2) == 0) b = clean(b);
    if constexpr (Op == kAdd || Op == kSub) AlignLanes(a, b);
    if constexpr (Op == kAdd) return result(AsBits(_mm_add_ps(AsFloats(a), AsFloats(b))));
    else if constexpr (Op == kSub) return result(AsBits(_mm_sub_ps(AsFloats(a), AsFloats(b))));
    else if constexpr (Op == kMul) return result(AsBits(_mm_mul_ps(AsFloats(a), AsFloats(b))));
    else if constexpr (Op == kMadd || Op == kMsub) {
        __m128i product = result(AsBits(_mm_mul_ps(AsFloats(a), AsFloats(b))));
        __m128i total = (Known & 4) != 0 ? acc : clean(acc);
        AlignLanes(total, product);
        return result(AsBits(Op == kMadd ? _mm_add_ps(AsFloats(total), AsFloats(product))
                                         : _mm_sub_ps(AsFloats(total), AsFloats(product))));
    }
}
#endif

inline u32 FloatToInt(double number) {
    if (number >= 2147483647.0) return 0x7FFFFFFFu;
    if (number <= -2147483648.0) return 0x80000000u;
    return (u32)(s32)number;
}

/*
 * The EFU (ESIN, EATAN, EEXP, ...) as the hardware computes it, not as the
 * host's libm does: the series the unit evaluates (constants as Play! has them,
 * Source/ee/MA_VU_Lower.cpp; BSD-2, the numbers only), each power by repeated
 * multiplication, the terms summed left to right; its adds and multiplies
 * truncate (round toward zero), its divides and square roots round to nearest;
 * pi/4 is 0x3F490FDA (what EATANxy of equal operands returns). Checked against
 * ps2autotests vu/lower/efu.expected (the hardware's): 166 of its 208 results
 * bit for bit since 2026-11-17 (exponent-255 numbers inside the unit, see
 * below; 130 of 175 before), the rest 1-2 ulp off except ESIN of pi (a result
 * that cancels to almost nothing); the libm path matches 91, and outside the
 * series' range (ESIN of 1e38, EATANxy of zeros) it is billions of ulp away
 * where this is the hardware's number. host/tests/vu_efu_hardware.inc holds the
 * 166. g_efu_exact: on by default (vu.cpp; PS2HOST_VU_EFU=fast turns it off: the libm path).
 */
inline bool g_efu_exact = false;

namespace efu {

/* The unit's numbers (2026-11-17): an exponent of 255 is a number like any other there (2^128 to 2^129 - the
   hardware returns such results: EATAN of -1 0xFFC90FD7, ERCPR of 0 0x7FFFFFFF, ESQRT of 0x7FFFFFFF 0x5FB504F3
   from the operand's own value), denormals zero, no infinities or NaNs. And the rounding explicit (Narrow):
   before, a divide's or root's "nearest" was the host's cast, which a VU run's MXCSR (toward zero, VuRounding)
   made a truncation - 119 of the test's 208 results bit for bit in a VU run (130 only outside one). Now 166 in
   either mode (an offline check against efu.expected, an off-tree harness); the adds take the
   adder's alignment (AlignOperands). The rest are 1-2 ulp (EATAN one low, the roots and reciprocals +-1) and
   ESIN of pi. */
constexpr double kTwo128 = 3.4028236692093846346e38;
PS2RT_VU_INLINE double Ext(u32 bits) {
    const u32 exponent = bits & 0x7F800000u;
    if (exponent == 0) return (bits & kSign) ? -0.0 : 0.0;
    if (exponent != 0x7F800000u) {
        float f;
        std::memcpy(&f, &bits, 4);
        return f;
    }
    const double m = 1.0 + double(bits & 0x7FFFFFu) / 8388608.0;
    return ((bits & kSign) ? -m : m) * kTwo128;
}
// A double as the unit's number: truncated (toward zero) or rounded to nearest (ties to even), whatever the
// host's rounding mode; past exponent 255 saturated, below the smallest normal a zero of its sign.
// (From the double's own bits since 2026-11-23: frexp, ldexp, floor and fmod were 2.2 % of VIF1's worker in ZOE's
// boss fight, as efu::Exact + ldexp. The same numbers: the 24 bits kept are the leading 1 and the top 23 of the
// double's 52, the 29 below them the rest - over half, or half with the kept bits odd, rounds up.)
PS2RT_VU_INLINE u32 Narrow(double number, bool nearest) {
    std::uint64_t bits;
    std::memcpy(&bits, &number, 8);
    const int field = int((bits >> 52) & 0x7FFu);
    const std::uint64_t mantissa = bits & 0xFFFFFFFFFFFFFull;
    if (field == 0x7FF && mantissa) return 0x7FFFFFFFu;                // NaN
    const u32 sign = (bits >> 63) ? kSign : 0u;
    if (field == 0x7FF) return sign | 0x7FFFFFFFu;                      // infinity: saturated
    int exponent = field - 1023;
    if (field == 0 || exponent < -126) return sign;  // below the smallest normal (2^-126): a zero of its sign
    u32 whole = 0x800000u | u32(mantissa >> 29);                        // 2^23 .. 2^24 - 1
    if (nearest) {
        const std::uint64_t rest = mantissa & 0x1FFFFFFFull, half = 0x10000000ull;
        if (rest > half || (rest == half && (whole & 1u))) ++whole;
        if (whole == 0x1000000u) {
            whole = 0x800000u;
            ++exponent;
        }
    }
    const int biased = exponent + 127;
    if (biased > 255) return sign | 0x7FFFFFFFu;
    if (biased < 1) return sign;
    return sign | (u32(biased) << 23) | (u32(whole) & 0x7FFFFFu);
}
PS2RT_VU_INLINE u32 Trunc(double number) { return Narrow(number, false); }
PS2RT_VU_INLINE u32 Round(double number) { return Narrow(number, true); }
PS2RT_VU_INLINE double Add(double a, double b) {
    u32 x = Trunc(a), y = Trunc(b);
    AlignOperands(x, y);
    return Ext(Trunc(Ext(x) + Ext(y)));
}
PS2RT_VU_INLINE double Mul(double a, double b) { return Ext(Trunc(a * b)); }
PS2RT_VU_INLINE double Div(double a, double b) {
    if (b == 0.0) {
        const bool negative = a == 0.0 ? std::signbit(b) : std::signbit(a) != std::signbit(b);
        return Ext((negative ? kSign : 0u) | 0x7FFFFFFFu);
    }
    return Ext(Round(a / b));
}
PS2RT_VU_INLINE double Sqrt(double a) { return Ext(Round(std::sqrt(std::fabs(a)))); }
PS2RT_VU_INLINE double Bits(u32 bits) { return Ext(bits); }

template <int N>
PS2RT_VU_INLINE double Series(double t, const int (&exponents)[N], const u32 (&constants)[N]) {
    double sum = 0.0;
    for (int k = 0; k < N; ++k) {
        double power = t;
        for (int e = 1; e < exponents[k]; ++e) power = Mul(power, t);
        const double term = Mul(power, Bits(constants[k]));
        sum = k == 0 ? term : Add(sum, term);
    }
    return sum;
}

PS2RT_VU_INLINE double AtanOf(double t) {
    static constexpr int kExp[8] = {1, 3, 5, 7, 9, 11, 13, 15};
    static constexpr u32 kC[8] = {0x3F7FFFF5u, 0xBEAAA61Cu, 0x3E4C40A6u, 0xBE0E6C63u,
                                  0x3DC577DFu, 0xBD6501C4u, 0x3CB31652u, 0xBB84D7E7u};
    return Add(Series(t, kExp, kC), Bits(0x3F490FDAu));
}

PS2RT_VU_INLINE double SumOfSquares(const u32* v) {
    const double x = Ext(v[0]), y = Ext(v[1]), z = Ext(v[2]);
    return Add(Add(Mul(x, x), Mul(y, y)), Mul(z, z));
}

/* The P register's value for EFU instruction `index` (0x70-0x7E: the lower
   opcode's index, as vu.cpp and vurecomp.py number them) on VF lanes `v`,
   `fsf` the scalar ones' lane. The operands as the unit's numbers (Ext). */
inline u32 Exact(int index, const u32* v, int fsf) {
    switch (index) {
        case 0x70: return Trunc(SumOfSquares(v));                                       // ESADD
        case 0x71: return Round(Div(1.0, SumOfSquares(v)));                             // ERSADD
        case 0x72: return Round(Sqrt(SumOfSquares(v)));                                 // ELENG
        case 0x73: return Round(Div(1.0, Sqrt(SumOfSquares(v))));                       // ERLENG
        case 0x74: {                                                                    // EATANxy
            const double x = Ext(v[0]), y = Ext(v[1]);
            return Trunc(AtanOf(Div(Add(y, -x), Add(y, x))));
        }
        case 0x75: {                                                                    // EATANxz
            const double x = Ext(v[0]), z = Ext(v[2]);
            return Trunc(AtanOf(Div(Add(z, -x), Add(z, x))));
        }
        case 0x76: return Trunc(Add(Add(Add(Ext(v[0]), Ext(v[1])), Ext(v[2])), Ext(v[3])));  // ESUM
        case 0x78: return Round(Sqrt(Ext(v[fsf])));                                     // ESQRT
        case 0x79: return Round(Div(1.0, Sqrt(Ext(v[fsf]))));                           // ERSQRT
        case 0x7A: return Round(Div(1.0, Ext(v[fsf])));                                 // ERCPR
        case 0x7C: {                                                                    // ESIN
            static constexpr int kExp[5] = {1, 3, 5, 7, 9};
            static constexpr u32 kC[5] = {0x3F800000u, 0xBE2AAAA4u, 0x3C08873Eu, 0xB94FB21Fu, 0x362E9C14u};
            return Trunc(Series(Ext(v[fsf]), kExp, kC));
        }
        case 0x7D: {                                                                    // EATAN
            const double x = Ext(v[fsf]);
            return Trunc(AtanOf(Div(Add(x, -1.0), Add(x, 1.0))));
        }
        case 0x7E: {                                                                    // EEXP
            static constexpr int kExp[6] = {1, 2, 3, 4, 5, 6};
            static constexpr u32 kC[6] = {0x3E7FFFA8u, 0x3D0007F4u, 0x3B29D3FFu, 0x3933E553u, 0x36B63510u,
                                          0x353961ACu};
            double s = Add(Series(Ext(v[fsf]), kExp, kC), 1.0);
            s = Mul(s, s);
            s = Mul(s, s);
            return Round(Div(1.0, s));
        }
    }
    return 0;
}

}  // namespace efu

}  // namespace lifted

#endif  /* PS2RT_VU_MATH_H */
