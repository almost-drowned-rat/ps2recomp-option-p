/*
 * Device registers with hardware meaning, stage one: just enough for code that
 * starts a transfer and waits for it, or waits for a vertical blank, to go on.
 *
 * DMA: setting a channel's STR bit starts a transfer; here it completes at once
 *      - the bit clears and the channel's interrupt status is raised. VIF0, VIF1
 *      and GIF transfers are walked (dma.cpp), chains included: VIF1's data
 *      goes to its VIFcode decoder, the rest is counted. Nothing reaches a GS
 *      yet.
 * GIF: while PATH3 is masked, a PATH3 transfer's data waits in the GIF FIFO
 *      (16 quadwords deep), and GIF_STAT's FQC counts it - code handing PATH3
 *      over to VIF1 waits for the GIF to have taken its data before letting VIF1
 *      go. Unmasking drains it.
 * GS:  CSR reports a vertical blank on every read, its field bit alternating, so
 *      a wait for the next frame ends.
 * Timers: in host time, with their interrupts (timers.cpp).
 * VIF1: quadwords stored to its FIFO are decoded as VIFcodes (vif.cpp);
 *      MSKPATH3 shows in GIF_STAT's M3P bit, which code that hands PATH3 over
 *      waits on.
 *
 * Every register the game touches is counted for the report, so the next device
 * to implement is the one the game leans on.
 */

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <functional>
#include <cstring>
#include <deque>
#include <mutex>
#include <memory>
#include <string>
#include <vector>

#include "ps2platform/platform.h"
#include "ps2host/dma.h"
#include "ps2host/timeline.h"
#ifdef PS2HOST_VULKAN
#include "ps2gpu/display.h"
#endif
#include "ps2host/gs.h"
#include "ps2host/ipu.h"
#include "ps2host/iop_vm.h"
#include "ps2host/memory.h"
#include "ps2host/path3.h"
#include "ps2host/music.h"
#include "ps2host/run.h"
#include "ps2host/settings.h"
#include "ps2host/vif.h"
#include "ps2host/vu.h"
#include "ps2host/profile.h"

namespace ps2host {

namespace {

struct Channel {
    u32 base;
    const char* name;
    u32 status_bit;                 // its bit in D_STAT
    bool walked;                    // its transfers are followed (dma.cpp)
};

const Channel kChannels[] = {
    {0x10008000u, "VIF0", 0, true}, {0x10009000u, "VIF1", 1, true},
    {0x1000A000u, "GIF", 2, true}, {0x1000B000u, "IPU_FROM", 3, false},
    {0x1000B400u, "IPU_TO", 4, false}, {0x1000C000u, "SIF0", 5, false},
    {0x1000C400u, "SIF1", 6, false}, {0x1000C800u, "SIF2", 7, false},
    {0x1000D000u, "SPR_FROM", 8, true}, {0x1000D400u, "SPR_TO", 9, true},
};

// Register offsets within a channel.
constexpr u32 kMadr = 0x10, kQwc = 0x20, kTadr = 0x30, kAsr0 = 0x40, kAsr1 = 0x50;
constexpr u32 kGifFifoDepth = 16;

constexpr u32 kDStat = 0x1000E010u;

// D_STAT: writing 1 clears a status bit (0-15) and flips a mask bit (16-31), so
// what a store leaves is computed from what was there, kept here.
u32 d_stat = 0;

void SetDStat(u32 value) {
    d_stat = value;
    *reinterpret_cast<u32*>(GuestMemory().Translate(kDStat, 4)) = value;
}
constexpr u32 kGsCsr = 0x12001000u;
constexpr u32 kGifStat = 0x10003020u;
constexpr u32 kGifStatM3P = 1u << 1;
constexpr u32 kGifStatFqcShift = 24, kGifStatFqc = 0x1Fu << kGifStatFqcShift;

// PATH3: GIF DMA data to the GS, held while VIF1 has PATH3 masked - it goes
// after the PATH2 data VIF1 sends meanwhile, as the GIF arbitrates.
//
// A mask takes effect only at the end of the GIF packet PATH3 is in - the one
// whose tag has EOP set - so unmasking and masking again at once lets exactly
// one EOP-ended group through. Burnout 3 queues a picture's texture uploads
// (161,280 words at the car select) on PATH3 while it is masked and opens a
// window like that before each group of draws: five groups, five windows,
// each texture slot refilled between the draws that use its contents.
// Releasing everything at the first window refilled every slot early, and the
// garage was drawn with whatever was uploaded last into its slots (the font).
Path3Queue g_path3_held;
bool g_path3_masked = false;

void Path3(const u32* words, u32 count, u32 from) {
    // Behind what is still held, even unmasked: the GIF keeps PATH3 in order.
    if (g_path3_masked || !g_path3_held.empty()) g_path3_held.Push(words, count);
    else GsWords(3, words, count, from);
}

/* The same from words `owner` keeps (VIF1's worker's copy): held, not copied. */
void Path3Kept(const u32* words, u32 count, u32 from, std::shared_ptr<const void> owner) {
    if (g_path3_masked || !g_path3_held.empty()) g_path3_held.Push(words, count, std::move(owner));
    else GsWords(3, words, count, from);
}

void ReleasePath3(bool all) {
    g_path3_held.ReleaseBlock(all, [](const u32* words, std::size_t count) {
        GsWords(3, words, u32(count));
    });
}

u32* Register(u32 address) { return reinterpret_cast<u32*>(GuestMemory().Translate(address, 4)); }

// The scratchpad channels: SADR (base + 0x80) is the scratchpad address.
// SPR_FROM copies QWC quadwords from it to MADR; SPR_TO copies from memory
// (normal or a source chain) into it.
constexpr u32 kSadr = 0x80;

// Main RAM or the scratchpad, with nothing watching stores: no hooks to run,
// so a copy may go through pointers.
bool Plain(const Memory& m, u32 address) {
    return !m.watching() && ((address & 0x1FFFFFFFu) < 0x10000000u || (address >> 28) == 0x7);
}

std::string Scratchpad(const Channel& ch, u32 chcr, bool describe) {
    Memory& m = GuestMemory();
    u32 sadr = u32(m.Read(ch.base + kSadr, 4)) & 0x3FF0u;
    const u32 madr = u32(m.Read(ch.base + kMadr, 4)), qwc = u32(m.Read(ch.base + kQwc, 4));
    char text[160];
    // Interleave mode (CHCR.MOD 2, ps2autotests dma/spr/interleave): D_SQWC's TQWC quadwords (bits 16-23) moved,
    // then SQWC (bits 0-7) skipped in main memory - after every whole block, the last one too; the scratchpad
    // side contiguous. TQWC 0: as a normal transfer (the console's behaviour there is not known).
    const u32 sqwc = u32(m.Read(0x1000E030u, 4));
    const u32 skip = sqwc & 0xFFu, block = (sqwc >> 16) & 0xFFu;
    if (((chcr >> 2) & 3) == 2 && block) {
        const bool from_spr = ch.base == 0x1000D000u;
        u32 at = madr;
        for (u32 q = 0; q < qwc; ++q) {
            const u32 spr = kScratchpadBase + ((sadr + 16 * q) & 0x3FF0u);
            for (u32 k = 0; k < 16; k += 4) {
                if (from_spr) m.Write(at + k, m.Read(spr + k, 4), 4);
                else m.Write(spr + k, m.Read(at + k, 4), 4);
            }
            at += 16;
            if ((q + 1) % block == 0) at += 16 * skip;
        }
        *Register(ch.base + kMadr) = at;
        *Register(ch.base + kQwc) = 0;
        *Register(ch.base + kSadr) = (sadr + 16 * qwc) & 0x3FF0u;
        if (!describe) return {};
        std::snprintf(text, sizeof text, "%u quadwords %s scratchpad 0x%04X, interleaved %u/%u", unsigned(qwc),
                      from_spr ? "from" : "to", unsigned(sadr), unsigned(block), unsigned(skip));
        return text;
    }
    if (ch.base == 0x1000D000u) {                       // SPR_FROM
        if ((chcr >> 2) & 3) return "chain mode is not modelled (nothing copied)";
        // Up to the scratchpad's end at a time (where SADR wraps); a
        // quadword at a time where it is not plain memory.
        for (u32 i = 0; i < 16 * qwc;) {
            const u32 from = kScratchpadBase + ((sadr + i) & 0x3FF0u);
            const u32 run = std::min(16 * qwc - i, kScratchpadBase + 0x4000u - from);
            u8* to = Plain(m, madr + i) ? m.Translate(madr + i, run) : nullptr;
            if (const u8* source = to ? m.Translate(from, run) : nullptr) {
                std::memmove(to, source, run);
                i += run;
                continue;
            }
            for (u32 k = 0; k < 16; k += 4) m.Write(madr + i + k, m.Read(from + k, 4), 4);
            i += 16;
        }
        *Register(ch.base + kMadr) = madr + 16 * qwc;
        *Register(ch.base + kQwc) = 0;
        *Register(ch.base + kSadr) = (sadr + 16 * qwc) & 0x3FF0u;
        if (!describe) return {};
        std::snprintf(text, sizeof text, "%u quadwords from scratchpad 0x%04X", unsigned(qwc), unsigned(sadr));
        return text;
    }
    DmaChannelState state{chcr, madr, qwc, u32(m.Read(ch.base + kTadr, 4)),
                          u32(m.Read(ch.base + kAsr0, 4)), u32(m.Read(ch.base + kAsr1, 4))};
    u8* scratchpad = Plain(m, kScratchpadBase) ? m.Translate(kScratchpadBase, kScratchpadSize) : nullptr;
    const DmaOutcome outcome = SendDmaBlocks(state, [&sadr, scratchpad](const u32* words, u32 count, u32 from) {
        if (scratchpad) {
            // Up to the scratchpad's end at a time (where SADR wraps).
            for (u32 i = 0; i < count;) {
                const u32 at = sadr & 0x3FFCu;
                const u32 n = std::min(count - i, (0x4000u - at) / 4);
                std::memcpy(scratchpad + at, &words[i], 4 * std::size_t(n));
                i += n;
                sadr = (sadr + 4 * n) & 0x3FFFu;
            }
            if (count) g_dma_source = from + 4 * (count - 1);
            return;
        }
        for (u32 i = 0; i < count; ++i) {
            g_dma_source = from + 4 * i;
            GuestMemory().Write(kScratchpadBase + (sadr & 0x3FFCu), words[i], 4);
            sadr = (sadr + 4) & 0x3FFFu;
        }
    });
    *Register(ch.base + kMadr) = state.madr;
    *Register(ch.base + kQwc) = state.qwc;
    *Register(ch.base + kTadr) = state.tadr;
    *Register(ch.base + kSadr) = sadr & 0x3FF0u;
    *Register(ch.base) = state.chcr;
    if (!describe) return {};
    std::snprintf(text, sizeof text, "%llu quadwords to scratchpad", static_cast<unsigned long long>(outcome.quadwords));
    return text;
}

// GIF_STAT as VIF1's worker will leave it, worked out on the EE's thread, so
// a read need not wait for the worker (Vif1Guess). Burnout 3 starts each
// display-list packet by waiting for VIF1, VU1 and the GIF to be idle; with
// its transfers ending as they start, every wait was the EE waiting for all
// the worker had - ~15% of the EE's time at a race's start, the pictures it
// missed. Only the worker changes GIF_STAT: VIF1's MSKPATH3 (from the FIFO or
// a transfer) and a GIF transfer held behind the mask filling the FIFO. The
// EE follows the FIFO's commands and the GIF transfers itself, and takes a
// VIF1 transfer to leave GIF_STAT as the one before it left it. Each guess
// is checked on the worker, in turn; a wrong one ends guessing.
struct GifShadow {
    bool known = false;
    u32 value = 0;
};
GifShadow g_gif_shadow;                         // the EE's
VifWalker g_vif1_walker;                        // the EE's: VIF1's stream as queued (vif.h)
// The EE's: the register last answered by a guess, nothing it follows
// queued since - read again, the game is waiting for it to change.
u32 g_guessed_last = 0;
std::atomic<u64> g_guesses{0}, g_wrong{0};
std::atomic<bool> g_guessing_off{false};
bool g_check_guesses = false;

bool Shadowing() { return g_check_guesses || Vif1Async(); }

// With nothing queued, GIF_STAT is what memory holds (a check takes the
// worker to be always behind: only its guesses count).
void GifShadowFromMemory() {
    if (!g_check_guesses && !Vif1Busy()) g_gif_shadow = {true, *Register(kGifStat)};
}

void ShadowMask(bool masked) {
    u32& v = g_gif_shadow.value;
    v = masked ? (v | kGifStatM3P) : (v & ~(kGifStatM3P | kGifStatFqc));
}

u32 HeldInFifo(u64 quadwords) { return quadwords < kGifFifoDepth ? u32(quadwords) : kGifFifoDepth; }

// A VIF1 or GIF chain's words as the EE walked it, for VIF1's worker (Walk).
// Kept in a pool: a race walks a hundred chains a frame, and a vector made
// for each grew by reallocating for each of its runs - a sixth of the EE's
// time went to copying them about.
struct ChainCopy {
    std::vector<u32> words;
    std::vector<std::pair<u32, u32>> runs;      // (count, from) each, in order
};
std::mutex g_chain_pool_lock;
std::vector<ChainCopy*> g_chain_pool;

std::shared_ptr<ChainCopy> NewChainCopy() {
    ChainCopy* copy = nullptr;
    {
        std::lock_guard<std::mutex> hold(g_chain_pool_lock);
        if (!g_chain_pool.empty()) {
            copy = g_chain_pool.back();
            g_chain_pool.pop_back();
        }
    }
    if (!copy) {
        copy = new ChainCopy;
        copy->words.reserve(1u << 16);
        copy->runs.reserve(1024);
    }
    copy->words.clear();            // (the capacity stays)
    copy->runs.clear();
    return std::shared_ptr<ChainCopy>(copy, [](ChainCopy* done) {
        std::lock_guard<std::mutex> hold(g_chain_pool_lock);
        if (g_chain_pool.size() < 16) g_chain_pool.push_back(done);
        else delete done;
    });
}

// The words copied for VIF1's worker (Walk), for the frames line.
u64 g_chain_words = 0;

// Walks a started transfer and leaves the registers as it would; returns a
// note (when `describe`: a transfer is noted only the first time).
std::string Walk(const Channel& ch, u32 chcr, bool describe) {
    Memory& m = GuestMemory();
    if (ch.base == 0x1000D000u || ch.base == 0x1000D400u) return Scratchpad(ch, chcr, describe);
    DmaChannelState state{chcr, u32(m.Read(ch.base + kMadr, 4)), u32(m.Read(ch.base + kQwc, 4)),
                          u32(m.Read(ch.base + kTadr, 4)), u32(m.Read(ch.base + kAsr0, 4)),
                          u32(m.Read(ch.base + kAsr1, 4))};
    // A run of words at a time, each handed on directly.
    auto to_vif = [](VifStream& vif) {
        return [&vif](const u32* words, u32 count, u32 from) { vif.Words(words, count, from); };
    };
    DmaBlockSink sink = [](const u32*, u32, u32) {};
    // VIF1's words followed on this thread as well (g_vif1_walker): the MSKPATH3s among them, in order.
    std::vector<bool> vif1_masks;
    const bool to_vif1 = ch.base == 0x10009000u;
    if (ch.base == 0x10008000u) sink = to_vif(Vif0());
    if (to_vif1)
        sink = [&vif1_masks](const u32* words, u32 count, u32 from) {
            g_vif1_walker.Words(words, count, &vif1_masks);
            Vif1().Words(words, count, from);
        };
    if (ch.base == 0x1000A000u) sink = Path3;
    // With VIF1's worker, VIF1's and the GIF's words are copied as the chain
    // is walked and handed to it in a command (the transfer ends here, as on
    // the console its data is taken).
    const bool queued = Vif1Async() && (ch.base == 0x10009000u || ch.base == 0x1000A000u);
    auto copy = queued ? NewChainCopy() : nullptr;
    if (queued)
        sink = [copy, to_vif1, &vif1_masks](const u32* words, u32 count, u32 from) {
            if (to_vif1) g_vif1_walker.Words(words, count, &vif1_masks);
            copy->words.insert(copy->words.end(), words, words + count);
            copy->runs.emplace_back(count, from);
            g_chain_words += count;
        };
    char text[160] = "";
    try {
        Timed timed(Section::Dma);      // (VU1 and the GS, which it drives, timed apart)
        // The GIF takes no chain tags (TTE): dma.h.
        const bool tags = ch.base != 0x1000A000u;
        const DmaOutcome outcome = SendDmaBlocks(state, sink, 1u << 16, tags);
        if (describe)
            std::snprintf(text, sizeof text, "%llu quadwords, %u tags%s",
                      static_cast<unsigned long long>(outcome.quadwords), outcome.tags,
                      outcome.ended ? "" : ", stopped: the chain does not end");
        const bool gif = ch.base == 0x1000A000u, vif1 = ch.base == 0x10009000u;
        const u64 quadwords = outcome.quadwords;
        auto fifo = [gif, quadwords] {
            if (gif && (*Register(kGifStat) & kGifStatM3P))
                *Register(kGifStat) = (*Register(kGifStat) & ~kGifStatFqc) | (HeldInFifo(quadwords) << kGifStatFqcShift);
        };
        if (Shadowing() && (gif || vif1)) {
            GifShadowFromMemory();
            g_guessed_last = 0;             // a read from here on asks again
            if (vif1) {
                // What this transfer's MSKPATH3s will leave (the walker found them; before 2026-11-17 the guess
                // was the state the previous VIF1 transfer left - one transfer behind, ps2autotests mskpath3).
                if (g_gif_shadow.known)
                    for (bool masked : vif1_masks) ShadowMask(masked);
            } else if (g_gif_shadow.known && (g_gif_shadow.value & kGifStatM3P)) {
                g_gif_shadow.value = (g_gif_shadow.value & ~kGifStatFqc) | (HeldInFifo(quadwords) << kGifStatFqcShift);
            }
        }
        if (queued) {
            Vif1Run([copy, gif, fifo] {
                const u32* at = copy->words.data();
                for (const auto& [count, from] : copy->runs) {
                    if (gif) Path3Kept(at, count, from, copy);
                    else Vif1().Words(at, count, from);
                    at += count;
                }
                fifo();
            });
        } else {
            fifo();
        }
    } catch (const MemoryFault& fault) {
        std::snprintf(text, sizeof text, "stopped: read outside memory at 0x%08X", unsigned(fault.address));
    }
    *Register(ch.base + kMadr) = state.madr;
    *Register(ch.base + kQwc) = state.qwc;
    *Register(ch.base + kTadr) = state.tadr;
    *Register(ch.base + kAsr0) = state.asr0;
    *Register(ch.base + kAsr1) = state.asr1;
    *Register(ch.base) = state.chcr;
    return text;
}
constexpr u32 kVif0Fifo = 0x10004000u;
constexpr u32 kVif1Fifo = 0x10005000u;

}  // namespace

namespace {
std::string g_frames_dir;
int g_frames_every = 30, g_frames_limit = 100, g_frames_written = 0;
u64 g_frames_from = 0;
}  // namespace

void SetFrameDumps(const std::string& dir, int every, int limit, u64 from) {
    g_frames_dir = dir;
    g_frames_every = every > 0 ? every : 1;
    g_frames_limit = limit;
    g_frames_from = from;
}

namespace {
long long g_trace_frame = -1;
std::function<void(u32*, u32, u32)> g_path2_filter;    // SetPath2Filter
}  // namespace

void SetPath2Filter(std::function<void(u32* words, u32 count, u32 from)> filter) { g_path2_filter = std::move(filter); }

void SetGsTraceFrame(long long frame) { g_trace_frame = frame; }

namespace {
long long g_dump_frame = -1;
}  // namespace

void SetGsDumpFrame(long long frame) { g_dump_frame = frame; }

namespace {
u64 g_until_frame = 0;
bool g_until_reached = false;
}  // namespace

void SetUntilFrame(u64 frame) { g_until_frame = frame; }

namespace {
unsigned g_frame_times = 0;
u64 g_vif1_transfers = 0;       // DMA to VIF1 started: a game's frame of geometry, usually one each
}  // namespace

void SetFrameTimes(unsigned every) { g_frame_times = every; }

namespace {
/* --frame-log: the clocks at every vertical blank, all cumulative (differences of two rows = that frame). Kept
   in memory - 2 hours at 60 Hz are ~430,000 rows, ~28 MB, reserved up front - and written only at the end, so
   the vertical blank's thread never waits on a file. */
struct FrameRow {
    u64 t_us, flips, gs_us, vif1_us, gpu_us, ee_wait_frame_us, ee_idle_us;
    u64 vu1_pairs, vu1_kicks, gs_prims;     // the work done: read across threads unlocked (counts, not exact)
    u64 iop_instr;                          // the IOP's instructions (ZOE's 66 ms holds: the EE idle, on what?)
    u32 pad;                                // the host controller's buttons (DualShock 2 order; 0 none) - a hold
                                            // beside a press (a scene skipped)
};
bool g_frame_log_on = false;
std::vector<FrameRow> g_frame_log;
std::chrono::steady_clock::time_point g_frame_log_start;
}  // namespace

void SetFrameLog(bool on) {
    g_frame_log_on = on;
    if (on) {
        g_frame_log.reserve(60 * 60 * 60 * 2);
        g_frame_log_start = std::chrono::steady_clock::now();
    }
}

bool WriteFrameLog(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return false;
    std::fprintf(f, "vblank,t_us,flips,gs_busy_us,vif1_busy_us,gpu_busy_us,ee_wait_frames_us,ee_idle_us,"
                    "vu1_pairs,vu1_kicks,gs_prims,iop_instr,pad\n");
    for (std::size_t i = 0; i < g_frame_log.size(); ++i) {
        const FrameRow& r = g_frame_log[i];
        std::fprintf(f, "%zu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%u\n", i, (unsigned long long)r.t_us,
                     (unsigned long long)r.flips, (unsigned long long)r.gs_us, (unsigned long long)r.vif1_us,
                     (unsigned long long)r.gpu_us, (unsigned long long)r.ee_wait_frame_us,
                     (unsigned long long)r.ee_idle_us, (unsigned long long)r.vu1_pairs,
                     (unsigned long long)r.vu1_kicks, (unsigned long long)r.gs_prims,
                     (unsigned long long)r.iop_instr, unsigned(r.pad));
    }
    std::fclose(f);
    return true;
}

/* --frame-times: the frames since the last line, and how fast they went. */
void FrameTimes(u64 frame) {
    using Clock = std::chrono::steady_clock;
    static Clock::time_point last = Clock::now();
    // PS2HOST_SAMPLE_HEAVY=1 (with --sample): the sampler's report counts only the blanks the EE never went idle
    // in - ZOE US's 66 ms holds start with one (every ~7 s in fights), drowned in the run's other 98 %.
    static const bool sample_heavy = std::getenv("PS2HOST_SAMPLE_HEAVY") != nullptr;
    if (sample_heavy) {
        // A lone heavy blank only: one between two that went idle (a loading screen's run of them is not it).
        static u64 blank_ns = 0, blank_idle = 0, held_from = 0, held_to = 0;
        static bool before_heavy = true;
        const u64 now_ns = u64(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
        const u64 idle_now = IdleNanoseconds();
        const bool heavy = blank_ns && now_ns > blank_ns && (idle_now - blank_idle) * 50 < now_ns - blank_ns;
        if (held_to && !heavy) SampleOnly(held_from, held_to);     // the one before was lone
        held_from = held_to = 0;
        if (heavy && !before_heavy) held_from = blank_ns, held_to = now_ns;
        before_heavy = heavy;
        blank_ns = now_ns;
        blank_idle = idle_now;
    }
    if (g_frame_log_on) {
        u64 join = 0, behind = 0, full = 0;
        Vif1WaitMicroseconds(&join, &behind, &full);
        HostPadState pad;
        const u32 buttons = HostPad(&pad) ? pad.buttons : 0;
        g_frame_log.push_back(FrameRow{
            u64(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - g_frame_log_start).count()),
            gs::DisplayFlips(), GsBusyMicroseconds(), Vif1BusyMicroseconds(), GpuBusyMicroseconds(), behind,
            IdleNanoseconds() / 1000, Vu1().pairs(), Vu1().kicks(), gs::TheGs().primitives(),
            iop::Instructions(), buttons});
    }
    static u64 last_frame = 0, last_transfers = 0, last_lost = 0, last_idle = 0, last_worker_us = 0, last_gs_us = 0;
    // Each call is a vertical blank: one that brought no new picture (gs::DisplayFlips unchanged) is one the
    // player sees the game hold - what a window's mean hides when the game swings between 60 and 30 (ZOE US
    // 2026-11-12: "52 pictures a second" on average, felt as 40). Counted every vertical blank, reported per line.
    static u64 vb_flips = 0, vb_without = 0, vb_seen = 0, vb_gap = 0, vb_longest_gap = 0;
    if (g_frame_times) {
        const u64 now_flips = gs::DisplayFlips();
        ++vb_seen;
        if (now_flips == vb_flips) {
            ++vb_without;
            vb_longest_gap = std::max(vb_longest_gap, ++vb_gap);
        } else {
            vb_gap = 0;
        }
        vb_flips = now_flips;
    }
    if (!g_frame_times || frame < last_frame + g_frame_times) return;
    const u64 lost = LostVblanks(), idle = IdleNanoseconds(), worker_us = Vif1BusyMicroseconds();
    const u64 gs_us = GsBusyMicroseconds();
    const Clock::time_point now = Clock::now();
    const double seconds = std::chrono::duration<double>(now - last).count();
    char job_text[1024] = "";
    if (seconds > 0)
        std::snprintf(job_text, sizeof job_text, "%s, GS thread busy %d%%",
                      worker_us != last_worker_us
                          ? (", VIF1's worker busy " +
                             std::to_string(int(100.0 * double(worker_us - last_worker_us) / 1e6 / seconds + 0.5)) + "%")
                                .c_str()
                          : "",
                      int(100.0 * double(gs_us - last_gs_us) / 1e6 / seconds + 0.5));
    last_worker_us = worker_us;
    last_gs_us = gs_us;
    // The EE's waits for the worker, when it waited at all.
    static u64 last_join = 0, last_behind = 0, last_full = 0;
    u64 join = 0, behind = 0, full = 0;
    Vif1WaitMicroseconds(&join, &behind, &full);
    if (seconds > 0 && (join != last_join || behind != last_behind || full != last_full)) {
        const auto share = [&](u64 us) { return int(100.0 * double(us) / 1e6 / seconds + 0.5); };
        const std::size_t used = std::strlen(job_text);
        std::snprintf(job_text + used, sizeof job_text - used, ", EE waiting: joins %d%%, frames %d%%, room %d%%",
                      share(join - last_join), share(behind - last_behind), share(full - last_full));
    }
    last_join = join;
    last_behind = behind;
    last_full = full;
    static u64 last_gpu_us = 0;
    const u64 gpu_us = GpuBusyMicroseconds();
    if (gpu_us != last_gpu_us && seconds > 0) {
        const std::size_t used = std::strlen(job_text);
        std::snprintf(job_text + used, sizeof job_text - used, ", GPU busy %d%%",
                      int(100.0 * double(gpu_us - last_gpu_us) / 1e6 / seconds + 0.5));
    }
    last_gpu_us = gpu_us;
#ifdef PS2HOST_VULKAN
    if (ps2gpu::Display* display = WindowDisplay()) {
        const ps2gpu::Display::Pacing p = display->TakePacing();
        if (p.presents && seconds > 0) {
            const std::size_t used = std::strlen(job_text);
            std::snprintf(job_text + used, sizeof job_text - used,
                          ", presents %.1f/s every %.2f ms (jitter %.2f, longest %.1f)", double(p.presents) / seconds,
                          p.mean_ms, p.jitter_ms, p.longest_ms);
        }
    }
#endif
    static u64 last_guesses = 0;
    u64 guesses = 0, wrong = 0;
    Vif1Guesses(&guesses, &wrong);
    if (guesses != last_guesses) {
        const std::size_t used = std::strlen(job_text);
        std::snprintf(job_text + used, sizeof job_text - used, ", %llu status reads guessed (%llu wrong in all)",
                      (unsigned long long)(guesses - last_guesses), (unsigned long long)wrong);
    }
    last_guesses = guesses;
    static u64 last_chain_words = 0;
    if (g_chain_words != last_chain_words && seconds > 0) {
        const std::size_t used = std::strlen(job_text);
        std::snprintf(job_text + used, sizeof job_text - used, ", %.1f MB a second copied for VIF1's worker",
                      double(g_chain_words - last_chain_words) * 4 / 1e6 / seconds);
    }
    last_chain_words = g_chain_words;
    // Frames here are vertical blanks; a game that cannot keep up draws a
    // picture every other one (or less), which the VIF1 transfers show.
    // VU1's work by program (the address each run started at): which microprograms grow when the VIF1 worker
    // limits (ZOE US explosions 2026-11-13: 1.7x a fight's pairs a picture).
    {
        static std::vector<u64> last_vu;
        const std::vector<u64>& now_vu = Vu1().start_pairs();
        if (last_vu.size() != now_vu.size()) last_vu.assign(now_vu.size(), 0);
        std::vector<std::pair<u64, u32>> top;
        u64 total = 0;
        for (std::size_t i = 0; i < now_vu.size(); ++i) {
            const u64 d = now_vu[i] - last_vu[i];
            last_vu[i] = now_vu[i];
            if (d) {
                top.emplace_back(d, u32(i * 8));
                total += d;
            }
        }
        if (total && seconds > 0) {
            std::sort(top.rbegin(), top.rend());
            std::string text = "; VU1 " + std::to_string(int(double(total) / 1e6 / seconds + 0.5)) + " M pairs/s:";
            for (std::size_t i = 0; i < top.size() && i < 5; ++i) {
                char one[48];
                std::snprintf(one, sizeof one, " 0x%04X %.0f%%", unsigned(top[i].second),
                              100.0 * double(top[i].first) / double(total));
                text += one;
            }
            const std::size_t used = std::strlen(job_text);
            std::snprintf(job_text + used, sizeof job_text - used, "%s", text.c_str());
        }
    }
    {
        const std::string cpu = timeline::ThreadCpu();
        if (!cpu.empty()) {
            const std::size_t used = std::strlen(job_text);
            std::snprintf(job_text + used, sizeof job_text - used, "; CPU: %s", cpu.c_str());
        }
    }
    // The game's own pictures (gs::DisplayFlips): what the player sees move - first, before the vertical
    // blanks ("frames a second"), which go on at 50/60 Hz however far the game falls behind.
    static u64 last_flips = 0;
    const u64 flips = gs::DisplayFlips();
    char game_text[192] = "";
    if (seconds > 0 && frame > last_frame)
        std::snprintf(game_text, sizeof game_text,
                      "game %.1f pictures a second (one every %.2f vertical blanks; %.0f%% of vertical blanks held, "
                      "longest hold %llu), ",
                      double(flips - last_flips) / seconds,
                      flips != last_flips ? double(frame - last_frame) / double(flips - last_flips) : 0.0,
                      vb_seen ? 100.0 * double(vb_without) / double(vb_seen) : 0.0,
                      static_cast<unsigned long long>(vb_longest_gap));
    last_flips = flips;
    vb_without = vb_seen = vb_longest_gap = 0;
    std::fprintf(stderr, "[frames] %llu-%llu: %.2f s, %s%.1f frames a second, %.1f VIF1 transfers a second%s\n",
                 static_cast<unsigned long long>(last_frame), static_cast<unsigned long long>(frame), seconds,
                 game_text, seconds > 0 ? double(frame - last_frame) / seconds : 0.0,
                 seconds > 0 ? double(g_vif1_transfers - last_transfers) / seconds : 0.0,
                 ((lost != last_lost ? ", " + std::to_string(lost - last_lost) + " vertical blanks lost (the EE busy through them)"
                                    : std::string()) +
                 (idle != last_idle && seconds > 0
                      ? ", EE idle " + std::to_string(int(100.0 * double(idle - last_idle) / 1e9 / seconds + 0.5)) + "%"
                      : std::string()) + job_text).c_str());
    last_lost = lost;
    last_idle = idle;
    last = now;
    last_frame = frame;
    last_transfers = g_vif1_transfers;
}
bool UntilFrameReached() { return g_until_reached; }

// F12: the next frame captured whole, for looking into what it
// drew - captures/<time>-frame<N>/ beside the PC settings: the picture at
// the internal resolution and at the PS2's, every drawing pass, GS local
// memory and the GS trace of the frame.
std::atomic<bool> g_capture_request{false};
long long g_capture_frame = -1;
u64 g_capture_flips = 0;        // gs::DisplayFlips as the capture began
u64 g_capture_prims = 0;        // the GS's primitives as the capture began
std::string g_capture_dir;

void StartCapture(u64 frame) {
    namespace fs = std::filesystem;
    // In data\captures beside the PC settings (the maintainer 2026-11-20: a player's folder keeps the game's own
    // files apart from what it writes for a report).
    fs::path base = SettingsPath().empty() ? fs::path("data") / "captures"
                                           : fs::path(SettingsPath()).parent_path() / "data" / "captures";
    char name[64];
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    tm = ps2platform::LocalTime(now);
    std::snprintf(name, sizeof name, "%04d%02d%02d-%02d%02d%02d-frame%llu", tm.tm_year + 1900, tm.tm_mon + 1,
                  tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<unsigned long long>(frame + 1));
    const fs::path dir = base / name;
    std::error_code error;
    fs::create_directories(dir / "passes", error);
    if (error) {
        std::fprintf(stderr, "[capture] no directory %s: %s\n", dir.string().c_str(), error.message().c_str());
        return;
    }
    g_capture_dir = dir.string();
    // Two frames: a game drawing a picture every other blank (Burnout 3)
    // draws nothing in one of them.
    g_capture_frame = (long long)frame + 2;
    GpuCaptureBegin(frame + 1, (dir / "passes").string());
    gs::Gs& g = gs::TheGs();
    // The frames' GS stream (replay.bin), for --gs-replay: memory as it is
    // now, the GPU's drawing in it, the registers, then what comes.
    SyncGs();
    g.Finish();
    if (g.on_memory) g.on_memory(0, 16384, false);
    g.BeginRecord(std::fopen((dir / "replay.bin").string().c_str(), "wb"));
    // Counted from here, with the GS thread caught up: what the recording holds (taken before SyncGs, the
    // flips still in the ring counted - 2026-11-13's captures ended with nothing recorded).
    g_capture_flips = gs::DisplayFlips();
    g_capture_prims = g.primitives();
    if (std::FILE* f = std::fopen((dir / "trace.txt").string().c_str(), "w")) {
        g.trace_out = f;
        g.trace = true;
        std::fprintf(f, "[gs] ---- frame %llu (a capture)\n", static_cast<unsigned long long>(frame + 1));
    }
    std::fprintf(stderr, "[capture] frame %llu to %s\n", static_cast<unsigned long long>(frame + 1),
                 g_capture_dir.c_str());
}

void FinishCapture(u64 frame) {
    gs::Gs& g = gs::TheGs();
    SyncGs();
    g.EndRecord();
    if (g.trace_out != stderr) {
        g.trace = false;
        std::fclose(g.trace_out);
        g.trace_out = stderr;
    }
    const std::string screen = GpuCaptureScreen(g_capture_dir);
    g.Finish();
    if (g.on_memory) g.on_memory(0, 16384, false);      // the GPU's drawing, back in memory
    if (std::FILE* f = std::fopen((g_capture_dir + "/gsmem.bin").c_str(), "wb")) {
        std::fwrite(g.memory.data(), 1, 4u << 20, f);
        std::fclose(f);
    }
    if (std::FILE* f = std::fopen((g_capture_dir + "/info.txt").c_str(), "w")) {
        std::fprintf(f, "frame %llu\n%s\nresolution setting: %s\n", static_cast<unsigned long long>(frame),
                     screen.c_str(), Setting("resolution", "original").c_str());
        std::fprintf(f, "recorded: %llu display flips, %llu primitives, %lld vertical blanks\n",
                     (unsigned long long)(gs::DisplayFlips() - g_capture_flips),
                     (unsigned long long)(g.primitives() - g_capture_prims),
                     (long long)frame - g_capture_frame + 2);
        std::fprintf(f, "files: screen.bmp (as drawn, at the internal resolution), screen_1x.bmp (the PS2's "
                        "640x448), passes/ (each target as a pass left it, at 1x), gsmem.bin (GS local memory "
                        "after the frame: engine/tools/gsmem.py), trace.txt (the frame's GS packets and "
                        "primitives), replay.bin (the frames' GS stream: --gs-replay, PS2HOST_REPLAY_GPU, "
                        "PS2HOST_REPLAY_SCALE, PS2HOST_REPLAY_STOP_PRIM)\n");
        std::fclose(f);
    }
    std::fprintf(stderr, "[capture] saved %s\n", g_capture_dir.c_str());
    g_capture_frame = -1;
}

void RequestCapture() { g_capture_request = true; }

// GAME SPEED (the PERFORMANCE OVERLAY's SPEED, the maintainer 2026-11-22): the vertical blanks the game took a
// real second, against the console's 59.94 - 100 % the game at its own pace, below it slowed (an EE behind
// loses blanks past the owed ones, and the game's time with them). Counted here, as each is delivered; the
// overlay reads it twice a second (GameSpeed), the log says each second and each 5 s's slowest
// (PS2HOST_SPEED_LOG=1 or the overlay showing SPEED). Not under --virtual-time: no real second to count in.
namespace {
constexpr double kGuestRate = 1e9 / 16683333.0;     // NTSC, 59.94 (the engine's blank, PAL discs too)
std::mutex g_speed_mutex;
u64 g_speed_frames = 0;                             // blanks counted, and when the last came (steady ns)
u64 g_speed_last_ns = 0;
u64 g_speed_stall_ns = 0;                           // waits between blanks past 1.5 periods: their time past one
constexpr u64 kBlankNs = 16683333;
bool g_speed_log = std::getenv("PS2HOST_SPEED_LOG") != nullptr;

u64 SteadyNs() {
    return u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch()).count());
}

void CountSpeed() {
    // (Under --virtual-time too, as an emulator's fast-forward shows: the blanks a real second, above 100 % when it
    // runs ahead - the maintainer 2026-11-22. Its stalls are no player's: only real time counts them.)
    const bool virtual_time = VirtualClock();
    const u64 now = SteadyNs();
    {
        std::lock_guard<std::mutex> lock(g_speed_mutex);
        ++g_speed_frames;
        if (!virtual_time && g_speed_last_ns && now - g_speed_last_ns > kBlankNs * 3 / 2)
            g_speed_stall_ns += now - g_speed_last_ns - kBlankNs;
        g_speed_last_ns = now;
    }
    // The log's: whole seconds of blanks, five of them a line.
    static u64 second_from = 0, second_frames = 0, second_stall = 0;
    static int seconds = 0;
    static double slowest = 1e9;
    if (!second_from) second_from = now;
    ++second_frames;
    if (now - second_from < 1'000'000'000ull) return;
    const double counted = 100.0 * double(second_frames) / ((now - second_from) / 1e9) / kGuestRate;
    u64 stall_total;
    {
        std::lock_guard<std::mutex> lock(g_speed_mutex);
        stall_total = g_speed_stall_ns;
    }
    const double ran = 100.0 * (1.0 - double(stall_total - second_stall) / double(now - second_from));
    const double speed = virtual_time ? counted : std::max(0.0, std::min(counted, ran));     // what the player sees
    second_stall = stall_total;
    const u64 second_from_log = second_from;
    second_from = now;
    second_frames = 0;
    // Each second too (the maintainer 2026-11-22: "nimm jede Sekunde auf").
    if (g_speed_log || SettingOverlayShows("speed")) {
        // ...with the pictures shown in it and the longest wait between two (60 and ~16.7 ms: no drop).
        static u64 presents_before = ~0ull;
        u64 presents = 0;
        double gap = 0;
        if (ps2gpu::Display* display = WindowDisplay()) {
            presents = display->PresentCount();
            gap = display->TakeLongestPresentGap();
        }
        if (presents_before == ~0ull) presents_before = presents - std::min<u64>(presents, 60);   // (the first: no count)
        // The game's own pictures first (what the overlay shows: speed = them against the game's rate, the longest
        // wait between two); then, to tell where a drop came from, the blanks' speed and the presents.
        static u64 game_before = ~0ull;
        const u64 game = GamePictures();
        if (game_before == ~0ull) game_before = game;
        const u64 game_pictures = game - game_before;
        const double game_speed = 100.0 * double(game_pictures) / ((now - second_from_log) / 1e9) / GamePictureRate();
        std::fprintf(stderr, "[speed] second: %.1f %%, %llu pictures, longest gap %.1f ms (blanks %.1f %%, %llu presents, "
                     "longest %.1f ms)\n", game_speed, (unsigned long long)game_pictures, TakeLongestGamePictureGap(1),
                     speed, (unsigned long long)(presents - presents_before), gap);
        game_before = game;
        presents_before = presents;
    }
    // Each 5 s: the worst only - no average (the maintainer 2026-11-22: an average's window can be chosen until it
    // says what one wants).
    slowest = std::min(slowest, speed);
    if (++seconds < 5) return;
    if (g_speed_log || SettingOverlayShows("speed"))
        std::fprintf(stderr, "[speed] 5 s: slowest second %.1f %%\n", slowest);
    seconds = 0;
    slowest = 1e9;
}
}  // namespace

bool GameSpeed(GameSpeedMark* mark, double* percent) {
    u64 frames, last, stall;
    {
        std::lock_guard<std::mutex> lock(g_speed_mutex);
        frames = g_speed_frames;
        last = g_speed_last_ns;
        stall = g_speed_stall_ns;
    }
    // To the last blank - or to now, when none came for longer than a few would (the game held up: that wait so
    // far counted as stopped, the rest of it when its blank comes).
    const u64 now = SteadyNs();
    const u64 to = last && now - last < 100'000'000ull ? last : now;
    if (!VirtualClock() && last && now - last > kBlankNs * 3 / 2 && to == now) stall += now - last - kBlankNs;
    const bool known = mark->ns && frames >= mark->frames && to > mark->ns && stall >= mark->stall_ns;
    if (known) {
        const double counted = 100.0 * double(frames - mark->frames) / ((to - mark->ns) / 1e9) / kGuestRate;
        const double ran = 100.0 * (1.0 - double(stall - mark->stall_ns) / double(to - mark->ns));
        *percent = VirtualClock() ? counted : std::max(0.0, std::min(counted, ran));
    }
    mark->frames = frames;
    mark->ns = to;
    mark->stall_ns = stall;
    return known && frames > 0;
}

void FrameDone(u64 frame) {
    CountSpeed();
    // PS2HOST_PICTURES_AT=a,b: the game's pictures counted at those frames (its flips between two blanks: how many
    // pictures a stretch of the game has when nothing holds it up - --virtual-time - against real time).
    static const std::vector<u64> pictures_at = [] {
        std::vector<u64> at;
        if (const char* e = std::getenv("PS2HOST_PICTURES_AT"))
            for (const char* p = e; *p;) {
                at.push_back(std::strtoull(p, const_cast<char**>(&p), 10));
                if (*p == ',') ++p;
                else break;
            }
        return at;
    }();
    for (u64 at : pictures_at)
        if (frame == at)
            std::fprintf(stderr, "[pictures] frame %llu: %llu game pictures so far\n", (unsigned long long)frame,
                         (unsigned long long)GamePictures());
    FrameTimes(frame);
    MusicBlank();
    timeline::Frame(frame);
    SampleFrame(frame);
    Vu::SnapshotFrame(frame);
    // Time passes: unmasked, PATH3 sends what it still holds (in turn with
    // VIF1's, whose state PATH3's is).
    Vif1Run([] {
        if (!g_path3_masked && !g_path3_held.empty()) ReleasePath3(true);
    });
    if (g_until_frame && frame >= g_until_frame && !g_until_reached) {
        g_until_reached = true;
        StopRequested() = true;
        ForEachContext([](lifted::Context& each) { each.budget = -1; });
    }
    const bool dump = !g_frames_dir.empty() && g_frames_written < g_frames_limit && frame >= g_frames_from &&
                      frame % u64(g_frames_every) == 0;
    // Nothing here reads the GS this frame (no picture to the CPU, no dump,
    // no trace): the GS thread ends the frame itself where the frame's
    // packets end - the GPU's frame, the window's picture - and the EE goes
    // straight on (it waited for the GS and the GPU at every frame before).
    // PS2HOST_CAPTURE_FRAME=N: as F12 at frame N (a scripted run's).
    static const long long capture_at = std::getenv("PS2HOST_CAPTURE_FRAME") ? std::atoll(std::getenv("PS2HOST_CAPTURE_FRAME")) : -1;
    if ((long long)frame == capture_at) g_capture_request = true;
    if (g_capture_request.exchange(false) && g_capture_frame < 0 && GpuPrimary()) StartCapture(frame);
    // The capture ends two vertical blanks on - and only once the game showed pictures in them: a game behind
    // gets the blanks it missed back to back, with nothing drawn between (ZOE US 2026-11-13: 7 of 8 captures in
    // explosions held GS memory and no packet). At least 3 display flips (a whole picture for a game that flips
    // twice a picture), or 120 blanks at most.
    // (Read unsynchronised from the GS thread: a picture shown and something drawn since the recording began.)
    const bool capturing = g_capture_frame >= 0 && (long long)frame >= g_capture_frame &&
                           ((gs::DisplayFlips() >= g_capture_flips + 3 && gs::TheGs().primitives() > g_capture_prims) ||
                            (long long)frame >= g_capture_frame + 120);
    const bool cpu_reads_gs = dump || (g_dump_frame >= 0 && (long long)frame == g_dump_frame) || g_trace_frame >= 0 ||
                              capturing;
    if (!cpu_reads_gs && GsFrame(frame)) {
        // The window's events first: the controllers' and the keyboard's
        // state as of now goes into the pad block the game reads this frame
        // (taken after it, every press reached the game a frame late).
        if (!PumpWindow()) throw GuestExit{0};          // the user closed the window
        if (!HoldForSelectMenu()) throw GuestExit{0};   // SELECT's menu: the game held still meanwhile
        DbcFrame();
        return;
    }
    // What reads the GS from here on - the picture, the GPU's frame, the
    // dumps - sees it with all of the frame's packets taken.
    JoinVif1();
    SyncGs();
    if (capturing) FinishCapture(frame);
    if (g_dump_frame >= 0 && (long long)frame == g_dump_frame) {
        gs::Gs& g = gs::TheGs();
        g.Finish();
        if (g.on_memory) g.on_memory(0, 16384, false);      // the GPU's drawing, back in memory
        const std::string path = (g_frames_dir.empty() ? std::string(".") : g_frames_dir) + "/gsmem_" +
                                 std::to_string(frame) + ".bin";
        if (std::FILE* f = std::fopen(path.c_str(), "wb")) {
            std::fwrite(g.memory.data(), 1, 4u << 20, f);
            std::fclose(f);
            std::fprintf(stderr, "[gs] local memory after frame %llu written to %s\n",
                         static_cast<unsigned long long>(frame), path.c_str());
        }
    }
    if (g_trace_frame >= 0) {
        // From the end of the frame before to the end of six frames on: the
        // game draws a picture across several.
        if ((long long)frame + 1 == g_trace_frame) gs::TheGs().trace = true;
        else if ((long long)frame == g_trace_frame + 5) gs::TheGs().trace = false;
        if (gs::TheGs().trace) std::fprintf(gs::TheGs().trace_out, "[gs] ---- frame %llu\n", static_cast<unsigned long long>(frame + 1));
    }
    DbcFrame();
    PresentFrame();
    if (WindowClosed()) throw GuestExit{0};             // the user closed the window
    GpuShadowFrame(frame, dump ? g_frames_dir : std::string());
    if (!dump) return;
    char name[64];
    std::snprintf(name, sizeof name, "/frame_%06llu.bmp", static_cast<unsigned long long>(frame));
    if (gs::TheGs().WriteBmp(g_frames_dir + name)) ++g_frames_written;
    // VU1's memories beside the picture, for engine/tools/vudis.py.
    for (const auto& [file, span] : {std::make_pair("/vu1_micro.bin", Vu1().micro), std::make_pair("/vu1_data.bin", Vu1().data)})
        if (FILE* out = std::fopen((g_frames_dir + file).c_str(), "wb")) {
            std::fwrite(span.data(), 1, span.size(), out);
            std::fclose(out);
        }
}

bool Vif1Guessable(u32 address, u32 size) { return size == 4 && address == kGifStat; }

bool Vif1Guess(u32 address, u32* value) {
    if (!Vif1Async() || address != kGifStat || g_guessing_off || !g_gif_shadow.known) return false;
    // Read again straight away, the game waiting for it to change: it waits
    // for the worker.
    if (g_guessed_last == address) return false;
    g_guessed_last = address;
    const u32 guess = g_gif_shadow.value;
    ++g_guesses;
    Vif1Run([guess] {
        const u32 actual = *Register(kGifStat);
        if (actual == guess) return;
        g_guessing_off = true;
        if (g_wrong.fetch_add(1) < 8)
            std::fprintf(stderr, "[vif1] GIF_STAT guessed 0x%08X, held 0x%08X: no more guesses\n", unsigned(guess),
                         unsigned(actual));
    });
    *value = guess;
    return true;
}

void Vif1Settled(u32 address, u32 value) {
    if (address != kGifStat) return;
    if (g_check_guesses) {
        // What real time would have answered: a guess, or a wait for the worker.
        if (g_gif_shadow.known && g_guessed_last != address) {
            g_guessed_last = address;
            ++g_guesses;
            if (value != g_gif_shadow.value && g_wrong.fetch_add(1) < 32)
                std::fprintf(stderr, "[check] GIF_STAT would have been guessed 0x%08X, holds 0x%08X\n",
                             unsigned(g_gif_shadow.value), unsigned(value));
        } else {
            g_guessed_last = 0;
        }
        g_gif_shadow = {true, value};
        return;
    }
    if (g_guessed_last == address) g_guessed_last = 0;
    GifShadowFromMemory();
}

void Vif1Guesses(u64* guesses, u64* wrong) {
    *guesses = g_guesses.load();
    *wrong = g_wrong.load();
}

void SetCheckGuesses(bool on) { g_check_guesses = on; }

void DmaChannelEnded(u32 channel) {
    SetDStat(d_stat | (1u << channel));
    PostDmac(channel);
}

void InstallDevices() {
    Memory& m = GuestMemory();
    // The scratchpad channels keep only what the console keeps (ps2autotests dma/spr): SADR's bits 4-13 (a
    // quadword in the 16 KB), MADR without bit 31 (main memory only: no SPR flag on these two).
    for (u32 base : {0x1000D000u, 0x1000D400u}) {
        m.OnWrite(base + kSadr, [base](u32, u64 value) { *Register(base + kSadr) = u32(value) & 0x3FF0u; });
        m.OnWrite(base + kMadr, [base](u32, u64 value) { *Register(base + kMadr) = u32(value) & 0x7FFFFFFFu; });
    }
    for (const Channel& ch : kChannels) {
        if (ch.base == 0x1000B000u || ch.base == 0x1000B400u) continue;     // the IPU's (ipu.cpp)
        const u32 chcr = ch.base, madr = ch.base + kMadr, qwc = ch.base + kQwc;
        m.OnWrite(chcr, [ch, madr, qwc, chcr](u32, u64 value) {
            if (!(value & 0x100)) return;                       // STR not set
            Memory& memory = GuestMemory();
            // The text only when it is logged: a DMA start is thousands a second.
            const std::string key = std::string("dma ") + ch.name;
            const bool traced = gs::TheGs().trace && (ch.base == 0x10009000u || ch.base == 0x1000A000u);
            const bool describe = traced || Noting(key);
            char text[256] = "";
            if (describe)
                std::snprintf(text, sizeof text,
                              "DMA %s started: CHCR=0x%08X MADR=0x%08X QWC=%u TADR=0x%08X (completed at once)",
                              ch.name, unsigned(value), unsigned(memory.Read(madr, 4)),
                              unsigned(memory.Read(qwc, 4)), unsigned(memory.Read(ch.base + kTadr, 4)));
            if (traced) std::fprintf(gs::TheGs().trace_out, "[dma] %s\n", text);
            if (ch.base == 0x10009000u) ++g_vif1_transfers;
            const std::string walked = ch.walked ? Walk(ch, u32(value), describe) : std::string();
            if (describe) Note(key, ch.walked ? std::string(text) + ": " + walked : std::string(text));
            *Register(chcr) &= ~0x100u;                         // done
            SetDStat(d_stat | (1u << ch.status_bit));
            PostDmac(ch.status_bit);
        });
    }
    m.OnWrite(kDStat, [](u32, u64 value) {
        const u32 written = u32(value);
        SetDStat((d_stat & ~(written & 0xFFFFu)) ^ (written & 0xFFFF0000u));
    });
    // COP0's condition (bc0t/bc0f, lifted::Cop0Condition): CPCOND0 = every channel D_PCR.CPC (bits 0-9) names has
    // its D_STAT.CIS set - with none named, true (ps2autotests dma/dmac/cpcond).
    lifted::cop0_condition_hook = [] {
        const u32 pcr = *Register(0x1000E020u);
        return ((~pcr | d_stat) & 0x3FFu) == 0x3FFu;
    };
    Vif1().on_mask_path3 = [](bool masked) {
        if (gs::TheGs().trace)
            std::fprintf(stderr, "[vif1] MSKPATH3 %d, %zu PATH3 words held\n", int(masked), g_path3_held.size());
        u32* stat = reinterpret_cast<u32*>(GuestMemory().Translate(kGifStat, 4));
        // Unmasking lets PATH3 take its turn: the packet group it is in goes
        // through before a mask can stop it again.
        *stat = masked ? (*stat | kGifStatM3P) : (*stat & ~(kGifStatM3P | kGifStatFqc));
        g_path3_masked = masked;
        if (!masked) ReleasePath3(false);
    };
    Vif1().on_direct = [](u32 word) { GsWord(2, word); };
    Vif1().on_direct_block = [](const u32* words, u32 count, u32 from) {
        if (!g_path2_filter || from == gs::Gs::kNoSource) return GsWords(2, words, count, from);
        static std::vector<u32> copy;
        copy.assign(words, words + count);
        g_path2_filter(copy.data(), count, from);
        GsWords(2, copy.data(), count, from);
    };
    Vif1().on_trace = [](const char* what) {
        if (gs::TheGs().trace) std::fprintf(gs::TheGs().trace_out, "[vif1] %s, %zu PATH3 words held\n", what, g_path3_held.size());
        // FLUSHA waits for PATH3 to finish: unmasked, all it holds goes.
        if (!g_path3_masked) ReleasePath3(true);
    };
    // VU1 behind VIF1; its XGKICKs are GIF PATH1.
    Vif1().vu = &Vu1();
    Vif1().double_buffered = true;
    Vif0().vu = &Vu0();
    Vu1().on_kick = [](u64 low, u64 high) { GsQword(1, low, high); };
    Vu1().on_kick_block = [](const u32* words, u32 count) { GsWords(1, words, count); };
    // The GS's privileged registers: what is displayed. A 64-bit store reaches
    // both words; each passes the whole register on.
    for (u32 reg : {0x12000000u, 0x12000020u, 0x12000070u, 0x12000080u, 0x12000090u, 0x120000A0u, 0x120000E0u}) {
        for (u32 half : {0u, 4u}) {
            GuestMemory().OnWrite(reg + half, [reg](u32, u64) {
                GsPrivileged(reg, GuestMemory().Read(reg, 8));
            });
        }
    }
    // The FIFO takes whole quadwords; the store's last word hands all four over.
    // With VIF1's worker, in turn after what it has (memory.cpp lets the
    // store through without waiting).
    m.OnWrite(kVif1Fifo + 12, [](u32, u64) {
        const u32* fifo = Register(kVif1Fifo);
        const std::array<u32, 4> words{fifo[0], fifo[1], fifo[2], fifo[3]};
        // The walker follows the FIFO's words as the DMA's (a quadword may be a command's data): its MSKPATH3s.
        std::vector<bool> masks;
        g_vif1_walker.Words(words.data(), 4, &masks);
        if (Shadowing()) {
            GifShadowFromMemory();
            g_guessed_last = 0;
            if (g_gif_shadow.known)
                for (bool masked : masks) ShadowMask(masked);
        }
        Vif1Run([words] {
            for (u32 word : words) Vif1().Word(word);
            Vif1().Mirror();
        });
    });
    // VIF0's FIFO the same way, on the EE's thread (VIF0 has no worker): its quadwords decoded as VIFcodes into
    // VU0 (ps2autotests dma/vif/basic - before, a store there went nowhere).
    m.OnWrite(kVif0Fifo + 12, [](u32, u64) {
        const u32* fifo = Register(kVif0Fifo);
        const std::array<u32, 4> words{fifo[0], fifo[1], fifo[2], fifo[3]};
        for (u32 word : words) Vif0().Word(word);
        Vif0().Mirror();
    });
    // VIFn_FBRST: RST (bit 0) drops a command waiting for its data (ps2autotests dma/vif/unpack resets between
    // cases); VIF1's in turn after what its worker has.
    // STC (bit 3): a stall by an interrupt bit ends, the VIF goes on with what waited (vif.h).
    m.OnWrite(0x10003810u, [](u32, u64 value) {
        if (value & 1) Vif0().Reset();
        else if (value & 8) Vif0().Continue();
    });
    m.OnWrite(0x10003C10u, [](u32, u64 value) {
        if (value & 1) {
            g_vif1_walker.Reset();
            Vif1Run([] { Vif1().Reset(); });
        }
        else if (value & 8) Vif1Run([] { Vif1().Continue(); });
    });
    // VIFn_MARK written: STAT.MRK cleared.
    m.OnWrite(0x10003830u, [](u32, u64) { Vif0().MarkWritten(); });
    m.OnWrite(0x10003C30u, [](u32, u64) { Vif1Run([] { Vif1().MarkWritten(); }); });
    // A VIFcode's interrupt bit: INTC's VIF0 (13) / VIF1 (14), the handlers run between threads (as the DMAC's,
    // PostDmac) - VIF1's from its worker too.
    Vif0().on_interrupt = [] { PostIntc(13); };
    Vif1().on_interrupt = [] { PostIntc(14); };
    m.OnRead(kGsCsr, [](u32, u64 stored) {
        static u64 field = 0;
        field ^= 1u << 13;                          // FIELD alternates each frame
        return (stored & ~u64(1u << 13)) | field | (1u << 3);   // VSINT pending
    });
    m.OnRead(0x1000F000u, [](u32, u64) { return u64(IntcStat()); });     // INTC_STAT
    m.OnWrite(0x1000F000u, [](u32, u64 value) { ClearIntcStat(u32(value) & 0x7FFFu); });
    InstallTimers();
    InstallIpu();
}

}  // namespace ps2host
