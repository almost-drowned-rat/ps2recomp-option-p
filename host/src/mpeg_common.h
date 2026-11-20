/*
 * MPEG-2 video's parts shared by the IPU model (ipu.cpp, tier E) and the
 * native libmpeg-level decoder (design 7f, tier N): the VLC tables (ISO/IEC
 * 13818-2 annex B, ipu_tables.inc) as 16-bit lookups, macroblock_type by
 * picture type, the scans, the non-linear quantiser scale and the IDCT - one
 * copy, so an intra block comes out of both bit for bit alike. Moved here
 * unchanged from ipu.cpp (2026-11-15).
 */

#ifndef PS2HOST_MPEG_COMMON_H
#define PS2HOST_MPEG_COMMON_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

#if defined(__AVX2__)
#include <immintrin.h>
#endif

#include "ps2host/memory.h"

namespace ps2host {
namespace mpeg {

struct Vlc {
    u16 code;
    u8 length;
    s32 value;
};
constexpr s32 kEscape = -1, kEndOfBlock = -2;

#include "ipu_tables.inc"

// A VLC table as a lookup on the next 16 bits: the length (0: no such
// code) and the value.
struct Lookup {
    std::vector<u8> length;
    std::vector<s32> value;
};

template <std::size_t N>
Lookup Build(const Vlc (&codes)[N]) {
    Lookup t{std::vector<u8>(65536, 0), std::vector<s32>(65536, 0)};
    for (const Vlc& v : codes) {
        const u32 first = u32(v.code) << (16 - v.length), count = 1u << (16 - v.length);
        for (u32 i = 0; i < count; ++i) {
            t.length[first + i] = v.length;
            t.value[first + i] = v.value;
        }
    }
    return t;
}

inline const Lookup& DcLuminance() { static const Lookup t = Build(kDcLuminance); return t; }
inline const Lookup& DcChrominance() { static const Lookup t = Build(kDcChrominance); return t; }
inline const Lookup& AddressIncrement() { static const Lookup t = Build(kAddressIncrement); return t; }
inline const Lookup& BlockPattern() { static const Lookup t = Build(kBlockPattern); return t; }
inline const Lookup& MotionCode() { static const Lookup t = Build(kMotionCode); return t; }
inline const Lookup& CoefficientsB14() { static const Lookup t = Build(kCoefficientsB14); return t; }
inline const Lookup& CoefficientsB15() { static const Lookup t = Build(kCoefficientsB15); return t; }

// B.2-B.4 macroblock_type, by picture type (1 I, 2 P, 3 B, 4 D): value =
// quant 0x10 | motion_forward 8 | motion_backward 4 | pattern 2 | intra 1.
const Vlc kTypeI[] = {{0x1, 1, 0x01}, {0x1, 2, 0x11}};
const Vlc kTypeP[] = {{0x1, 1, 0x0A}, {0x1, 2, 0x02}, {0x1, 3, 0x08}, {0x3, 5, 0x01},
                      {0x2, 5, 0x1A}, {0x1, 5, 0x12}, {0x1, 6, 0x11}};
const Vlc kTypeB[] = {{0x2, 2, 0x0C}, {0x3, 2, 0x0E}, {0x2, 3, 0x04}, {0x3, 3, 0x06},
                      {0x2, 4, 0x08}, {0x3, 4, 0x0A}, {0x3, 5, 0x01}, {0x2, 5, 0x1E},
                      {0x3, 6, 0x1A}, {0x2, 6, 0x16}, {0x1, 6, 0x11}};
const Vlc kTypeD[] = {{0x1, 1, 0x01}};
inline const Lookup& MacroblockType(u32 picture) {
    static const Lookup i = Build(kTypeI), p = Build(kTypeP), b = Build(kTypeB), d = Build(kTypeD);
    return picture == 2 ? p : picture == 3 ? b : picture == 4 ? d : i;
}

// Coefficient order: the zig-zag scan and MPEG-2's alternate scan, as the
// position in the block (row * 8 + column) of each coefficient in turn.
constexpr u8 kZigZag[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
                            12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
                            35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
                            58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};
constexpr u8 kAlternate[64] = {0,  8,  16, 24, 1,  9,  2,  10, 17, 25, 32, 40, 48, 56, 57, 49,
                               41, 33, 26, 18, 3,  11, 4,  12, 19, 27, 34, 42, 50, 58, 35, 43,
                               51, 59, 20, 28, 5,  13, 6,  14, 21, 29, 36, 44, 52, 60, 37, 45,
                               53, 61, 22, 30, 7,  15, 23, 31, 38, 46, 54, 62, 39, 47, 55, 63};
// quantiser_scale_code to quantiser_scale, q_scale_type 1 (table 7-6).
constexpr u8 kNonLinearScale[32] = {0,  1,  2,  3,  4,  5,  6,  7,  8,  10, 12, 14, 16, 18,  20,  22,
                                    24, 28, 32, 36, 40, 44, 48, 52, 56, 64, 72, 80, 88, 96, 104, 112};

// The IDCT, separable, in double precision and rounded once (IEEE 1180's
// reference): out[y][x] = sum C(u) C(v) / 4 F[v][u] cos((2x+1)u pi/16) cos((2y+1)v pi/16).
inline void Idct(const s32 in[64], s32 out[64]) {
    // basis[x][u], and its transpose (column u's eight terms, by x, together).
    struct Basis {
        alignas(32) double c[8][8], t[8][8];
    };
    static const Basis basis = [] {
        Basis b{};
        const double pi = 3.14159265358979323846;
        for (int x = 0; x < 8; ++x)
            for (int u = 0; u < 8; ++u) {
                b.c[x][u] = 0.5 * (u == 0 ? 1.0 / std::sqrt(2.0) : 1.0) * std::cos((2 * x + 1) * u * pi / 16.0);
                b.t[u][x] = b.c[x][u];
            }
        return b;
    }();
    // Most coefficients are zero, and a zero term adds nothing to a sum (a
    // signed zero at most, which no later sum or the rounding sees): only the
    // others are summed, in the same order - the same result, bit for bit.
    alignas(32) double rows[64];
    int terms[8], live[8], live_rows = 0;
#if defined(__AVX2__)
    // Four of a row's outputs at a time: each lane's products and sums are the
    // scalar ones, in the same order (a multiply, then an add - never fused),
    // so the pictures are the same; a movie's macroblocks took 6% of the EE.
    for (int v = 0; v < 8; ++v) {
        int n = 0;
        for (int u = 0; u < 8; ++u)
            if (in[v * 8 + u]) terms[n++] = u;
        if (n) live[live_rows++] = v;
        __m256d low = _mm256_setzero_pd(), high = _mm256_setzero_pd();
        for (int k = 0; k < n; ++k) {
            const __m256d f = _mm256_set1_pd(double(in[v * 8 + terms[k]]));
            low = _mm256_add_pd(low, _mm256_mul_pd(_mm256_load_pd(&basis.t[terms[k]][0]), f));
            high = _mm256_add_pd(high, _mm256_mul_pd(_mm256_load_pd(&basis.t[terms[k]][4]), f));
        }
        _mm256_store_pd(&rows[v * 8], low);
        _mm256_store_pd(&rows[v * 8 + 4], high);
    }
    const __m256d half = _mm256_set1_pd(0.5);
    const __m128i lowest = _mm_set1_epi32(-256), highest = _mm_set1_epi32(255);
    for (int y = 0; y < 8; ++y) {
        __m256d low = _mm256_setzero_pd(), high = _mm256_setzero_pd();
        for (int k = 0; k < live_rows; ++k) {
            const __m256d b = _mm256_set1_pd(basis.c[y][live[k]]);
            low = _mm256_add_pd(low, _mm256_mul_pd(b, _mm256_load_pd(&rows[live[k] * 8])));
            high = _mm256_add_pd(high, _mm256_mul_pd(b, _mm256_load_pd(&rows[live[k] * 8 + 4])));
        }
        // floor(sum + 0.5), then to an integer (exact: it is whole) and clamped.
        const auto round = [&](__m256d sum) {
            const __m128i whole = _mm256_cvttpd_epi32(_mm256_floor_pd(_mm256_add_pd(sum, half)));
            return _mm_min_epi32(_mm_max_epi32(whole, lowest), highest);
        };
        _mm_storeu_si128(reinterpret_cast<__m128i*>(&out[y * 8]), round(low));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(&out[y * 8 + 4]), round(high));
    }
#else
    for (int v = 0; v < 8; ++v) {
        int n = 0;
        for (int u = 0; u < 8; ++u)
            if (in[v * 8 + u]) terms[n++] = u;
        if (n) live[live_rows++] = v;
        for (int x = 0; x < 8; ++x) {
            double sum = 0;
            for (int k = 0; k < n; ++k) sum += basis.c[x][terms[k]] * in[v * 8 + terms[k]];
            rows[v * 8 + x] = sum;
        }
    }
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) {
            double sum = 0;
            for (int k = 0; k < live_rows; ++k) sum += basis.c[y][live[k]] * rows[live[k] * 8 + x];
            out[y * 8 + x] = std::clamp(s32(std::floor(sum + 0.5)), -256, 255);
        }
#endif
}

}  // namespace mpeg
}  // namespace ps2host

#endif
