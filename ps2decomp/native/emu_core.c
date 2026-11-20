/*
 * emu_core.c - the native core of the verification interpreter (ps2decomp/emu.py), M3 2026-11-13.
 *
 * Why: a full verify run was ~2 h per version, 93 % of it in emu.py's step (136 k instructions a second, profile
 * 2026-11-13). This core runs the common instructions - integer, shifts, multiply/divide (both pipelines),
 * loads and stores in RAM, the windows and the pages mapped for tests, branches and jumps with their delay
 * slots, lq/sq - directly on emu.Memory's buffer, journalling every write as Memory.write does.
 *
 * Rule: it does exactly what emu.py does, or nothing. Every instruction is first CHECKED without any effect
 * (supported, every address it touches resolvable here, the journal within its limit); if not, the core stops
 * with NEED_PYTHON before touching anything and emu.py executes that one step itself - faults, stubbed calls,
 * the FPU, COP0/COP2, MMI beyond multiply-add, traps, syscalls, unaligned loads, invalid words: all stay in the
 * Python code, as they were. tests/test_emu_native.py runs both on random states and instructions and compares
 * every register, HI/LO, memory byte, journal entry and watch_zero result.
 */

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#if defined(_MSC_VER)
#define EXPORT __declspec(dllexport)
#else
#define EXPORT __attribute__((visibility("default")))
#endif

#define RETURN_SENTINEL 0xDEAD0000u
#define PAGE_SIZE 4096u
#define PAGE_MASK 0xFFFu

enum { STOP_RETURNED = 0, NEED_PYTHON = 1, OVER_BUDGET = 2, JOURNAL_FULL = 3 };

typedef struct {
    uint64_t regs[32];
    uint64_t regs_hi[32];
    uint64_t hi, lo, hi1, lo1;
    uint32_t pc;
    uint32_t mode;              /* 0: run until pc == RETURN_SENTINEL (Machine._call_once); 1: run_to_return */
    uint32_t return_address;
    uint32_t entry_sp;
    int64_t executed;           /* in/out */
    int64_t budget;             /* OVER_BUDGET once executed > budget, as emu.py raises */
    uint32_t code_start, code_end;
    /* Memory: emu.Memory.bytes, and where RAM, the windows and the pages sit in it. */
    uint8_t* mem;
    uint64_t mem_len;
    uint32_t ram_base;
    uint32_t pad0;
    uint64_t ram_size;
    uint32_t win_start[2], win_end[2];
    uint64_t win_at[2];
    uint32_t nwin;
    uint32_t npages;
    const uint32_t* page_addr;  /* npages entries, sorted ascending */
    const uint64_t* page_at;
    /* The journal (Memory._journal): each write's buffer offset, length and previous bytes. */
    uint32_t journal_on;
    uint32_t jr_count, jr_cap;
    uint32_t pad1;
    int64_t journal_bytes, journal_limit;
    uint64_t* jr_off;
    uint8_t* jr_len;
    uint8_t* jr_prev;           /* 16 bytes an entry (sqc2 writes 16 at once) */
    /* Memory.watch_zero: the address of the last 4-byte read that gave zero. */
    uint32_t watch_zero;
    uint32_t last_zero_valid;
    uint32_t last_zero_read;
    uint32_t pad2;
    /* COP1: the registers as bit patterns, the condition, the accumulator (emu.Machine fpr, fp_cond, fp_acc). */
    uint32_t fpr[32];
    uint32_t fp_cond;
    uint32_t fp_acc;
    /* VU0 in macro mode (emu.Machine vf, vc, vacc; vu0.py). */
    uint32_t vf[32][4];
    uint32_t vc[32];
    uint32_t vacc[4];
    /* Coverage (Machine.coverage): one byte per instruction from cov_start, set when a step starts there;
       cov_len 0 = off. A delay slot is not marked on its own - as in emu.py, it runs inside its branch's step. */
    uint8_t* cov;
    uint32_t cov_start, cov_len;
    /* FCR31's other writable bits (emu.Machine fcr31, eefpu.FCR31_WRITABLE): kept by ctc1, read by cfc1. */
    uint32_t fcr31, pad3;
} EmuState;

static inline int64_t sign32(uint64_t v) { return (int64_t)(int32_t)(uint32_t)v; }
static inline uint64_t set32v(uint64_t v) { return (uint64_t)(int64_t)(int32_t)(uint32_t)v; }
static inline uint64_t get(const EmuState* s, unsigned r) { return r ? s->regs[r] : 0; }
static inline uint32_t get32(const EmuState* s, unsigned r) { return (uint32_t)get(s, r); }
static inline void set(EmuState* s, unsigned r, uint64_t v) { if (r) s->regs[r] = v; }
static inline void set32(EmuState* s, unsigned r, uint64_t v) { if (r) s->regs[r] = set32v(v); }

/* Memory._offset: where [addr, addr+size) lives in the buffer, or -1 (a fault: emu.py's to raise). */
static int64_t offset_of(const EmuState* s, uint64_t addr, uint64_t size) {
    const int64_t off = (int64_t)addr - (int64_t)s->ram_base;
    if (off >= 0 && (uint64_t)off + size <= s->ram_size) return off;
    for (uint32_t i = 0; i < s->nwin; ++i)
        if (s->win_start[i] <= addr && addr + size <= s->win_end[i]) return (int64_t)(s->win_at[i] + addr - s->win_start[i]);
    const uint32_t page = (uint32_t)(addr & ~(uint64_t)PAGE_MASK);
    uint32_t lo = 0, hi = s->npages;
    while (lo < hi) {
        const uint32_t mid = (lo + hi) / 2;
        if (s->page_addr[mid] < page) lo = mid + 1;
        else hi = mid;
    }
    if (lo < s->npages && s->page_addr[lo] == page && (addr & PAGE_MASK) + size <= PAGE_SIZE)
        return (int64_t)(s->page_at[lo] + (addr & PAGE_MASK));
    return -1;
}

static inline int resolvable(const EmuState* s, uint64_t addr, uint64_t size) {
    const int64_t off = offset_of(s, addr, size);
    return off >= 0 && (uint64_t)off + size <= s->mem_len;
}

static uint64_t read_le(const EmuState* s, uint64_t addr, unsigned size) {
    const uint8_t* p = s->mem + offset_of(s, addr, size);
    uint64_t v = 0;
    for (unsigned i = 0; i < size; ++i) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

/* Memory.read_int's watch: a 4-byte read that gave zero. */
static inline void watch(EmuState* s, uint64_t addr, unsigned size, uint64_t value) {
    if (s->watch_zero && size == 4 && value == 0) {
        s->last_zero_valid = 1;
        s->last_zero_read = (uint32_t)addr;
    }
}

/* Memory.write of `size` bytes (up to 16) at once: one journal entry. */
static void write_bytes(EmuState* s, uint64_t addr, const uint8_t* bytes, unsigned size) {
    const int64_t off = offset_of(s, addr, size);
    uint8_t* p = s->mem + off;
    if (s->journal_on) {
        s->journal_bytes += size;
        const uint32_t n = s->jr_count++;
        s->jr_off[n] = (uint64_t)off;
        s->jr_len[n] = (uint8_t)size;
        memcpy(s->jr_prev + 16 * n, p, size);
    }
    memcpy(p, bytes, size);
}

static void write_le(EmuState* s, uint64_t addr, uint64_t value, unsigned size) {
    uint8_t bytes[8];
    for (unsigned i = 0; i < size; ++i) bytes[i] = (uint8_t)(value >> (8 * i));
    write_bytes(s, addr, bytes, size);
}

/* Room for `size` more journalled bytes, as Memory.write allows them. */
static inline int journal_room(const EmuState* s, int64_t extra) {
    return !s->journal_on || s->journal_bytes + extra <= s->journal_limit;
}

static inline int is_code(const EmuState* s, uint32_t a) { return s->code_start <= a && a < s->code_end; }

#define OP(w) ((w) >> 26)
#define RS(w) (((w) >> 21) & 31)
#define RT(w) (((w) >> 16) & 31)
#define RD(w) (((w) >> 11) & 31)
#define SA(w) (((w) >> 6) & 31)
#define FN(w) ((w) & 63)
#define SIMM(w) ((int64_t)(int16_t)((w) & 0xFFFF))
#define UIMM(w) ((uint64_t)((w) & 0xFFFF))

static int cop1_runs_here(uint32_t w);
static int vu0_runs_here(uint32_t w);
static int cop2_move_runs_here(uint32_t w);

/* A plain (non-transfer) instruction: can it run here, without any effect? Its memory, the journal. */
static int can_run(const EmuState* s, uint32_t w) {
    const unsigned op = OP(w);
    switch (op) {
        case 0x11: return cop1_runs_here(w);
        case 0x12: return vu0_runs_here(w) || cop2_move_runs_here(w);
        case 0x36: {    /* lqc2: four 4-byte reads, as emu.py makes them */
            const uint64_t a = (get32(s, RS(w)) + SIMM(w)) & 0xFFFFFFFFu & ~(uint64_t)0xF;
            return resolvable(s, a, 4) && resolvable(s, a + 4, 4) && resolvable(s, a + 8, 4) && resolvable(s, a + 12, 4);
        }
        case 0x3E: {    /* sqc2: one 16-byte write */
            const uint64_t a = (get32(s, RS(w)) + SIMM(w)) & 0xFFFFFFFFu & ~(uint64_t)0xF;
            return resolvable(s, a, 16) && journal_room(s, 16);
        }
        case 0x31: return resolvable(s, (get32(s, RS(w)) + SIMM(w)) & 0xFFFFFFFFu, 4);                       /* lwc1 */
        case 0x39: return resolvable(s, (get32(s, RS(w)) + SIMM(w)) & 0xFFFFFFFFu, 4) && journal_room(s, 4);  /* swc1 */
        case 0x00: {
            switch (FN(w)) {
                case 0x00: case 0x02: case 0x03: case 0x04: case 0x06: case 0x07:
                case 0x0A: case 0x0B: case 0x0F:
                case 0x10: case 0x11: case 0x12: case 0x13:
                case 0x14: case 0x16: case 0x17:
                case 0x18: case 0x19: case 0x1A: case 0x1B:
                case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27:
                case 0x2A: case 0x2B: case 0x2C: case 0x2D: case 0x2E: case 0x2F:
                case 0x38: case 0x3A: case 0x3B: case 0x3C: case 0x3E: case 0x3F:
                    return 1;
                default:
                    return 0;
            }
        }
        case 0x1C:      /* MMI: madd/maddu, the second pipeline's mult/div/moves */
            switch (FN(w)) {
                case 0x00: case 0x01: case 0x10: case 0x11: case 0x12: case 0x13:
                case 0x18: case 0x19: case 0x1A: case 0x1B:
                    return 1;
                default:
                    return 0;
            }
        case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E: case 0x0F:
        case 0x18: case 0x19:
        case 0x2F: case 0x33:   /* cache, pref: no effect */
            return 1;
        case 0x20: case 0x24: return resolvable(s, (get32(s, RS(w)) + SIMM(w)) & 0xFFFFFFFFu, 1);
        case 0x21: case 0x25: return resolvable(s, (get32(s, RS(w)) + SIMM(w)) & 0xFFFFFFFFu, 2);
        case 0x23: case 0x27: return resolvable(s, (get32(s, RS(w)) + SIMM(w)) & 0xFFFFFFFFu, 4);
        case 0x37: return resolvable(s, (get32(s, RS(w)) + SIMM(w)) & 0xFFFFFFFFu, 8);
        case 0x28: return resolvable(s, (get32(s, RS(w)) + SIMM(w)) & 0xFFFFFFFFu, 1) && journal_room(s, 1);
        case 0x29: return resolvable(s, (get32(s, RS(w)) + SIMM(w)) & 0xFFFFFFFFu, 2) && journal_room(s, 2);
        case 0x2B: return resolvable(s, (get32(s, RS(w)) + SIMM(w)) & 0xFFFFFFFFu, 4) && journal_room(s, 4);
        case 0x3F: return resolvable(s, (get32(s, RS(w)) + SIMM(w)) & 0xFFFFFFFFu, 8) && journal_room(s, 8);
        case 0x1E: {    /* lq */
            const uint64_t a = (get32(s, RS(w)) + SIMM(w)) & 0xFFFFFFFFu & ~(uint64_t)0xF;
            return resolvable(s, a, 8) && resolvable(s, a + 8, 8);
        }
        case 0x1F: {    /* sq */
            const uint64_t a = (get32(s, RS(w)) + SIMM(w)) & 0xFFFFFFFFu & ~(uint64_t)0xF;
            return resolvable(s, a, 8) && resolvable(s, a + 8, 8) && journal_room(s, 16);
        }
        default:
            return 0;
    }
}

/* EE division (emu.divide): every input has a result. */
static void divide(uint32_t a32, uint32_t b32, int is_signed, uint64_t* lo, uint64_t* hi) {
    if (is_signed) {
        const int64_t a = (int32_t)a32, b = (int32_t)b32;
        if (b == 0) {
            *lo = a < 0 ? 1 : (uint64_t)-1;
            *hi = (uint64_t)a;
            return;
        }
        int64_t q = (a < 0 ? -a : a) / (b < 0 ? -b : b);
        if ((a < 0) != (b < 0)) q = -q;
        *lo = set32v((uint64_t)q);
        *hi = set32v((uint64_t)(a - b * q));
        return;
    }
    if (b32 == 0) {
        *lo = ~(uint64_t)0;
        *hi = set32v(a32);
        return;
    }
    *lo = set32v(a32 / b32);
    *hi = set32v(a32 % b32);
}

static void mult(EmuState* s, uint32_t w, int is_signed, uint64_t* lo, uint64_t* hi) {
    const uint64_t product = is_signed ? (uint64_t)((int64_t)(int32_t)get32(s, RS(w)) * (int64_t)(int32_t)get32(s, RT(w)))
                                       : (uint64_t)get32(s, RS(w)) * (uint64_t)get32(s, RT(w));
    *lo = set32v(product);
    *hi = set32v(product >> 32);
    if (RD(w)) set32(s, RD(w), product);
}

static void madd(EmuState* s, uint32_t w, int is_signed) {
    const uint64_t product = is_signed ? (uint64_t)((int64_t)(int32_t)get32(s, RS(w)) * (int64_t)(int32_t)get32(s, RT(w)))
                                       : (uint64_t)get32(s, RS(w)) * (uint64_t)get32(s, RT(w));
    const uint64_t total = (((s->hi & 0xFFFFFFFFu) << 32) | (s->lo & 0xFFFFFFFFu)) + product;
    s->lo = set32v(total);
    s->hi = set32v(total >> 32);
    if (RD(w)) set(s, RD(w), s->lo);
}

/* COP1 as eefpu.py models it: an exponent of 255 is a number (decode), the operation in double (Python's
   float), the result rounded to nearest by encode - saturating at +-0x7FFFFFFF, NaN (none arises) to 0. */
static inline double fval(uint32_t bits) {
    if ((bits & 0x7F800000u) == 0x7F800000u) {
        const double m = ldexp((double)((bits & 0x7FFFFFu) | 0x800000u), 105);
        return (bits & 0x80000000u) ? -m : m;
    }
    float f;
    memcpy(&f, &bits, 4);
    return (double)f;
}
static uint32_t f32(double v) {
    if (v != v) return 0;
    const uint32_t sign = signbit(v) ? 0x80000000u : 0;
    const double a = fabs(v);
    if (a >= 6.8056469327705772e38) return sign | 0x7FFFFFFFu;          /* the value of 0x7FFFFFFF */
    if (a < 1.1754943508222875e-38) {                                   /* 2**-126: IEEE's denormal rounding */
        const float f = (float)a;
        uint32_t bits;
        memcpy(&bits, &f, 4);
        return sign | bits;
    }
    int exponent;
    const double fraction = frexp(a, &exponent);
    int biased = exponent + 126;
    double mantissa = nearbyint((fraction * 2.0 - 1.0) * 8388608.0);   /* half to even, as Python's round */
    if (mantissa == 8388608.0) { mantissa = 0; biased += 1; }
    if (biased > 255) return sign | 0x7FFFFFFFu;
    return sign | ((uint32_t)biased << 23) | (uint32_t)mantissa;
}
/* eefpu._order: min and max compare sign-magnitude patterns, -0 below +0. */
static inline long long ford(uint32_t bits) {
    return (bits & 0x80000000u) ? -(long long)(bits & 0x7FFFFFFFu) - 1 : (long long)bits;
}

static int cop1_runs_here(uint32_t w) {
    switch (RS(w)) {
        case 0x00: case 0x02: case 0x04: case 0x06: return 1;     /* mfc1 cfc1 mtc1 ctc1 */
        case 0x10:
            switch (FN(w)) {
                case 0x00: case 0x01: case 0x02: case 0x03: case 0x05: case 0x06: case 0x07:
                case 0x18: case 0x19: case 0x1A: case 0x1C: case 0x1D: case 0x1E: case 0x1F:
                case 0x24: case 0x28: case 0x29: case 0x30: case 0x32: case 0x34: case 0x36:
                    return 1;
                default:
                    return 0;               /* sqrt.s, rsqrt.s (pow in Python), the rest: emu.py's */
            }
        case 0x14: return FN(w) == 0x20;    /* cvt.s.w */
        default: return 0;
    }
}

static void cop1(EmuState* s, uint32_t w) {
    const unsigned rt = RT(w), rd = RD(w);
    const unsigned fd = SA(w), fs = RD(w), ft = RT(w);
    switch (RS(w)) {
        case 0x00: set32(s, rt, s->fpr[rd]); return;                                    /* mfc1 */
        case 0x02: set32(s, rt, rd < 16 ? 0x2E30u
                                        : 0x01000001u | (s->fp_cond ? 1u << 23 : 0) | s->fcr31); return;  /* cfc1 */
        case 0x04: s->fpr[rd] = get32(s, rt); return;                                   /* mtc1 */
        case 0x06:                                                                      /* ctc1 */
            if (rd == 31) {
                s->fp_cond = (get32(s, rt) >> 23) & 1;
                s->fcr31 = get32(s, rt) & 0x0003C078u;
            }
            return;
        case 0x14: s->fpr[fd] = f32((double)(int32_t)s->fpr[fs]); return;               /* cvt.s.w */
    }
    const double a = fval(s->fpr[fs]), b = fval(s->fpr[ft]), acc = fval(s->fp_acc);
    volatile double product;            /* two roundings, as Python's acc + a * b: never a fused multiply-add */
    switch (FN(w)) {
        case 0x00: s->fpr[fd] = f32(a + b); break;
        case 0x01: s->fpr[fd] = f32(a - b); break;
        case 0x02: s->fpr[fd] = f32(a * b); break;
        case 0x03: s->fpr[fd] = b == 0.0 ? (((s->fpr[fs] ^ s->fpr[ft]) & 0x80000000u) | 0x7FFFFFFFu)
                                         : f32(a / b); break;
        case 0x05: s->fpr[fd] = s->fpr[fs] & 0x7FFFFFFFu; break;      /* abs, neg, mov: bit operations */
        case 0x06: s->fpr[fd] = s->fpr[fs]; break;
        case 0x07: s->fpr[fd] = s->fpr[fs] ^ 0x80000000u; break;
        case 0x18: s->fp_acc = f32(a + b); break;
        case 0x19: s->fp_acc = f32(a - b); break;
        case 0x1A: s->fp_acc = f32(a * b); break;
        case 0x1C: product = a * b; s->fpr[fd] = f32(acc + product); break;
        case 0x1D: product = a * b; s->fpr[fd] = f32(acc - product); break;
        case 0x1E: product = a * b; s->fp_acc = f32(acc + product); break;
        case 0x1F: product = a * b; s->fp_acc = f32(acc - product); break;
        case 0x24: {                                                                    /* cvt.w.s */
            const uint32_t bits = s->fpr[fs];
            if ((bits & 0x7F800000u) <= 0x4E800000u) s->fpr[fd] = (uint32_t)(int64_t)fval(bits);
            else s->fpr[fd] = (bits & 0x80000000u) ? 0x80000000u : 0x7FFFFFFFu;
            break;
        }
        case 0x28: s->fpr[fd] = ford(s->fpr[ft]) > ford(s->fpr[fs]) ? s->fpr[ft] : s->fpr[fs]; break;   /* max */
        case 0x29: s->fpr[fd] = ford(s->fpr[ft]) < ford(s->fpr[fs]) ? s->fpr[ft] : s->fpr[fs]; break;   /* min */
        case 0x30: s->fp_cond = 0; break;
        case 0x32: s->fp_cond = a == b; break;                  /* eefpu.compare: the values */
        case 0x34: s->fp_cond = a < b; break;
        case 0x36: s->fp_cond = a <= b; break;
    }
}

/* VU0 in macro mode, as vu0.py models it (its docstring): an operand with a zero exponent is its signed zero,
   one with the maximum exponent the signed largest finite; the operation in double, the result rounded to
   single, saturated, denormals flushed; madd/msub round the product first; MAX/MINI compare sign-and-magnitude
   and hand back the operand as it is. */
#define VSIGN 0x80000000u
#define VMAX 0x7F7FFFFFu
#define VONE 0x3F800000u
enum { VREG_STATUS = 16, VREG_MAC = 17, VREG_CLIP = 18, VREG_R = 20, VREG_I = 21, VREG_Q = 22, VREG_VPU_STAT = 29 };

static inline uint32_t vclean(uint32_t bits) {
    const uint32_t e = bits & 0x7F800000u;
    if (e == 0) return bits & VSIGN;
    if (e == 0x7F800000u) return (bits & VSIGN) | VMAX;
    return bits;
}
static inline double vvalue(uint32_t bits) { return fval(vclean(bits)); }
static uint32_t vresult(double number) {
    if (number != number) return VMAX;
    const float f = (float)number;              /* beyond the largest single: an infinity, saturated below */
    uint32_t bits;
    memcpy(&bits, &f, 4);
    if ((bits & 0x7F800000u) == 0x7F800000u) return (bits & VSIGN) | VMAX;
    if ((bits & 0x7F800000u) == 0) return bits & VSIGN;
    return bits;
}
static inline int64_t vorder(uint32_t bits) {
    const int64_t magnitude = bits & 0x7FFFFFFFu;
    return (bits & VSIGN) ? -magnitude : magnitude;
}
enum { V_ADD, V_SUB, V_MUL, V_MADD, V_MSUB, V_MAX, V_MINI };
static uint32_t vbinary(int op, uint32_t a, uint32_t b, uint32_t acc) {
    const double x = vvalue(a), y = vvalue(b);
    switch (op) {
        case V_ADD: return vresult(x + y);
        case V_SUB: return vresult(x - y);
        case V_MUL: return vresult(x * y);
        case V_MADD: return vresult(vvalue(acc) + vvalue(vresult(x * y)));
        case V_MSUB: return vresult(vvalue(acc) - vvalue(vresult(x * y)));
        case V_MAX: return vorder(a) < vorder(b) ? b : a;
        default: return vorder(b) < vorder(a) ? b : a;     /* MINI */
    }
}
/* vu0._set_flags: MAC zero (bits 3-0) and sign (7-4) of each field dest names, x highest; STATUS Z/S + sticky. */
static void vflags(EmuState* s, const uint32_t values[4], unsigned dest) {
    uint32_t mac = 0;
    for (unsigned lane = 0; lane < 4; ++lane) {
        if (!(dest & (8u >> lane))) continue;
        const uint32_t bits = vclean(values[lane]);
        if ((bits & 0x7FFFFFFFu) == 0) mac |= 8u >> lane;
        if (bits & VSIGN) mac |= 0x80u >> lane;
    }
    s->vc[VREG_MAC] = mac;
    uint32_t status = (s->vc[VREG_STATUS] & ~0x3u) | ((mac & 0xF) ? 1u : 0u) | ((mac & 0xF0) ? 2u : 0u);
    s->vc[VREG_STATUS] = status | ((status & 3u) << 6);
}
static void vwrite(EmuState* s, unsigned index, const uint32_t values[4], unsigned dest) {
    if (index == 0) return;
    for (unsigned lane = 0; lane < 4; ++lane)
        if (dest & (8u >> lane)) s->vf[index][lane] = values[lane];
}
static uint32_t vftoi(uint32_t x, unsigned shift) {
    const double number = vvalue(x) * (double)(1u << shift);
    if (number >= 2147483647.0) return 0x7FFFFFFFu;
    if (number <= -2147483648.0) return 0x80000000u;
    return (uint32_t)(int64_t)number;
}

/* An FMAC operation's form from its encoding: op, to the accumulator, operand (0-3 a field, 4 Q, 5 I, 6 vector);
   0 when the encoding is none of them. */
static int vfmac(uint32_t w, int* op, int* to_acc, int* operand) {
    const unsigned funct = w & 0x3F;
    *to_acc = 0;
    if (funct < 0x3C) {
        if (funct <= 0x1B) {                    /* add sub madd msub max mini mul, broadcast x y z w */
            static const int ops[7] = {V_ADD, V_SUB, V_MADD, V_MSUB, V_MAX, V_MINI, V_MUL};
            *op = ops[funct >> 2];
            *operand = funct & 3;
            return 1;
        }
        switch (funct) {
            case 0x1C: *op = V_MUL; *operand = 4; return 1;
            case 0x1D: *op = V_MAX; *operand = 5; return 1;
            case 0x1E: *op = V_MUL; *operand = 5; return 1;
            case 0x1F: *op = V_MINI; *operand = 5; return 1;
            case 0x20: *op = V_ADD; *operand = 4; return 1;
            case 0x21: *op = V_MADD; *operand = 4; return 1;
            case 0x22: *op = V_ADD; *operand = 5; return 1;
            case 0x23: *op = V_MADD; *operand = 5; return 1;
            case 0x24: *op = V_SUB; *operand = 4; return 1;
            case 0x25: *op = V_MSUB; *operand = 4; return 1;
            case 0x26: *op = V_SUB; *operand = 5; return 1;
            case 0x27: *op = V_MSUB; *operand = 5; return 1;
            case 0x28: *op = V_ADD; *operand = 6; return 1;
            case 0x29: *op = V_MADD; *operand = 6; return 1;
            case 0x2A: *op = V_MUL; *operand = 6; return 1;
            case 0x2B: *op = V_MAX; *operand = 6; return 1;
            case 0x2C: *op = V_SUB; *operand = 6; return 1;
            case 0x2D: *op = V_MSUB; *operand = 6; return 1;
            case 0x2F: *op = V_MINI; *operand = 6; return 1;
            default: return 0;
        }
    }
    const unsigned code = ((w >> 4) & 0x7C) | (w & 3);
    *to_acc = 1;
    if (code <= 0x0F) {                          /* adda suba madda msuba, broadcast */
        static const int ops[4] = {V_ADD, V_SUB, V_MADD, V_MSUB};
        *op = ops[code >> 2];
        *operand = code & 3;
        return 1;
    }
    if (code >= 0x18 && code <= 0x1B) { *op = V_MUL; *operand = code & 3; return 1; }
    switch (code) {
        case 0x1C: *op = V_MUL; *operand = 4; return 1;
        case 0x1E: *op = V_MUL; *operand = 5; return 1;
        case 0x20: *op = V_ADD; *operand = 4; return 1;
        case 0x21: *op = V_MADD; *operand = 4; return 1;
        case 0x22: *op = V_ADD; *operand = 5; return 1;
        case 0x23: *op = V_MADD; *operand = 5; return 1;
        case 0x24: *op = V_SUB; *operand = 4; return 1;
        case 0x25: *op = V_MSUB; *operand = 4; return 1;
        case 0x26: *op = V_SUB; *operand = 5; return 1;
        case 0x27: *op = V_MSUB; *operand = 5; return 1;
        case 0x28: *op = V_ADD; *operand = 6; return 1;
        case 0x29: *op = V_MADD; *operand = 6; return 1;
        case 0x2A: *op = V_MUL; *operand = 6; return 1;
        case 0x2C: *op = V_SUB; *operand = 6; return 1;
        case 0x2D: *op = V_MSUB; *operand = 6; return 1;
        default: return 0;
    }
}

/* The macro-mode operations this core runs (vu0.SUPPORTED's, by encoding); 0 for the others (vcallms, the
   VI loads and stores, vlqi..., and every encoding vu0.py does not have) - emu.py's. */
static int vu0_runs_here(uint32_t w) {
    if (!(w & (1u << 25))) return 0;
    int op, to_acc, operand;
    if (vfmac(w, &op, &to_acc, &operand)) return 1;
    const unsigned funct = w & 0x3F;
    if (funct < 0x3C) return funct == 0x2E || funct == 0x30 || funct == 0x31 || funct == 0x32 || funct == 0x34 ||
                             funct == 0x35;    /* vopmsub viadd visub viaddi viand vior */
    const unsigned code = ((w >> 4) & 0x7C) | (w & 3);
    switch (code) {
        case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17:  /* vitof, vftoi */
        case 0x1D: case 0x2E: case 0x2F: case 0x30: case 0x31:  /* vabs vopmula vnop vmove vmr32 (vclipw: emu.py) */
        case 0x38: case 0x39: case 0x3A: case 0x3B: case 0x3C: case 0x3D:  /* vdiv vsqrt vrsqrt vwaitq vmtir vmfir */
        case 0x40: case 0x41: case 0x42: case 0x43:                          /* vrnext vrget vrinit vrxor */
            return 1;
        default:
            return 0;
    }
}

static void vu0_execute(EmuState* s, uint32_t w) {
    const unsigned dest = (w >> 21) & 0xF, ft = (w >> 16) & 31, fs = (w >> 11) & 31, fd = (w >> 6) & 31;
    int op, to_acc, operand;
    if (vfmac(w, &op, &to_acc, &operand)) {
        uint32_t b[4], out[4];
        for (unsigned lane = 0; lane < 4; ++lane)
            b[lane] = operand < 4 ? s->vf[ft][operand] : operand == 4 ? s->vc[VREG_Q]
                    : operand == 5 ? s->vc[VREG_I] : s->vf[ft][lane];
        for (unsigned lane = 0; lane < 4; ++lane) out[lane] = vbinary(op, s->vf[fs][lane], b[lane], s->vacc[lane]);
        if (to_acc) {
            for (unsigned lane = 0; lane < 4; ++lane)
                if (dest & (8u >> lane)) s->vacc[lane] = out[lane];
        } else {
            vwrite(s, fd, out, dest);
        }
        if (op != V_MAX && op != V_MINI) vflags(s, out, dest);
        return;
    }
    const unsigned funct = w & 0x3F, fsf = (w >> 21) & 3, ftf = (w >> 23) & 3;
    if (funct < 0x3C) {
        switch (funct) {
            case 0x2E: {                                                                   /* vopmsub */
                const uint32_t* a = s->vf[fs];
                const uint32_t* bb = s->vf[ft];
                const uint32_t out[4] = {vbinary(V_MSUB, a[1], bb[2], s->vacc[0]), vbinary(V_MSUB, a[2], bb[0], s->vacc[1]),
                                         vbinary(V_MSUB, a[0], bb[1], s->vacc[2]), 0};
                if (fd != 0)
                    for (unsigned lane = 0; lane < 3; ++lane)
                        if (dest & (8u >> lane)) s->vf[fd][lane] = out[lane];
                if (fd) {
                    uint32_t now[4];
                    memcpy(now, s->vf[fd], sizeof now);
                    vflags(s, now, dest);
                } else {
                    vflags(s, out, dest);
                }
                return;
            }
            case 0x30: case 0x31: case 0x34: case 0x35: {                                 /* viadd visub viand vior */
                const uint32_t a = s->vc[fs], bb = s->vc[ft];
                const uint32_t out = funct == 0x30 ? a + bb : funct == 0x31 ? a - bb : funct == 0x34 ? (a & bb) : (a | bb);
                if (fd > 0 && fd < 16) s->vc[fd] = out & 0xFFFF;
                return;
            }
            case 0x32: {                                                                   /* viaddi */
                const int32_t imm = (fd & 0x10) ? (int32_t)fd - 32 : (int32_t)fd;
                if (ft > 0 && ft < 16) s->vc[ft] = (uint32_t)((int64_t)s->vc[fs] + imm) & 0xFFFF;
                return;
            }
        }
        return;
    }
    const unsigned code = ((w >> 4) & 0x7C) | (w & 3);
    uint32_t out[4];
    switch (code) {
        case 0x10: case 0x11: case 0x12: case 0x13: {                                     /* vitof 0 4 12 15 */
            static const unsigned shifts[4] = {0, 4, 12, 15};
            for (unsigned lane = 0; lane < 4; ++lane)
                out[lane] = vresult((double)(int32_t)s->vf[fs][lane] / (double)(1u << shifts[code & 3]));
            vwrite(s, ft, out, dest);
            return;
        }
        case 0x14: case 0x15: case 0x16: case 0x17: {                                     /* vftoi 0 4 12 15 */
            static const unsigned shifts[4] = {0, 4, 12, 15};
            for (unsigned lane = 0; lane < 4; ++lane) out[lane] = vftoi(s->vf[fs][lane], shifts[code & 3]);
            vwrite(s, ft, out, dest);
            return;
        }
        case 0x1D:                                                                         /* vabs */
            for (unsigned lane = 0; lane < 4; ++lane) out[lane] = vclean(s->vf[fs][lane]) & ~VSIGN;
            vwrite(s, ft, out, dest);
            return;
        case 0x2F: case 0x3B: return;                                                     /* vnop vwaitq */
        case 0x2E: {                                                                       /* vopmula */
            const uint32_t* a = s->vf[fs];
            const uint32_t* bb = s->vf[ft];
            const uint32_t prod[3] = {vresult(vvalue(a[1]) * vvalue(bb[2])), vresult(vvalue(a[2]) * vvalue(bb[0])),
                                      vresult(vvalue(a[0]) * vvalue(bb[1]))};
            for (unsigned lane = 0; lane < 3; ++lane)
                if (dest & (8u >> lane)) s->vacc[lane] = prod[lane];
            uint32_t now[4];
            memcpy(now, s->vacc, sizeof now);
            vflags(s, now, dest);
            return;
        }
        case 0x30:                                                                         /* vmove */
            memcpy(out, s->vf[fs], sizeof out);
            vwrite(s, ft, out, dest);
            return;
        case 0x31: {                                                                       /* vmr32 */
            const uint32_t* a = s->vf[fs];
            out[0] = a[1]; out[1] = a[2]; out[2] = a[3]; out[3] = a[0];
            vwrite(s, ft, out, dest);
            return;
        }
        case 0x38: {                                                                       /* vdiv */
            const uint32_t num = s->vf[fs][fsf], den = s->vf[ft][ftf];
            s->vc[VREG_Q] = vvalue(den) == 0.0 ? (((num ^ den) & VSIGN) | VMAX) : vresult(vvalue(num) / vvalue(den));
            return;
        }
        case 0x39:                                                                         /* vsqrt */
            s->vc[VREG_Q] = vresult(sqrt(fabs(vvalue(s->vf[ft][ftf]))));
            return;
        case 0x3A: {                                                                       /* vrsqrt */
            const uint32_t num = s->vf[fs][fsf];
            const double root = sqrt(fabs(vvalue(s->vf[ft][ftf])));
            s->vc[VREG_Q] = root == 0.0 ? ((num & VSIGN) | VMAX) : vresult(vvalue(num) / root);
            return;
        }
        case 0x3C:                                                                         /* vmtir */
            if (ft != 0 && ft < 16) s->vc[ft] = s->vf[fs][fsf] & 0xFFFF;
            return;
        case 0x3D: {                                                                       /* vmfir */
            uint32_t extended = s->vc[fs] & 0xFFFF;
            if (extended & 0x8000) extended = extended - 0x10000u;
            for (unsigned lane = 0; lane < 4; ++lane) out[lane] = extended;
            vwrite(s, ft, out, dest);
            return;
        }
        case 0x40: {                                                                       /* vrnext */
            if (ft == 0) return;
            const uint32_t r = s->vc[VREG_R];
            const uint32_t bit = ((r >> 4) & 1) ^ ((r >> 22) & 1);
            s->vc[VREG_R] = VONE | (((r << 1) ^ bit) & 0x7FFFFF);
            for (unsigned lane = 0; lane < 4; ++lane) out[lane] = s->vc[VREG_R];
            vwrite(s, ft, out, dest);
            return;
        }
        case 0x41:                                                                         /* vrget */
            for (unsigned lane = 0; lane < 4; ++lane) out[lane] = s->vc[VREG_R];
            vwrite(s, ft, out, dest);
            return;
        case 0x42: s->vc[VREG_R] = VONE | (s->vf[fs][fsf] & 0x7FFFFF); return;               /* vrinit */
        case 0x43: s->vc[VREG_R] = VONE | ((s->vc[VREG_R] ^ s->vf[fs][fsf]) & 0x7FFFFF); return;  /* vrxor */
    }
}

/* COP2 moves (vu0.move_to / move_from): qmtc2, ctc2, qmfc2, cfc2 - the interlock bit clear only. */
static int cop2_move_runs_here(uint32_t w) {
    if (w & (1u << 25)) return 0;
    if (w & 0x7FF) return 0;
    switch (RS(w)) {
        case 0x01: case 0x05: return 1;
        case 0x06: return RD(w) != 31;                                  /* ctc2: CMSAR1 starts VU1 (vu0.py refuses) */
        case 0x02: return RD(w) != 26;                                                  /* cfc2: TPC not modelled */
        default: return 0;
    }
}

static void cop2_move(EmuState* s, uint32_t w) {
    const unsigned rt = RT(w), index = RD(w);
    switch (RS(w)) {
        case 0x05: {                                                                       /* qmtc2 */
            if (!index) return;
            const uint64_t lo = get(s, rt), hi = s->regs_hi[rt];
            s->vf[index][0] = (uint32_t)lo; s->vf[index][1] = (uint32_t)(lo >> 32);
            s->vf[index][2] = (uint32_t)hi; s->vf[index][3] = (uint32_t)(hi >> 32);
            return;
        }
        case 0x06: {                                                                       /* ctc2 */
            const uint64_t v = get(s, rt);
            /* vu0.move_to: STATUS its sticky bits only; MAC, TPC, VPU-STAT, 19, 23-25, 28 not at all; CMSAR0 16 bits. */
            if (index == 16) s->vc[16] = (s->vc[16] & 0x3Fu) | ((uint32_t)v & 0xFC0u);
            else if (index == 17 || index == 19 || (index >= 23 && index <= 26) || index == 28 || index == 29) { }
            else if (index == 27) s->vc[27] = (uint32_t)v & 0xFFFF;
            else if (index < 16) { if (index) s->vc[index] = (uint32_t)v & 0xFFFF; }
            else if (index == VREG_R) s->vc[VREG_R] = VONE | ((uint32_t)v & 0x7FFFFF);
            else if (index == VREG_CLIP) s->vc[VREG_CLIP] = (uint32_t)v & 0xFFFFFF;
            else s->vc[index] = (uint32_t)v;
            return;
        }
        case 0x01: {                                                                       /* qmfc2 */
            const uint32_t* v = s->vf[index];
            if (rt) {
                s->regs[rt] = (uint64_t)v[0] | (uint64_t)v[1] << 32;
                s->regs_hi[rt] = (uint64_t)v[2] | (uint64_t)v[3] << 32;
            }
            return;
        }
        case 0x02:                                                                         /* cfc2 */
            /* vu0.move_from: R reads its 23 fraction bits, 19 and 24 read constants. */
            set32(s, rt, index == VREG_VPU_STAT ? 0 : index == 19 ? 0x2E30u : index == 24 ? 0x0C0Cu
                         : index == VREG_R ? (s->vc[VREG_R] & 0x7FFFFFu) : s->vc[index]);
            return;
    }
}

/* Execute a plain instruction can_run accepted. */
static void run(EmuState* s, uint32_t w) {
    const unsigned rs = RS(w), rt = RT(w), rd = RD(w), sa = SA(w);
    switch (OP(w)) {
        case 0x00:
            switch (FN(w)) {
                case 0x00: set32(s, rd, (uint64_t)get32(s, rt) << sa); break;
                case 0x02: set32(s, rd, get32(s, rt) >> sa); break;
                case 0x03: set32(s, rd, (uint64_t)((int32_t)get32(s, rt) >> sa)); break;
                case 0x04: set32(s, rd, (uint64_t)get32(s, rt) << (get(s, rs) & 31)); break;
                case 0x06: set32(s, rd, get32(s, rt) >> (get(s, rs) & 31)); break;
                case 0x07: set32(s, rd, (uint64_t)((int32_t)get32(s, rt) >> (get(s, rs) & 31))); break;
                case 0x0A: if (get(s, rt) == 0) set(s, rd, get(s, rs)); break;
                case 0x0B: if (get(s, rt) != 0) set(s, rd, get(s, rs)); break;
                case 0x0F: break;                                   /* sync */
                case 0x10: set(s, rd, s->hi); break;
                case 0x11: s->hi = get(s, rs); break;
                case 0x12: set(s, rd, s->lo); break;
                case 0x13: s->lo = get(s, rs); break;
                case 0x14: set(s, rd, get(s, rt) << (get(s, rs) & 63)); break;
                case 0x16: set(s, rd, get(s, rt) >> (get(s, rs) & 63)); break;
                case 0x17: set(s, rd, (uint64_t)((int64_t)get(s, rt) >> (get(s, rs) & 63))); break;
                case 0x18: mult(s, w, 1, &s->lo, &s->hi); break;
                case 0x19: mult(s, w, 0, &s->lo, &s->hi); break;
                case 0x1A: divide(get32(s, rs), get32(s, rt), 1, &s->lo, &s->hi); break;
                case 0x1B: divide(get32(s, rs), get32(s, rt), 0, &s->lo, &s->hi); break;
                case 0x20: case 0x21: set32(s, rd, (uint64_t)get32(s, rs) + get32(s, rt)); break;
                case 0x22: case 0x23: set32(s, rd, (uint64_t)get32(s, rs) - get32(s, rt)); break;
                case 0x24: set(s, rd, get(s, rs) & get(s, rt)); break;
                case 0x25: set(s, rd, get(s, rs) | get(s, rt)); break;
                case 0x26: set(s, rd, get(s, rs) ^ get(s, rt)); break;
                case 0x27: set(s, rd, ~(get(s, rs) | get(s, rt))); break;
                case 0x2A: set32(s, rd, (int64_t)get(s, rs) < (int64_t)get(s, rt) ? 1 : 0); break;
                case 0x2B: set32(s, rd, get(s, rs) < get(s, rt) ? 1 : 0); break;
                case 0x2C: case 0x2D: set(s, rd, get(s, rs) + get(s, rt)); break;
                case 0x2E: case 0x2F: set(s, rd, get(s, rs) - get(s, rt)); break;
                case 0x38: set(s, rd, get(s, rt) << sa); break;
                case 0x3A: set(s, rd, get(s, rt) >> sa); break;
                case 0x3B: set(s, rd, (uint64_t)((int64_t)get(s, rt) >> sa)); break;
                case 0x3C: set(s, rd, get(s, rt) << (sa + 32)); break;
                case 0x3E: set(s, rd, get(s, rt) >> (sa + 32)); break;
                case 0x3F: set(s, rd, (uint64_t)((int64_t)get(s, rt) >> (sa + 32))); break;
            }
            break;
        case 0x1C:
            switch (FN(w)) {
                case 0x00: madd(s, w, 1); break;
                case 0x01: madd(s, w, 0); break;
                case 0x10: set(s, rd, s->hi1); break;
                case 0x11: s->hi1 = get(s, rs); break;
                case 0x12: set(s, rd, s->lo1); break;
                case 0x13: s->lo1 = get(s, rs); break;
                case 0x18: mult(s, w, 1, &s->lo1, &s->hi1); break;
                case 0x19: mult(s, w, 0, &s->lo1, &s->hi1); break;
                case 0x1A: divide(get32(s, rs), get32(s, rt), 1, &s->lo1, &s->hi1); break;
                case 0x1B: divide(get32(s, rs), get32(s, rt), 0, &s->lo1, &s->hi1); break;
            }
            break;
        case 0x11: cop1(s, w); break;
        case 0x12:
            if (w & (1u << 25)) vu0_execute(s, w);
            else cop2_move(s, w);
            break;
        case 0x36: {
            const uint64_t a = (get32(s, rs) + SIMM(w)) & 0xFFFFFFFFu & ~(uint64_t)0xF;
            uint32_t lanes[4];
            for (unsigned i = 0; i < 4; ++i) {
                lanes[i] = (uint32_t)read_le(s, a + 4 * i, 4);
                watch(s, a + 4 * i, 4, lanes[i]);
            }
            if (rt) memcpy(s->vf[rt], lanes, sizeof lanes);
            break;
        }
        case 0x3E: {
            const uint64_t a = (get32(s, rs) + SIMM(w)) & 0xFFFFFFFFu & ~(uint64_t)0xF;
            uint8_t bytes[16];
            for (unsigned i = 0; i < 4; ++i)
                for (unsigned k = 0; k < 4; ++k) bytes[4 * i + k] = (uint8_t)(s->vf[rt][i] >> (8 * k));
            write_bytes(s, a, bytes, 16);
            break;
        }
        case 0x31: {
            const uint64_t a = (get32(s, rs) + SIMM(w)) & 0xFFFFFFFFu;
            const uint64_t v = read_le(s, a, 4);
            watch(s, a, 4, v);
            s->fpr[rt] = (uint32_t)v;
            break;
        }
        case 0x39: write_le(s, (get32(s, rs) + SIMM(w)) & 0xFFFFFFFFu, s->fpr[rt], 4); break;
        case 0x08: case 0x09: set32(s, rt, (uint64_t)get32(s, rs) + (uint64_t)SIMM(w)); break;
        case 0x0A: set32(s, rt, (int64_t)get(s, rs) < SIMM(w) ? 1 : 0); break;
        case 0x0B: set32(s, rt, get(s, rs) < (uint64_t)SIMM(w) ? 1 : 0); break;
        case 0x0C: set(s, rt, get(s, rs) & UIMM(w)); break;
        case 0x0D: set(s, rt, get(s, rs) | UIMM(w)); break;
        case 0x0E: set(s, rt, get(s, rs) ^ UIMM(w)); break;
        case 0x0F: set32(s, rt, UIMM(w) << 16); break;
        case 0x18: case 0x19: set(s, rt, get(s, rs) + (uint64_t)SIMM(w)); break;
        case 0x2F: case 0x33: break;
        case 0x20: case 0x21: case 0x23: case 0x24: case 0x25: case 0x27: case 0x37: {
            const unsigned op = OP(w);
            const unsigned size = (op == 0x20 || op == 0x24) ? 1 : (op == 0x21 || op == 0x25) ? 2 : op == 0x37 ? 8 : 4;
            const uint64_t a = (get32(s, rs) + SIMM(w)) & 0xFFFFFFFFu;
            const uint64_t v = read_le(s, a, size);
            watch(s, a, size, v);
            switch (op) {
                case 0x20: set32(s, rt, (uint64_t)(int64_t)(int8_t)v); break;
                case 0x21: set32(s, rt, (uint64_t)(int64_t)(int16_t)v); break;
                case 0x23: set32(s, rt, v); break;
                default: set(s, rt, v); break;          /* lbu lhu lwu ld: zero-extended */
            }
            break;
        }
        case 0x28: write_le(s, (get32(s, rs) + SIMM(w)) & 0xFFFFFFFFu, get(s, rt), 1); break;
        case 0x29: write_le(s, (get32(s, rs) + SIMM(w)) & 0xFFFFFFFFu, get(s, rt), 2); break;
        case 0x2B: write_le(s, (get32(s, rs) + SIMM(w)) & 0xFFFFFFFFu, get(s, rt), 4); break;
        case 0x3F: write_le(s, (get32(s, rs) + SIMM(w)) & 0xFFFFFFFFu, get(s, rt), 8); break;
        case 0x1E: {
            const uint64_t a = (get32(s, rs) + SIMM(w)) & 0xFFFFFFFFu & ~(uint64_t)0xF;
            set(s, rt, read_le(s, a, 8));
            s->regs_hi[rt] = read_le(s, a + 8, 8);     /* emu.py writes regs_hi even for $zero */
            break;
        }
        case 0x1F: {
            const uint64_t a = (get32(s, rs) + SIMM(w)) & 0xFFFFFFFFu & ~(uint64_t)0xF;
            write_le(s, a, get(s, rt), 8);
            write_le(s, a + 8, s->regs_hi[rt], 8);
            break;
        }
    }
}

/* A transfer (Machine._resolve_transfer): 1 if it is one this core takes, its target (or none), link,
   nullify; 0 for one emu.py must take (COP branches, the *all forms, anything else). */
static int transfer(const EmuState* s, uint32_t w, uint32_t pc, int* has_target, uint32_t* target, int* link,
                    int* nullify, int* indirect) {
    const unsigned op = OP(w);
    const int64_t rsv = (int64_t)get(s, RS(w)), rtv = (int64_t)get(s, RT(w));
    const uint32_t branch_target = (uint32_t)(pc + 4 + (uint32_t)(SIMM(w) << 2));
    int taken, likely = 0;
    *link = 0;
    *nullify = 0;
    *indirect = 0;
    *has_target = 0;
    switch (op) {
        case 0x02: case 0x03:
            *has_target = 1;
            *target = ((pc + 4) & 0xF0000000u) | ((w & 0x03FFFFFFu) << 2);
            *link = op == 0x03;
            return 1;
        case 0x00:
            if (FN(w) == 0x08) {        /* jr */
                *has_target = 1;
                *target = get32(s, RS(w));
                *indirect = RS(w) != 31;
                return 1;
            }
            if (FN(w) == 0x09) {        /* jalr: emu.py links $ra, whatever rd says */
                *has_target = 1;
                *target = get32(s, RS(w));
                *link = 1;
                *indirect = 1;
                return 1;
            }
            return 0;
        case 0x04: taken = rsv == rtv; break;
        case 0x05: taken = rsv != rtv; break;
        case 0x06: taken = rsv <= 0; break;
        case 0x07: taken = rsv > 0; break;
        case 0x14: taken = rsv == rtv; likely = 1; break;
        case 0x15: taken = rsv != rtv; likely = 1; break;
        case 0x16: taken = rsv <= 0; likely = 1; break;
        case 0x17: taken = rsv > 0; likely = 1; break;
        case 0x10: case 0x11:           /* bc0f/t(l): COP0's condition reads as done; bc1f/t(l): fp_cond */
            if (RS(w) != 0x08 || RT(w) > 3) return 0;
            taken = op == 0x10 ? (RT(w) & 1) : ((RT(w) & 1) ? s->fp_cond != 0 : s->fp_cond == 0);
            likely = RT(w) >= 2;
            break;
        case 0x01:
            switch (RT(w)) {
                case 0x00: taken = rsv < 0; break;
                case 0x01: taken = rsv >= 0; break;
                case 0x02: taken = rsv < 0; likely = 1; break;
                case 0x03: taken = rsv >= 0; likely = 1; break;
                case 0x10: taken = rsv < 0; *link = 1; break;      /* bltzal: links taken or not */
                case 0x11: taken = rsv >= 0; *link = 1; break;     /* bgezal */
                default: return 0;
            }
            break;
        default:
            return 0;
    }
    if (taken) {
        *has_target = 1;
        *target = branch_target;
    } else if (likely) {
        *has_target = 1;
        *target = pc + 8;
        *nullify = 1;
    }
    return 1;
}

static inline int is_transfer(uint32_t w) {
    const unsigned op = OP(w);
    if (op == 0x00) return FN(w) == 0x08 || FN(w) == 0x09;
    if (op == 0x01) {
        const unsigned rt = RT(w);
        return rt <= 0x03 || (rt >= 0x10 && rt <= 0x13);
    }
    if (op == 0x10 || op == 0x11 || op == 0x12) return RS(w) == 0x08;      /* the coprocessor branches */
    return (op >= 0x02 && op <= 0x07) || (op >= 0x14 && op <= 0x17);
}

/* One Machine.step, or 0 (nothing done) when emu.py must take it. */
static int step_once(EmuState* s);

static int step(EmuState* s) {
    const uint32_t pc = s->pc;
    if (!step_once(s)) return 0;
    if (s->cov_len && pc - s->cov_start < 4u * s->cov_len) s->cov[(pc - s->cov_start) >> 2] = 1;
    return 1;
}

static int step_once(EmuState* s) {
    const uint32_t pc = s->pc;
    if (!is_code(s, pc) || !resolvable(s, pc, 4)) return 0;
    const uint32_t w = (uint32_t)read_le(s, pc, 4);
    if (is_transfer(w)) {
        int has_target, link, nullify, indirect;
        uint32_t target = 0;
        if (!transfer(s, w, pc, &has_target, &target, &link, &nullify, &indirect)) return 0;
        uint32_t slot = 0;
        if (!nullify) {
            if (!resolvable(s, pc + 4, 4)) return 0;
            slot = (uint32_t)read_le(s, pc + 4, 4);
            if (is_transfer(slot) || !can_run(s, slot)) return 0;
        }
        /* A call or tail call through a pointer to nowhere: emu.py's stub. */
        if (indirect && has_target && target != RETURN_SENTINEL && !is_code(s, target)) return 0;
        /* Effects, in emu.py's order: fetch (watch), the slot's fetch and run, the link, the new pc. */
        watch(s, pc, 4, w);
        if (!nullify) {
            watch(s, pc + 4, 4, slot);
            s->pc = pc + 4;
            run(s, slot);
        }
        if (link) set(s, 31, (uint64_t)(pc + 8));
        s->pc = has_target ? target : pc + 8;
        return 1;
    }
    if (!can_run(s, w)) return 0;
    watch(s, pc, 4, w);
    run(s, w);
    s->pc = pc + 4;
    return 1;
}

static inline int stopped(const EmuState* s) {
    if (s->mode == 0) return s->pc == RETURN_SENTINEL;
    return s->pc == s->return_address && (uint32_t)get(s, 29) >= s->entry_sp;
}

/* Run as Machine._call_once / run_to_return's loops do, until they would stop, or until the next step is
   emu.py's (NEED_PYTHON: nothing of it done), the budget is spent, or the journal buffer is nearly full. */
EXPORT int emu_core_run(EmuState* s) {
    for (;;) {
        if (stopped(s)) return STOP_RETURNED;
        if (s->journal_on && s->jr_count + 4 > s->jr_cap) return JOURNAL_FULL;
        if (!step(s)) return NEED_PYTHON;
        if (++s->executed > s->budget) return OVER_BUDGET;
    }
}

/* Only one step (the differential test drives this). 1 done, 0 emu.py's. */
EXPORT int emu_core_step(EmuState* s) { return step(s); }

EXPORT int emu_core_state_size(void) { return (int)sizeof(EmuState); }
