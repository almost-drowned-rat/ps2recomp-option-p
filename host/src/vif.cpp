/*
 * VIFcode decoding (EE User's Manual, chapter 6). A VIFcode is
 *   bits  0-15 IMMEDIATE, 16-23 NUM, 24-30 CMD, 31 interrupt.
 * Data lengths, in 32-bit words:
 *   STMASK 1; STROW, STCOL 4; MPG NUM*2 (NUM 0 = 256);
 *   DIRECT, DIRECTHL IMMEDIATE*4 (0 = 65536 quadwords);
 *   UNPACK (0x60-0x7F) by its element format, NUM and STCYCL's CL and WL;
 *   every other command none.
 *
 * UNPACK's CMD is 0x60 | m << 4 | vn << 2 | vl: m masks, vn + 1 components of
 * 32 >> vl bits (vn 3, vl 3 is V4-5, 16 bits of RGBA 5-5-5-1). IMMEDIATE holds
 * the address in quadwords (bits 0-9), USN (14: zero-extend, not sign-extend)
 * and FLG (15: relative to TOPS). With CL >= WL each CL quadwords take WL
 * elements and skip the rest; with WL > CL each WL take CL elements and the
 * rest are filled from the row and column registers by the mask.
 */

#include "ps2host/vif.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "ps2host/dma.h"
#include "ps2host/memory.h"
#include "ps2host/vu.h"

namespace ps2host {
bool OnVif1Worker();            // run.h's (not included here: its Stats would meet this file's)
}

namespace ps2host {

namespace {
// Words of data an UNPACK of `num` elements in `format` reads, with STCYCL's CL and WL as UNPACK takes them.
std::uint32_t UnpackDataWords(std::uint32_t format, std::uint32_t num, std::uint32_t cl, std::uint32_t wl) {
    const std::uint32_t vl = format & 3, vn = (format >> 2) & 3;
    // Bits per element: components (vn + 1) of 32 >> vl bits; V4-5 packs 16.
    const std::uint32_t bits = (vn == 3 && vl == 3) ? 16 : (vn + 1) * (32u >> vl);
    // With WL > CL (filling writes) only CL of every WL elements come from data.
    std::uint32_t elements = num;
    if (wl > cl) {
        const std::uint32_t partial = num % wl;
        elements = cl * (num / wl) + (partial < cl ? partial : cl);
    }
    return (elements * bits + 31) / 32;
}
}  // namespace

std::uint32_t VifStream::UnpackWords(std::uint32_t format, std::uint32_t num) const {
    return UnpackDataWords(format, num, Cl(), Wl());
}

void VifWalker::Words(const std::uint32_t* words, std::uint32_t count, std::vector<bool>* masks) {
    for (std::uint32_t i = 0; i < count;) {
        if (pending_) {
            const std::uint32_t skip = std::min(pending_, count - i);
            pending_ -= skip;
            i += skip;
            continue;
        }
        const std::uint32_t word = words[i++];
        const std::uint32_t command = (word >> 24) & 0x7F, num = (word >> 16) & 0xFF, immediate = word & 0xFFFF;
        if (command >= 0x60) {
            pending_ = UnpackDataWords(command & 0xF, num ? num : 256, cl_, wl_ ? wl_ : 256);
            continue;
        }
        switch (command) {
            case 0x01: cl_ = immediate & 0xFF; wl_ = immediate >> 8; break;      // STCYCL
            case 0x06: masks->push_back((immediate & 0x8000) != 0); break;      // MSKPATH3
            case 0x20: pending_ = 1; break;                                      // STMASK
            case 0x30: case 0x31: pending_ = 4; break;                           // STROW, STCOL
            case 0x4A: pending_ = (num ? num : 256) * 2; break;                  // MPG
            case 0x50: case 0x51: pending_ = (immediate ? immediate : 0x10000) * 4; break;     // DIRECT(HL)
            default: break;
        }
    }
}

void VifStream::WriteElement(const std::uint32_t value[4], bool from_data) {
    Unpack& u = unpack_;
    if (u.written >= u.num) return;
    const std::uint32_t cl = Cl(), wl = Wl();
    const std::uint32_t k = u.written++;
    const std::uint32_t address = wl <= cl ? u.address + (k / wl) * cl + (k % wl) : u.address + k;
    u.last_address = address;
    if (!vu) return;
    std::uint32_t* q = vu->Quad(address);
    // V3 writes w too: the stream's next component (ps2autotests dma/vif/unpack - the hardware reads on).
    if (from_data && !u.masked && mode == 0) {      // the usual case: the element as it is
        std::memcpy(q, value, 16);
        return;
    }
    const std::uint32_t position = std::min(k % wl, 3u);
    for (std::uint32_t f = 0; f < 4; ++f) {
        std::uint32_t m = u.masked ? (mask >> (position * 8 + f * 2)) & 3 : 0;
        if (!from_data && m == 0) m = 1;            // a filling write: the row
        switch (m) {
            case 0: {
                std::uint32_t v = value[f];
                if (mode == 1) v += row[f];
                else if (mode == 2) row[f] = v = v + row[f];
                else if (mode == 3) row[f] = v;     // undocumented: the data as it is, and kept in ROW (dma/vif/stmod)
                q[f] = v;
                break;
            }
            case 1: q[f] = row[f]; break;
            case 2: q[f] = col[position]; break;
            default: break;                         // write-protected
        }
    }
}

void VifStream::UnpackFill() {
    Unpack& u = unpack_;
    const std::uint32_t cl = Cl(), wl = Wl();
    if (wl <= cl) return;
    const std::uint32_t zero[4] = {};
    while (u.written < u.num && (u.written % wl) >= cl) WriteElement(zero, false);
}

void VifStream::V3Pending(std::uint32_t address, bool w_missing) {
    Unpack& u = unpack_;
    u.w_pending = w_missing && !u.masked && mode == 0 && vu;
    u.w_address = address;
}

void VifStream::V3Resolve(std::uint32_t first) {
    Unpack& u = unpack_;
    if (!u.w_pending) return;
    u.w_pending = false;
    if (vu) vu->Quad(u.w_address)[3] = first;
}

void VifStream::Element(const std::uint8_t* b, std::uint32_t c[4], std::size_t avail, bool* w_missing) const {
    const Unpack& u = unpack_;
    const std::uint32_t vl = u.format & 3, vn = (u.format >> 2) & 3;
    c[0] = c[1] = c[2] = c[3] = 0;
    if (w_missing) *w_missing = false;
    if (vn == 3 && vl == 3) {                       // V4-5
        const std::uint32_t v = std::uint32_t(b[0]) | std::uint32_t(b[1]) << 8;
        c[0] = (v & 0x1F) << 3;
        c[1] = ((v >> 5) & 0x1F) << 3;
        c[2] = ((v >> 10) & 0x1F) << 3;
        c[3] = ((v >> 15) & 1) << 7;
        return;
    }
    const std::uint32_t width = 4u >> vl;           // bytes per component
    for (std::uint32_t i = 0; i <= vn; ++i) {
        const std::uint8_t* e = b + i * width;
        if (width == 4) std::memcpy(&c[i], e, 4);
        else if (width == 2) {
            const std::uint16_t h = std::uint16_t(e[0] | e[1] << 8);
            c[i] = u.unsigned_ ? h : std::uint32_t(std::int32_t(std::int16_t(h)));
        } else {
            c[i] = u.unsigned_ ? e[0] : std::uint32_t(std::int32_t(std::int8_t(e[0])));
        }
    }
    if (vn == 0) c[1] = c[2] = c[3] = c[0];             // S: every field
    else if (vn == 1) { c[2] = c[0]; c[3] = c[1]; }     // V2: x y x y
    else if (vn == 2) {                                 // V3: w is the component after the element
        const std::uint8_t* e = b + 3 * width;
        if (avail >= 4 * width) {
            if (width == 4) std::memcpy(&c[3], e, 4);
            else if (width == 2) {
                const std::uint16_t h = std::uint16_t(e[0] | e[1] << 8);
                c[3] = u.unsigned_ ? h : std::uint32_t(std::int32_t(std::int16_t(h)));
            } else {
                c[3] = u.unsigned_ ? e[0] : std::uint32_t(std::int32_t(std::int8_t(e[0])));
            }
        } else if (w_missing) {
            *w_missing = true;                          // not here yet: the next element's x, when it comes
        }
    }
}

void VifStream::UnpackData(std::uint32_t word) {
    Unpack& u = unpack_;
    for (int i = 0; i < 4; ++i) u.bytes.push_back(std::uint8_t(word >> (8 * i)));
    const std::uint32_t vl = u.format & 3, vn = (u.format >> 2) & 3;
    const bool v45 = vn == 3 && vl == 3;
    const std::uint32_t size = v45 ? 2 : (vn + 1) * (4u >> vl);
    std::size_t at = 0;
    while (u.bytes.size() - at >= size && u.written < u.num) {
        std::uint32_t c[4];
        bool missing = false;
        Element(u.bytes.data() + at, c, u.bytes.size() - at, &missing);
        at += size;
        V3Resolve(c[0]);
        WriteElement(c, true);
        V3Pending(u.last_address, missing && u.written < u.num);
        UnpackFill();
    }
    u.bytes.erase(u.bytes.begin(), u.bytes.begin() + std::ptrdiff_t(at));
}

namespace {
/* PS2HOST_VIF_STATS=1: the words UNPACK took, by format, mask, STMOD, CL/WL and whether a part-read element was
   pending - printed at the end (which paths a game's data takes; diagnosis only, one relaxed check when off). */
struct VifStats {
    bool on = std::getenv("PS2HOST_VIF_STATS") != nullptr;
    std::atomic<unsigned long long> words[16][2][4][2][2] = {};
    void Print() {
        if (!on) return;
        std::fprintf(stderr, "[vif stats] UNPACK words by format (vn<<2|vl), masked, STMOD, WL>CL, part-read pending:\n");
        for (int f = 0; f < 16; ++f)
            for (int m = 0; m < 2; ++m)
                for (int s = 0; s < 4; ++s)
                    for (int w = 0; w < 2; ++w)
                        for (int p = 0; p < 2; ++p)
                            if (unsigned long long n = words[f][m][s][w][p].load())
                                std::fprintf(stderr, "[vif stats] format 0x%X masked %d stmod %d wl>cl %d pending %d: %llu words\n",
                                             f, m, s, w, p, n);
    }
};
VifStats& Stats() {
    static VifStats stats;
    return stats;
}
}  // namespace

// at the end of a run (main.cpp, before the process is taken down - no destructors run)
void PrintVifStats() { Stats().Print(); }

void VifStream::UnpackRun(const std::uint32_t* words, std::uint32_t count) {
    Unpack& u = unpack_;
    const std::uint32_t vl = u.format & 3, vn = (u.format >> 2) & 3;
    const std::uint32_t size = (vn == 3 && vl == 3) ? 2 : (vn + 1) * (4u >> vl);
    if (VifStats& st = Stats(); st.on)
        st.words[u.format & 15][u.masked ? 1 : 0][mode & 3][Wl() > Cl() ? 1 : 0][u.bytes.empty() ? 0 : 1].fetch_add(
            count, std::memory_order_relaxed);
    // A V3-32 quadword from the last run waiting for its w: this run's first word (the next element's x).
    if (u.w_pending && u.format == 0x8 && u.bytes.empty() && count) V3Resolve(words[0]);
    // The usual case, in one loop: unmasked, no addition (STMOD 0), CL >= WL
    // (no filling writes), elements of whole words, none part-read - each
    // stored as WriteElement would, its address kept by counting rather than
    // dividing.
    // PS2HOST_VIF_SLOW=1: every element the general way (Element, WriteElement) - the fast paths below checked
    // against it on a game's own data (diagnosis only).
    static const bool slow = std::getenv("PS2HOST_VIF_SLOW") != nullptr;
    if (!slow && u.bytes.empty() && size % 4 == 0 && !u.masked && mode == 0 && vu &&
        Wl() <= Cl() && (u.format == 0xC || u.format == 0x8 || u.format == 0x4 ||
                                           u.format == 0x0 || u.format == 0xD || u.format == 0x5 ||
                                           u.format == 0xE)) {
        const std::uint32_t per = size / 4, wl = Wl(), cl = Cl();
        std::uint32_t k = u.written, in_cycle = k % wl, base = u.address + (k / wl) * cl, used = 0;
        const bool zero_extend = u.unsigned_;
        auto half = [zero_extend](std::uint32_t h) {
            return zero_extend ? (h & 0xFFFF) : std::uint32_t(std::int32_t(std::int16_t(std::uint16_t(h))));
        };
        auto byte = [zero_extend](std::uint32_t b) {
            return zero_extend ? (b & 0xFF) : std::uint32_t(std::int32_t(std::int8_t(std::uint8_t(b))));
        };
        // V4-32 writing every quadword it passes (WL = CL): the words as they
        // are into consecutive quadwords - a run at a time, up to the end of
        // data memory (the vertices the game sends, most of an UNPACK's data).
        if (u.format == 0xC && wl == cl) {
            while (count - used >= 4 && k < u.num) {
                const std::uint32_t at = u.address + k;
                const std::uint32_t n = std::min({(count - used) / 4, u.num - k, vu->QuadsToEnd(at)});
                std::memcpy(vu->Quad(at), words + used, std::size_t(n) * 16);
                used += 4 * n;
                k += n;
            }
            in_cycle = k % wl;
            base = u.address + (k / wl) * cl;
        }
        while (count - used >= per && k < u.num) {
            std::uint32_t* q = vu->Quad(base + in_cycle);
            const std::uint32_t* w = words + used;
            switch (u.format) {
                case 0xC: std::memcpy(q, w, 16); break;                             // V4-32
                case 0x8: {                                                         // V3-32: w the next word
                    std::memcpy(q, w, 12);
                    if (count - used > per) {
                        q[3] = w[3];
                    } else {
                        q[3] = 0;                       // not here yet: the next run's first word, or 0 at the end
                        if (k + 1 < u.num) { u.w_pending = true; u.w_address = base + in_cycle; }
                    }
                    break;
                }
                case 0x4: q[0] = w[0]; q[1] = w[1]; q[2] = w[0]; q[3] = w[1]; break; // V2-32: x y x y
                case 0x0: q[0] = q[1] = q[2] = q[3] = w[0]; break;                  // S-32
                case 0xD:                                                           // V4-16
                    q[0] = half(w[0]); q[1] = half(w[0] >> 16); q[2] = half(w[1]); q[3] = half(w[1] >> 16);
                    break;
                case 0x5: {                                                         // V2-16
                    const std::uint32_t x = half(w[0]), y = half(w[0] >> 16);
                    q[0] = x; q[1] = y; q[2] = x; q[3] = y;
                    break;
                }
                default:                                                            // V4-8
                    q[0] = byte(w[0]); q[1] = byte(w[0] >> 8); q[2] = byte(w[0] >> 16); q[3] = byte(w[0] >> 24);
                    break;
            }
            used += per;
            ++k;
            if (++in_cycle == wl) {
                in_cycle = 0;
                base += cl;
            }
        }
        u.written = k;
        words += used;
        count -= used;
    }
    // V3-16 (6 bytes an element) in the usual case - unmasked, STMOD 0, CL >= WL:
    // two elements at a time (three words), so a run stays on whole words;
    // what is left (fewer than three words, or one element) goes the byte way
    // below. w is the stream's next component, as Element reads it: the next
    // element's x, or - past this run - the next run's first (w_pending), or
    // 0 at the unpack's end. A third of ZOE's UNPACK words in its fight
    // (PS2HOST_VIF_STATS, 2026-11-18) - the byte path was ~8 % of a core.
    if (!slow && u.format == 0x9 && !u.masked && mode == 0 && vu && Wl() <= Cl()) {
        // leftover bytes of a part-read element first (at most two words make it whole again)
        while (!u.bytes.empty() && count) {
            UnpackData(*words++);
            --count;
        }
        if (u.bytes.empty()) {
            const std::uint32_t wl = Wl(), cl = Cl();
            std::uint32_t k = u.written, in_cycle = k % wl, base = u.address + (k / wl) * cl, used = 0;
            const bool zero_extend = u.unsigned_;
            auto half = [zero_extend](std::uint32_t h) {
                return zero_extend ? (h & 0xFFFF) : std::uint32_t(std::int32_t(std::int16_t(std::uint16_t(h))));
            };
            auto store = [&](std::uint32_t x, std::uint32_t y, std::uint32_t z, std::uint32_t w) {
                std::uint32_t* q = vu->Quad(base + in_cycle);
                q[0] = x; q[1] = y; q[2] = z; q[3] = w;
                u.last_address = base + in_cycle;
                ++k;
                if (++in_cycle == wl) {
                    in_cycle = 0;
                    base += cl;
                }
            };
            if (count >= 3 && k + 2 <= u.num && u.w_pending) V3Resolve(half(words[0]));
            while (count - used >= 3 && k + 2 <= u.num) {
                const std::uint32_t* w = words + used;
                const std::uint32_t x0 = half(w[0]), y0 = half(w[0] >> 16), z0 = half(w[1]);
                const std::uint32_t x1 = half(w[1] >> 16), y1 = half(w[2]), z1 = half(w[2] >> 16);
                store(x0, y0, z0, x1);
                used += 3;
                if (count - used >= 1) {
                    store(x1, y1, z1, half(w[3]));
                } else {
                    store(x1, y1, z1, 0);   // the next component not here yet: the next run's first, or 0 at the end
                    if (k < u.num) { u.w_pending = true; u.w_address = u.last_address; }
                }
            }
            u.written = k;
            words += used;
            count -= used;
        }
    }
    // Elements of whole words, none part-read: straight from the words (the
    // bytes are the same; the byte buffer would hold just these).
    if (u.bytes.empty() && size % 4 == 0) {
        const std::uint32_t per = size / 4;
        const std::uint8_t* bytes = reinterpret_cast<const std::uint8_t*>(words);
        std::uint32_t used = 0;
        while (count - used >= per && u.written < u.num) {
            std::uint32_t c[4];
            bool missing = false;
            Element(bytes + std::size_t(used) * 4, c, std::size_t(count - used) * 4, &missing);
            used += per;
            V3Resolve(c[0]);
            WriteElement(c, true);
            V3Pending(u.last_address, missing && u.written < u.num);
            UnpackFill();
        }
        words += used;
        count -= used;
    }
    for (std::uint32_t i = 0; i < count; ++i) UnpackData(words[i]);
}

void VifStream::Start(std::uint32_t pc) {
    if (double_buffered) {
        top = tops;
        dbf ^= 1;
        tops = base + (dbf ? offset : 0);
    }
    itop = itops;
    if (!vu) return;
    vu->top = top;
    vu->itop = itop;
    vu->Run(pc);
    end_pc_ = vu->resume;
}

void VifStream::Words(const std::uint32_t* words, std::uint32_t count, std::uint32_t from) {
    for (std::uint32_t i = 0; i < count;) {
        if (stalled_) {                         // the rest waits behind the interrupt's stall (Stall)
            held_.insert(held_.end(), words + i, words + count);
            break;
        }
        if (pending_ && data_ == Data::Unpack) {
            const std::uint32_t run = std::min(pending_, count - i);
            pending_ -= run;
            data_words_ += run;
            data_index_ += run;
            g_dma_source = from + 4 * i;
            UnpackRun(words + i, run);
            i += run;
            if (!pending_ && interrupt_after_) Stall();
            continue;
        }
        if (pending_ && data_ == Data::Direct && on_direct_block) {
            const std::uint32_t run = std::min(pending_, count - i);
            pending_ -= run;
            data_words_ += run;
            data_index_ += run;
            on_direct_block(words + i, run, from + 4 * i);
            i += run;
            if (!pending_ && interrupt_after_) Stall();
            continue;
        }
        g_dma_source = from + 4 * i;
        Word(words[i++]);
    }
    mirror_due_ = true;             // mirrored when the registers are read (MirrorIfDue)
}

void VifStream::Reset() {
    pending_ = 0;
    data_ = Data::None;
    data_index_ = 0;
    unpack_.bytes.clear();
    unpack_.w_pending = false;
    unpack_.written = unpack_.num;
    stalled_ = interrupt_after_ = false;
    held_.clear();
    stat_flags_ = 0;
    Mirror();
}

namespace {
bool NoStall() {
    static const bool off = std::getenv("PS2HOST_VIF_NO_STALL") != nullptr;
    return off;
}
}  // namespace

void VifStream::Stall() {
    interrupt_after_ = false;
    stat_flags_ |= kStatVis | kStatInt;
    if (NoStall()) {
        stat_flags_ &= ~kStatVis;
    } else {
        stalled_ = true;
        if (stalls_++ == 0) {
            std::fprintf(stderr, "[vif] VIF%d stalled by a VIFcode's interrupt bit (0x%08X) - goes on at FBRST.STC\n",
                         register_base == 0x10003C00u ? 1 : 0, unsigned(code_));
            // The codes before it, oldest first, with where they came from: a code read out of data (a stream
            // out of step) shows as an unknown command after an UNPACK / DIRECT whose size was wrong.
            std::string recent;
            for (std::uint32_t k = 0; k < 8; ++k) {
                const std::uint32_t at = (recent_at_ + k) & 7;
                if (!recent_codes_[at] && !recent_from_[at]) continue;
                char one[40];
                std::snprintf(one, sizeof one, " %08X@%08X", unsigned(recent_codes_[at]), unsigned(recent_from_[at]));
                recent += one;
            }
            std::fprintf(stderr, "[vif]   the VIFcodes before it (code@memory):%s; worker %s\n", recent.c_str(),
                         OnVif1Worker() ? "yes" : "no");
            // (The tags as of now on the EE's thread: with the worker, the chains walked since this data was copied.)
            const std::uint32_t last_from = recent_from_[(recent_at_ - 1) & 7];
            std::fprintf(stderr, "[vif]   DMA tags walked lately at or towards %08X +-1 KB (#n tag@TADR):%s\n",
                         unsigned(last_from), RecentDmaTags(last_from, 0x400).c_str());
            std::fprintf(stderr, "[dma trace] the last tags walked in PS2HOST_DMA_TRACE's range:%s\n",
                         RangedDmaTags(40).c_str());
        }
    }
    if (on_interrupt) on_interrupt();
}

void VifStream::Continue() {
    stat_flags_ &= ~(kStatVis | kStatInt);
    if (!stalled_) {
        Mirror();
        return;
    }
    stalled_ = false;
    std::vector<std::uint32_t> words;
    words.swap(held_);
    std::size_t i = 0;
    for (; i < words.size() && !stalled_; ++i) Word(words[i]);
    // Stalled again: what is left waits again, ahead of anything newer.
    if (i < words.size()) held_.insert(held_.begin(), words.begin() + std::ptrdiff_t(i), words.end());
    Mirror();
}

void VifStream::Mirror() {
    mirror_due_ = false;
    if (!register_base) return;
    // The register block (0x180 bytes, one window) translated once, and a register written only when it changes:
    // this runs after every DMA chunk (~8,000 a second in ZOE's fight) and the EE's thread reads the same cache
    // lines - rewriting all 22 every time was 7.8 % of a core on VIF1's worker (P-Gate 3 profile, 2026-11-17).
    // (Guest memory's blocks stay where they are for the run: translated once.)
    if (!mirror_bytes_) mirror_bytes_ = GuestMemory().Translate(register_base, 0x180);
    std::uint8_t* const regs = mirror_bytes_;
    if (!regs) return;
    auto put = [regs](std::uint32_t offset_in, std::uint32_t value) {
        std::uint32_t old;
        std::memcpy(&old, regs + offset_in, 4);
        if (old != value) std::memcpy(regs + offset_in, &value, 4);
    };
    // STAT: VPS (bits 0-1) 1 while a command waits for its data; the rest as they are.
    std::uint32_t stat = 0;
    std::memcpy(&stat, regs, 4);
    // MRK, VIS, INT as this model keeps them (stat_flags_); VPS 2 with a VIFcode waiting behind a stall.
    const std::uint32_t vps = stalled_ ? (held_.empty() ? 0u : 2u) : (pending_ && data_ != Data::None ? 1u : 0u);
    put(0x00, (stat & ~(3u | kStatMrk | kStatVis | kStatInt)) | stat_flags_ | vps);
    put(0x40, cycle_length_ | write_length_ << 8);                      // CYCLE
    put(0x50, mode);                                                    // MODE
    // NUM: what is left to write - an UNPACK of NUM 0 (256) that has written nothing reads 256 on VIF1 but 0 on
    // VIF0 (its register is 8 bits wide; ps2autotests dma/vif/unpack, the console's readings, lines 27 and 54).
    put(0x60, data_ == Data::Unpack && pending_
                  ? (unpack_.num - unpack_.written) & (double_buffered ? 0x1FFu : 0xFFu) : 0);   // NUM
    put(0x70, mask);                                                    // MASK
    put(0x80, code_);                                                   // CODE
    put(0x90, itops);                                                   // ITOPS
    if (double_buffered) {
        put(0xA0, base);                                                // BASE
        put(0xB0, offset);                                              // OFST
        put(0xC0, tops);                                                // TOPS
        put(0xE0, top);                                                 // TOP
    }
    put(0xD0, itop);                                                    // ITOP
    for (int i = 0; i < 4; ++i) {
        put(0x100 + 0x10 * i, row[i]);                                  // R0-R3
        put(0x140 + 0x10 * i, col[i]);                                  // C0-C3
    }
}

void VifStream::Word(std::uint32_t word) {
    if (stalled_) {                             // behind the interrupt's stall until FBRST.STC (Stall)
        held_.push_back(word);
        return;
    }
    if (pending_) {
        --pending_;
        ++data_words_;
        const std::uint32_t index = data_index_++;
        switch (data_) {
            case Data::Direct: if (on_direct) on_direct(word); break;
            case Data::Mpg:
                if (vu) {
                    const std::size_t at = (mpg_address_ + 4 * index) & (vu->micro.size() - 1);
                    // Games upload the same microcode again and again (Burnout 3
                    // every frame): only a word that changes makes the compiled
                    // programs be compared again.
                    if (std::memcmp(vu->micro.data() + at, &word, 4) != 0) {
                        std::memcpy(vu->micro.data() + at, &word, 4);
                        vu->MicroWritten();
                    }
                }
                break;
            case Data::Unpack: UnpackData(word); break;
            case Data::Mask: mask = word; break;
            case Data::Row: row[index & 3] = word; break;
            case Data::Col: col[index & 3] = word; break;
            default: break;
        }
        if (!pending_ && interrupt_after_) Stall();     // the command's data done: its interrupt bit now
        return;
    }
    data_ = Data::None;
    data_index_ = 0;
    ++commands_;
    code_ = word;
    recent_codes_[recent_at_ & 7] = word;
    recent_from_[recent_at_ & 7] = g_dma_source;
    ++recent_at_;
    {
        // PS2HOST_VIF_TRACE_AT=<hex address> (diagnosis): a VIFcode read from there, and the 6 after it, said with
        // where each came from and the stream's STCYCL (the first 4 times).
        static const std::uint32_t trace_at = [] {
            const char* e = std::getenv("PS2HOST_VIF_TRACE_AT");
            return e ? std::uint32_t(std::strtoul(e, nullptr, 16)) : 0u;
        }();
        static int tracing = 0, traces = 0;
        if (trace_at && g_dma_source == trace_at && traces < 4) {
            ++traces;
            tracing = 7;
        }
        if (tracing > 0) {
            --tracing;
            std::fprintf(stderr, "[vif trace] %08X@%08X CL %u WL %u%s\n", unsigned(word), unsigned(g_dma_source),
                         unsigned(cycle_length_), unsigned(write_length_), tracing == 0 ? " (end)" : "");
        }
    }
    const std::uint32_t command = (word >> 24) & 0x7F;
    ++by_command_[command];
    const std::uint32_t num = (word >> 16) & 0xFF;
    const std::uint32_t immediate = word & 0xFFFF;
    // The interrupt bit: once the command (and its data) is done (Stall). MARK's raises INT without a stall.
    interrupt_after_ = (word & 0x80000000u) != 0 && command != 0x07;
    if (command >= 0x60) {
        Unpack& u = unpack_;
        u.format = command & 0xF;
        u.masked = (command & 0x10) != 0;
        u.unsigned_ = (immediate & 0x4000) != 0;
        u.address = immediate & 0x3FF;
        if (double_buffered && (immediate & 0x8000)) u.address += tops;
        u.num = num ? num : 256;
        u.written = 0;
        u.bytes.clear();
        u.w_pending = false;
        pending_ = UnpackWords(command & 0xF, u.num);
        data_ = Data::Unpack;
        UnpackFill();
        if (!pending_ && interrupt_after_) Stall();
        return;
    }
    switch (command) {
        case 0x01:                                          // STCYCL
            cycle_length_ = immediate & 0xFF;
            write_length_ = immediate >> 8;
            break;
        case 0x02:                                          // OFFSET
            offset = immediate & 0x3FF;
            dbf = 0;
            tops = base;
            break;
        case 0x03: base = immediate & 0x3FF; break;         // BASE
        case 0x04: itops = immediate & 0x3FF; break;        // ITOP
        case 0x05: mode = immediate & 3; break;             // STMOD
        case 0x06:                                          // MSKPATH3
            if (on_mask_path3) on_mask_path3((immediate & 0x8000) != 0);
            break;
        case 0x14: case 0x15: Start(immediate * 8); break;  // MSCAL, MSCALF
        case 0x17: Start(end_pc_); break;                   // MSCNT
        case 0x20: pending_ = 1; data_ = Data::Mask; break; // STMASK
        case 0x30: pending_ = 4; data_ = Data::Row; break;  // STROW
        case 0x31: pending_ = 4; data_ = Data::Col; break;  // STCOL
        case 0x4A:                                          // MPG
            pending_ = (num ? num : 256) * 2;
            mpg_address_ = immediate * 8;
            data_ = Data::Mpg;
            break;
        case 0x50: case 0x51:                               // DIRECT, DIRECTHL
            pending_ = (immediate ? immediate : 0x10000) * 4;
            data_ = Data::Direct;
            break;
        case 0x13:                                          // FLUSHA: nothing waits here either
            if (on_trace) on_trace("FLUSHA");
            break;
        case 0x07:                                          // MARK: STAT.MRK; its interrupt bit no stall
            stat_flags_ |= kStatMrk;
            if (word & 0x80000000u) {
                stat_flags_ |= kStatInt;
                if (on_interrupt) on_interrupt();
            }
            break;
        default: break;                                     // NOP, FLUSH*: nothing waits
    }
    if (!pending_ && interrupt_after_) Stall();
}

std::string VifStream::Summary() const {
    static const std::pair<std::uint32_t, const char*> kNames[] = {
        {0x00, "NOP"}, {0x01, "STCYCL"}, {0x02, "OFFSET"}, {0x03, "BASE"}, {0x04, "ITOP"},
        {0x05, "STMOD"}, {0x06, "MSKPATH3"}, {0x07, "MARK"}, {0x10, "FLUSHE"}, {0x11, "FLUSH"},
        {0x13, "FLUSHA"}, {0x14, "MSCAL"}, {0x15, "MSCALF"}, {0x17, "MSCNT"}, {0x20, "STMASK"},
        {0x30, "STROW"}, {0x31, "STCOL"}, {0x4A, "MPG"}, {0x50, "DIRECT"}, {0x51, "DIRECTHL"}};
    std::string out;
    std::uint64_t unpacks = 0;
    for (std::uint32_t c = 0x60; c < 0x80; ++c) unpacks += by_command_[c];
    for (const auto& [code, name] : kNames)
        if (by_command_[code]) out += std::string(" ") + name + " " + std::to_string(by_command_[code]);
    if (unpacks) out += " UNPACK " + std::to_string(unpacks);
    if (stalls_) out += "; " + std::to_string(stalls_) + " interrupt-bit stalls";
    if (vu) out += "; VU: " + std::to_string(vu->pairs()) + " instruction pairs (" +
                   std::to_string(vu->compiled_pairs()) + " compiled: " + std::to_string(vu->compiled_entries()) +
                   " entered, " + std::to_string(vu->compiled_stale()) + " stale, " +
                   std::to_string(vu->compiled_checked() >> 20) + " MB checked), " + std::to_string(vu->kicks()) +
                   " XGKICKs";
    if (vu) {
        out += "; started at";
        for (const auto& [pc, n] : vu->starts()) {
            char text[32];
            std::snprintf(text, sizeof text, " 0x%X x%llu", unsigned(pc), static_cast<unsigned long long>(n));
            out += text;
        }
    }
    return out;
}

VifStream& Vif1() {
    static VifStream stream = [] {
        VifStream s;
        s.register_base = 0x10003C00u;
        return s;
    }();
    return stream;
}

VifStream& Vif0() {
    static VifStream stream = [] {
        VifStream s;
        s.register_base = 0x10003800u;
        return s;
    }();
    return stream;
}

}  // namespace ps2host
