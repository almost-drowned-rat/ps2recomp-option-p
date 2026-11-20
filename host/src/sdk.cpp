/*
 * Host implementations of Sony SDK library routines, applied by name.
 *
 * Library routines that drive the hardware - wait for the graphics paths, poll
 * a DMA channel - are better replaced whole than emulated register by register:
 * the routine's contract is small, and on the host the hardware it waits for is
 * already done. A game's analysis names such routines (config/names.csv, from
 * diagnostic strings and the cross-game signature library); every routine named
 * here is overridden wherever the game's dispatch table gives that name, so a new
 * game gets these for free once its routines are named.
 */

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <string>

#include "mpeg2_video.h"
#include "ps2host/host.h"
#include "ps2host/ipu.h"
#include "ps2host/memory.h"
#include "ps2host/run.h"

namespace ps2host {

namespace {

void Return(lifted::Context& c, u64 value) { c.r[2] = u64(s64(s32(u32(value)))); }

// sceGsSyncPath(mode, timeout): wait until the paths to the GS are idle - VIF1,
// GIF and VU1 - and return 0, or a negative value on timeout. The host's
// transfers complete when they are started, so the paths are always idle.
void GsSyncPath(lifted::Context& c) { Return(c, 0); }

struct SdkRoutine {
    const char* name;
    GuestFunction function;
};

const SdkRoutine kRoutines[] = {
    {"sceGsSyncPath", GsSyncPath},
};

// PS2HOST_MPEG_LOG=<file>: every picture sceMpegGetPicture(mp, rgb32, mbcount) hands the game - its number, the
// macroblocks asked for, the result and a CRC-32 of the RGB32 macroblocks written (1024 bytes each) - after the
// game's own libmpeg (the translated function) ran. The movies' reference for the native decoder (design 7f):
// the same log from both paths, compared picture by picture.
GuestFunction g_get_picture = nullptr;      // the translated sceMpegGetPicture

u32 Crc32(const u8* data, std::size_t size) {
    static u32 table[256];
    static bool made = false;
    if (!made) {
        for (u32 i = 0; i < 256; ++i) {
            u32 c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        made = true;
    }
    u32 crc = ~0u;
    for (std::size_t i = 0; i < size; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

// PS2HOST_MPEG_TIMES=<file>: each picture handed to the game on either path - its number, the guest clock then
// (ClockNs; the vertical blank = ns * 60 / 1001 / 1e6) and its pts - where the two paths' timing parts.
void LogPictureTime(s64 pts) {
    static std::FILE* log = std::getenv("PS2HOST_MPEG_TIMES") ? std::fopen(std::getenv("PS2HOST_MPEG_TIMES"), "w") : nullptr;
    static u64 pictures = 0;
    if (!log) return;
    std::fprintf(log, "%llu %llu %lld\n", static_cast<unsigned long long>(pictures++),
                 static_cast<unsigned long long>(ClockNs()), static_cast<long long>(pts));
    std::fflush(log);
}

void LoggedGetPicture(lifted::Context& c) {
    static std::FILE* log = std::getenv("PS2HOST_MPEG_LOG") ? std::fopen(std::getenv("PS2HOST_MPEG_LOG"), "w") : nullptr;
    static u64 pictures = 0;
    const u32 mp = u32(c.r[4]), rgb32 = u32(c.r[5]), mbcount = u32(c.r[6]);
    g_get_picture(c);
    const s32 result = s32(u32(c.r[2]));
    if (result > 0) LogPictureTime(s64(GuestMemory().Read(mp + 0x10, 8)));
    if (!log) return;
    std::vector<u8> bytes(std::size_t(std::min(mbcount, 4096u)) * 1024u);
    GuestMemory().ReadBytes(rgb32, bytes.data(), bytes.size());
    // And a CRC of the colour alone (alpha cleared) over the picture's own macroblocks (PS2HOST_MPEG_LOG_MBS:
    // the buffer may be larger than the picture) - what --mpeg-decode's native pictures are compared by.
    const char* mbs_text = std::getenv("PS2HOST_MPEG_LOG_MBS");
    const std::size_t mbs = std::min<std::size_t>(mbs_text ? std::size_t(std::atoi(mbs_text)) : mbcount, mbcount);
    std::vector<u8> colour(bytes.begin(), bytes.begin() + std::ptrdiff_t(std::min(bytes.size(), mbs * 1024u)));
    for (std::size_t i = 3; i < colour.size(); i += 4) colour[i] = 0;
    // PS2HOST_MPEG_LOG_RAW=n: the first n pictures' bytes too, as <log>.<n>.raw (the picture's own macroblocks).
    if (const char* raw = std::getenv("PS2HOST_MPEG_LOG_RAW"); raw && pictures < u64(std::atoi(raw))) {
        const std::string name = std::string(std::getenv("PS2HOST_MPEG_LOG")) + "." + std::to_string(pictures) + ".raw";
        if (std::FILE* o = std::fopen(name.c_str(), "wb")) {
            std::fwrite(bytes.data(), 1, std::min(bytes.size(), mbs * 1024u), o);
            std::fclose(o);
        }
    }
    // sceMpeg's public fields after it (libmpeg.h: width 0, height 4, frameCount 8, pts 0x10, dts 0x18, flags 0x20,
    // pts2nd 0x28, dts2nd 0x30, flags2nd 0x38) - what the native path has to keep up.
    Memory& m = GuestMemory();
    std::fprintf(log, "%llu %u %d %08X %08X w %u h %u n %u pts %lld dts %lld fl %llu pts2 %lld dts2 %lld fl2 %llu\n",
                 static_cast<unsigned long long>(pictures++), unsigned(mbcount), int(result),
                 unsigned(Crc32(bytes.data(), bytes.size())), unsigned(Crc32(colour.data(), colour.size())),
                 unsigned(m.Read(mp, 4)), unsigned(m.Read(mp + 4, 4)), unsigned(m.Read(mp + 8, 4)),
                 static_cast<long long>(m.Read(mp + 0x10, 8)), static_cast<long long>(m.Read(mp + 0x18, 8)),
                 static_cast<unsigned long long>(m.Read(mp + 0x20, 8)), static_cast<long long>(m.Read(mp + 0x28, 8)),
                 static_cast<long long>(m.Read(mp + 0x30, 8)), static_cast<unsigned long long>(m.Read(mp + 0x38, 8)));
    std::fflush(log);
}

// The native movie path (design 7f; the default since 2026-11-16, PS2HOST_MPEG_TIER_E=1 for tier E): sceMpegGetPicture decoded by the
// native decoder (mpeg2_video.h) instead of libmpeg and the IPU model. The game's DMA to the IPU feeds the
// decoder (SetIpuStreamSink); with no whole picture yet the game's no-data callback runs as libmpeg would run it
// (_dispatchMpegCbNodata, by name). The game's demux, ring and audio stay its own code.
mpeg::VideoDecoder* g_native = nullptr;
u32 g_nodata = 0;                           // _dispatchMpegCbNodata
u32 g_get_address = 0;                      // sceMpegGetPicture: the native wait's loop head
GuestFunction g_create = nullptr, g_delete = nullptr, g_reset = nullptr;

u64 g_sunk = 0;                             // bytes the sink took (PS2HOST_MPEG_DEBUG)

// PS2HOST_MPEG_ADDBS_LOG=<file>: every sceMpegAddBs(mp, address, size) the game makes, on either path - where the
// two paths' feeding parts.
GuestFunction g_add_bs = nullptr;
void LoggedAddBs(lifted::Context& c) {
    static std::FILE* log = std::fopen(std::getenv("PS2HOST_MPEG_ADDBS_LOG"), "w");
    static u64 calls = 0;
    if (log) {
        std::fprintf(log, "%llu 0x%08X %u\n", static_cast<unsigned long long>(calls++), unsigned(u32(c.r[5])),
                     unsigned(u32(c.r[6])));
        std::fflush(log);
    }
    g_add_bs(c);
}

// Where each run of the fed stream came from in the game's memory (its video ring): the time stamp callback
// finds a picture's time stamps by the ring address the IPU has read to.
struct Run {
    u64 offset;                 // in the stream fed since the decoder's reset
    u32 address, length;
};
std::vector<Run> g_runs;
u64 g_fed = 0;
lifted::Context* g_ts_context = nullptr;    // NativeGetPicture's, while the decoder runs inside it
u32 g_ts_mp = 0;
u32 g_dispatch = 0;                         // _dispatchMpegCallback
u64 g_shown = 0;                            // pictures handed out since the create (sceMpeg's frameCount)

void ResetRuns() {
    g_runs.clear();
    g_fed = 0;
}

u32 AddressOf(u64 offset) {
    auto it = std::upper_bound(g_runs.begin(), g_runs.end(), offset,
                               [](u64 o, const Run& r) { return o < r.offset; });
    if (it == g_runs.begin()) return 0;
    --it;
    return offset < it->offset + it->length ? u32(it->address + (offset - it->offset)) : 0;
}

/* After each picture header, as libmpeg (0x00474E84 in ZOE): the time stamp callback (type 5) through libmpeg's
   own dispatcher, the IPU's position - channel 4's MADR, the FIFO empty - at the picture's start in the game's
   ring; the pts / dts it gives (-1 for none) kept with the picture. */
void NativeOnPicture(u64 offset, mpeg::Picture& picture) {
    if (!g_ts_context || !g_dispatch) return;
    const u32 address = AddressOf(offset + 8);  // past the picture start code and header's first bytes
    if (!address) return;
    lifted::Context& c = *g_ts_context;
    const lifted::Context saved = c;
    Memory& m = GuestMemory();
    m.Write(0x1000B410u, address, 4);            // D4_MADR: where the IPU has read to
    const u32 data = (u32(c.r[29]) - 0x80u) & ~15u;
    m.Write(data, 5, 4);                        // sceMpegCbTimeStamp
    m.Write(data + 8, ~u64(0), 8);
    m.Write(data + 16, ~u64(0), 8);
    c.r[4] = g_ts_mp;
    c.r[5] = data;
    c.r[29] = data - 0x40u;
    lifted::Call(c, g_dispatch, u32(saved.r[31]));
    picture.pts = s64(m.Read(data + 8, 8));
    picture.dts = s64(m.Read(data + 16, 8));
    c = saved;
}

void NativeSink(const unsigned char* bytes, unsigned long count, unsigned address) {
    if (!g_runs.empty() && g_runs.back().offset + g_runs.back().length == g_fed &&
        g_runs.back().address + g_runs.back().length == address)
        g_runs.back().length += u32(count);
    else
        g_runs.push_back({g_fed, u32(address), u32(count)});
    g_fed += count;
    g_sunk += count;
    // PS2HOST_MPEG_SINK_DUMP=<file>: the video stream as the game sent it (gap-free) - for an outside decoder.
    static std::FILE* dump = std::getenv("PS2HOST_MPEG_SINK_DUMP") ? std::fopen(std::getenv("PS2HOST_MPEG_SINK_DUMP"), "wb")
                                                                   : nullptr;
    if (dump) {
        std::fwrite(bytes, 1, count, dump);
        std::fflush(dump);
    }
    if (g_native) g_native->Feed(bytes, count);
}

void NativeCreate(lifted::Context& c) {
    g_create(c);
    if (!g_native) {
        g_native = new mpeg::VideoDecoder;
        g_native->SetOnPicture(NativeOnPicture);
    }
    g_native->Reset();
    ResetRuns();
    g_shown = 0;
    SetIpuStreamSink(NativeSink);
}

void NativeDelete(lifted::Context& c) {
    SetIpuStreamSink(nullptr);
    if (g_native) g_native->Reset();
    ResetRuns();
    g_delete(c);
}

void NativeReset(lifted::Context& c) {
    if (g_native) g_native->Reset();
    ResetRuns();
    g_reset(c);
}

void NativeGetPicture(lifted::Context& c) {
    const u32 mp = u32(c.r[4]), rgb32 = u32(c.r[5]), mbcount = u32(c.r[6]);
    const u64 ra = c.r[31], sp = c.r[29];
    mpeg::Picture picture;
    bool got = false;
    // As libmpeg: no whole picture, the game's no-data callback (it starts the DMA that feeds the sink) - its
    // result is not looked at (libmpeg's own call site ignores it); the stream's sequence end code ends a movie.
    // Waiting as libmpeg waits: between tries the other threads run (a loop head: Tick) - the game's reader and
    // demux fill the ring its no-data callback hands on (sceMpegAddBs -> channel 4 -> the sink).
    int tries = 0;
    g_ts_context = &c;                      // (NativeOnPicture's: the time stamp callback runs on this thread)
    g_ts_mp = mp;
    for (;; ++tries) {
        if (g_native->Next(&picture)) {
            got = true;
            break;
        }
        if (g_native->ended()) break;
        c.r[4] = mp;
        lifted::Call(c, g_nodata, u32(ra));
        c.r[29] = sp;
        c.r[31] = ra;
        if (g_native->Next(&picture)) {
            got = true;
            break;
        }
        if (g_native->ended()) break;
        // Nothing handed on: asleep until the next interrupt, as libmpeg's wait on the IPU blocks - the game's other
        // threads (its reader, its sound) run, its interrupts come, the clock goes on. (A wait that only gave way
        // to ready threads kept the clock still: ZOE's reader sleeps until a blank's handler wakes it.)
        // PS2HOST_MPEG_WAIT=frame: the whole blank (as until 2026-11-16) - for an A/B of the game's other threads.
        static const bool frame_wait = [] {
            const char* w = std::getenv("PS2HOST_MPEG_WAIT");
            return w && std::strcmp(w, "frame") == 0;
        }();
        if (frame_wait) SleepUntilNextFrame();
        else SleepUntilNextInterrupt();
        static const bool waiting_debug = std::getenv("PS2HOST_MPEG_DEBUG") != nullptr;
        if (waiting_debug && tries % 20000 == 19999)
            std::fprintf(stderr, "[mpeg] GetPicture still waiting: %d no-data calls, %llu bytes in, decoder: %s\n", tries + 1,
                         static_cast<unsigned long long>(g_sunk), g_native->error().c_str());
    }
    static const bool debug = std::getenv("PS2HOST_MPEG_DEBUG") != nullptr;
    if (debug)
        std::fprintf(stderr, "[mpeg] GetPicture(0x%08X, 0x%08X, %u): %s after %d no-data calls; %llu bytes in, %d skipped%s%s\n",
                     unsigned(mp), unsigned(rgb32), unsigned(mbcount), got ? "a picture" : "none", tries,
                     static_cast<unsigned long long>(g_sunk), g_native->errors(), g_native->ended() ? ", ended" : "",
                     g_native->error().empty() ? "" : (" - " + g_native->error()).c_str());
    g_ts_context = nullptr;
    if (got) {
        std::vector<u32> rgba;
        unsigned th0 = 0, th1 = 0;
        IpuThresholds(&th0, &th1);                  // the game's SETTH, as the IPU's CSC takes them
        mpeg::ToRgb32Macroblocks(picture, th0, th1, &rgba);
        const std::size_t bytes = std::min<std::size_t>(rgba.size() * 4, std::size_t(mbcount) * 1024u);
        Memory& m = GuestMemory();
        m.WriteBytes(rgb32, reinterpret_cast<const u8*>(rgba.data()), bytes);
        // sceMpeg's public fields as libmpeg leaves them (libmpeg.h; read off tier E, ZOE's movies): the size, the
        // pictures handed out so far, the picture's time stamps, flags 0xD8 | its type.
        m.Write(mp, u32(picture.width), 4);
        m.Write(mp + 4, u32(picture.height), 4);
        m.Write(mp + 8, u32(g_shown++), 4);
        m.Write(mp + 0x10, u64(picture.pts), 8);
        m.Write(mp + 0x18, u64(picture.dts), 8);
        m.Write(mp + 0x20, u64(0xD8u | u32(picture.type)), 8);
        m.Write(mp + 0x28, ~u64(0), 8);
        m.Write(mp + 0x30, ~u64(0), 8);
        m.Write(mp + 0x38, u64(0), 8);
        c.r[2] = 1;
        LogPictureTime(picture.pts);
    } else {
        // The movie's end: sceMpegIsEnd reads the first word of libmpeg's state (*(mp + 0x40)).
        const u32 state = u32(GuestMemory().Read(mp + 0x40, 4));
        GuestMemory().Write(state, 1, 4);
        c.r[2] = 0;
    }
    c.r[31] = ra;
    c.r[29] = sp;
    static std::FILE* log = std::getenv("PS2HOST_MPEG_LOG") ? std::fopen(std::getenv("PS2HOST_MPEG_LOG"), "w") : nullptr;
    static u64 pictures = 0;
    if (log && got) {
        std::vector<u8> bytes(std::size_t(std::min(mbcount, 4096u)) * 1024u);
        GuestMemory().ReadBytes(rgb32, bytes.data(), bytes.size());
        const char* mbs_text = std::getenv("PS2HOST_MPEG_LOG_MBS");
        const std::size_t mbs = std::min<std::size_t>(mbs_text ? std::size_t(std::atoi(mbs_text)) : mbcount, mbcount);
        std::vector<u8> colour(bytes.begin(), bytes.begin() + std::ptrdiff_t(std::min(bytes.size(), mbs * 1024u)));
        for (std::size_t i = 3; i < colour.size(); i += 4) colour[i] = 0;
        // Raw pictures: PS2HOST_MPEG_LOG_RAW=n the first n (as tier E's log), PS2HOST_MPEG_LOG_RAW_FROM=n 16 from n.
        const char* raw_first = std::getenv("PS2HOST_MPEG_LOG_RAW");
        const char* raw_from = std::getenv("PS2HOST_MPEG_LOG_RAW_FROM");
        if (raw_first || raw_from) {
            const u64 from = raw_from ? u64(std::atoll(raw_from)) : 0;
            const u64 count = raw_first ? u64(std::atoll(raw_first)) : 16;
            if (pictures >= from && pictures < from + count) {
                const std::string name = std::string(std::getenv("PS2HOST_MPEG_LOG")) + "." + std::to_string(pictures) + ".raw";
                if (std::FILE* o = std::fopen(name.c_str(), "wb")) {
                    std::fwrite(bytes.data(), 1, std::min(bytes.size(), mbs * 1024u), o);
                    std::fclose(o);
                }
            }
        }
        Memory& m = GuestMemory();         // the fields as the tier-E log has them, then the native extras
        std::fprintf(log, "%llu %u 1 %08X %08X w %u h %u n %u pts %lld dts %lld fl %llu pts2 %lld dts2 %lld fl2 %llu "
                          "native type %d tref %d\n",
                     static_cast<unsigned long long>(pictures++), unsigned(mbcount),
                     unsigned(Crc32(bytes.data(), bytes.size())), unsigned(Crc32(colour.data(), colour.size())),
                     unsigned(m.Read(mp, 4)), unsigned(m.Read(mp + 4, 4)), unsigned(m.Read(mp + 8, 4)),
                     static_cast<long long>(m.Read(mp + 0x10, 8)), static_cast<long long>(m.Read(mp + 0x18, 8)),
                     static_cast<unsigned long long>(m.Read(mp + 0x20, 8)), static_cast<long long>(m.Read(mp + 0x28, 8)),
                     static_cast<long long>(m.Read(mp + 0x30, 8)), static_cast<unsigned long long>(m.Read(mp + 0x38, 8)),
                     picture.type, picture.temporal_reference);
        std::fflush(log);
    }
}

}  // namespace

void ApplySdkOverrides(const FunctionEntry* table, std::size_t count) {
    // The native movie path needs all of its partners named; otherwise tier E as ever.
    bool native = false;
    // The default since 2026-11-16 (the maintainer's night order, point 1: every movie on the disc complete and
    // correct - docs/option_p/02_design.md 7f); PS2HOST_MPEG_TIER_E=1 goes back to libmpeg + the IPU model, in a
    // build with tier E only (without it there is no IPU model to go back to).
#if PS2HOST_NO_TIER_E
    const bool want_native = true;
#else
    const bool want_native = !std::getenv("PS2HOST_MPEG_TIER_E");
#endif
    if (want_native) {
        u32 get = 0;
        for (std::size_t i = 0; i < count; ++i) {
            if (!table[i].name || !table[i].function) continue;
            const std::string name = table[i].name;
            if (name == "_dispatchMpegCbNodata") g_nodata = table[i].address;
            else if (name == "_dispatchMpegCallback") g_dispatch = table[i].address;
            else if (name == "sceMpegCreate") g_create = table[i].function;
            else if (name == "sceMpegDelete") g_delete = table[i].function;
            else if (name == "sceMpegReset") g_reset = table[i].function;
            else if (name == "sceMpegGetPicture") get = table[i].address;
        }
        if (g_nodata && g_dispatch && g_create && g_delete && g_reset && get) {
            for (std::size_t i = 0; i < count; ++i) {
                if (!table[i].name) continue;
                const std::string name = table[i].name;
                if (name == "sceMpegCreate") Override(table[i].address, NativeCreate, "sceMpegCreate (native movies)");
                if (name == "sceMpegDelete") Override(table[i].address, NativeDelete, "sceMpegDelete (native movies)");
                if (name == "sceMpegReset") Override(table[i].address, NativeReset, "sceMpegReset (native movies)");
            }
            Override(get, NativeGetPicture, "sceMpegGetPicture (native movies)");
            g_get_address = get;
            AddIdleLoop(get);                       // its wait: the virtual clock goes on to the next event
            native = true;                          // (the tier-E log wrapper is not wanted beside it)
        }
        // Only a game that plays movies has libmpeg (none of the names: nothing to say).
        if (native) std::fprintf(stderr, "[sdk] movies: native decoder\n");
        else if (g_nodata || g_dispatch || g_create || g_delete || g_reset || get)
            std::fprintf(stderr, "[sdk] movies: libmpeg's routines are not all named - %s\n",
#if PS2HOST_NO_TIER_E
                         "no movies (this build has no IPU model)");
#else
                         "tier E (libmpeg + the IPU model)");
#endif
    } else {
        std::fprintf(stderr, "[sdk] movies: tier E (PS2HOST_MPEG_TIER_E)\n");
    }
    for (std::size_t i = 0; i < count; ++i) {
        if (!table[i].name) continue;
        if (std::getenv("PS2HOST_MPEG_ADDBS_LOG") && table[i].function && std::strcmp(table[i].name, "sceMpegAddBs") == 0) {
            g_add_bs = table[i].function;
            Override(table[i].address, LoggedAddBs, "sceMpegAddBs, logged (PS2HOST_MPEG_ADDBS_LOG)");
        }
        if (!native && (std::getenv("PS2HOST_MPEG_LOG") || std::getenv("PS2HOST_MPEG_TIMES")) && table[i].function &&
            std::strcmp(table[i].name, "sceMpegGetPicture") == 0) {
            g_get_picture = table[i].function;
            Override(table[i].address, LoggedGetPicture, "sceMpegGetPicture, logged (PS2HOST_MPEG_LOG)");
        }
        for (const SdkRoutine& routine : kRoutines) {
            if (std::strcmp(table[i].name, routine.name) == 0) {
                Override(table[i].address, routine.function, routine.name);
                Note(std::string("sdk ") + routine.name,
                     std::string("SDK routine ") + routine.name + " runs on the host");
            }
        }
    }
}

}  // namespace ps2host
