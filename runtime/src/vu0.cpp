/*
 * VU0 macro mode for lifted code: a line-for-line mirror of engine/ps2decomp/vu0.py,
 * which documents the arithmetic model (and where it departs from the hardware).
 * The two are tested against each other through the lifted-code check.
 */

#include "ps2rt/lifted.h"
#include "ps2rt/vu_math.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

#include "ps2rt/fp_mode.h"

namespace lifted {

namespace {

/* The special1 FMAC table: (operation, operand: 0-3 a lane of ft, 4 Q, 5 I, 6 ft). */
struct Fmac { int op; int operand; bool to_acc; };

bool Special1(u32 funct, Fmac* out) {
    static const int kBroadcast[7] = {kAdd, kSub, kMadd, kMsub, kMax_, kMini, kMul};
    if (funct <= 0x1B) { *out = {kBroadcast[funct >> 2], (int)(funct & 3), false}; return true; }
    switch (funct) {
        case 0x1C: *out = {kMul, 4, false}; return true;
        case 0x1D: *out = {kMax_, 5, false}; return true;
        case 0x1E: *out = {kMul, 5, false}; return true;
        case 0x1F: *out = {kMini, 5, false}; return true;
        case 0x20: *out = {kAdd, 4, false}; return true;
        case 0x21: *out = {kMadd, 4, false}; return true;
        case 0x22: *out = {kAdd, 5, false}; return true;
        case 0x23: *out = {kMadd, 5, false}; return true;
        case 0x24: *out = {kSub, 4, false}; return true;
        case 0x25: *out = {kMsub, 4, false}; return true;
        case 0x26: *out = {kSub, 5, false}; return true;
        case 0x27: *out = {kMsub, 5, false}; return true;
        case 0x28: *out = {kAdd, 6, false}; return true;
        case 0x29: *out = {kMadd, 6, false}; return true;
        case 0x2A: *out = {kMul, 6, false}; return true;
        case 0x2B: *out = {kMax_, 6, false}; return true;
        case 0x2C: *out = {kSub, 6, false}; return true;
        case 0x2D: *out = {kMsub, 6, false}; return true;
        case 0x2F: *out = {kMini, 6, false}; return true;
    }
    return false;
}

/* The special2 accumulator forms, by their index. */
bool AccumulatorForm(u32 index, Fmac* out) {
    static const int kBroadcast[4] = {kAdd, kSub, kMadd, kMsub};
    if (index <= 0x0F) { *out = {kBroadcast[index >> 2], (int)(index & 3), true}; return true; }
    if (index >= 0x18 && index <= 0x1B) { *out = {kMul, (int)(index & 3), true}; return true; }
    switch (index) {
        case 0x1C: *out = {kMul, 4, true}; return true;
        case 0x1E: *out = {kMul, 5, true}; return true;
        case 0x20: *out = {kAdd, 4, true}; return true;
        case 0x21: *out = {kMadd, 4, true}; return true;
        case 0x22: *out = {kAdd, 5, true}; return true;
        case 0x23: *out = {kMadd, 5, true}; return true;
        case 0x24: *out = {kSub, 4, true}; return true;
        case 0x25: *out = {kMsub, 4, true}; return true;
        case 0x26: *out = {kSub, 5, true}; return true;
        case 0x27: *out = {kMsub, 5, true}; return true;
        case 0x28: *out = {kAdd, 6, true}; return true;
        case 0x29: *out = {kMadd, 6, true}; return true;
        case 0x2A: *out = {kMul, 6, true}; return true;
        case 0x2C: *out = {kSub, 6, true}; return true;
        case 0x2D: *out = {kMsub, 6, true}; return true;
    }
    return false;
}

}  // namespace

}  // namespace lifted

namespace generated {

u32 VuAdd(u32 a, u32 b) { return lifted::Binary(lifted::kAdd, a, b, 0); }
u32 VuSub(u32 a, u32 b) { return lifted::Binary(lifted::kSub, a, b, 0); }
u32 VuMul(u32 a, u32 b) { return lifted::Binary(lifted::kMul, a, b, 0); }
u32 VuMax(u32 a, u32 b) { return lifted::Binary(lifted::kMax_, a, b, 0); }
u32 VuMin(u32 a, u32 b) { return lifted::Binary(lifted::kMini, a, b, 0); }
u32 VuMadd(u32 acc, u32 a, u32 b) { return lifted::Binary(lifted::kMadd, a, b, acc); }
u32 VuMsub(u32 acc, u32 a, u32 b) { return lifted::Binary(lifted::kMsub, a, b, acc); }
u32 VuAbs(u32 a) { return lifted::Clean(a) & ~lifted::kSign; }
u32 VuItof(u32 a, u32 shift) { return lifted::Result((double)(s32)a / (double)(1u << shift)); }
u32 VuFtoi(u32 a, u32 shift) {
    return lifted::FloatToInt(lifted::Value(a) * (double)(1u << shift));
}
u32 VuDiv(u32 num, u32 den) {
    return lifted::Value(den) == 0.0 ? ((num ^ den) & lifted::kSign) | lifted::kMax
                                     : lifted::Result(lifted::Value(num) / lifted::Value(den));
}
u32 VuSqrt(u32 a) { return lifted::Result(std::sqrt(std::fabs(lifted::Value(a)))); }
u32 VuRsqrt(u32 num, u32 den) {
    const double root = std::sqrt(std::fabs(lifted::Value(den)));
    return root == 0.0 ? (num & lifted::kSign) | lifted::kMax
                       : lifted::Result(lifted::Value(num) / root);
}

}  // namespace generated

namespace lifted {

namespace {
alignas(16) u8 g_own_vu0_data[4096];
}  // namespace
u8* vu0_data = g_own_vu0_data;

bool DecodeFmac(u32 word, FmacOp* out) {
    const u32 funct = word & 0x3F;
    Fmac fmac;
    const bool is_fmac = funct < 0x3C ? Special1(funct, &fmac)
                                      : AccumulatorForm((word & 3) | ((word >> 4) & 0x7C), &fmac);
    if (!is_fmac) return false;
    out->op = fmac.op;
    out->operand = fmac.operand;
    out->to_acc = fmac.to_acc;
    out->fd = (word >> 6) & 0x1F;
    out->fs = (word >> 11) & 0x1F;
    out->ft = (word >> 16) & 0x1F;
    out->dest = (word >> 21) & 0xF;
    return true;
}

namespace {

#if PS2RT_VU_SSE
template <int Op>
void Lanes(const Context& c, const FmacOp& f, u32 out[4]) {
    const __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(c.vf[f.fs]));
    __m128i b;
    if (f.operand == 6) b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(c.vf[f.ft]));
    else b = _mm_set1_epi32(int(f.operand < 4 ? c.vf[f.ft][f.operand]
                              : f.operand == 4 ? c.vc[kRegQ] : c.vc[kRegI]));
    const __m128i acc = _mm_loadu_si128(reinterpret_cast<const __m128i*>(c.vacc));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(out), LanesSse<Op>(a, b, acc));
}
#else
template <int Op>
void Lanes(const Context& c, const FmacOp& f, u32 out[4]) {
    const u32* fs = c.vf[f.fs];
    const u32 broadcast = f.operand < 4 ? c.vf[f.ft][f.operand]
                        : f.operand == 4 ? c.vc[kRegQ] : c.vf[f.ft][0];
    const u32 each = f.operand == 5 ? c.vc[kRegI] : broadcast;
    for (int lane = 0; lane < 4; ++lane) {
        if (!(f.dest & (8u >> lane))) continue;
        const u32 b = f.operand == 6 ? c.vf[f.ft][lane] : each;
        out[lane] = BinaryOf<Op>(fs[lane], b, c.vacc[lane]);
    }
}
#endif

}  // namespace

void RunFmacScalar(Context& c, const FmacOp& f) {
    u32 out[4];
    const u32 broadcast = f.operand < 4 ? c.vf[f.ft][f.operand]
                        : f.operand == 4 ? c.vc[kRegQ] : f.operand == 5 ? c.vc[kRegI] : 0;
    for (int lane = 0; lane < 4; ++lane) {
        if (!(f.dest & (8u >> lane))) continue;
        out[lane] = Binary(f.op, c.vf[f.fs][lane], f.operand == 6 ? c.vf[f.ft][lane] : broadcast, c.vacc[lane]);
    }
    u32* to = f.to_acc ? c.vacc : f.fd ? c.vf[f.fd] : nullptr;
    if (!to) return;
    for (int lane = 0; lane < 4; ++lane)
        if (f.dest & (8u >> lane)) to[lane] = out[lane];
}

void FmacResult(const Context& c, const FmacOp& f, u32 out[4]) {
    u32 lanes[4] = {};
    switch (f.op) {
        case kAdd: Lanes<kAdd>(c, f, lanes); break;
        case kSub: Lanes<kSub>(c, f, lanes); break;
        case kMul: Lanes<kMul>(c, f, lanes); break;
        case kMadd: Lanes<kMadd>(c, f, lanes); break;
        case kMsub: Lanes<kMsub>(c, f, lanes); break;
        case kMax_: Lanes<kMax_>(c, f, lanes); break;
        default: Lanes<kMini>(c, f, lanes); break;
    }
    for (int lane = 0; lane < 4; ++lane) out[lane] = (f.dest & (8u >> lane)) ? lanes[lane] : 0;
}

void RunFmac(Context& c, const FmacOp& f) {
    // Every lane is read before any is written (fd may be fs or ft); a lane
    // dest leaves alone is not computed at all - the arithmetic has no other
    // effect.
    u32 out[4];
    switch (f.op) {
        case kAdd: Lanes<kAdd>(c, f, out); break;
        case kSub: Lanes<kSub>(c, f, out); break;
        case kMul: Lanes<kMul>(c, f, out); break;
        case kMadd: Lanes<kMadd>(c, f, out); break;
        case kMsub: Lanes<kMsub>(c, f, out); break;
        case kMax_: Lanes<kMax_>(c, f, out); break;
        default: Lanes<kMini>(c, f, out); break;
    }
    u32* to = f.to_acc ? c.vacc : f.fd ? c.vf[f.fd] : nullptr;
    if (!to) return;
    for (int lane = 0; lane < 4; ++lane)
        if (f.dest & (8u >> lane)) to[lane] = out[lane];
}

/* The VU's float arithmetic rounds toward zero and flushes denormals: the
   host's SSE control word set so for an instruction, as ps2host's Vu::Run sets
   it for a microprogram (VuRounding) - macro mode is the same VU0, called from
   EE code, which runs with the host's default (to nearest).
   PS2RT_EE_ROUND_NEAREST=1 leaves it to nearest, as emu.py's interpreter
   (eefpu.cpp: what verification compares against). */
extern const bool kMacroNearest = std::getenv("PS2RT_EE_ROUND_NEAREST") != nullptr;

struct MacroRounding {
    ps2rt::FpMode saved = ps2rt::GetFpMode();
    MacroRounding() { if (!kMacroNearest) ps2rt::SetFpMode(ps2rt::VuFpMode(saved)); }
    ~MacroRounding() { if (!kMacroNearest) ps2rt::SetFpMode(saved); }
};

void Cop2Slow(Context& c, u32 word) {
    MacroRounding rounding;
    const u32 fd = (word >> 6) & 0x1F, fs = (word >> 11) & 0x1F, ft = (word >> 16) & 0x1F;
    const u32 dest = (word >> 21) & 0xF;
    const bool lane_on[4] = {(dest & 8) != 0, (dest & 4) != 0, (dest & 2) != 0, (dest & 1) != 0};
    auto write = [&](u32 index, const u32 values[4]) {
        if (index == 0) return;
        for (int lane = 0; lane < 4; ++lane)
            if (lane_on[lane]) c.vf[index][lane] = values[lane];
    };

    const u32 funct = word & 0x3F;
    const u32 index = funct < 0x3C ? 0 : (word & 3) | ((word >> 4) & 0x7C);
    FmacOp fmac;
    if (DecodeFmac(word, &fmac)) {
        u32 out[4];
        FmacResult(c, fmac, out);                          // before the write: fd may be fs or ft
        RunFmac(c, fmac);
        if (fmac.op != kMax_ && fmac.op != kMini) MacroFlags(c, out, dest);
        return;
    }

    const u32* a = c.vf[fs];
    if (funct == 0x2E) {                                   /* vopmsub */
        const u32* bb = c.vf[ft];
        const u32 out[3] = {Binary(kMsub, a[1], bb[2], c.vacc[0]),
                            Binary(kMsub, a[2], bb[0], c.vacc[1]),
                            Binary(kMsub, a[0], bb[1], c.vacc[2])};
        if (fd != 0)
            for (int lane = 0; lane < 3; ++lane)
                if (lane_on[lane]) c.vf[fd][lane] = out[lane];
        // The flags of what fd holds now (w as it was), or of the result into VF0 (w 0).
        const u32 flagged[4] = {fd ? c.vf[fd][0] : out[0], fd ? c.vf[fd][1] : out[1], fd ? c.vf[fd][2] : out[2],
                                fd ? c.vf[fd][3] : 0u};
        MacroFlags(c, flagged, dest);
        return;
    }
    if (funct >= 0x30 && funct <= 0x35) {                  /* integer unit */
        const u32 x = c.vc[fs], y = c.vc[ft];
        if (funct == 0x32) {
            const s32 imm = (fd & 0x10) ? (s32)fd - 32 : (s32)fd;
            if (ft > 0 && ft < 16) c.vc[ft] = (u32)(c.vc[fs] + imm) & 0xFFFF;
            return;
        }
        const u32 out = funct == 0x30 ? x + y : funct == 0x31 ? x - y
                      : funct == 0x34 ? (x & y) : (x | y);
        if (fd > 0 && fd < 16) c.vc[fd] = out & 0xFFFF;
        return;
    }
    const u32 fsf = (word >> 21) & 3, ftf = (word >> 23) & 3;
    switch (index) {
        case 0x2E: {                                       /* vopmula */
            const u32* bb = c.vf[ft];
            const u32 out[3] = {Result(Value(a[1]) * Value(bb[2])),
                                Result(Value(a[2]) * Value(bb[0])),
                                Result(Value(a[0]) * Value(bb[1]))};
            for (int lane = 0; lane < 3; ++lane)
                if (lane_on[lane]) c.vacc[lane] = out[lane];
            MacroFlags(c, c.vacc, dest);                   // ACC as it is now (w unchanged)
            return;
        }
        case 0x30: { const u32 v[4] = {a[0], a[1], a[2], a[3]}; write(ft, v); return; }
        case 0x31: { const u32 v[4] = {a[1], a[2], a[3], a[0]}; write(ft, v); return; }
        case 0x1D: {
            u32 v[4];
            for (int lane = 0; lane < 4; ++lane) v[lane] = Clean(a[lane]) & ~kSign;
            write(ft, v);
            return;
        }
        case 0x10: case 0x11: case 0x12: case 0x13: {
            static const double kScale[4] = {1.0, 16.0, 4096.0, 32768.0};
            u32 v[4];
            for (int lane = 0; lane < 4; ++lane)
                v[lane] = Result((double)(s32)a[lane] / kScale[index & 3]);
            write(ft, v);
            return;
        }
        case 0x14: case 0x15: case 0x16: case 0x17: {
            static const double kScale[4] = {1.0, 16.0, 4096.0, 32768.0};
            u32 v[4];
            for (int lane = 0; lane < 4; ++lane)
                v[lane] = FloatToInt(Value(a[lane]) * kScale[index & 3]);
            write(ft, v);
            return;
        }
        case 0x2F: case 0x3B:                              /* vnop, vwaitq */
            return;
        case 0x1F: {                                       /* vclipw: fs.xyz against |ft.w| (vu0.py, ps2host CLIP) */
            const double w = std::fabs(Value(c.vf[ft][3]));
            u32 judgement = 0;
            for (int lane = 0; lane < 3; ++lane) {
                const double x = Value(a[lane]);
                if (x > w) judgement |= 1u << (2 * lane);
                if (x < -w) judgement |= 2u << (2 * lane);
            }
            c.vc[kRegClip] = ((c.vc[kRegClip] << 6) | judgement) & 0xFFFFFFu;
            return;
        }
        case 0x38: {                                       /* vdiv */
            const u32 num = c.vf[fs][fsf], den = c.vf[ft][ftf];
            c.vc[kRegQ] = Value(den) == 0.0 ? ((num ^ den) & kSign) | kMax
                                            : Result(Value(num) / Value(den));
            return;
        }
        case 0x39:                                         /* vsqrt */
            c.vc[kRegQ] = Result(std::sqrt(std::fabs(Value(c.vf[ft][ftf]))));
            return;
        case 0x3A: {                                       /* vrsqrt */
            const u32 num = c.vf[fs][fsf];
            const double root = std::sqrt(std::fabs(Value(c.vf[ft][ftf])));
            c.vc[kRegQ] = root == 0.0 ? (num & kSign) | kMax : Result(Value(num) / root);
            return;
        }
        case 0x3C:                                         /* vmtir */
            if (ft != 0 && ft < 16) c.vc[ft] = c.vf[fs][fsf] & 0xFFFF;
            return;
        case 0x3D: {                                       /* vmfir */
            u32 x = c.vc[fs] & 0xFFFF;
            if (x & 0x8000) x |= 0xFFFF0000u;
            const u32 v[4] = {x, x, x, x};
            write(ft, v);
            return;
        }
        case 0x42:                                         /* vrinit */
            c.vc[kRegR] = kOne | (c.vf[fs][fsf] & 0x7FFFFF);
            return;
        case 0x43:                                         /* vrxor */
            c.vc[kRegR] = kOne | ((c.vc[kRegR] ^ c.vf[fs][fsf]) & 0x7FFFFF);
            return;
        case 0x41: { const u32 v[4] = {c.vc[kRegR], c.vc[kRegR], c.vc[kRegR], c.vc[kRegR]};
                     write(ft, v); return; }
        case 0x40: {                                       /* vrnext */
            if (ft == 0) return;
            const u32 r = c.vc[kRegR];
            const u32 bit = ((r >> 4) & 1) ^ ((r >> 22) & 1);
            c.vc[kRegR] = kOne | (((r << 1) ^ bit) & 0x7FFFFF);
            const u32 v[4] = {c.vc[kRegR], c.vc[kRegR], c.vc[kRegR], c.vc[kRegR]};
            write(ft, v);
            return;
        }
        // VU0's data memory from macro mode (vu0.py the same): a quadword at VI * 16, the 4 KB wrapping
        // (ps2autotests cpu/vu0_macro/integer; ZOE has a vlqi and a vsqi). VI 0 is never changed.
        case 0x34: case 0x36: {                            /* vlqi vft, (vis++) / vlqd vft, (--vis) */
            if (index == 0x36 && fs != 0) c.vc[fs] = (c.vc[fs] - 1) & 0xFFFF;
            const u32 at = (c.vc[fs] & 0xFF) * 16;
            u32 v[4];
            for (int lane = 0; lane < 4; ++lane) std::memcpy(&v[lane], vu0_data + at + 4 * lane, 4);
            write(ft, v);
            if (index == 0x34 && fs != 0) c.vc[fs] = (c.vc[fs] + 1) & 0xFFFF;
            return;
        }
        case 0x35: case 0x37: {                            /* vsqi vfs, (vit++) / vsqd vfs, (--vit) */
            if (index == 0x37 && ft != 0) c.vc[ft] = (c.vc[ft] - 1) & 0xFFFF;
            const u32 at = (c.vc[ft] & 0xFF) * 16;
            for (int lane = 0; lane < 4; ++lane)
                if (lane_on[lane]) std::memcpy(vu0_data + at + 4 * lane, &a[lane], 4);
            if (index == 0x35 && ft != 0) c.vc[ft] = (c.vc[ft] + 1) & 0xFFFF;
            return;
        }
        case 0x3E: {                                       /* vilwr.dest vit, (vis): the first lane dest names */
            int lane = 0;
            while (lane < 3 && !lane_on[lane]) ++lane;
            u32 x;
            std::memcpy(&x, vu0_data + (c.vc[fs] & 0xFF) * 16 + 4 * lane, 4);
            if (ft != 0 && ft < 16) c.vc[ft] = x & 0xFFFF;
            return;
        }
        case 0x3F: {                                       /* viswr.dest vit, (vis): every lane dest names */
            const u32 x = c.vc[ft] & 0xFFFF;
            for (int lane = 0; lane < 4; ++lane)
                if (lane_on[lane]) std::memcpy(vu0_data + (c.vc[fs] & 0xFF) * 16 + 4 * lane, &x, 4);
            return;
        }
    }
    throw Abort{word, 5};
}

void Cop2Move(Context& c, u32 word, u32 rt) {
    const u32 kind = (word >> 21) & 0x1F, index = (word >> 11) & 0x1F;
    switch (kind) {
        case 0x05:                                         /* qmtc2 */
            if (index) {
                c.vf[index][0] = (u32)c.r[rt];
                c.vf[index][1] = (u32)(c.r[rt] >> 32);
                c.vf[index][2] = (u32)c.rh[rt];
                c.vf[index][3] = (u32)(c.rh[rt] >> 32);
            }
            return;
        case 0x06:                                         /* ctc2 */
            /* As the console takes them (ps2autotests cpu/vu0_macro/transfer): STATUS its sticky bits only,
               MAC, TPC, VPU-STAT and the reserved 19, 23-25, FBRST (28) not at all, CMSAR0 16 bits. */
            if (index == 16) {
                c.vc[16] = (c.vc[16] & 0x3Fu) | ((u32)c.r[rt] & 0xFC0u);
            } else if (index == 17 || index == 19 || (index >= 23 && index <= 26) || index == 28 || index == 29) {
                /* (read-only, or written without effect here) */
            } else if (index == 27) {
                c.vc[27] = (u32)c.r[rt] & 0xFFFF;
            } else if (index < 16) {
                if (index) c.vc[index] = (u32)c.r[rt] & 0xFFFF;
            } else if (index == (u32)kRegR) {
                c.vc[kRegR] = kOne | ((u32)c.r[rt] & 0x7FFFFF);
            } else if (index == (u32)kRegClip) {
                c.vc[kRegClip] = (u32)c.r[rt] & 0xFFFFFFu;
            } else if (index == 31) {                      /* CMSAR1: VU1 starts at it (pairs, x8 bytes) */
                c.vc[31] = (u32)c.r[rt] & 0xFFFF;
                StartVu1(c, c.vc[31] * 8);
            } else {
                c.vc[index] = (u32)c.r[rt];
            }
            return;
        case 0x01:                                         /* qmfc2 */
            if (rt) {
                const u32* v = c.vf[index];
                c.r[rt] = (u64)v[0] | ((u64)v[1] << 32);
                c.rh[rt] = (u64)v[2] | ((u64)v[3] << 32);
            }
            return;
        case 0x02:                                         /* cfc2 */
            /* VPU-STAT (29): no VU program runs here, so both units are idle. R (20) reads its 23 fraction bits;
               the reserved 19 and 24 read constants (cpu/vu0_macro/transfer: 0x2E30, 0x0C0C). */
            if (rt) c.r[rt] = index == 29 ? 0 : index == 19 ? 0x2E30 : index == 24 ? 0x0C0C
                            : index == (u32)kRegR ? (c.vc[kRegR] & 0x7FFFFFu) : Sx32(c.vc[index]);
            return;
    }
    throw Abort{word, 5};
}

}  // namespace lifted
