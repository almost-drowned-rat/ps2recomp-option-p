/*
 * Source DMA transfers, walked at once (dma.h).
 */

#include "ps2host/dma.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

#include "ps2host/memory.h"

namespace ps2host {

std::uint32_t g_dma_source = 0;


namespace {

constexpr std::uint32_t kScratchpad = 0x70000000u;

// RecentDmaTags: the last tags walked, where they were and what was walked (a count to order them).
constexpr std::uint32_t kRecentTags = 4096;
struct RecentTag { std::uint64_t tag = 0; std::uint32_t tadr = 0; std::uint32_t serial = 0; };
RecentTag g_recent[kRecentTags];
std::uint32_t g_recent_at = 0;
// PS2HOST_DMA_TRACE's: the tags walked in its range, their upper halves (RangedDmaTags).
constexpr std::uint32_t kRangedTags = 256;
struct RangedTag { std::uint64_t tag = 0; std::uint32_t tadr = 0, upper0 = 0, upper1 = 0, serial = 0; };
RangedTag g_ranged[kRangedTags];
std::uint32_t g_ranged_at = 0;

std::uint32_t Address(std::uint64_t tag) {
    const std::uint32_t address = std::uint32_t(tag >> 32) & 0x7FFFFFF0u;
    return (tag >> 63) ? kScratchpad | (address & 0x3FF0u) : address;
}

void Send(std::uint32_t from, std::uint32_t quadwords, DmaOutcome& outcome, const DmaBlockSink& words) {
    Memory& m = GuestMemory();
    // Main RAM or the scratchpad, as DMA reads them: one pointer for the lot
    // (a word at a time through Read cost a tenth of a frame). Anything with
    // register hooks goes word by word.
    const std::uint32_t physical = from & 0x1FFFFFFFu;
    const bool plain = physical < 0x10000000u || (from >> 28) == 0x7;
    const std::uint8_t* p = plain && quadwords ? m.Translate(from, 16 * quadwords) : nullptr;
    if (p && reinterpret_cast<std::uintptr_t>(p) % 4 == 0) {
        words(reinterpret_cast<const std::uint32_t*>(p), 4 * quadwords, from);
    } else {
        for (std::uint32_t i = 0; i < 4 * quadwords; ++i) {
            const std::uint32_t value = std::uint32_t(m.Read(from + 4 * i, 4));
            words(&value, 1, from + 4 * i);
        }
    }
    outcome.quadwords += quadwords;
}

}  // namespace

DmaOutcome SendDma(DmaChannelState& ch, const std::function<void(std::uint32_t)>& word,
                   std::uint32_t max_tags, bool tags) {
    return SendDmaBlocks(ch, [&word](const std::uint32_t* words, std::uint32_t count, std::uint32_t from) {
        for (std::uint32_t i = 0; i < count; ++i) {
            g_dma_source = from + 4 * i;
            word(words[i]);
        }
    }, max_tags, tags);
}

bool UnwrittenTag(std::uint64_t tag, std::uint32_t upper0, std::uint32_t upper1) {
    if (tag != 0) return false;
    auto wants_data = [](std::uint32_t code) {
        const std::uint32_t command = (code >> 24) & 0x7F, num = (code >> 16) & 0xFF, immediate = code & 0xFFFF;
        if (command >= 0x60) return true;                       // UNPACK (num 0 is 256)
        if (command == 0x50 || command == 0x51) return true;    // DIRECT, DIRECTHL (immediate 0 is 65536 qwords)
        if (command == 0x4A) return true;                       // MPG (num 0 is 256)
        if (command == 0x20 || command == 0x30 || command == 0x31) return true;     // STMASK, STROW, STCOL
        (void)num;
        (void)immediate;
        return false;
    };
    return wants_data(upper0) || wants_data(upper1);
}

DmaOutcome SendDmaBlocks(DmaChannelState& ch, const DmaBlockSink& word, std::uint32_t max_tags,
                         bool tags, int unwritten) {
    DmaOutcome outcome;
    const std::uint32_t mode = (ch.chcr >> 2) & 3;
    // Normal mode, or a chain resumed with QWC still owed: data from MADR first.
    if (ch.qwc) {
        Send(ch.madr, ch.qwc, outcome, word);
        ch.madr += 16 * ch.qwc;
        ch.qwc = 0;
    }
    if (mode != 1) {
        outcome.ended = true;
        return outcome;
    }
    // TTE: the VIFs take a tag's upper half as VIFcodes; the GIF does not. Sent
    // there, the zeros ahead of a packet misaligned its GIFtag - Resident Evil
    // Outbreak starts PATH3 with TTE set in a scene after a door, its frame's
    // register packet (ZBUF's depth clear among them) was lost, and the scene
    // was drawn against a depth buffer never cleared: black.
    const bool transfer_tags = tags && ((ch.chcr >> 6) & 1), irq_ends = (ch.chcr >> 7) & 1;
    std::uint32_t depth = (ch.chcr >> 4) & 3;       // ASP: call stack depth
    Memory& m = GuestMemory();
    while (outcome.tags < max_tags) {
        const std::uint64_t tag = m.Read(ch.tadr, 8);
        // A tag the game has not written yet (UnwrittenTag): the walk waits there, nothing of it sent - or, given up
        // waiting, ends there as the all-zero tag says (refe, no data), its upper half not sent.
        if (unwritten != kUnwrittenSend && transfer_tags &&
            UnwrittenTag(tag, std::uint32_t(m.Read(ch.tadr + 8, 4)), std::uint32_t(m.Read(ch.tadr + 12, 4)))) {
            if (unwritten == kUnwrittenWait) {
                outcome.waiting = true;
                break;
            }
            ++outcome.tags;
            outcome.dropped = true;
            outcome.dropped_at = ch.tadr;
            ch.chcr &= 0xFFFFu;                 // TAG: the zero tag's
            ch.madr = 0;
            ch.tadr += 16;
            outcome.ended = true;
            break;
        }
        ++outcome.tags;
        g_recent[g_recent_at % kRecentTags] = RecentTag{tag, ch.tadr, g_recent_at};
        ++g_recent_at;
        {
            // PS2HOST_DMA_TRACE=<lo hex>-<hi hex> (diagnosis): each tag walked at a TADR in that range, with its upper
            // half and the data it sends (the last 600 kept in memory, said at the end of each walk that had any).
            static const std::pair<std::uint32_t, std::uint32_t> range = [] {
                unsigned long lo = 0, hi = 0;
                if (const char* e = std::getenv("PS2HOST_DMA_TRACE")) std::sscanf(e, "%lx-%lx", &lo, &hi);
                return std::make_pair(std::uint32_t(lo), std::uint32_t(hi));
            }();
            if (range.second && ch.tadr >= range.first && ch.tadr <= range.second) {
                RangedTag& r = g_ranged[g_ranged_at++ % kRangedTags];
                r = RangedTag{tag, ch.tadr, std::uint32_t(m.Read(ch.tadr + 8, 4)), std::uint32_t(m.Read(ch.tadr + 12, 4)),
                              g_recent_at};
            }
        }
        ch.chcr = (ch.chcr & 0xFFFFu) | (std::uint32_t(tag) & 0xFFFF0000u);     // TAG
        if (transfer_tags) {
            const std::uint32_t high[2] = {std::uint32_t(m.Read(ch.tadr + 8, 4)), std::uint32_t(m.Read(ch.tadr + 12, 4))};
            // Diagnosis (the VIF1 stall, 2026-11-23): a tag whose upper half holds no VIFcode - a chain that reached
            // data, or data where a tag should be - said with the tags walked before it (the first few times).
            // (A command's data may also run on through a tag's upper half: then it is no error.)
            auto vifcode = [](std::uint32_t w) {
                const std::uint32_t c = (w >> 24) & 0x7F;
                return c <= 0x07 || c == 0x10 || c == 0x11 || (c >= 0x13 && c <= 0x15) || c == 0x17 || c == 0x20 ||
                       c == 0x30 || c == 0x31 || c == 0x4A || c == 0x50 || c == 0x51 || c >= 0x60;
            };
            static int told = 0;
            if ((!vifcode(high[0]) || !vifcode(high[1])) && told < 3) {
                ++told;
                std::string before;
                for (std::uint32_t k = 12; k >= 1; --k) {
                    const RecentTag& r = g_recent[(g_recent_at - 1 - k) % kRecentTags];
                    char one[48];
                    std::snprintf(one, sizeof one, " %016llX@%08X", static_cast<unsigned long long>(r.tag), unsigned(r.tadr));
                    before += one;
                }
                std::fprintf(stderr, "[dma] a tag's upper half is no VIFcode: tag %016llX@%08X, upper %08X %08X, CHCR %08X; "
                             "the 12 tags before it:%s\n", static_cast<unsigned long long>(tag), unsigned(ch.tadr),
                             unsigned(high[0]), unsigned(high[1]), unsigned(ch.chcr), before.c_str());
            }
            word(high, 2, ch.tadr + 8);
        }
        const std::uint32_t qwc = std::uint32_t(tag) & 0xFFFFu;
        const std::uint32_t id = std::uint32_t(tag >> 28) & 7;
        const std::uint32_t after = ch.tadr + 16;
        bool end = false;
        std::uint32_t data = after;
        switch (id) {
            case 0: data = Address(tag); ch.tadr = after; end = true; break;        // refe (TADR on: dma/spr/chain)
            case 1: ch.tadr = after + 16 * qwc; break;                              // cnt
            case 2: ch.tadr = Address(tag); break;                                  // next
            case 3: case 4:                                                         // ref, refs
                data = Address(tag);
                ch.tadr = after;
                // Diagnosis (2026-11-23): refs is ref with DMA stall control (D_CTRL.STD, D_STADR) - not modelled:
                // its data read at once. Said the first time, with where its data lies.
                if (id == 4) {
                    static bool told = false;
                    if (!told) {
                        told = true;
                        std::fprintf(stderr, "[dma] a refs tag (stall control, not modelled): tag %016llX@%08X, data %08X\n",
                                     static_cast<unsigned long long>(tag), unsigned(ch.tadr - 16), unsigned(data));
                    }
                }
                break;
            case 5:                                                                 // call
                if (depth == 0) ch.asr0 = after + 16 * qwc;
                else ch.asr1 = after + 16 * qwc;
                if (depth < 2) ++depth;
                ch.tadr = Address(tag);
                break;
            case 6:                                                                 // ret
                if (depth == 0) end = true;
                else ch.tadr = (--depth == 0) ? ch.asr0 : ch.asr1;
                break;
            case 7: end = true; break;                                              // end
        }
        Send(data, qwc, outcome, word);
        ch.madr = data + 16 * qwc;
        if (end || (irq_ends && (tag >> 31) & 1)) {
            outcome.ended = true;
            break;
        }
    }
    ch.chcr = (ch.chcr & ~0x30u) | (depth << 4);
    return outcome;
}

std::string RangedDmaTags(std::uint32_t last) {
    std::string out;
    const std::uint32_t n = std::min(last, std::min(g_ranged_at, kRangedTags));
    for (std::uint32_t k = n; k >= 1; --k) {
        const RangedTag& r = g_ranged[(g_ranged_at - k) % kRangedTags];
        char one[96];
        std::snprintf(one, sizeof one, "\n[dma trace]   #%u %016llX@%08X upper %08X %08X", unsigned(r.serial),
                      static_cast<unsigned long long>(r.tag), unsigned(r.tadr), unsigned(r.upper0), unsigned(r.upper1));
        out += one;
    }
    return out;
}

std::string RecentDmaTags(std::uint32_t near, std::uint32_t span) {
    std::string out;
    int shown = 0;
    for (std::uint32_t k = 0; k < kRecentTags && shown < 40; ++k) {
        const RecentTag& r = g_recent[(g_recent_at + k) % kRecentTags];
        if (!r.tag && !r.tadr) continue;
        const std::uint32_t data = std::uint32_t(r.tag >> 32) & 0x7FFFFFF0u;
        const bool close = (r.tadr + span >= near && r.tadr <= near + span) || (data + span >= near && data <= near + span);
        if (span && !close) continue;
        char one[64];
        std::snprintf(one, sizeof one, " #%u %016llX@%08X", unsigned(r.serial), static_cast<unsigned long long>(r.tag),
                      unsigned(r.tadr));
        out += one;
        ++shown;
    }
    return out;
}

}  // namespace ps2host
