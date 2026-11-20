/*
 * The host's floating-point mode as the VUs' arithmetic needs it: rounded
 * toward zero, denormals flushed (inputs and results). Set for a VU
 * microprogram's run (host/src/vu.cpp's VuRounding) and for a macro-mode COP2
 * instruction (vu0.cpp's MacroRounding), the mode before put back after.
 *
 * x86-64: the SSE control word (MXCSR: RC = toward zero, FTZ, DAZ) - what the
 * two did in place before 2026-11-17, unchanged.
 * AArch64 (the NEON builds: Linux on ARM, Android): FPCR - RMode = toward zero
 * (bits 23:22 = 11) and FZ (bit 24), which on AArch64 flushes single-precision
 * denormal inputs and results alike (x86's FTZ and DAZ together).
 */

#ifndef PS2RT_FP_MODE_H
#define PS2RT_FP_MODE_H

#include <cstdint>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <xmmintrin.h>
#define PS2RT_FP_MODE_X86 1
#elif defined(__aarch64__) || defined(_M_ARM64)
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#endif
#define PS2RT_FP_MODE_ARM64 1
#else
#error "ps2rt/fp_mode.h: the VU's rounding mode on this processor (x86-64 and AArch64 so far)"
#endif

namespace ps2rt {

#if defined(PS2RT_FP_MODE_X86)
using FpMode = unsigned;
inline FpMode GetFpMode() { return _mm_getcsr(); }
inline void SetFpMode(FpMode mode) { _mm_setcsr(mode); }
// Toward zero (RC = 11), flush to zero (FTZ, 0x8000), denormals are zero (DAZ, 0x0040).
inline FpMode VuFpMode(FpMode from) { return (from & ~0x6000u) | 0x6000u | 0x8000u | 0x0040u; }
#else
using FpMode = std::uint64_t;
inline FpMode GetFpMode() {
#if defined(_MSC_VER) && !defined(__clang__)
    return FpMode(_ReadStatusReg(ARM64_FPCR));
#else
    FpMode fpcr;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
    return fpcr;
#endif
}
inline void SetFpMode(FpMode mode) {
#if defined(_MSC_VER) && !defined(__clang__)
    _WriteStatusReg(ARM64_FPCR, __int64(mode));
#else
    __asm__ __volatile__("msr fpcr, %0" : : "r"(mode));
#endif
}
// RMode toward zero (bits 23:22 = 11), FZ (bit 24).
inline FpMode VuFpMode(FpMode from) { return from | (FpMode(3) << 22) | (FpMode(1) << 24); }
#endif

}  // namespace ps2rt

#endif
