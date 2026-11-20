/*
 * The lifted code's arithmetic helpers, written to match engine/ps2decomp/emu.py.
 */

#include "ps2rt/lifted.h"

#include <cmath>
#include <cstring>
#if defined(__SSE2__) || defined(_M_X64)
#include <emmintrin.h>
#endif

namespace lifted {

void (*loop_head_hook)(Context& c, u32 address) = nullptr;
bool (*cop0_condition_hook)() = nullptr;

void BudgetSpent(Context& c, u32 address) {
    if (loop_head_hook) loop_head_hook(c, address);
    else throw Abort{address, 2};
}

void Multiply(u64& hi, u64& lo, u64 a, u64 b, bool is_signed, bool accumulate) {
    const u64 product = is_signed
        ? (u64)((s64)(s32)(u32)a * (s64)(s32)(u32)b)
        : (u64)(u32)a * (u64)(u32)b;
    u64 total = product;
    if (accumulate) {
        total += ((hi & 0xFFFFFFFFull) << 32) | (lo & 0xFFFFFFFFull);
    }
    lo = Sx32(total);
    hi = Sx32(total >> 32);
}

/* As the EE divides (emu.divide): a zero divisor gives LO = 1 for a negative
   signed dividend, else -1, and the dividend in HI. The signed case is computed in
   64 bits, where INT_MIN / -1 is defined. */
void Divide(u64& hi, u64& lo, u64 a, u64 b, bool is_signed) {
    if (is_signed) {
        const s64 x = (s32)(u32)a, y = (s32)(u32)b;
        if (y == 0) {
            lo = x < 0 ? 1 : ~0ull;
            hi = (u64)x;
            return;
        }
        const s64 q = x / y;
        lo = Sx32((u64)q);
        hi = Sx32((u64)(x - y * q));
    } else {
        const u32 x = (u32)a, y = (u32)b;
        if (y == 0) {
            lo = ~0ull;
            hi = Sx32(x);
            return;
        }
        lo = Sx32(x / y);
        hi = Sx32(x % y);
    }
}

u32 FloatMulAdd(u32 acc, u32 a, u32 b, bool subtract) {
    // Operands as the FPU reads them (a denormal is zero), the result as every
    // FPU result is narrowed (eefpu.cpp).
    const auto operand = [](u32 bits) { return generated::FloatOperand(bits); };
    const double x = operand(acc);
    const double product = operand(a) * operand(b);
    return generated::FloatNarrow(subtract ? x - product : x + product);
}

void AddBytes(u64& lo, u64& hi, u64 alo, u64 ahi, u64 blo, u64 bhi, bool subtract) {
    u64 out[2] = {0, 0};
    const u64 a[2] = {alo, ahi}, b[2] = {blo, bhi};
    for (int half = 0; half < 2; ++half) {
        for (int lane = 0; lane < 8; ++lane) {
            const u32 x = (u32)(a[half] >> (lane * 8)) & 0xFF;
            const u32 y = (u32)(b[half] >> (lane * 8)) & 0xFF;
            const u32 z = (subtract ? x - y : x + y) & 0xFF;
            out[half] |= (u64)z << (lane * 8);
        }
    }
    lo = out[0];
    hi = out[1];
}

namespace {

u32 Lane32(int op, u32 a, u32 b) {
    switch (op) {
        case kPmaxw: return (s32)a >= (s32)b ? a : b;
        case kPminw: return (s32)a <= (s32)b ? a : b;
        case kPsubw: return a - b;
        case kPaddw: return a + b;
        case kPadduw: { const u64 sum = (u64)a + b; return sum > 0xFFFFFFFFull ? 0xFFFFFFFFu : (u32)sum; }
        case kPabsw: { const s64 x = (s32)b; const s64 m = x < 0 ? -x : x;
                       return m > 0x7FFFFFFF ? 0x7FFFFFFFu : (u32)m; }
    }
    return 0;
}

/* Comparisons and halfword max/min, lane by lane at `width` bits. */
u64 NarrowLanes(int op, u64 a, u64 b, u32 width) {
    const u64 mask = width == 64 ? ~0ull : (1ull << width) - 1, sign = 1ull << (width - 1);
    u64 out = 0;
    for (u32 shift = 0; shift < 64; shift += width) {
        const u64 x = (a >> shift) & mask, y = (b >> shift) & mask;
        const s64 sx = (s64)(x ^ sign) - (s64)sign, sy = (s64)(y ^ sign) - (s64)sign;
        u64 lane = 0;
        switch (op) {
            case kPcgtb: case kPcgth: case kPcgtw: lane = sx > sy ? mask : 0; break;
            case kPceqb: case kPceqh: case kPceqw: lane = x == y ? mask : 0; break;
            case kPmaxh: lane = sx >= sy ? x : y; break;
            case kPminh: lane = sx <= sy ? x : y; break;
        }
        out |= lane << shift;
    }
    return out;
}

u64 LanesOf(int op, u64 a, u64 b) {
    u64 out = 0;
    switch (op) {
        case kPcgtb: case kPceqb: return NarrowLanes(op, a, b, 8);
        case kPcgth: case kPceqh: case kPmaxh: case kPminh: return NarrowLanes(op, a, b, 16);
        case kPcgtw: case kPceqw: return NarrowLanes(op, a, b, 32);
        default: break;
    }
    if (op == kPaddh) {
        for (int shift = 0; shift < 64; shift += 16)
            out |= ((((a >> shift) & 0xFFFF) + ((b >> shift) & 0xFFFF)) & 0xFFFF) << shift;
        return out;
    }
    if (op == kPaddub) {
        for (int shift = 0; shift < 64; shift += 8) {
            const u32 sum = (u32)((a >> shift) & 0xFF) + (u32)((b >> shift) & 0xFF);
            out |= (u64)(sum > 0xFF ? 0xFF : sum) << shift;
        }
        return out;
    }
    for (int shift = 0; shift < 64; shift += 32) {
        out |= (u64)Lane32(op, (u32)(a >> shift), (u32)(b >> shift)) << shift;
    }
    return out;
}

}  // namespace

#if defined(__SSE2__) || defined(_M_X64)
/* The same lanes in SSE2 for the operations it has exactly (signed compares,
   halfword max and min, wrapping and unsigned-saturating adds); `done` false
   for the rest. LanesAreScalar (host tests) compares the two. */
bool LanesSse2(int op, u64& lo, u64& hi, u64 alo, u64 ahi, u64 blo, u64 bhi) {
    const __m128i a = _mm_set_epi64x(s64(ahi), s64(alo)), b = _mm_set_epi64x(s64(bhi), s64(blo));
    __m128i r;
    switch (op) {
        case kPcgtb: r = _mm_cmpgt_epi8(a, b); break;
        case kPcgth: r = _mm_cmpgt_epi16(a, b); break;
        case kPcgtw: r = _mm_cmpgt_epi32(a, b); break;
        case kPceqb: r = _mm_cmpeq_epi8(a, b); break;
        case kPceqh: r = _mm_cmpeq_epi16(a, b); break;
        case kPceqw: r = _mm_cmpeq_epi32(a, b); break;
        case kPmaxh: r = _mm_max_epi16(a, b); break;
        case kPminh: r = _mm_min_epi16(a, b); break;
        case kPaddh: r = _mm_add_epi16(a, b); break;
        case kPaddub: r = _mm_adds_epu8(a, b); break;
        case kPaddw: r = _mm_add_epi32(a, b); break;
        case kPsubw: r = _mm_sub_epi32(a, b); break;
        default: return false;
    }
    lo = u64(_mm_cvtsi128_si64(r));
    hi = u64(_mm_cvtsi128_si64(_mm_unpackhi_epi64(r, r)));
    return true;
}
#endif

void LanesScalar(int op, u64& lo, u64& hi, u64 alo, u64 ahi, u64 blo, u64 bhi) {
    lo = LanesOf(op, alo, blo);
    hi = LanesOf(op, ahi, bhi);
}

void Lanes(int op, u64& lo, u64& hi, u64 alo, u64 ahi, u64 blo, u64 bhi) {
#if defined(__SSE2__) || defined(_M_X64)
    if (LanesSse2(op, lo, hi, alo, ahi, blo, bhi)) return;
#endif
    LanesScalar(op, lo, hi, alo, ahi, blo, bhi);
}

void ShiftLanesScalar(u64& lo, u64& hi, u64 blo, u64 bhi, u32 width, u32 kind, u32 amount) {
    const u64 mask = (1ull << width) - 1;
    amount &= width - 1;
    const u64 in[2] = {blo, bhi};
    u64 out[2] = {0, 0};
    for (int half = 0; half < 2; ++half) {
        for (u32 shift = 0; shift < 64; shift += width) {
            u64 lane = (in[half] >> shift) & mask;
            if (kind == 0) {
                lane = (lane << amount) & mask;
            } else if (kind == 1) {
                lane >>= amount;
            } else {
                const u64 sign = 1ull << (width - 1);
                const s64 value = (s64)(lane ^ sign) - (s64)sign;   /* sign-extend the lane */
                lane = (u64)(value >> amount) & mask;
            }
            out[half] |= lane << shift;
        }
    }
    lo = out[0];
    hi = out[1];
}

/* Halfword and word lanes in SSE2 (psllw/psrlw/psraw, and the d forms): the
   same shifts, eight or four lanes at once; a movie's decoder makes them by
   the million (Resident Evil Outbreak's Sofdec). */
void ShiftLanes(u64& lo, u64& hi, u64 blo, u64 bhi, u32 width, u32 kind, u32 amount) {
#if defined(__SSE2__) || defined(_M_X64)
    if ((width == 16 || width == 32) && kind <= 2) {
        const __m128i x = _mm_set_epi64x(s64(bhi), s64(blo));
        const __m128i n = _mm_cvtsi32_si128(int(amount & (width - 1)));
        __m128i r;
        if (width == 16) r = kind == 0 ? _mm_sll_epi16(x, n) : kind == 1 ? _mm_srl_epi16(x, n) : _mm_sra_epi16(x, n);
        else r = kind == 0 ? _mm_sll_epi32(x, n) : kind == 1 ? _mm_srl_epi32(x, n) : _mm_sra_epi32(x, n);
        lo = u64(_mm_cvtsi128_si64(r));
        hi = u64(_mm_cvtsi128_si64(_mm_unpackhi_epi64(r, r)));
        return;
    }
#endif
    ShiftLanesScalar(lo, hi, blo, bhi, width, kind, amount);
}

void ShiftHalfwordsRight(u64& lo, u64& hi, u64 blo, u64 bhi, u32 amount) {
    ShiftLanes(lo, hi, blo, bhi, 16, 1, amount);
}

void InterleaveScalar(u64& lo, u64& hi, u64 a, u64 b, u32 width) {
    const u64 mask = (1ull << width) - 1;
    u64 out[2] = {0, 0};
    for (u32 i = 0; i < 64 / width; ++i) {
        const u32 even = 2 * i * width, odd = (2 * i + 1) * width;
        out[even / 64] |= ((b >> (i * width)) & mask) << (even % 64);
        out[odd / 64] |= ((a >> (i * width)) & mask) << (odd % 64);
    }
    lo = out[0];
    hi = out[1];
}

/* PEXTLB, PEXTLH, PEXTLW: SSE2's unpack does the same (b's lanes even). */
void Interleave(u64& lo, u64& hi, u64 a, u64 b, u32 width) {
#if defined(__SSE2__) || defined(_M_X64)
    if (width == 8 || width == 16 || width == 32) {
        const __m128i x = _mm_cvtsi64_si128(s64(b)), y = _mm_cvtsi64_si128(s64(a));
        const __m128i r = width == 8 ? _mm_unpacklo_epi8(x, y) : width == 16 ? _mm_unpacklo_epi16(x, y)
                                                                             : _mm_unpacklo_epi32(x, y);
        lo = u64(_mm_cvtsi128_si64(r));
        hi = u64(_mm_cvtsi128_si64(_mm_unpackhi_epi64(r, r)));
        return;
    }
#endif
    InterleaveScalar(lo, hi, a, b, width);
}

void PackHalfwords(u64& lo, u64& hi, u64 alo, u64 ahi, u64 blo, u64 bhi) {
    u64 low = 0, high = 0;
    for (int i = 0; i < 2; ++i) {
        low |= ((blo >> (32 * i)) & 0xFFFF) << (16 * i);
        low |= ((bhi >> (32 * i)) & 0xFFFF) << (16 * (i + 2));
        high |= ((alo >> (32 * i)) & 0xFFFF) << (16 * i);
        high |= ((ahi >> (32 * i)) & 0xFFFF) << (16 * (i + 2));
    }
    lo = low;
    hi = high;
}

void PackBytes(u64& lo, u64& hi, u64 alo, u64 ahi, u64 blo, u64 bhi) {
    u64 low = 0, high = 0;
    for (int i = 0; i < 4; ++i) {
        low |= ((blo >> (16 * i)) & 0xFF) << (8 * i);
        low |= ((bhi >> (16 * i)) & 0xFF) << (8 * (i + 4));
        high |= ((alo >> (16 * i)) & 0xFF) << (8 * i);
        high |= ((ahi >> (16 * i)) & 0xFF) << (8 * (i + 4));
    }
    lo = low;
    hi = high;
}

void FunnelShift(u64& lo, u64& hi, u64 alo, u64 ahi, u64 blo, u64 bhi, u32 sa) {
    /* The 256-bit value ahi:alo:bhi:blo, shifted right by sa bits; the low 128. */
    const u64 words[4] = {blo, bhi, alo, ahi};
    sa &= 0xFF;
    const u32 skip = sa / 64, bits = sa % 64;
    u64 out[2];
    for (u32 i = 0; i < 2; ++i) {
        const u64 low = skip + i < 4 ? words[skip + i] : 0;
        const u64 high = skip + i + 1 < 4 ? words[skip + i + 1] : 0;
        out[i] = bits ? (low >> bits) | (high << (64 - bits)) : low;
    }
    lo = out[0];
    hi = out[1];
}

u32 FloatRsqrt(u32 fs, u32 ft) {
    // eefpu.py rsqrt: the root rounded to nearest (sqrt.s), its reciprocal rounded to nearest, then the
    // product truncated - the console's three steps; a zero root gives the largest magnitude with the
    // numerator's sign.
    const double root = generated::FloatOperand(generated::FloatSqrt(ft));
    if (root == 0.0) return (fs & 0x80000000u) | 0x7FFFFFFFu;
    const double reciprocal = generated::FloatOperand(generated::FloatNarrowRounded(1.0 / root, true));
    return generated::FloatNarrowRounded(generated::FloatOperand(fs) * reciprocal, false);
}

u64 LoadWordLeft(u32 address, u64 old) {
    const u32 keep = (3 - (address & 3)) * 8;
    const u32 word = (u32)generated::ReadU32(address & ~3u);
    const u32 low = keep ? (u32)old & ((1u << keep) - 1) : 0;
    return Sx32((keep ? word << keep : word) | low);
}

u64 LoadWordRight(u32 address, u64 old) {
    const u32 drop = (address & 3) * 8;
    const u32 word = (u32)generated::ReadU32(address & ~3u);
    const u32 merged = (word >> drop) | ((u32)old & ~(0xFFFFFFFFu >> drop));
    return drop == 0 ? Sx32(merged) : (old & 0xFFFFFFFF00000000ull) | merged;
}

u64 LoadDoubleLeft(u32 address, u64 old) {
    const u32 keep = (7 - (address & 7)) * 8;
    const u64 value = generated::ReadU64(address & ~7u);
    return keep ? (value << keep) | (old & ((1ull << keep) - 1)) : value;
}

u64 LoadDoubleRight(u32 address, u64 old) {
    const u32 drop = (address & 7) * 8;
    const u64 value = generated::ReadU64(address & ~7u);
    return (value >> drop) | (old & ~(~0ull >> drop));
}

void StoreLeft(u32 address, u64 value, u32 size) {
    const u32 offset = address & (size - 1), count = offset + 1;
    const u64 data = (size == 8 ? value : value & 0xFFFFFFFFull) >> (8 * (size - count));
    for (u32 i = 0; i < count; ++i)
        generated::Write8(address - offset + i, (u32)(data >> (8 * i)) & 0xFF);
}

void StoreRight(u32 address, u64 value, u32 size) {
    const u32 count = size - (address & (size - 1));
    for (u32 i = 0; i < count; ++i)
        generated::Write8(address + i, (u32)(value >> (8 * i)) & 0xFF);
}

/* The rarer MMI operations, decoded from the instruction word: emu.py's MMI_REST, case for case
   (M8 gate 2026-11-14, ps2autotests cpu/ee_simd). HI and LO are (hi1:hi) and (lo1:lo), words 0-3. */
namespace {

struct Q {
    u32 w[4];
    u16 h(int i) const { return (u16)(w[i / 2] >> (16 * (i % 2))); }
    void set_h(int i, u32 v) {
        const int s = 16 * (i % 2);
        w[i / 2] = (w[i / 2] & ~(0xFFFFu << s)) | ((v & 0xFFFFu) << s);
    }
    u8 b(int i) const { return (u8)(w[i / 4] >> (8 * (i % 4))); }
    void set_b(int i, u32 v) {
        const int s = 8 * (i % 4);
        w[i / 4] = (w[i / 4] & ~(0xFFu << s)) | ((v & 0xFFu) << s);
    }
};

Q Load(u64 lo, u64 hi) { return Q{{(u32)lo, (u32)(lo >> 32), (u32)hi, (u32)(hi >> 32)}}; }
u64 Low(const Q& q) { return (u64)q.w[0] | (u64)q.w[1] << 32; }
u64 High(const Q& q) { return (u64)q.w[2] | (u64)q.w[3] << 32; }

void Store(Context& c, u32 rd, const Q& q) {
    if (rd == 0) return;
    c.r[rd] = Low(q);
    c.rh[rd] = High(q);
}

s64 Clamp(s64 v, s64 low, s64 high) { return v < low ? low : v > high ? high : v; }

/* padds/psubs (signed) and paddu/psubu (unsigned) over lanes of `width` bits. */
Q Saturating(const Q& a, const Q& b, int width, bool subtract, bool is_signed) {
    Q out{};
    const int lanes = 128 / width;
    for (int i = 0; i < lanes; ++i) {
        s64 x, y;
        // Each branch as s64 on its own: `c ? (s32)x : (u32)y` would convert the signed one to unsigned.
        if (width == 32) {
            x = is_signed ? (s64)(s32)a.w[i] : (s64)a.w[i];
            y = is_signed ? (s64)(s32)b.w[i] : (s64)b.w[i];
        } else if (width == 16) {
            x = is_signed ? (s64)(s16)a.h(i) : (s64)a.h(i);
            y = is_signed ? (s64)(s16)b.h(i) : (s64)b.h(i);
        } else {
            x = is_signed ? (s64)(s8)a.b(i) : (s64)a.b(i);
            y = is_signed ? (s64)(s8)b.b(i) : (s64)b.b(i);
        }
        const s64 low = is_signed ? -(1ll << (width - 1)) : 0;
        const s64 high = is_signed ? (1ll << (width - 1)) - 1 : (1ll << width) - 1;
        const u32 v = (u32)Clamp(subtract ? x - y : x + y, low, high);
        if (width == 32) out.w[i] = v;
        else if (width == 16) out.set_h(i, v);
        else out.set_b(i, v);
    }
    return out;
}

/* emu.divide on 32-bit operands: (LO, HI), sign-extended. */
void Div32(u32 a, u32 b, bool is_signed, u64& lo, u64& hi) { Divide(hi, lo, a, b, is_signed); }

}  // namespace

void Mmi(Context& c, u32 w) {
    const u32 rs = (w >> 21) & 31, rt = (w >> 16) & 31, rd = (w >> 11) & 31, sa = (w >> 6) & 31;
    const u32 funct = w & 0x3F;
    const Q a = Load(rs ? c.r[rs] : 0, rs ? c.rh[rs] : 0), b = Load(rt ? c.r[rt] : 0, rt ? c.rh[rt] : 0);
    Q lo = Load(c.lo, c.lo1), hi = Load(c.hi, c.hi1);
    const auto put_hilo = [&]() { c.lo = Low(lo); c.lo1 = High(lo); c.hi = Low(hi); c.hi1 = High(hi); };
    Q out{};
    if ((w >> 26) == 0) {                                   // SPECIAL: mfsa, mtsa
        if (funct == 0x28) { if (rd) c.r[rd] = c.sa; }
        else if (funct == 0x29) c.sa = (u32)(rs ? c.r[rs] : 0) & 0xF;      // in bytes (emu.py Machine.sa)
        return;
    }
    switch (funct) {
        case 0x04: {                                        // plzcw: rs's two low words
            u64 value = 0;
            for (int i = 0; i < 2; ++i) {
                u32 x = a.w[i];
                if (x & 0x80000000u) x = ~x;
                int length = 0;
                while (length < 32 && !(x & (0x80000000u >> length))) ++length;
                value |= (u64)(u32)(length - 1) << (32 * i);
            }
            if (rd) c.r[rd] = value;
            return;
        }
        case 0x30: {                                        // pmfhl.*
            if (sa == 0) out = Q{{lo.w[0], hi.w[0], lo.w[2], hi.w[2]}};
            else if (sa == 1) out = Q{{lo.w[1], hi.w[1], lo.w[3], hi.w[3]}};
            else if (sa == 2) {
                u64 parts[2];
                for (int i = 0; i < 2; ++i) {
                    const s64 whole = (s64)(((u64)hi.w[2 * i] << 32) | lo.w[2 * i]);
                    parts[i] = (u64)Clamp(whole, -(1ll << 31), (1ll << 31) - 1);
                }
                out = Load(parts[0], parts[1]);
            } else if (sa == 3) {
                const int src[8][2] = {{0, 0}, {0, 2}, {1, 0}, {1, 2}, {0, 4}, {0, 6}, {1, 4}, {1, 6}};
                for (int i = 0; i < 8; ++i) out.set_h(i, (src[i][0] ? hi : lo).h(src[i][1]));
            } else {
                const u32 words[8] = {lo.w[0], lo.w[1], hi.w[0], hi.w[1], lo.w[2], lo.w[3], hi.w[2], hi.w[3]};
                for (int i = 0; i < 8; ++i) out.set_h(i, (u32)Clamp((s32)words[i], -0x8000, 0x7FFF));
            }
            Store(c, rd, out);
            return;
        }
        case 0x31:                                          // pmthl.lw
            lo.w[0] = a.w[0]; hi.w[0] = a.w[1]; lo.w[2] = a.w[2]; hi.w[2] = a.w[3];
            put_hilo();
            return;
        case 0x08:                                          // MMI0
            switch (sa) {
                case 0x05: for (int i = 0; i < 8; ++i) out.set_h(i, a.h(i) - b.h(i)); break;   // psubh
                case 0x10: out = Saturating(a, b, 32, false, true); break;
                case 0x11: out = Saturating(a, b, 32, true, true); break;
                case 0x14: out = Saturating(a, b, 16, false, true); break;
                case 0x15: out = Saturating(a, b, 16, true, true); break;
                case 0x18: out = Saturating(a, b, 8, false, true); break;
                case 0x19: out = Saturating(a, b, 8, true, true); break;
                case 0x1E:                                  // pext5
                    for (int i = 0; i < 4; ++i) {
                        const u32 x = b.w[i];
                        out.w[i] = ((x & 0x1F) << 3) | (((x >> 5) & 0x1F) << 11) | (((x >> 10) & 0x1F) << 19)
                                 | (((x >> 15) & 1) << 31);
                    }
                    break;
                case 0x1F:                                  // ppac5
                    for (int i = 0; i < 4; ++i) {
                        const u32 x = b.w[i];
                        out.w[i] = ((x >> 3) & 0x1F) | (((x >> 11) & 0x1F) << 5) | (((x >> 19) & 0x1F) << 10)
                                 | (((x >> 31) & 1) << 15);
                    }
                    break;
                default: throw Abort{0, 1};
            }
            Store(c, rd, out);
            return;
        case 0x28:                                          // MMI1
            switch (sa) {
                case 0x04:                                  // padsbh
                    for (int i = 0; i < 8; ++i) out.set_h(i, i < 4 ? a.h(i) - b.h(i) : a.h(i) + b.h(i));
                    break;
                case 0x05:                                  // pabsh
                    for (int i = 0; i < 8; ++i) {
                        const s32 x = (s16)b.h(i);
                        out.set_h(i, (u32)(x < 0 ? (-x > 0x7FFF ? 0x7FFF : -x) : x));
                    }
                    break;
                case 0x11: out = Saturating(a, b, 32, true, false); break;    // psubuw
                case 0x14: out = Saturating(a, b, 16, false, false); break;   // padduh
                case 0x15: out = Saturating(a, b, 16, true, false); break;    // psubuh
                case 0x19: out = Saturating(a, b, 8, true, false); break;     // psubub
                default: throw Abort{0, 1};
            }
            Store(c, rd, out);
            return;
        case 0x09:                                          // MMI2
        case 0x29: {                                        // MMI3
            const bool three = funct == 0x29;
            const u32 op = (three ? 0x100 : 0) | sa;
            switch (op) {
                case 0x02: case 0x03: case 0x103: {         // psllvw, psrlvw, psravw
                    u64 parts[2];
                    for (int i = 0; i < 2; ++i) {
                        const u32 amount = a.w[2 * i] & 31, x = b.w[2 * i];
                        const u32 v = op == 0x02 ? x << amount : op == 0x03 ? x >> amount : (u32)((s32)x >> amount);
                        parts[i] = Sx32(v);
                    }
                    Store(c, rd, Load(parts[0], parts[1]));
                    return;
                }
                case 0x00: case 0x04: case 0x0C: case 0x100: case 0x10C: {   // pmaddw pmsubw pmultw pmadduw pmultuw
                    const bool is_signed = !three;
                    const int accumulate = (op == 0x00 || op == 0x100) ? 1 : op == 0x04 ? -1 : 0;
                    u64 results[2];
                    for (int k = 0; k < 2; ++k) {
                        const int i = 2 * k;
                        u64 product = is_signed ? (u64)((s64)(s32)a.w[i] * (s64)(s32)b.w[i])
                                                : (u64)a.w[i] * (u64)b.w[i];
                        const u64 acc = ((u64)hi.w[i] << 32) | lo.w[i];
                        if (accumulate > 0) product = acc + product;
                        else if (accumulate < 0) product = acc - product;
                        lo.w[i] = (u32)product; lo.w[i + 1] = (product & 0x80000000u) ? 0xFFFFFFFFu : 0;
                        hi.w[i] = (u32)(product >> 32); hi.w[i + 1] = (product >> 63) ? 0xFFFFFFFFu : 0;
                        results[k] = product;
                    }
                    put_hilo();
                    Store(c, rd, Load(results[0], results[1]));
                    return;
                }
                case 0x0D: case 0x10D:                      // pdivw, pdivuw
                    Div32(a.w[0], b.w[0], !three, c.lo, c.hi);
                    Div32(a.w[2], b.w[2], !three, c.lo1, c.hi1);
                    return;
                case 0x1D: {                                // pdivbw
                    const u32 divisor = (u32)(s32)(s16)b.h(0);
                    for (int i = 0; i < 4; ++i) {
                        u64 q, r;
                        Div32(a.w[i], divisor, true, q, r);
                        lo.w[i] = (u32)q;
                        hi.w[i] = (u32)r;
                    }
                    put_hilo();
                    return;
                }
                case 0x10: case 0x14: {                     // pmaddh, pmsubh
                    const int place[8][2] = {{0, 0}, {0, 1}, {1, 0}, {1, 1}, {0, 2}, {0, 3}, {1, 2}, {1, 3}};
                    u32 products[8];
                    for (int i = 0; i < 8; ++i) {
                        Q& target = place[i][0] ? hi : lo;
                        const s32 p = (s32)(s16)a.h(i) * (s32)(s16)b.h(i);
                        const u32 acc = target.w[place[i][1]];
                        products[i] = op == 0x10 ? acc + (u32)p : acc - (u32)p;
                        target.w[place[i][1]] = products[i];
                    }
                    put_hilo();
                    Store(c, rd, Q{{products[0], products[2], products[4], products[6]}});
                    return;
                }
                case 0x11: case 0x15: {                     // phmadh, phmsbh
                    // Each HI/LO doubleword: the pair's sum (difference) below, its high product above - PHMSBH
                    // that product inverted (ps2autotests cpu/ee_simd/muldiv, the console's HI/LO).
                    u32 sums[4], highs[4];
                    for (int j = 0; j < 4; ++j) {
                        const s32 high = (s32)(s16)a.h(2 * j + 1) * (s32)(s16)b.h(2 * j + 1);
                        const s32 low = (s32)(s16)a.h(2 * j) * (s32)(s16)b.h(2 * j);
                        sums[j] = op == 0x11 ? (u32)high + (u32)low : (u32)high - (u32)low;
                        highs[j] = op == 0x11 ? (u32)high : ~(u32)high;
                    }
                    lo.w[0] = sums[0]; lo.w[1] = highs[0]; hi.w[0] = sums[1]; hi.w[1] = highs[1];
                    lo.w[2] = sums[2]; lo.w[3] = highs[2]; hi.w[2] = sums[3]; hi.w[3] = highs[3];
                    put_hilo();
                    Store(c, rd, Q{{sums[0], sums[1], sums[2], sums[3]}});
                    return;
                }
                case 0x08: Store(c, rd, hi); return;        // pmfhi
                case 0x09: Store(c, rd, lo); return;        // pmflo
                case 0x108:                                 // pmthi
                    c.hi = rs ? c.r[rs] : 0; c.hi1 = rs ? c.rh[rs] : 0; return;
                case 0x109:                                 // pmtlo
                    c.lo = rs ? c.r[rs] : 0; c.lo1 = rs ? c.rh[rs] : 0; return;
                case 0x0A:                                  // pinth
                    for (int i = 0; i < 8; ++i) out.set_h(i, i % 2 == 0 ? b.h(i / 2) : a.h(4 + i / 2));
                    Store(c, rd, out);
                    return;
                case 0x10A:                                 // pinteh
                    for (int i = 0; i < 8; ++i) out.set_h(i, i % 2 == 0 ? b.h(i) : a.h(i - 1));
                    Store(c, rd, out);
                    return;
                case 0x1A: case 0x1B: case 0x11A: {         // pexeh, prevh, pexch
                    const int orders[3][4] = {{2, 1, 0, 3}, {3, 2, 1, 0}, {0, 2, 1, 3}};
                    const int* order = orders[op == 0x1A ? 0 : op == 0x1B ? 1 : 2];
                    for (int i = 0; i < 8; ++i) out.set_h(i, b.h(4 * (i / 4) + order[i % 4]));
                    Store(c, rd, out);
                    return;
                }
                default: throw Abort{0, 1};
            }
        }
        default: throw Abort{0, 1};
    }
}

}  // namespace lifted
