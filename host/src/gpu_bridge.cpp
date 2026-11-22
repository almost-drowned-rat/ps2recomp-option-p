/*
 * The GPU GS renderer (engine/gpu, engine/docs/GPU.md) driven by the host's GS
 * model, in one of two modes.
 *
 * --gpu-shadow: beside the software GS, and checked against it. Every
 * primitive the software GS draws is also handed to ps2gpu with the state it was
 * drawn with. Textures are decoded from the software GS's local memory (with
 * --gpu-shadow-rtt, a texture that is a GPU target is sampled from the target
 * instead); depth buffers start each frame as local memory has them. At the end
 * of each frame every target drawn into is read back and compared with local
 * memory, and the agreement is reported.
 *
 * --gpu: instead of the software GS. The GS model still takes every GIF packet,
 * register and image transfer, but draws nothing (Gs::rasterize false); the GPU
 * draws, and its images stand for regions of local memory.
 *
 * Local memory is kept twice: the GS model's, which the CPU reads and writes,
 * and a copy on the GPU (GsRenderer::Memory*), which images are swizzled into
 * and out of by a compute shader; per 256-byte block the bridge knows which
 * copy is current. Each GPU image (a target or depth buffer, with the blocks
 * it covers) knows whether the GPU has drawn what memory lacks (gpu_newer) or
 * memory has changed under it (stale). Before the CPU reads or writes local
 * memory - a transfer, a local copy, a CLUT load, a texture decoded, the
 * picture shown - the GPU images over it are swizzled into the GPU's copy and
 * the blocks the CPU lacks come back to it; after it is written, the images
 * there are stale and reload, from the GPU's copy once the CPU's writes have
 * gone over. A target drawn under another width or format than one it
 * overlaps (Burnout 3's reflection buffers, the race's blur passes in its
 * depth buffer's memory), or a depth buffer drawn into as colour, is
 * reconciled the same way - all of it on the GPU, waiting for nothing. An image stands for the
 * rows drawn into so far, not all the scissor allows: Burnout 3's depth buffer
 * starts where its frame buffer's 448 rows end, and a 512-row reach would have
 * each depth write make the frame stale. A texture that is a target is sampled
 * from the target itself. Other textures are decoded into a cache keyed by
 * their registers, TEXA and their CLUT, dropped when memory under them is
 * written or the GPU draws over them.
 *
 * Known difference: the GPU's blender rounds where the GS truncates, so a
 * blended pixel can be 1 brighter (invisible; in shadow runs, Burnout 3's frame
 * copy doubles it into the 16-bit display buffer).
 *
 * Without PS2HOST_VULKAN the options report that they are unavailable.
 */

#include <algorithm>
#include <atomic>
#include <mutex>
#include <utility>
#include <bitset>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#define XXH_INLINE_ALL
#include "xxhash.h"

#include "ps2host/gs.h"
#include "ps2host/profile.h"
#include "ps2host/run.h"
#include "ps2host/timeline.h"
#include "ps2host/settings.h"
#include "ps2host/texture_pack.h"

#if PS2HOST_VULKAN
#include "ps2gpu/display.h"
#include "ps2gpu/gs_renderer.h"
#endif

namespace ps2host {

namespace {
std::vector<u32> g_pipeline_hints;      // SetGpuPipelineHints
}  // namespace

void SetGpuPipelineHints(const u32* keys, std::size_t count) { g_pipeline_hints.assign(keys, keys + count); }

#if PS2HOST_VULKAN

ps2gpu::Gpu* WindowGpu();       // window.cpp: the window's device, when there is one

namespace {

using gs::u32;
using gs::u64;

u32 Bits(u64 value, int low, int count) { return u32((value >> low) & ((1ull << count) - 1)); }

/* Bits a pixel of a format takes in local memory (the 8H/4HL/4HH formats live
   in 32-bit pixels). */
u32 PixelBits(u32 psm) {
    switch (psm) {
        case gs::PSMCT16: case gs::PSMCT16S: case gs::PSMZ16: case gs::PSMZ16S: return 16;
        case gs::PSMT8: return 8;
        case gs::PSMT4: return 4;
        default: return 32;
    }
}

bool Is16(u32 psm) { return psm == gs::PSMCT16 || psm == gs::PSMCT16S || psm == gs::PSMZ16 || psm == gs::PSMZ16S; }

/* The blocks a buffer's first `rows` rows take: whole rows of pages (32 blocks
   each), FBW pages wide. */
std::pair<u32, u32> Blocks(u32 fbp, u32 fbw, u32 psm, u32 rows) {
    const u32 page_rows = Is16(psm) ? 64 : 32;
    const u32 begin = fbp * 32;
    return {begin, begin + std::max(1u, (rows + page_rows - 1) / page_rows) * std::max(1u, fbw) * 32};
}

u32 From16(u32 c) {
    return ((c & 31) << 3) | (((c >> 5) & 31) << 11) | (((c >> 10) & 31) << 19) | ((c & 0x8000) ? 0x80000000u : 0);
}

struct Extent {
    u32 x1 = 0, y1 = 0;             // the furthest pixel drawn, exclusive
};

/* Local memory's 512 pages (8 KB, 32 blocks each), as a set. */
using Pages = std::bitset<512>;

/* A GPU image standing for a region of local memory (--gpu). */
struct Owned {
    u32 fbp = 0, fbw = 1, psm = 0;  // FBP (ZBP) in 2048-word units, FBW, PSM
    bool depth = false;
    u32 width = 0, height = 0;      // the image
    // The blocks it stands for: the rows used so far (drawn into, or sampled),
    // of all it could hold; of those, the ones the GPU has drawn into.
    u32 begin = 0, end = 0, full_end = 0;
    u32 drawn_end = 0;
    // The blocks drawn into since it last went to GPU memory: what another
    // image over them must wait for (not all it was ever drawn over - the
    // race's depth buffer is cleared to 512 rows once a frame, over the glow
    // buffers after its 448, and drawn into below 448 ten thousand times).
    u32 dirty_begin = ~0u, dirty_end = 0;
    u32 x1 = 0, y1 = 0;             // how much of it the GPU has drawn
    bool gpu_newer = false;         // drawn on the GPU, not yet in local memory
    bool stale = true;              // local memory changed since the image was loaded
    bool stale_beyond = false;      // ...in rows past `end`: stale when it grows into them
    int why = 0;                    // what made it stale: 0 new, 1 memory written, 2 a draw over it, 3 another's write-back
    bool by16 = false;              // ...only ever a 16-bit view's drawing (Burnout 3's depth into the frame's alpha)
    // The rows a reload brings back (stale ones), [stale_row0, stale_row1):
    // a reload was the whole image, and the race's blur passes, drawn into
    // the first rows of its depth buffer's memory, had its 640x448 reloaded
    // a thousand times a second for a few rows each.
    u32 stale_row0 = 0, stale_row1 = ~0u;
    // The same by page, as finely as known: what changed under the image
    // since it was loaded, and what it drew since its last write-back. A
    // texture sampled from it needs neither unless the pages it reads are
    // in them (the race's road reads 16 texels of a 1024x256 view over its
    // depth buffer, while drawing Z into other pages of it - 200 reloads,
    // each with the depth buffer's write-back, a picture).
    Pages stale_pages, dirty_pages;
    // The page rectangle last drawn (x0, x1, y0, y1, inclusive), all of it
    // in dirty_pages while `rect_dirty`: most primitives land inside it.
    u32 rect[4] = {};
    bool rect_dirty = false;
    u64 reloads[4] = {};            // reloads, by why
    u64 drawn_serial = 0;           // when the GPU last drew into it (Bridge::serial)
    // ...into each of local memory's first pages (kLowPages): where a texture past the end of memory wraps
    // to (CachedTexture). ZOE's upload slots at 0x3E90 / 0x3F81 / 0x3EDC wrap into the frame buffer's first
    // page; any draw into the frame made them decode again - ~230 decodes a picture, half the GPU's time.
    static constexpr u32 kLowPages = 16;
    u64 low_page_serial[kLowPages] = {};
    u64 checked = 0;                // Bridge::newer_epoch when Use last looked at the others
};

/* A texture decoded into the renderer (--gpu). */
struct Cached {
    u64 renderer_key = 0;
    // A texture pack's replacement (texture_pack.h): drawn instead, or its
    // file still being read (looked for again each time it is drawn); as of
    // the pack's generation (Tab turns it on and off).
    bool replaced = false, pack_pending = false;
    texture_pack::Name pack_name;
    u64 replacement = 0;            // the renderer's image for it (texture_pack::Image::id)
    u64 pack_generation = 0;
    u32 begin = 0, end = 0;
    // Blocks [0, wrap_end) it reads past the end of local memory, where the GS's
    // addresses wrap (0: none) - ZOE's title logo and copyright line, PSMT4 512
    // wide at TBW 6 from 0x3FA9 / 0x3FB6, read the frame buffer's first blocks
    // there; with the range stopping at 16384 they were never sent to the GPU.
    // Only what a draw has sampled there so far (WrapSampled): textures are
    // declared bigger than the picture in them (ZOE's background 3E10 is TH 512
    // for 448 rows), and their whole size reaching past the end had the frame
    // buffer at block 0 read back and decoded again every frame (gameplay CPU
    // 83 -> 144 %, 2026-11-12). `wrap_full` is all it could read there.
    u32 wrap_end = 0;
    u32 wrap_full = 0;
    // The texels [0, dec_w) x [0, dec_h) its image holds as decoded (the part the draws sampled, tier N;
    // all of TW x TH where that is not known, and with PS2HOST_GPU_WHOLE_DECODE=1, tier E). A draw sampling
    // past them decodes it again with the union, its memory unchanged (valid) or the CPU's hash the same.
    u32 dec_w = 0, dec_h = 0;
    u32 dec_wrap_end = 0;           // of [0, wrap_end), the blocks those texels lie in (TextureBlocks)
    bool valid = false;
    bool indexed = false;           // in its pages' lists (Bridge::IndexTexture)
    u64 decoded_serial = 0;         // Bridge::serial when decoded
    u64 content = 0;                // a hash of its blocks when decoded
    bool decoded = false;
    // The images whose blocks it can share (all they could ever cover), as of
    // `owned_seen` images: images are only ever added, and live as long as
    // the bridge.
    std::vector<const Owned*> over;
    std::size_t owned_seen = 0;
    std::list<u64>::iterator lru;
};

struct Bridge {
    std::unique_ptr<ps2gpu::Gpu> gpu;
    std::unique_ptr<ps2gpu::GsRenderer> renderer;
    bool primary = false;
    // --gpu-shadow-rtt: a texture that is a GPU target is sampled from it, as
    // the GPU will have to on its own.
    bool targets_as_textures = false;

    // Shadow: the texture last decoded, by what decides its contents, and the
    // memory drawn into since.
    std::tuple<u64, u64, u64, u64> texture_state{~0ull, 0, 0, 0};
    u64 texture_key = 0;
    std::map<u32, u32> since_decode;  // start -> blocks
    bool drawn_into(u32 bp, u32 blocks) const {
        for (const auto& [start, count] : since_decode)
            if (start < bp + blocks && bp < start + count) return true;
        return false;
    }
    std::map<std::tuple<u32, u32, u32>, Extent> drawn;  // this frame's targets
    u64 frames = 0;
    double agreement_sum = 0;
    std::string dump_dir;           // a dump asked for, made at the next frame with drawing
    std::set<u32> depth_seeded;     // depth buffers loaded from local memory this frame

    // Primary.
    std::map<std::tuple<u32, u32, u32, bool>, Owned> owned;
    std::unordered_map<u64, Cached> textures;
    std::list<u64> texture_lru;     // most recent first
    // The textures over each page (32 blocks) of local memory, for what a
    // write there invalidates: scanning all 2,048 on every write was 3% of a
    // race. Elements of `textures` stay put until erased. Each with its
    // blocks (set before it is indexed, and never after): a write looks
    // through the list without going to each texture - a cache miss each,
    // 4% of the GS thread in splitscreen.
    struct PageTexture {
        u32 begin, end;
        Cached* t;
        bool operator==(const Cached* other) const { return t == other; }
    };
    std::vector<std::vector<PageTexture>> texture_pages = std::vector<std::vector<PageTexture>>(512);
    // Only textures that may be valid are listed (every valid one is): one a
    // write invalidates leaves its pages' lists, and comes back to them when
    // it is decoded again (Validated). Kept listed, the textures the game's
    // upload slots had ever held were each marked invalid again on every
    // write there - 245 of them a write in splitscreen, 0.2 still valid: 4%
    // of the GS thread.
    void InvalidateTextures(u32 begin, u32 end) {
        static std::vector<Cached*> going;
        going.clear();
        for (u32 page = begin >> 5; page <= ((end - 1) >> 5) && page < 512; ++page)
            for (const PageTexture& t : texture_pages[page])
                if (t.begin < end && begin < t.end) {
                    t.t->valid = false;
                    going.push_back(t.t);
                }
        for (Cached* t : going) IndexTexture(t, false);
    }
    void Validated(Cached* t) {
        t->valid = true;
        IndexTexture(t, true);
    }
    void IndexTexture(Cached* t, bool add) {
        if (t->indexed == add) return;
        t->indexed = add;
        auto pages = [&](u32 begin, u32 end) {
            for (u32 page = begin >> 5; page <= ((std::max(end, begin + 1) - 1) >> 5) && page < 512; ++page) {
                std::vector<PageTexture>& list = texture_pages[page];
                if (add) {
                    list.push_back(PageTexture{begin, end, t});
                } else {
                    auto found = std::find(list.begin(), list.end(), t);
                    if (found != list.end()) {
                        *found = list.back();
                        list.pop_back();
                    }
                }
            }
        };
        pages(t->begin, t->end);
        if (t->wrap_end) pages(0, t->wrap_end);
    }
    u64 next_texture = 1;
    u64 serial = 0;                 // counts GPU draws, so a texture knows if one went over it
    // Bumped when an image starts holding drawing memory lacks, or reaches
    // further: until then, an image Use has checked against the others needs
    // no second look (tens of thousands of draws a frame go to one target).
    u64 newer_epoch = 1;
    // Local memory is kept twice: the GS model's (the CPU's) and the
    // renderer's (the GPU's), which images are swizzled into and out of. Per
    // 256-byte block, which copy is current.
    enum : u8 { kSame = 0, kCpuNewer = 1, kGpuNewer = 2 };
    std::vector<u8> blocks = std::vector<u8>(16384, kSame);
    u64 downloads = 0, uploads = 0;  // blocks copied GPU -> CPU, CPU -> GPU
    // Why blocks came back to the CPU (PS2HOST_GPU_LOG): 0 a texture decoded,
    // 1 the GS model reading, 2 the GS model writing.
    int cause = 1;
    u64 downloads_by[3] = {};
    // A transfer's blocks it writes in part where the GPU holds them newer:
    // its pixels there patched into the GPU's memory once it is done
    // (ApplyPatch), not the blocks brought back first (the CPU would wait for
    // every draw before them).
    struct Patch {
        gs::Gs::TouchSeen transfer{};
        std::vector<u32> blocks;            // sorted
    } patch;
    u64 patched_words = 0, patches = 0;
    std::map<u32, u64> texture_downloads;   // by the texture's TEX0 PSM << 16 | TBW
    u64 write_backs = 0, reloads = 0, decodes = 0;
    // GPU decodes (tier N's partial ones among them): count, texels decoded, texels a whole decode would take.
    u64 partial_texels[3] = {};
    u64 unchanged = 0;              // textures invalidated whose texels proved the same
    u64 gpu_decodes = 0;            // textures decoded on the GPU from its copy of local memory
    u64 replacements = 0;           // a texture pack's in their place (texture_pack.h)
    // The CLUT, in rows of 16 entries (bit r for row r): which the GS model
    // holds as loaded (a load from blocks the CPU has), which the GPU's copy
    // holds (loaded from GPU memory, where the GPU drew them, or sent over).
    // Every row is at least one of the two. A GPU load bumps its rows'
    // generations, which key the textures read through them.
    u32 clut_cpu = 0xFFFF, clut_gpu = 0;
    u64 clut_generation[16] = {};
    u64 gpu_clut_loads = 0;
    // PS2HOST_GPU_DUMP_FRAME=N (and PS2HOST_GPU_DUMP_DIR): during frame N, each
    // target as it stood when drawing moved on to another, as a BMP, with a
    // line per pass on stderr - to see which pass of a frame goes wrong.
    u64 frame = 0;
    long long dump_frame = -1, dump_last = -1;    // the frames whose passes are dumped
    bool Dumping() const { return (long long)frame >= dump_frame && (long long)frame <= std::max(dump_frame, dump_last); }
    std::string dump_passes = ".";
    std::tuple<u32, u32, u32> last_target{~0u, 0, 0};
    u32 pass = 0;
};

Bridge* g_bridge = nullptr;

// Bumped by whatever could change what OnDraw works out for a primitive from
// its registers alone - an image's staleness, memory synced, a write-back, a
// CLUT load, a texture decoded or released, an image made or reloaded, a
// draw outside the blocks its target had dirty: while it stands still, a
// primitive with the registers of the one before draws as that one did
// (OnDraw's fast path).
u64 g_mutations = 0;
std::atomic<u64> g_game_pictures{0};      // GamePictures
// The game's pictures' times (steady ns), the last 256, and the longest wait between two per reader (0 the overlay,
// 1 the log) - GamePicturesInLast / TakeLongestGamePictureGap.
std::mutex g_picture_times_lock;
u64 g_picture_times[256] = {};
u64 g_picture_longest[2] = {};
double g_game_picture_rate = 1e9 / 16683333.0;   // SetGamePictureRate (59.94)
// Of those, the CLUT loads' (LoadClut); and why primitives missed the fast path (the summary's "fast path" line):
// 0 none to repeat, 1 only CLUT loads since and the same registers, 2 only CLUT loads and TEX0 differing only in
// its CLUT fields (CBP, CPSM, CSM, CSA, CLD), 3 other mutations, 4 other registers, 5 the rest.
u64 g_clut_mutations = 0;
u64 g_fast_hits = 0, g_fast_misses[6] = {};
// Diagnosis 2: per register index of OnDraw's regs[12] (prim, frame, scissor, tex0, tex1, clamp, alpha, test, zbuf,
// texa, fogcol|pabe, miptbp) how often it differed in an "other registers" miss; misses with one register only;
// misses a ring of the last eight cacheable states would have hit; "the rest" by cause (context, more rows,
// dumping, stale source, partial texture).
u64 g_miss_reg[12] = {}, g_miss_single = 0, g_miss_ring_hit = 0, g_miss_rest[5] = {};
struct FastRingEntry { u64 mutations = ~0ull; const void* context = nullptr; u64 regs[12] = {}; };
FastRingEntry g_fast_ring[8];
unsigned g_fast_ring_next = 0;
// Diagnosis 3: which site made the mutation an "other changes" miss saw last (the summary's "mutation sites" line).
enum MutationSite {
    kMutMarkStale, kMutWriteBack, kMutSync, kMutSyncWrite, kMutUseCheck, kMutDrewOutside, kMutLoadClut,
    kMutReplacement, kMutNewTexture, kMutTextureInvalid, kMutClutRows, kMutEvict, kMutNewerEpoch, kMutSites
};
u64 g_mutation_site_count[kMutSites] = {}, g_miss_site[kMutSites] = {};
int g_last_mutation_site = 0;
inline void Mutate(int site) {
    ++g_mutations;
    ++g_mutation_site_count[site];
    g_last_mutation_site = site;
}

/* Whether the CLUT has rows the GPU's copy lacks. */
bool b_clut_rows_cpu_only(const Bridge& b) { return (b.clut_cpu & ~b.clut_gpu) != 0; }
std::map<std::string, u64> g_decode_kinds;      // PS2HOST_GPU_COUNT

// ------------------------------------------------------------------ --gpu

}  // namespace

u64 GpuBusyMicroseconds() { return ps2gpu::GpuBusyNanoseconds() / 1000; }
std::string GpuProfileText() { return ps2gpu::GpuProfileReport(); }

/* SetPresentAtBlankEnd's (the GS thread's): a frame's picture waits, from
   its end in the stream, for the stream to move past the displayed buffer -
   the first draw elsewhere, or the next frame's end. */
bool g_present_at_blank_end = false;
bool g_present_waits = false;
u64 g_present_frame = 0;
void PresentWaiting();

namespace {

/* PS2HOST_GPU_BLOCK_TRACE=<first>-<last>[@<frame>-<frame>] (blocks hex, frames
   decimal): every step of the bridge's over those blocks - the CPU's writes and
   reads (with the transfer), write-backs, blocks sent either way, image reloads
   - one line each on stderr, 4,000 at most. Off by default (M3 2026-11-12: ZOE's
   title logo, whose texture wraps past the end of memory into the frame buffer's
   first blocks, drawn wrong under --gpu). */
struct BlockTrace {
    bool on = false;
    u32 lo = 0, hi = 0;
    unsigned long long f0 = 0, f1 = ~0ull;
    int lines = 0;
};
BlockTrace& Trace() {
    static BlockTrace t = [] {
        BlockTrace made;
        if (const char* e = std::getenv("PS2HOST_GPU_BLOCK_TRACE")) {
            unsigned lo = 0, hi = 0;
            if (std::sscanf(e, "%x-%x@%llu-%llu", &lo, &hi, &made.f0, &made.f1) >= 2) {
                made.on = true;
                made.lo = lo;
                made.hi = hi;
            }
        }
        return made;
    }();
    return t;
}
void TraceBlocks(const Bridge& b, u32 begin, u32 end, const char* format, ...) {
    BlockTrace& t = Trace();
    if (!t.on || b.frame < t.f0 || b.frame > t.f1 || end <= t.lo || begin > t.hi || t.lines >= 4000) return;
    ++t.lines;
    std::fprintf(stderr, "[blocks] f%llu p%llu ", (unsigned long long)b.frame,
                 (unsigned long long)gs::TheGs().primitives());
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
    std::fputc('\n', stderr);
}

/* Blocks [begin, end) whose copy on one side is behind the other's brought up
   to date, in runs: `to_gpu` the GPU's from the CPU's, else the CPU's from the
   GPU's (which waits for the GPU). */
void Coherent(Bridge& b, u32 begin, u32 end, bool to_gpu) {
    end = std::min(end, 16384u);
    const u8 behind = to_gpu ? Bridge::kCpuNewer : Bridge::kGpuNewer;
    u8* bytes = gs::TheGs().memory.data();
    const u8* blocks = b.blocks.data();
    for (u32 i = begin; i < end;) {
        // To the next block behind (memchr, sixteen at a time and more: a
        // reload looks over all of an image's blocks, ~300 times a picture,
        // and finds none behind, or few).
        const void* found = std::memchr(blocks + i, behind, end - i);
        if (!found) break;
        i = u32(static_cast<const u8*>(found) - blocks);
        u32 j = i;
        while (j < end && b.blocks[j] == behind) b.blocks[j++] = Bridge::kSame;
        TraceBlocks(b, i, j, "blocks %X-%X %s", unsigned(i), unsigned(j), to_gpu ? "sent to the GPU" : "back to the CPU");
        if (to_gpu) {
            b.renderer->MemoryWrite(i, j, bytes + std::size_t(i) * 256);
            b.uploads += j - i;
        } else {
            // The CPU waits for the GPU here: on the profiler's timeline, by why.
            static const char* const kWhy[3] = {"GPU to CPU: a texture", "GPU to CPU: a read", "GPU to CPU: a write"};
            const u64 from = timeline::On() ? timeline::Now() : 0;
            b.renderer->MemoryRead(i, j, bytes + std::size_t(i) * 256);
            if (from) timeline::Span(kWhy[b.cause], from, timeline::Now());
            static const bool log_writes = std::getenv("PS2HOST_GPU_WRITE_LOG") != nullptr;
            if (log_writes && b.cause == 2) {
                const auto& t = gs::TheGs().last_transfer;
                std::fprintf(stderr, "[gpu] blocks %X-%X back to the CPU for a write: bp %X bw %u psm %02X at %u,%u %ux%u\n",
                             unsigned(i), unsigned(j), unsigned(t.bp), unsigned(t.bw), unsigned(t.psm), unsigned(t.x),
                             unsigned(t.y), unsigned(t.w), unsigned(t.h));
                for (const auto& [key, o] : b.owned)
                    if (o.begin <= i && i < o.full_end)
                        std::fprintf(stderr, "[gpu]   over image %s %X/%u/%02X %ux%u blocks %X-%X (to %X), dirty %X-%X\n",
                                     o.depth ? "Z" : "C", unsigned(o.fbp), unsigned(o.fbw), unsigned(o.psm),
                                     unsigned(o.width), unsigned(o.height), unsigned(o.begin), unsigned(o.end),
                                     unsigned(o.full_end), unsigned(o.dirty_begin), unsigned(o.dirty_end));
            }
            b.downloads += j - i;
            b.downloads_by[b.cause] += j - i;
        }
        i = j;
    }
}

/* `o` stale for `why`, in the rows over blocks [b0, b1) (all of it, past its
   blocks or by default). */
void MarkStale(Owned& o, int why, u32 b0 = 0, u32 b1 = ~0u, const Pages* pages = nullptr, bool by16 = false) {
    Mutate(kMutMarkStale);
    if (pages) {
        o.stale_pages |= *pages;
    } else if (b1 != ~0u && b0 < b1) {
        for (u32 p = b0 / 32; p <= (b1 - 1) / 32 && p < 512; ++p) o.stale_pages.set(p);
    } else {
        o.stale_pages.set();
    }
    u32 r0 = 0, r1 = ~0u;
    if (b1 != ~0u && b0 < b1 && b1 > o.begin) {
        const u32 page_rows = Is16(o.psm) ? 64 : 32, row_blocks = std::max(1u, o.fbw) * 32;
        r0 = (std::max(b0, o.begin) - o.begin) / row_blocks * page_rows;
        r1 = (b1 - o.begin + row_blocks - 1) / row_blocks * page_rows;
    }
    o.by16 = (o.stale ? o.by16 : true) && by16;
    if (!o.stale) {
        o.stale = true;
        o.why = why;
        o.stale_row0 = r0;
        o.stale_row1 = r1;
    } else {
        o.stale_row0 = std::min(o.stale_row0, r0);
        o.stale_row1 = std::max(o.stale_row1, r1);
    }
}

/* A step of the bridge's on the profiler's timeline (PS2HOST_PROFILE), by name. */
struct TimelineStep {
    const char* name;
    u64 from;
    explicit TimelineStep(const char* what) : name(what), from(timeline::On() ? timeline::Now() : 0) {}
    ~TimelineStep() {
        if (from) timeline::Span(name, from, timeline::Now());
    }
};

/* The columns [first, end) of image `o` that `pages` reach (a page is 64
   pixels wide in every format), within [0, limit), as PagesOf numbers
   them; all of them where not known (none; an image reaching past the end
   of local memory, whose pages there PagesOf leaves out; a page before it). */
void ColumnsOf(const Owned& o, const Pages& pages, u32 limit, u32* first, u32* end) {
    *first = 0;
    *end = limit;
    const u32 fbw = std::max(1u, o.fbw), base = o.begin / 32;
    u32 lo = ~0u, hi = 0;
    if (pages.none() || o.full_end > 16384) return;
    for (u32 p = 0; p < 512; ++p) {
        if (!pages.test(p)) continue;
        if (p < base) return;
        const u32 col = (p - base) % fbw;
        lo = std::min(lo, col);
        hi = std::max(hi, col);
    }
    *first = std::min(lo * 64, limit);
    *end = std::min((hi + 1) * 64, limit);
}

/* What the GPU drew into an image, swizzled into the GPU's local memory - on
   the GPU, nothing waiting. */
void WriteBack(Bridge& b, Owned& o) {
    Mutate(kMutWriteBack);
    Timed timed(Section::GpuSync);
    const TimelineStep step("sync: a write-back");
    o.gpu_newer = false;
    // Only the columns of the pages drawn since (a page is 64 pixels wide in
    // every format): the fog's triangles in splitscreen's lower view, a few
    // columns each, wrote back the depth buffer's whole width ~110 times a
    // frame.
    u32 first_col = 0, end_col = o.x1;
    ColumnsOf(o, o.dirty_pages, o.x1, &first_col, &end_col);
    o.dirty_pages.reset();
    o.rect_dirty = false;
    if (o.dirty_end <= o.dirty_begin) return;
    // Only the rows drawn into since the last write-back: the image's other
    // rows may be stale - another image drew over them since, and memory has
    // that, not this.
    const u32 page_rows = Is16(o.psm) ? 64 : 32, row_blocks = std::max(1u, o.fbw) * 32;
    const u32 first = (o.dirty_begin - o.begin) / row_blocks * page_rows;
    const u32 last = std::min(o.y1, (o.dirty_end - o.begin) / row_blocks * page_rows);
    TraceBlocks(b, o.dirty_begin, o.dirty_end, "write-back %s %X/%u/%02X dirty %X-%X rows %u-%u cols %u-%u",
                o.depth ? "Z" : "C", unsigned(o.fbp), unsigned(o.fbw), unsigned(o.psm), unsigned(o.dirty_begin),
                unsigned(o.dirty_end), unsigned(first), unsigned(last), unsigned(first_col), unsigned(end_col));
    // Blocks the CPU wrote go over first: the image covers them only in part.
    Coherent(b, o.dirty_begin, o.dirty_end, true);
    if (gs::TheGs().trace)   // in a capture's trace: an image written back to memory, which rows
        std::fprintf(gs::TheGs().trace_out, "[gpu] write-back %s %X/%u/%02X rows %u-%u cols %u\n", o.depth ? "Z" : "C",
                     unsigned(o.fbp), unsigned(o.fbw), unsigned(o.psm), unsigned(first), unsigned(last),
                     unsigned(o.x1));
    const bool done = o.depth ? b.renderer->DepthToMemory(o.fbp, o.width, o.height, o.fbw, o.psm, end_col, last, first,
                                                          first_col)
                              : b.renderer->TargetToMemory(o.fbp, o.fbw, o.psm, end_col, last, first, first_col);
    if (!done && gs::TheGs().trace) std::fprintf(gs::TheGs().trace_out, "[gpu]   (nothing to write back)\n");
    if (done) {
        ++b.write_backs;
        std::fill(b.blocks.begin() + std::min(o.dirty_begin, 16384u),
                  b.blocks.begin() + std::min(o.dirty_end, 16384u), Bridge::kGpuNewer);
    }
    o.dirty_begin = ~0u;
    o.dirty_end = 0;
}

/* Before the CPU reads or writes local memory [begin, end): what the GPU drew
   there comes back to it; written, the GPU images there must reload. */
void ApplyPatch(Bridge& b);

void Sync(Bridge& b, u32 begin, u32 end, bool write, const Owned* except = nullptr) {
    // A mutation only where something changes: a write, a patch applied, blocks brought back from the GPU
    // (WriteBack counts its own). A read that finds nothing to do - a CLUT load's, most of them - leaves the
    // fast path standing (ZOE's boss fight, 2026-11-22: ~5 M of 7 M fast-path misses came right after one).
    if (write || !b.patch.blocks.empty()) Mutate(kMutSync);
    {
        const auto& t = gs::TheGs().last_transfer;
        TraceBlocks(b, begin, end, "CPU %s %X-%X (last transfer: bp %X bw %u psm %02X at %u,%u %ux%u)",
                    write ? "writes" : "reads", unsigned(begin), unsigned(end), unsigned(t.bp), unsigned(t.bw),
                    unsigned(t.psm), unsigned(t.x), unsigned(t.y), unsigned(t.w), unsigned(t.h));
    }
    ApplyPatch(b);
    for (auto& [key, o] : b.owned)
        if (&o != except && o.gpu_newer && o.dirty_begin < end && begin < o.dirty_end) WriteBack(b, o);
    // Timed only with a block to bring back (2026-11-19: most syncs - a CLUT load's 8 blocks - have none, and the
    // two clock reads were ~1 % of the GS thread in the hangar).
    if (const u32 e = std::min(end, 16384u);
        begin < e && std::memchr(b.blocks.data() + begin, Bridge::kGpuNewer, e - begin)) {
        if (!write) Mutate(kMutSync);
        Timed timed(Section::GpuSync);
        const TimelineStep step("sync: memory for the CPU");
        Coherent(b, begin, end, false);
    }
    if (write) std::fill(b.blocks.begin() + begin, b.blocks.begin() + std::min(end, 16384u), Bridge::kCpuNewer);
    for (auto& [key, o] : b.owned) {
        if (!write) break;                  // (a read - a CLUT load's - stales nothing: not every image looked at)
        if (&o == except) continue;
        if (end <= o.begin || o.full_end <= begin) continue;
        if (begin < o.end) {
            MarkStale(o, 1, begin, end);
        } else {
            o.stale_beyond = true;
        }
    }
    if (write && begin < end) b.InvalidateTextures(begin, end);
}

/* An image transfer about to write blocks [begin, end) (Gs::on_write): the
   blocks it writes in part come back from the GPU first, those it writes
   whole need nothing of what was there - a read-back waits for all the GPU
   has queued, and a race's texture uploads into blocks the GPU drew over
   cost a wait each (1,400 of ~6 ms in a minute of racing, 4 KB each). */
void SyncWrite(Bridge& b, u32 begin, u32 end, const u8* cover) {
    Mutate(kMutSyncWrite);
    {
        const auto& t = gs::TheGs().last_transfer;
        TraceBlocks(b, begin, std::min(end, 16384u), "CPU writes (exact) %X-%X (transfer: bp %X bw %u psm %02X at %u,%u %ux%u)",
                    unsigned(begin), unsigned(end), unsigned(t.bp), unsigned(t.bw), unsigned(t.psm), unsigned(t.x),
                    unsigned(t.y), unsigned(t.w), unsigned(t.h));
    }
    ApplyPatch(b);                          // one before it, if it never finished - once for the whole transfer
    b.patch.transfer = gs::TheGs().last_transfer;
    // A transfer past the end of local memory goes on from block 0 (Gs::Touch: `end` past 16384, the cover of
    // block i - begin either way): its two parts, each as one range. ZOE's font and HUD uploads at 0x3F9D-0x3FF7
    // wrap into the frame buffer's first blocks; as a coarse Sync each brought those back from the GPU, a wait
    // for all it had queued (~22 % of the GS thread in a fight, 2026-11-13).
    const auto part = [&](u32 from, u32 to, const u8* c) {
        u32 first = to, last = from;        // the blocks written
        for (u32 i = from; i < to; ++i)
            if (c[i - from]) {
                first = std::min(first, i);
                last = i + 1;
            }
        if (first >= last) return;
        // What the GPU images drew over them into the GPU's memory first (on the
        // GPU, nothing waiting): written back later, it would land over these.
        for (auto& [key, o] : b.owned)
            if (o.gpu_newer && o.dirty_begin < last && first < o.dirty_end) WriteBack(b, o);
        // Blocks written in part where the GPU's are newer: the transfer's pixels
        // patched into the GPU's when it is done, the blocks still the GPU's.
        for (u32 i = first; i < last; ++i)
            if (c[i - from] == 1 && b.blocks[i] == Bridge::kGpuNewer) b.patch.blocks.push_back(i);
        for (u32 i = first; i < last; ++i)
            if (c[i - from] && !(c[i - from] == 1 && b.blocks[i] == Bridge::kGpuNewer)) b.blocks[i] = Bridge::kCpuNewer;
        for (auto& [key, o] : b.owned) {
            if (last <= o.begin || o.full_end <= first) continue;
            if (first < o.end) MarkStale(o, 1, first, last);
            else o.stale_beyond = true;
        }
        b.InvalidateTextures(first, last);
    };
    part(begin, std::min(end, 16384u), cover);
    if (end > 16384) part(0, end - 16384, cover + (16384 - begin));
}

/* The pending patch (Bridge::Patch): each pixel of the transfer's rectangle
   that lies in a block it waits for, its bytes in its word (the CPU's copy
   has them, the transfer written) under the pixel's mask - 32, 24 or 16
   bits, a byte, a nibble - into the GPU's memory. */
void ApplyPatch(Bridge& b) {
    if (b.patch.blocks.empty()) return;
    const gs::Gs::TouchSeen t = b.patch.transfer;
    std::vector<u32> blocks;
    blocks.swap(b.patch.blocks);
    std::sort(blocks.begin(), blocks.end());
    blocks.erase(std::unique(blocks.begin(), blocks.end()), blocks.end());
    u32 bytes = 0, base_mask = 0;
    switch (t.psm) {
        case 0x00: case 0x30: bytes = 4; base_mask = 0xFFFFFFFFu; break;          // CT32, Z32
        case 0x01: case 0x31: bytes = 3; base_mask = 0x00FFFFFFu; break;          // CT24, Z24
        case 0x02: case 0x0A: case 0x32: case 0x3A: bytes = 2; base_mask = 0xFFFFu; break;
        case 0x13: bytes = 1; base_mask = 0xFFu; break;                           // T8
        case 0x14: bytes = 0; base_mask = 0xFu; break;                            // T4
        default: return;                    // (Touch sends no others here)
    }
    const u8* memory = gs::TheGs().memory.data();
    std::map<u32, u32> masks;               // word -> its mask
    for (u32 y = t.y; y < t.y + t.h; ++y)
        for (u32 x = t.x; x < t.x + t.w; ++x) {
            u32 nibble = 0;
            const u32 address = gs::Memory::Address(t.bp, t.bw, x % 2048, y % 2048, t.psm, &nibble);
            if (!std::binary_search(blocks.begin(), blocks.end(), address / 256)) continue;
            const u32 shift = bytes ? (address % 4) * 8 : (address % 4) * 8 + 4 * nibble;
            masks[address / 4] |= base_mask << shift;
        }
    std::vector<u32> words;
    words.reserve(masks.size() * 3);
    for (const auto& [word, mask] : masks) {
        u32 value;
        std::memcpy(&value, memory + std::size_t(word) * 4, 4);
        words.insert(words.end(), {word, value, mask});
    }
    b.renderer->MemoryPatch(words.data(), masks.size());
    b.patched_words += masks.size();
    ++b.patches;
}

/* A target or depth buffer about to be drawn into or sampled: up to date with
   memory and whatever else the GPU drew over its blocks. */
Owned& Use(Bridge& b, u32 fbp, u32 fbw, u32 psm, bool depth, u32 width, u32 height, u32 rows) {
    // Tens of thousands of draws a frame go to the same target as the one
    // before: that one, without the map (its elements stay put).
    static Owned* last = nullptr;
    static std::tuple<u32, u32, u32, bool> last_key;
    // A depth buffer's Z32 and Z24 over the same pages are one (agent L, 2026-11-21): the same 32-bit layout, the
    // renderer's one depth image for both (depths_ by ZBP and size). Kept apart, each switch of the format wrote one
    // back and reloaded the other - ZOE's hangar takes turns between the two ~7 times a frame (11 write-backs and 19
    // render passes a frame: on a tiling GPU, the Odin 2's Adreno, a third of its frame). Written back in the format
    // last drawn with (Z24's leaves the top byte as it is).
    const bool z_alias = depth && (psm == gs::PSMZ24 || psm == gs::PSMZ32);
    const auto key = std::make_tuple(fbp, fbw, z_alias ? u32(gs::PSMZ32) : psm, depth);
    if (!last || key != last_key) {
        last = &b.owned[key];
        last_key = key;
    }
    Owned& o = *last;
    if (z_alias && o.width) o.psm = psm;
    if (!o.width) {
        o.fbp = fbp;
        o.fbw = fbw;
        o.psm = psm;
        o.depth = depth;
        o.width = width;
        o.height = height;
        std::tie(o.begin, o.end) = Blocks(fbp, fbw, psm, rows);
        o.full_end = Blocks(fbp, fbw, psm, height).second;
        o.drawn_end = o.begin;
        o.stale_pages.set();
    } else {
        const u32 end = Blocks(fbp, fbw, psm, rows).second;
        if (end > o.end) {
            o.end = end;
            o.checked = 0;
            if (o.stale_beyond) MarkStale(o, 1);     // rows drawn over before it reached them (stale or not)
        }
    }
    if (!o.stale && o.checked == b.newer_epoch) return o;
    Mutate(kMutUseCheck);
    // Another image's drawing over these blocks reaches this one through memory.
    for (auto& [key, other] : b.owned)
        if (&other != &o && other.gpu_newer && other.dirty_begin < o.end && o.begin < other.dirty_end) {
            const u32 b0 = other.dirty_begin, b1 = other.dirty_end;
            WriteBack(b, other);
            MarkStale(o, 3, b0, b1, nullptr, Is16(other.psm));
        }
    o.checked = b.newer_epoch;
    if (o.stale) {
        // Loaded from the GPU's local memory, the CPU's writes there first -
        // and its own drawing not yet there: an image goes stale for rows
        // another drew over while newer than memory in rows of its own.
        if (o.gpu_newer) WriteBack(b, o);
        Timed timed(Section::GpuSync);
        const TimelineStep step("sync: an image reloaded");
        ++b.reloads;
        Coherent(b, o.begin, o.full_end, true);
        const u32 r0 = std::min(o.stale_row0, o.height), r1 = std::min(o.stale_row1, o.height);
        TraceBlocks(b, o.begin, o.full_end, "reload %s %X/%u/%02X why %d rows %u-%u", depth ? "Z" : "C",
                    unsigned(o.fbp), unsigned(o.fbw), unsigned(o.psm), o.why, unsigned(r0), unsigned(r1));
        if (gs::TheGs().trace)   // in a capture's trace: an image brought back from memory, and why (Owned::why)
            std::fprintf(gs::TheGs().trace_out, "[gpu] reload %s %X/%u/%02X why %d rows %u-%u\n", depth ? "Z" : "C",
                         unsigned(o.fbp), unsigned(o.fbw), unsigned(o.psm), o.why, unsigned(r0), unsigned(r1));
        // A 32-bit image stale only from a 16-bit view's drawing over it (the
        // race's depth into the frame's alpha): its own colour kept where only
        // alpha changed (ReloadKeepsColour) - not a picture as memory holds it.
        b.renderer->ReloadKeepsColour(o.by16 && !Is16(o.psm));
        // Depth buffers come back whole: the renderer keeps one a target
        // size over the same memory, and a part of one reloaded left the
        // race's garage a few pixels off the software GS.
        if ((r0 == 0 && r1 >= o.height) || depth) {
            if (depth) b.renderer->DepthFromMemory(o.fbp, o.width, o.height, o.fbw, o.psm);
            else b.renderer->TargetFromMemory(o.fbp, o.fbw, o.psm, o.width, o.height);
        } else if (r0 < r1) {
            if (depth) b.renderer->DepthRowsFromMemory(o.fbp, o.width, o.height, o.fbw, o.psm, r0, r1 - r0);
            else {
                // The stale pages' columns alone (the image is memory's
                // elsewhere: written back first, above): the fog's own
                // texture view, a 1024-wide one, came back whole for a few
                // columns of drawing ~110 times a splitscreen frame.
                u32 c0 = 0, c1 = o.width;
                ColumnsOf(o, o.stale_pages, o.width, &c0, &c1);
                b.renderer->TargetRowsFromMemory(o.fbp, o.fbw, o.psm, o.width, o.height, r0, r1 - r0, c0, c1);
            }
        }
        b.renderer->ReloadKeepsColour(false);
        o.stale_row0 = 0;
        o.stale_row1 = ~0u;
        o.stale_pages.reset();
        ++o.reloads[o.why];
        o.stale = o.stale_beyond = false;
    }
    return o;
}

/* The pages of `o` under its pixels [x0, x1) x [y0, y1) (64x32 a page, 64x64
   16-bit). */
Pages PagesOf(const Owned& o, u32 x0, u32 x1, u32 y0, u32 y1) {
    Pages pages;
    const u32 shift = Is16(o.psm) ? 6 : 5, fbw = std::max(1u, o.fbw);       // a page's rows: 64 or 32
    x1 = std::min(x1, fbw * 64);
    x0 = std::min(x0, x1 ? x1 - 1 : 0);
    x1 = std::max(x1, x0 + 1);
    y1 = std::max(y1, y0 + 1);
    for (u32 py = y0 >> shift; py <= (y1 - 1) >> shift; ++py)
        for (u32 px = x0 / 64; px <= (x1 - 1) / 64; ++px) {
            const u32 page = o.begin / 32 + py * fbw + px;
            if (page < 512) pages.set(page);
        }
    return pages;
}

/* After the GPU drew into `o`, pixels [x0, x1) x [y0, y1): memory and the
   other images over those blocks are behind. */
void Drew(Bridge& b, Owned& o, u32 x0, u32 x1, u32 y1, u32 y0) {
    const u32 shift = Is16(o.psm) ? 6 : 5, fbw = std::max(1u, o.fbw), row_blocks = fbw * 32;    // a page's rows: 64 or 32
    y1 = std::min(std::max(y1, y0 + 1), o.height);
    y0 = std::min(y0, y1 - 1);
    // Already newer than memory over these pages: the others over them were
    // made stale then (any reloaded since wrote this one back first).
    const u32 cx1 = std::min(x1, fbw * 64);
    const u32 cx0 = std::min(x0, cx1 ? cx1 - 1 : 0);
    const u32 r[4] = {cx0 / 64, (std::max(cx1, cx0 + 1) - 1) / 64, y0 >> shift, (y1 - 1) >> shift};
    const bool in_rect = o.rect_dirty && r[0] >= o.rect[0] && r[1] <= o.rect[1] && r[2] >= o.rect[2] && r[3] <= o.rect[3];
    // Inside the rectangle the last draw outside it marked (most primitives):
    // newer than memory there already, its blocks and pages dirty, the rows
    // it reaches drawn - only the pixels drawn so far and the serial move
    // (as below, the rest unchanged; a write-back clears the rectangle).
    // Memory's first pages it reaches (Owned::low_page_serial), by the page rectangle.
    const auto low_pages = [&](u64 serial) {
        const u32 base = o.begin / 32;
        if (base >= Owned::kLowPages) return;
        for (u32 py = r[2]; py <= r[3]; ++py) {
            const u32 row = base + py * fbw;
            if (row >= Owned::kLowPages) break;
            for (u32 px = r[0]; px <= r[1] && row + px < Owned::kLowPages; ++px) o.low_page_serial[row + px] = serial;
        }
    };
    if (in_rect) {
        o.x1 = std::max(o.x1, std::min(x1, o.width));
        o.y1 = std::max(o.y1, y1);
        o.drawn_serial = ++b.serial;
        low_pages(o.drawn_serial);
        return;
    }
    const u32 db = o.begin + r[2] * row_blocks;
    const u32 de = o.begin + (r[3] + 1) * row_blocks;
    const Pages pages = PagesOf(o, x0, x1, y0, y1);
    const bool inside = o.gpu_newer && db >= o.dirty_begin && de <= o.dirty_end && (pages & ~o.dirty_pages).none();
    o.gpu_newer = true;
    o.x1 = std::max(o.x1, std::min(x1, o.width));
    o.y1 = std::max(o.y1, y1);
    o.drawn_end = std::max(o.drawn_end, Blocks(o.fbp, o.fbw, o.psm, o.y1).second);
    o.end = std::max(o.end, o.drawn_end);
    o.dirty_begin = std::min(o.dirty_begin, db);
    o.dirty_end = std::max(o.dirty_end, de);
    o.dirty_pages |= pages;
    std::memcpy(o.rect, r, sizeof r);
    o.rect_dirty = true;
    o.drawn_serial = ++b.serial;    // textures over it decoded before this are out of date
    low_pages(o.drawn_serial);
    if (inside) return;
    Mutate(kMutDrewOutside);
    ++b.newer_epoch;
    for (auto& [key, other] : b.owned) {
        if (&other == &o || de <= other.begin || other.full_end <= db) continue;
        if (db < other.end) {
            MarkStale(other, 2, db, de, &pages, Is16(o.psm));
        } else {
            other.stale_beyond = true;
        }
    }
}

/* The pages of target `o` a primitive sampling it as a TW x TH texture can
   read, by its texture coordinates (a texel more each side, for bilinear
   filtering); false where that is not known - the region modes, a texture
   wider than the buffer, a vertex with Q not above 0. Perspective-correct
   coordinates inside a triangle are weighted means of its vertices' S/Q and
   T/Q (weights the barycentrics times Q, all positive), so they lie between
   the vertices' least and greatest. */
bool SampledPages(const Owned& o, u64 clamp, u32 tw, u32 th, const ps2gpu::GsVertex* v, std::size_t n, Pages* out) {
    const u32 wms = u32(clamp & 3), wmt = u32(clamp >> 2 & 3);
    if (wms > 1 || wmt > 1 || tw > std::max(1u, o.fbw) * 64 || !n) return false;
    float s0 = 0, s1 = 0, t0 = 0, t1 = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (!(v[i].q > 0)) return false;
        const float s = v[i].s / v[i].q, t = v[i].t / v[i].q;
        if (!std::isfinite(s) || !std::isfinite(t)) return false;
        if (i == 0 || s < s0) s0 = s;
        if (i == 0 || s > s1) s1 = s;
        if (i == 0 || t < t0) t0 = t;
        if (i == 0 || t > t1) t1 = t;
    }
    // Texels [lo, hi) of `size`: the whole span where REPEAT would wrap.
    const auto span = [](float c0, float c1, u32 size, u32 mode, u32* lo, u32* hi) {
        const float a = std::floor(c0 * float(size)) - 1, b = std::ceil(c1 * float(size)) + 1;
        if (mode == 0 && (a < 0 || b > float(size))) {
            *lo = 0;
            *hi = size;
            return;
        }
        *lo = u32(std::clamp(a, 0.0f, float(size - 1)));
        *hi = u32(std::clamp(b, float(*lo + 1), float(size)));
    };
    u32 x0, x1, y0, y1;
    span(s0, s1, tw, wms, &x0, &x1);
    span(t0, t1, th, wmt, &y0, &y1);
    *out = PagesOf(o, x0, x1, y0, y1);
    return true;
}

/* Target `o` about to be sampled as a texture, over `pages`: brought up to
   date (Use) only if what it lacks lies there - changed under it since it
   was loaded, or drawn by another image the GPU has not written back. */
void Sample(Bridge& b, Owned& o, const Pages& pages) {
    bool needed = o.stale && (o.stale_pages & pages).any();
    for (auto it = b.owned.begin(); !needed && it != b.owned.end(); ++it)
        needed = &it->second != &o && it->second.gpu_newer && (it->second.dirty_pages & pages).any();
    if (needed) Use(b, o.fbp, o.fbw, o.psm, false, o.width, o.height, 1);   // sampling grows nothing
}

/* A 64-bit hash of `size` bytes (a multiple of 8). Four independent lanes,
   folded at the end: one chain of multiplies waits on each before it, and
   textures are hashed hundreds of thousands of times a run (only compared
   within a run, so the values themselves are free to change). */
u64 Hash(const unsigned char* data, std::size_t size) {
    // XXH3 (as texture_pack.cpp has it): a re-uploaded texture's blocks are
    // hashed each time it is used again (Burnout 3 sends its textures every
    // picture) - several times as fast as the four-lane hash it replaced.
    return XXH3_64bits(data, size);
}

bool GpuHolds(const Bridge& b, u32 begin, u32 end);

bool Palettized(u32 psm) {
    return psm == gs::PSMT8 || psm == gs::PSMT4 || psm == gs::PSMT8H || psm == gs::PSMT4HL || psm == gs::PSMT4HH;
}

/* The CLUT rows a palettized texture reads: all 16 for 8-bit, CSA's for 4-bit. */
u32 ClutRowsOf(u32 psm, u32 csa) {
    if (!Palettized(psm)) return 0;
    return (psm == gs::PSMT8 || psm == gs::PSMT8H) ? 0xFFFFu : 1u << (csa % 16);
}

/* The CLUT rows a palettized texture reads, as a hash: their entries where
   the GS model holds them, their generation where only the GPU does. */
u64 ClutHash(const Bridge& b, u32 psm, u32 csa) {
    const u32 rows = ClutRowsOf(psm, csa);
    if (!rows) return 0;
    // Hashed once a change of the CLUT: hundreds of thousands of palettized draws a frame.
    static u64 changes = ~0ull, row_hash[16];
    if (gs::TheGs().clut_changes() != changes) {
        changes = gs::TheGs().clut_changes();
        const auto& clut = gs::TheGs().Clut();
        for (u32 r = 0; r < 16; ++r) {
            u64 hash = 1469598103934665603ull;
            for (u32 i = 0; i < 16; ++i) hash = (hash ^ clut[r * 16 + i]) * 1099511628211ull;
            row_hash[r] = hash;
        }
    }
    u64 hash = 0x9E3779B97F4A7C15ull;
    for (u32 r = 0; r < 16; ++r) {
        if (!(rows >> r & 1)) continue;
        const u64 row = (b.clut_cpu >> r & 1) ? row_hash[r] : ~(b.clut_generation[r] * 0xC2B2AE3D27D4EB4Full);
        hash = (hash ^ row ^ r) * 0xFF51AFD7ED558CCDull;
    }
    return hash;
}

/* A CLUT load (on_clut_load): from the GPU's copy of local memory when the
   GPU holds its blocks newer - the race's crash and glow scenes load CLUTs
   from images just drawn, and bringing those back waited ~8 ms each - else
   left to the GS model. */
bool LoadClut(Bridge& b, u64 tex0, u64 texa) {
    Mutate(kMutLoadClut);
    ++g_clut_mutations;
    const u32 psm = Bits(tex0, 20, 6), cbp = Bits(tex0, 37, 14), cpsm = Bits(tex0, 51, 4);
    // CSM2 (bit 55) loads from TEXCLUT's row, at entry 0 whatever CSA: the GS
    // model's load does it (the GPU's knows CSM1 only).
    const bool csm2 = Bits(tex0, 55, 1);
    const u32 csa = csm2 ? 0 : Bits(tex0, 56, 5);
    const u32 rows = ClutRowsOf(psm, csa);
    if (csm2 || !GpuHolds(b, cbp, cbp + 8)) {
        b.clut_cpu |= rows;
        b.clut_gpu &= ~rows;
        return false;
    }
    Timed timed(Section::GpuSync);
    const TimelineStep step("sync: a CLUT load");
    for (auto& [key, o] : b.owned)
        if (o.gpu_newer && o.dirty_begin < cbp + 8 && cbp < o.dirty_end) WriteBack(b, o);
    Coherent(b, cbp, cbp + 8, true);
    ps2gpu::GsClutLoad load;
    load.cbp = cbp;
    load.cpsm = cpsm;
    load.csa = csa;
    load.eight = psm == gs::PSMT8 || psm == gs::PSMT8H;
    load.texa_lo = u32(texa & 0x80FF);
    load.ta1 = Bits(texa, 32, 8);
    b.renderer->ClutFromMemory(load);
    b.clut_gpu |= rows;
    b.clut_cpu &= ~rows;
    for (u32 r = 0; r < 16; ++r)
        if (rows >> r & 1) ++b.clut_generation[r];
    ++b.gpu_clut_loads;
    return true;
}

/* The renderer's key for the texture the current primitive samples, decoded if
   need be (--gpu). */
/* Whether the GPU holds blocks [begin, end) newer than the CPU: an image drawn
   over them and not yet swizzled into GPU memory, or GPU memory newer. */
bool GpuHolds(const Bridge& b, u32 begin, u32 end) {
    for (const auto& [key, o] : b.owned)
        if (o.gpu_newer && o.dirty_begin < end && begin < o.dirty_end) return true;
    end = std::min(end, 16384u);
    for (u32 i = begin; i < end; ++i)
        if (b.blocks[i] == Bridge::kGpuNewer) return true;
    return false;
}

/* The renderer's key for the texture the current primitive samples, decoded if
   need be: by the CPU from its copy of local memory, or - when the GPU holds
   the texture's blocks newer (the race reads its own targets' alpha as
   PSMT8H textures, its depth buffer as PSMZ16) - by the GPU from its own,
   rather than wait for them to come back. */
void WriteBmp(const std::string& path, const std::vector<u32>& rgba, u32 stride, u32 w, u32 h, bool alpha = false);

// A texture pack's replacement for the texture, uploaded in its place if its
// file has been read (texture_pack::Find); false if not (yet).
bool Replace(Bridge& b, Cached& t) {
    bool pending = false;
    const std::shared_ptr<const texture_pack::Image> image = texture_pack::Find(t.pack_name, &pending);
    t.pack_pending = pending;
    if (!image) return false;
    // One image on the GPU for every texture it replaces.
    if (!b.renderer->HasReplacement(image->id) &&
        !b.renderer->UploadReplacement(image->id, image->format, image->width, image->height, image->levels,
                                       image->data.data(), image->data.size()))
        return false;
    Mutate(kMutReplacement);
    b.renderer->UseReplacement(t.renderer_key, image->id);
    t.replaced = true;
    t.replacement = image->id;
    t.decoded_serial = b.serial;
    ++b.replacements;
    return true;
}

/* The blocks a w x h texture at (tbp, tbw, psm) really reads, block by block through the GS's own
   addressing (gs::Memory::Address, which wraps at the end of memory as the GS does): `end` one past
   the highest block at or above tbp, `wrap_end` one past the highest block below it - where its addresses
   ran past the end of local memory and wrapped (0: none). Exact, not whole pages: ZOE's background 3E10
   (PSMT4 512x448, 16 blocks into a page) fills only the top half of its last row of pages - blocks 0-15 of
   each, so it ends at 0x3FFF - while counting whole pages had it wrap into the frame buffer at block 0 and
   be read back and decoded again every frame (gameplay CPU 83 -> 144 %, 2026-11-12). Remembered: a texture
   is made once per key, but keys (they include the CLUT) come and go. */
void TextureBlocks(u32 tbp, u32 tbw, u32 psm, u32 w, u32 h, u32* end, u32* wrap_end) {
    static std::unordered_map<u64, std::pair<u32, u32>> known;
    const u64 key = u64(tbp) | u64(tbw) << 14 | u64(psm) << 20 | u64(w) << 26 | u64(h) << 38;
    auto found = known.find(key);
    if (found == known.end()) {
        u32 bw, bh;     // a block, in pixels
        switch (psm) {
            case gs::PSMT4: bw = 32; bh = 16; break;
            case gs::PSMT8: bw = 16; bh = 16; break;
            case gs::PSMCT16: case gs::PSMCT16S: case gs::PSMZ16: case gs::PSMZ16S: bw = 16; bh = 8; break;
            default: bw = 8; bh = 8; break;
        }
        u32 high = tbp, wrapped = 0;
        for (u32 y = 0; y < h; y += bh)
            for (u32 x = 0; x < w; x += bw) {
                u32 nibble = 0;
                const u32 block = gs::Memory::Address(tbp, tbw, x, y, psm, &nibble) / 256;
                if (block >= tbp) high = std::max(high, block);
                else wrapped = std::max(wrapped, block + 1);
            }
        if (known.size() > 65536) known.clear();
        found = known.emplace(key, std::make_pair(high + 1, wrapped)).first;
    }
    *end = found->second.first;
    *wrap_end = found->second.second;
}

/* How far a primitive samples its texture: texels [0, *x1) x [0, *y1) hold all it reads, from its
   vertices' S/Q, T/Q and the wrap modes (CLAMP), as SampledPages works it out for a target. A texel of
   margin only where it is filtered (`filtered`: TEX1's MMAG or MMIN linear - point sampling reads the texel
   alone); REPEAT past an edge reads the whole axis (both ends). False when it cannot be known
   (REGION_REPEAT, no vertices, a vertex behind the eye). Taking "REPEAT from 0" for unknown - the margin
   reached -1 - had ZOE's 512x512 sprites at 0x3E90 / 0x3F81 decoded ~150 times a frame (2026-11-13). */
bool SampledTexels(u64 clamp, u64 tex1, u32 tw, u32 th, const ps2gpu::GsVertex* v, std::size_t n, u32* x1, u32* y1) {
    // REGION_REPEAT on both axes: every texel read, its filter's neighbours too, is (c & MSK) | FIX - inside
    // [0, (MSK | FIX) + 1) whatever the vertices say (ZOE's explosion sprites at 0x3FDC / 0x3FA6, 512x512 with
    // a 64x8 region, Q 0 in the trace: taken as the whole texture, past the end of memory into the frame
    // buffer, and decoded again ~500 times a picture).
    const u32 wms = u32(clamp & 3), wmt = u32(clamp >> 2 & 3);
    const auto region = [](u32 msk, u32 fix, u32 size) { return std::min(size, (msk | fix) + 1); };
    if (wms == 3 && wmt == 3) {
        *x1 = region(u32(Bits(clamp, 4, 10)), u32(Bits(clamp, 14, 10)), tw);
        *y1 = region(u32(Bits(clamp, 24, 10)), u32(Bits(clamp, 34, 10)), th);
        return true;
    }
    if (!v || !n) return false;
    const u32 mmin = Bits(tex1, 6, 3);
    const bool filtered = Bits(tex1, 5, 1) != 0 || mmin == 1 || mmin == 4 || mmin == 5;
    const float margin = filtered ? 1.0f : 0.0f;
    float s1 = 0, t1 = 0, s0 = 0, t0 = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (!(v[i].q > 0)) return false;
        const float s = v[i].s / v[i].q, t = v[i].t / v[i].q;
        if (!std::isfinite(s) || !std::isfinite(t)) return false;
        if (i == 0 || s < s0) s0 = s;
        if (i == 0 || s > s1) s1 = s;
        if (i == 0 || t < t0) t0 = t;
        if (i == 0 || t > t1) t1 = t;
    }
    // One past the furthest texel read along an axis of `size` (a texel of filtering to spare), or false.
    const auto far = [margin](float c0, float c1, u32 size, u32 mode, u32 region_max, u32* hi) {
        // Below: the filter's neighbour only. Above: a texel more either way (a texel exactly on the edge).
        const float a = std::floor(c0 * float(size)) - margin, b = std::ceil(c1 * float(size)) + 1;
        switch (mode) {
            case 0: {   // REPEAT: inside the texture, or it wraps over all of it - the whole axis
                // PS2HOST_GPU_EDGE_NO_WRAP=1 (experiment, inexact): the filter's texel across an edge (the
                // other end of the axis, at a texel's weight) left out - ZOE's explosion sprites at
                // 0x3F80/0x3FDC read their last rows past the end of memory, in the frame buffer, that way.
                static const bool edge_no_wrap = std::getenv("PS2HOST_GPU_EDGE_NO_WRAP") != nullptr;
                if (edge_no_wrap && c0 >= 0 && c1 <= 1.0f) {
                    *hi = u32(std::clamp(b, 1.0f, float(size)));
                    return true;
                }
                *hi = (a < 0 || b > float(size)) ? size : u32(std::clamp(b, 1.0f, float(size)));
                return true;
            }
            case 1:     // CLAMP
                *hi = u32(std::clamp(b, 1.0f, float(size)));
                return true;
            case 2:     // REGION_CLAMP: never past MAXU / MAXV
                *hi = std::min(u32(std::clamp(b, 1.0f, float(size))), std::min(size, region_max + 1));
                return true;
            default:    // REGION_REPEAT
                return false;
        }
    };
    if (wms == 3) *x1 = region(u32(Bits(clamp, 4, 10)), u32(Bits(clamp, 14, 10)), tw);
    if (wmt == 3) *y1 = region(u32(Bits(clamp, 24, 10)), u32(Bits(clamp, 34, 10)), th);
    return (wms == 3 || far(s0, s1, tw, wms, u32(Bits(clamp, 14, 10)), x1)) &&
           (wmt == 3 || far(t0, t1, th, wmt, u32(Bits(clamp, 34, 10)), y1));
}

/* The pages of local memory's first 16 (bit p) that texels [x0, x1) x [y0, y1) of a texture lie in where its
   addresses wrap past the end of memory; bit 31: some lie further (or the rectangle is past the texture).
   Cached by the rectangle in blocks. */
u32 RectWrapPages(u32 tbp, u32 tbw, u32 psm, u32 x0, u32 x1, u32 y0, u32 y1) {
    u32 bw, bh;     // a block, in pixels
    switch (psm) {
        case gs::PSMT4: bw = 32; bh = 16; break;
        case gs::PSMT8: bw = 16; bh = 16; break;
        case gs::PSMCT16: case gs::PSMCT16S: case gs::PSMZ16: case gs::PSMZ16S: bw = 16; bh = 8; break;
        default: bw = 8; bh = 8; break;
    }
    if (x0 >= x1 || y0 >= y1) return 0;
    x0 = x0 / bw * bw;
    y0 = y0 / bh * bh;
    static std::unordered_map<u64, u32> known;
    const u64 key = u64(tbp) | u64(tbw) << 14 | u64(psm) << 20 | u64(x0 / bw) << 26 | u64((x1 + bw - 1) / bw) << 33 |
                    u64(y0 / bh) << 40 | u64((y1 + bh - 1) / bh) << 47;
    auto found = known.find(key);
    if (found != known.end()) return found->second;
    u32 mask = 0;
    for (u32 y = y0; y < y1; y += bh)
        for (u32 x = x0; x < x1; x += bw) {
            u32 nibble = 0;
            const u32 block = gs::Memory::Address(tbp, tbw, x % 2048, y % 2048, psm, &nibble) / 256;
            if (block >= tbp) continue;
            if (block / 32 < 16) mask |= 1u << (block / 32);
            else mask |= 1u << 31;
        }
    if (known.size() > 65536) known.clear();
    known.emplace(key, mask);
    return mask;
}

/* Of texels [0, w) x [0, h) of a texture, the bounding rectangle of those lying past the end of memory in
   pages `mask` names (RectWrapPages' bits 0-15), in whole blocks; false when none do. Cached. */
bool WrapPagesRect(u32 tbp, u32 tbw, u32 psm, u32 w, u32 h, u32 mask, u32* x0, u32* y0, u32* x1, u32* y1) {
    u32 bw, bh;     // a block, in pixels (as RectWrapPages)
    switch (psm) {
        case gs::PSMT4: bw = 32; bh = 16; break;
        case gs::PSMT8: bw = 16; bh = 16; break;
        case gs::PSMCT16: case gs::PSMCT16S: case gs::PSMZ16: case gs::PSMZ16S: bw = 16; bh = 8; break;
        default: bw = 8; bh = 8; break;
    }
    mask &= 0xFFFFu;
    if (!mask || !w || !h) return false;
    struct Rect { u32 x0, y0, x1, y1; };
    static std::unordered_map<u64, Rect> known;
    const u64 key = u64(tbp) | u64(tbw) << 14 | u64(psm) << 20 | u64((w + bw - 1) / bw) << 26 |
                    u64((h + bh - 1) / bh) << 34 | u64(mask) << 42;
    Rect r{~0u, ~0u, 0, 0};
    if (auto found = known.find(key); found != known.end()) {
        r = found->second;
    } else {
        for (u32 y = 0; y < h; y += bh)
            for (u32 x = 0; x < w; x += bw) {
                u32 nibble = 0;
                const u32 block = gs::Memory::Address(tbp, tbw, x % 2048, y % 2048, psm, &nibble) / 256;
                if (block >= tbp || block / 32 >= 16 || !(mask >> (block / 32) & 1)) continue;
                r.x0 = std::min(r.x0, x);
                r.y0 = std::min(r.y0, y);
                r.x1 = std::max(r.x1, std::min(x + bw, w));
                r.y1 = std::max(r.y1, std::min(y + bh, h));
            }
        if (known.size() > 65536) known.clear();
        known.emplace(key, r);
    }
    if (r.x0 >= r.x1 || r.y0 >= r.y1) return false;
    *x0 = r.x0;
    *y0 = r.y0;
    *x1 = r.x1;
    *y1 = r.y1;
    return true;
}

/* The pages past the end of memory a primitive's texels lie in (RectWrapPages), from the texels it reads as
   SampledTexels works them out but not from texel 0: along an axis [lo, hi), and - REPEAT crossing an edge
   by the filter's margin alone - the one texel at the other end. False when not known (as SampledTexels). */
bool SampledWrapPages(const gs::Context& c, u32 tw, u32 th, const ps2gpu::GsVertex* v, std::size_t n, u32* pages) {
    const u64 clamp = c.clamp;
    const u32 wms = u32(clamp & 3), wmt = u32(clamp >> 2 & 3);
    if (wms == 3 || wmt == 3 || !v || !n) return false;
    const u32 mmin = Bits(c.tex1, 6, 3);
    const bool filtered = Bits(c.tex1, 5, 1) != 0 || mmin == 1 || mmin == 4 || mmin == 5;
    const float margin = filtered ? 1.0f : 0.0f;
    float s0 = 0, s1 = 0, t0 = 0, t1 = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (!(v[i].q > 0)) return false;
        const float s = v[i].s / v[i].q, t = v[i].t / v[i].q;
        if (!std::isfinite(s) || !std::isfinite(t)) return false;
        if (i == 0 || s < s0) s0 = s;
        if (i == 0 || s > s1) s1 = s;
        if (i == 0 || t < t0) t0 = t;
        if (i == 0 || t > t1) t1 = t;
    }
    struct Axis { u32 lo, hi; bool far_edge, near_edge; };
    const auto axis = [margin](float c0, float c1, u32 size, u32 mode, u32 rmin, u32 rmax, Axis* out) {
        const float a = std::floor(c0 * float(size)) - margin, b = std::ceil(c1 * float(size)) + 1;
        *out = {u32(std::clamp(a, 0.0f, float(size - 1))), u32(std::clamp(b, 1.0f, float(size))), false, false};
        if (mode == 2) {
            // REGION_CLAMP: every coordinate clamped into [MIN, MAX] - the region's edge texel read for
            // coordinates past it, which may lie outside [a, b).
            const float lo = float(std::min(rmin, size - 1)), hi = float(std::min(std::max(rmax, rmin), size - 1));
            *out = {u32(std::clamp(a, lo, hi)), u32(std::clamp(b, lo + 1, hi + 1)), false, false};
        }
        if (mode == 0) {
            if (c0 < 0 || c1 > 1.0f) *out = {0, size, false, false};   // coordinates beyond it: the whole axis
            else {
                out->far_edge = a < 0;          // the filter's texel below 0: texel size - 1
                out->near_edge = b > float(size);   // past the end: texel 0
            }
        }
        if (out->lo >= out->hi) out->lo = out->hi - 1;
    };
    Axis s, t;
    axis(s0, s1, tw, wms, u32(Bits(clamp, 4, 10)), u32(Bits(clamp, 14, 10)), &s);
    axis(t0, t1, th, wmt, u32(Bits(clamp, 24, 10)), u32(Bits(clamp, 34, 10)), &t);
    const u32 tbp = Bits(c.tex0, 0, 14), tbw = Bits(c.tex0, 14, 6), psm = Bits(c.tex0, 20, 6);
    // The rectangle, and the edges' texels: columns and rows as ranges.
    const std::pair<u32, u32> xs[3] = {{s.lo, s.hi}, {tw - 1, tw}, {0, 1}};
    const std::pair<u32, u32> ys[3] = {{t.lo, t.hi}, {th - 1, th}, {0, 1}};
    const bool use_x[3] = {true, s.far_edge, s.near_edge}, use_y[3] = {true, t.far_edge, t.near_edge};
    u32 mask = 0;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            if (use_x[i] && use_y[j])
                mask |= RectWrapPages(tbp, tbw, psm, xs[i].first, xs[i].second, ys[j].first, ys[j].second);
    *pages = mask;
    return true;
}

/* Tier N's lazy texture decode: the texels [0, *w) x [0, *h) of a TW x TH texture a primitive reads
   (SampledTexels), rounded up to 32 (a block row of most formats; the next primitives of a batch mostly fall
   inside), for a texture of one level; all of it otherwise - mipmaps, an extent not known - and with
   PS2HOST_GPU_WHOLE_DECODE=1 (tier E: every decode the whole texture, as before). False if all of it. */
/* A switch back to tier E's way (PS2HOST_GPU_...=1); none in a build without tier E (PS2HOST_TIER_E=OFF). */
bool TierESwitch(const char* name) {
#if PS2HOST_NO_TIER_E
    (void)name;
    return false;
#else
    return std::getenv(name) != nullptr;
#endif
}

bool DecodeExtent(const gs::Context& c, u32 tw, u32 th, u32 levels, const ps2gpu::GsVertex* v, std::size_t n,
                  u32* w, u32* h) {
    static const bool whole = TierESwitch("PS2HOST_GPU_WHOLE_DECODE");
    *w = tw;
    *h = th;
    if (whole || levels != 1) return false;
    u32 x1 = tw, y1 = th;
    if (!SampledTexels(c.clamp, c.tex1, tw, th, v, n, &x1, &y1)) return false;
    // PS2HOST_GPU_EXTENT_LOG=<tbp hex>: each primitive's coordinates against that texture (diagnosis).
    static const char* const extent_log = std::getenv("PS2HOST_GPU_EXTENT_LOG");
    static int extent_lines = 0;
    if (extent_log && Bits(c.tex0, 0, 14) == u32(std::strtoul(extent_log, nullptr, 16)) && extent_lines < 3000) {
        ++extent_lines;
        float s0 = 1e9f, s1 = -1e9f, t0 = 1e9f, t1 = -1e9f;
        for (std::size_t i = 0; i < n; ++i) {
            s0 = std::min(s0, v[i].s / v[i].q), s1 = std::max(s1, v[i].s / v[i].q);
            t0 = std::min(t0, v[i].t / v[i].q), t1 = std::max(t1, v[i].t / v[i].q);
        }
        std::fprintf(stderr, "[extent] tbp %X %ux%u s %.6f..%.6f t %.6f..%.6f (texels %.3f..%.3f, %.3f..%.3f) "
                     "tex1 %llX clamp %llX -> %u,%u n %zu\n", unsigned(Bits(c.tex0, 0, 14)), unsigned(tw),
                     unsigned(th), s0, s1, t0, t1, s0 * tw, s1 * tw, t0 * th, t1 * th,
                     (unsigned long long)c.tex1, (unsigned long long)c.clamp, unsigned(x1), unsigned(y1), n);
    }
    *w = std::min(tw, (std::max(1u, x1) + 31) & ~31u);
    *h = std::min(th, (std::max(1u, y1) + 31) & ~31u);
    return *w < tw || *h < th;
}

u64 CachedTexture(Bridge& b, const gs::Context& c, u64 texa, u32 tw, u32 th,
                  const ps2gpu::GsVertex* vertices = nullptr, std::size_t vertex_count = 0,
                  const Cached** partial = nullptr, bool* palette_out = nullptr, u32* wrap_end_out = nullptr) {
    const u32 tbp = Bits(c.tex0, 0, 14), tbw = Bits(c.tex0, 14, 6), psm = Bits(c.tex0, 20, 6);
    // What decides the decoded texels: where and how it is stored (TBP0, TBW,
    // PSM, TW, TH), how a region mode wraps it (CLAMP), TEXA, the CLUT's
    // contents (wherever it was loaded from).
    u64 key = c.tex0 & 0x3FFFFFFFFull;
    if (Bits(c.clamp, 0, 2) >= 2 || Bits(c.clamp, 2, 2) >= 2) key ^= c.clamp * 0xC2B2AE3D27D4EB4Full;
    key ^= texa * 0x165667B19E3779F9ull;
    const u32 clut_rows = ClutRowsOf(psm, Bits(c.tex0, 56, 5));
    // Tier N: a palettized texture whose CLUT rows the GS model holds is decoded as its indices, the CLUT
    // looked up in the draw (GsDrawState::palette) - a new CLUT (ZOE's explosions animate theirs, ~800 a
    // picture) is no new decode. Keyed without the CLUT, and without CSA (the indices are the same).
    // PS2HOST_GPU_BAKED_CLUT=1: the CLUT decoded into the texels, as tier E does.
    static const bool baked = TierESwitch("PS2HOST_GPU_BAKED_CLUT");
    // PS2HOST_GPU_PALETTE_RANGE=<first>-<last>: the palette path only for primitives in that range (bisecting).
    static const std::pair<u64, u64> palette_range = [] {
        unsigned long long lo = 0, hi = ~0ull;
        if (const char* e = std::getenv("PS2HOST_GPU_PALETTE_RANGE")) std::sscanf(e, "%llu-%llu", &lo, &hi);
        return std::make_pair(u64(lo), u64(hi));
    }();
    const u64 prim = gs::TheGs().primitives();
    const bool palette = !baked && clut_rows && !(clut_rows & ~b.clut_cpu) && b.renderer->CanPaletteInShader() &&
                         !(texture_pack::Active() && texture_pack::Wanted(Bits(c.tex0, 20, 14))) &&
                         prim >= palette_range.first && prim <= palette_range.second;
    if (palette_out) *palette_out = palette;
    if (palette) key = (key & ~(u64(0x1F) << 56)) ^ 0x5A17E77E5A17E77Eull;
    else key ^= ClutHash(b, psm, Bits(c.tex0, 56, 5)) * 0x27D4EB2F165667C5ull;
    gs::MipLevels mips = gs::LevelsOf(c);
    if (!gs::MipMode(c)) mips.count = 1;        // levels never read: not decoded
    for (u32 n = 1; n < mips.count; ++n) key ^= (u64(mips.tbp[n]) << 6 | mips.tbw[n]) * (0x9E3779B97F4A7C15ull + n);
    Cached& t = b.textures[key];
    if (!t.renderer_key) {
        t.renderer_key = b.next_texture++;
        b.texture_lru.push_front(key);
        t.lru = b.texture_lru.begin();
        Mutate(kMutNewTexture);
        // Every level's blocks (they follow level 0, as a rule): the range as
        // it always was (rows of the buffer's width, cut at the end of memory),
        // widened to the blocks it really reads (TextureBlocks: a texture wider
        // than its buffer reads on into the next row's pages), and, past the end
        // of local memory, from block 0 on (wrap_end), as the GS addresses wrap.
        t.begin = 16384;
        t.end = 0;
        t.wrap_end = 0;
        t.wrap_full = 0;
        // Diagnostic: PS2HOST_GPU_NO_WRAP=1 keeps the range as it was before the wrap fix - for measuring.
        static const bool no_wrap = std::getenv("PS2HOST_GPU_NO_WRAP") != nullptr;
        for (u32 n = 0; n < mips.count; ++n) {
            const u32 w = std::max(1u, tw >> n), h = std::max(1u, th >> n);
            const u32 plain = mips.tbp[n] + std::max(1u, std::max(1u, mips.tbw[n]) * 64 * h * PixelBits(psm) / 8 / 256 + 32);
            u32 exact = 0, wrap = 0;
            if (!no_wrap) TextureBlocks(mips.tbp[n], mips.tbw[n], psm, w, h, &exact, &wrap);
            t.begin = std::min(t.begin, mips.tbp[n]);
            t.end = std::max(t.end, std::min(16384u, std::max(plain, exact)));
            t.wrap_full = std::max(t.wrap_full, wrap);
            // PS2HOST_GPU_RANGE_LOG: each new texture whose range the wrap fix widened (once per shape).
            static const bool range_log = std::getenv("PS2HOST_GPU_RANGE_LOG") != nullptr;
            static std::set<u64> logged;
            if (range_log && (wrap || exact > std::min(16384u, plain)) &&
                logged.insert(u64(mips.tbp[n]) | u64(mips.tbw[n]) << 14 | u64(psm) << 20 | u64(w) << 26 | u64(h) << 38).second)
                std::fprintf(stderr, "[range] tbp %X tbw %u psm %02X %ux%u level %u: plain to %X, exact to %X, wraps to %X\n",
                             unsigned(mips.tbp[n]), unsigned(mips.tbw[n]), unsigned(psm), unsigned(w), unsigned(h),
                             unsigned(n), unsigned(plain), unsigned(exact), unsigned(wrap));
        }
        b.IndexTexture(&t, true);
    } else {
        b.texture_lru.splice(b.texture_lru.begin(), b.texture_lru, t.lru);
    }
    // The part past the end of memory this primitive samples (WrapSampled): more than the texture
    // has been decoded with, and it is decoded again with that much, its blocks indexed, synced and
    // hashed from then on. Unknown (REPEAT over the edge, mipmaps, no vertices): all of it.
    if (t.wrap_full > t.wrap_end) {
        u32 needed = t.wrap_full, x1 = 0, y1 = 0;
        if (mips.count == 1 && SampledTexels(c.clamp, c.tex1, tw, th, vertices, vertex_count, &x1, &y1)) {
            u32 sampled_end = 0;
            TextureBlocks(tbp, tbw, psm, x1, y1, &sampled_end, &needed);
            needed = std::min(needed, t.wrap_full);
        }
        if (needed > t.wrap_end) {
            // PS2HOST_GPU_RANGE_LOG: each time a texture's active part past the end of memory grows - why.
            static const bool range_log = std::getenv("PS2HOST_GPU_RANGE_LOG") != nullptr;
            if (range_log)
                std::fprintf(stderr, "[range] tbp %X tbw %u psm %02X %ux%u: wrap part %X -> %X of %X (sampled to %u,%u; "
                             "clamp %llX; %s; %zu vertices)\n", unsigned(tbp), unsigned(tbw), unsigned(psm), unsigned(tw),
                             unsigned(th), unsigned(t.wrap_end), unsigned(needed), unsigned(t.wrap_full), unsigned(x1),
                             unsigned(y1), static_cast<unsigned long long>(c.clamp),
                             mips.count != 1 ? "mipmapped" : x1 ? "from the vertices" : "extent unknown", vertex_count);
            const bool was_indexed = t.indexed;
            if (was_indexed) b.IndexTexture(&t, false);
            t.wrap_end = needed;
            if (was_indexed) b.IndexTexture(&t, true);
            t.owned_seen = ~std::size_t(0);     // the images over it, again with the new part
            t.valid = false;
        }
    }
    // The GPU drawing over its blocks since it was decoded makes it out of date
    // as surely as memory written there.
    if (t.owned_seen != b.owned.size()) {
        t.over.clear();
        for (const auto& [okey, o] : b.owned)
            if ((o.begin < t.end && t.begin < o.full_end) || o.begin < t.wrap_end) t.over.push_back(&o);
        t.owned_seen = b.owned.size();
    }
    // The part past the end of memory: only a draw into the pages it wraps to (Owned::low_page_serial; all
    // of the image's drawing with PS2HOST_GPU_WRAP_ANY_DRAW=1, as before, or where it wraps further).
    static const bool wrap_any = TierESwitch("PS2HOST_GPU_WRAP_ANY_DRAW");
    // ...and of those, the pages this primitive's texels lie in (SampledWrapPages): an atlas's sprites far from
    // its wrapped rows wait for no drawing there (ZOE's explosions: ~750 decodes a picture, each after a
    // write-back of the frame). Texels another primitive reads later are checked then, against when it was
    // decoded.
    u32 need_pages = 0;
    // PS2HOST_GPU_WRAP_PER_PRIMITIVE=0: by the decoded part's reach alone (4d9cae5's rule).
    static const bool per_primitive = [] {
        const char* e = TierESwitch("PS2HOST_GPU_WRAP_PER_PRIMITIVE") ? std::getenv("PS2HOST_GPU_WRAP_PER_PRIMITIVE") : nullptr;
        return !e || std::strcmp(e, "0") != 0;
    }();
    const bool need_known = per_primitive && t.wrap_end && !wrap_any && mips.count == 1 && !t.replaced &&
                            SampledWrapPages(c, tw, th, vertices, vertex_count, &need_pages) && !(need_pages >> 31);
    // PS2HOST_GPU_WHATIF_KEEPWRAP=1: a wrapped part drawn over never decodes again (what-if ceiling, WRONG
    // pictures possible - diagnosis only).
    static const bool keep_wrap = std::getenv("PS2HOST_GPU_WHATIF_KEEPWRAP") != nullptr;
    const auto wrap_drawn = [&](const Owned* o) {
        if (keep_wrap) return false;
        if (!(o->begin < t.wrap_end && 0 < o->drawn_end)) return false;
        if (wrap_any) return true;
        if (need_known) {
            for (u32 p = 0; p < Owned::kLowPages; ++p)
                if ((need_pages >> p & 1) && o->low_page_serial[p] > t.decoded_serial) return true;
            return false;
        }
        // Of it, the blocks the decoded texels lie in (a decode in part reaches less far).
        const u32 reach = t.replaced ? t.wrap_end : t.dec_wrap_end;
        if (!reach || o->begin >= reach) return false;
        if (reach > Owned::kLowPages * 32) return true;
        for (u32 p = 0; p <= (reach - 1) / 32; ++p)
            if (o->low_page_serial[p] > t.decoded_serial) return true;
        return false;
    };
    // Why it is decoded again (PS2HOST_GPU_COUNT's decode kinds): 0 memory written / new, 1 the part grew,
    // 2 drawn over, 3 its wrapped part drawn over, 4 its wrapped part grew.
    int why = t.valid ? -1 : 0;
    if (t.wrap_end && !t.valid && why == 0 && t.decoded_serial) why = 4;
    // PS2HOST_GPU_WRAP_COMPARE=1: where the per-primitive rule keeps a texture the decoded-reach rule would
    // decode again (diagnosis).
    static const bool wrap_compare = std::getenv("PS2HOST_GPU_WRAP_COMPARE") != nullptr;
    if (wrap_compare && need_known && t.valid)
        for (const Owned* o : t.over) {
            if (!(o->drawn_serial > t.decoded_serial && o->begin < t.wrap_end && 0 < o->drawn_end) || wrap_drawn(o)) continue;
            const u32 reach = t.dec_wrap_end;
            u32 drawn = 0;
            for (u32 p = 0; reach && p <= (reach - 1) / 32 && p < Owned::kLowPages; ++p)
                if (o->low_page_serial[p] > t.decoded_serial) drawn |= 1u << p;
            if (drawn)
                std::fprintf(stderr, "[wrapcmp] prim %llu tbp %X psm %02X tex0 %llX clamp %llX tex1 %llX need %X drawn %X dec %ux%u\n",
                             (unsigned long long)prim, unsigned(tbp), unsigned(psm), (unsigned long long)c.tex0,
                             (unsigned long long)c.clamp, (unsigned long long)c.tex1, unsigned(need_pages), unsigned(drawn),
                             unsigned(t.dec_w), unsigned(t.dec_h));
        }
    // A decode out of date only by drawing into its wrapped part: the pages of it drawn since (within what
    // was decoded), for a partial re-decode below - all of them known only where the decoded part wraps into
    // memory's first 16 pages (Owned::low_page_serial).
    const bool was_valid = t.valid;
    bool main_drawn = false, wrap_unknown = false;
    u32 wrap_drawn_pages = 0;
    for (const Owned* o : t.over)
        if (o->drawn_serial > t.decoded_serial && ((o->begin < t.end && t.begin < o->drawn_end) || wrap_drawn(o))) {
            const bool main = o->begin < t.end && t.begin < o->drawn_end;
            if (t.valid) why = main ? 2 : 3;
            t.valid = false;
            if (main) {
                main_drawn = true;
                continue;
            }
            const u32 reach = t.replaced ? t.wrap_end : t.dec_wrap_end;
            if (!reach || reach > Owned::kLowPages * 32) {
                wrap_unknown = true;
                continue;
            }
            for (u32 p = 0; p <= (reach - 1) / 32; ++p)
                if (o->low_page_serial[p] > t.decoded_serial) wrap_drawn_pages |= 1u << p;
        }
    // PS2HOST_GPU_PARTIAL_REDECODE=0: decoded whole again (as before; tier E's reference).
    static const bool partial_redecode = [] {
        const char* e = TierESwitch("PS2HOST_GPU_PARTIAL_REDECODE") ? std::getenv("PS2HOST_GPU_PARTIAL_REDECODE") : nullptr;
        return !e || std::strcmp(e, "0") != 0;
    }();
    // Only what does not depend on the CLUT or TEXA when decoded (a palettized format's indices, CT32): the
    // texels kept were decoded with the same.
    const bool part_format = partial_redecode && mips.count == 1 && !t.replaced && Bits(c.clamp, 0, 2) < 2 &&
                             Bits(c.clamp, 2, 2) < 2 && (palette || psm == gs::PSMCT32) && t.dec_w && t.dec_h;
    // The part this primitive samples (DecodeExtent): past what the image holds, decoded again.
    u32 want_w = tw, want_h = th;
    DecodeExtent(c, tw, th, mips.count, vertices, vertex_count, &want_w, &want_h);
    const bool grows = !t.replaced && (want_w > t.dec_w || want_h > t.dec_h);   // (a replacement is all of it)
    if (t.valid && grows) {
        // Memory unchanged since: the union, decoded again (on the GPU or the CPU path below).
        want_w = std::max(want_w, t.dec_w);
        want_h = std::max(want_h, t.dec_h);
        t.valid = false;
        t.decoded = false;              // no hash shortcut: the image lacks texels this draw reads
        why = 1;
        Mutate(kMutTextureInvalid);
    }
    // A texture pack (texture_pack.h), for textures from memory - not ones the
    // GPU drew, as PCSX2 replaces none of those. Turned on or off (Tab):
    // every texture comes back once, replaced or as the game has it.
    const bool pack = texture_pack::Active() && texture_pack::Wanted(Bits(c.tex0, 20, 14));
    if (t.pack_generation != texture_pack::Generation()) {
        t.pack_generation = texture_pack::Generation();
        if (pack || t.replaced) t.valid = t.decoded = false;
    }
    // A partial re-decode: of the part decoded before, the texels in the wrapped pages drawn since; and the
    // part this primitive samples past it (memory there as it is now, the rest unchanged since the decode).
    // One rectangle around both; the decoded part then the larger of the two.
    bool redecode_part = false, grow_only = false;
    u32 part_x0 = 0, part_y0 = 0, part_x1 = 0, part_y1 = 0;
    if (part_format && !pack && was_valid && !main_drawn && !wrap_unknown && (wrap_drawn_pages || grows)) {
        const u32 new_w = std::max(want_w, t.dec_w), new_h = std::max(want_h, t.dec_h);
        u32 x0 = ~0u, y0 = ~0u, x1 = 0, y1 = 0;
        if (wrap_drawn_pages)
            WrapPagesRect(tbp, tbw, psm, t.dec_w, t.dec_h, wrap_drawn_pages, &x0, &y0, &x1, &y1);
        if (new_w > t.dec_w) {          // the columns past it, all rows
            x0 = std::min(x0, t.dec_w);
            y0 = 0;
            x1 = new_w;
            y1 = new_h;
        }
        if (new_h > t.dec_h) {          // the rows past it, all columns
            x0 = 0;
            y0 = std::min(y0, t.dec_h);
            x1 = new_w;
            y1 = new_h;
        }
        if (x0 < x1 && y0 < y1) {
            redecode_part = true;
            grow_only = !wrap_drawn_pages;
            part_x0 = x0;
            part_y0 = y0;
            part_x1 = x1;
            part_y1 = y1;
            want_w = new_w;
            want_h = new_h;
        }
    }
    if (t.valid && t.pack_pending && pack) Replace(b, t);       // its file read since
    // Its replacement gone from the GPU (not drawn for a while): again.
    if (t.replaced && !b.renderer->HasReplacement(t.replacement)) t.valid = t.decoded = false;
    // Decoded on the GPU, from its copy of local memory and its CLUT, the
    // CPU's newer blocks and CLUT rows sent over first.
    bool gpu_holds_now = false;
    auto gpu_decode = [&] {
        for (auto& [okey, o] : b.owned)
            if (o.gpu_newer && ((o.dirty_begin < t.end && t.begin < o.dirty_end) || o.dirty_begin < t.wrap_end))
                WriteBack(b, o);
        Coherent(b, t.begin, t.end, true);
        if (t.wrap_end) Coherent(b, 0, t.wrap_end, true);
        ps2gpu::GsTextureDecode d;
        d.tbp = tbp;
        d.tbw = tbw;
        d.psm = psm;
        d.tw = tw;
        d.th = th;
        d.wms = Bits(c.clamp, 0, 2);
        d.wmt = Bits(c.clamp, 2, 2);
        d.minu = Bits(c.clamp, 4, 10);
        d.maxu = Bits(c.clamp, 14, 10);
        d.minv = Bits(c.clamp, 24, 10);
        d.maxv = Bits(c.clamp, 34, 10);
        d.texa_lo = u32(texa & 0x80FF);
        d.ta1 = Bits(texa, 32, 8);
        d.csa = Bits(c.tex0, 56, 5);
        d.levels = mips.count;
        for (u32 n = 0; n < mips.count; ++n) {
            d.level_tbp[n] = mips.tbp[n];
            d.level_tbw[n] = mips.tbw[n];
        }
        if (want_w < tw || want_h < th) {
            d.decode_w = want_w;
            d.decode_h = want_h;
        }
        if (redecode_part) {
            d.part_x0 = part_x0;
            d.part_y0 = part_y0;
            d.part_x1 = part_x1;
            d.part_y1 = part_y1;
        }
        t.dec_w = want_w;
        t.dec_h = want_h;
        if (t.wrap_end && mips.count == 1) {
            u32 dec_end = 0, dec_wrap = 0;
            TextureBlocks(tbp, tbw, psm, want_w, want_h, &dec_end, &dec_wrap);
            t.dec_wrap_end = std::min(t.wrap_end, dec_wrap);
        } else {
            t.dec_wrap_end = t.wrap_end;
        }
        ++b.partial_texels[0];
        b.partial_texels[1] += u64(want_w) * want_h;
        b.partial_texels[2] += u64(tw) * th;
        // Through the GPU's CLUT, its rows the GPU lacks sent over first - or, palette, its indices alone.
        d.clut = nullptr;
        d.indices = palette;
        if (clut_rows && !palette) {
            b.renderer->ClutRows(gs::TheGs().Clut().data(), b.clut_cpu & ~b.clut_gpu);
            b.clut_gpu |= b.clut_cpu;
        }
        Mutate(kMutClutRows);
        TraceBlocks(b, 0, 16384, "texture decoded on the GPU: tbp %X tbw %u psm %02X %ux%u (blocks %X-%X)%s",
                    unsigned(tbp), unsigned(tbw), unsigned(psm), unsigned(tw), unsigned(th), unsigned(t.begin),
                    unsigned(t.end), tbp >= 0x3F00 ? " <- near the end of memory" : "");
        b.renderer->TextureFromMemory(t.renderer_key, d);
        // PS2HOST_GPU_TEXDUMP=TBP (hex): each decode of a texture there, as the
        // GPU has it, to gpu_tex_<n>_<primitive>.bmp (the CPU's beside it as
        // cpu_tex_<n>.bmp) - what a draw sampled, against the GS model.
        // (The switches read once: a decode is dozens of times a frame.)
        static const char* const texdump = std::getenv("PS2HOST_GPU_TEXDUMP");
        static const bool count_decodes = std::getenv("PS2HOST_GPU_COUNT") != nullptr;
        if (const char* want = texdump) {
            static int dumped = 0;
            if (tbp == u32(std::strtoul(want, nullptr, 16)) && dumped < 64) {
                std::vector<u32> pixels;
                u32 w = 0, h = 0;
                if (b.renderer->ReadTexture(t.renderer_key, &pixels, &w, &h)) {
                    const std::string n = std::to_string(dumped) + "_" + std::to_string(gs::TheGs().primitives());
                    if (std::FILE* raw = std::fopen(("gpu_tex_" + n + ".rgba").c_str(), "wb")) { std::fwrite(pixels.data(), 4, pixels.size(), raw); std::fclose(raw); }
                    std::fprintf(stderr, "[texdump] %s: %ux%u psm %02X, %s\n", n.c_str(), unsigned(w), unsigned(h),
                                 unsigned(psm), gpu_holds_now ? "from the GPU's memory" : "from the CPU's");
                    // Where its blocks stand: its range, the state of the first 0x60 blocks (where a
                    // texture past the end of memory wraps to), the GPU images over them.
                    std::string states;
                    for (u32 i = 0; i < 0x60; ++i)
                        states += b.blocks[i] == Bridge::kCpuNewer ? 'C' : b.blocks[i] == Bridge::kGpuNewer ? 'G' : '.';
                    std::fprintf(stderr, "[texdump]   blocks %X-%X; blocks 0-5F: %s\n", unsigned(t.begin),
                                 unsigned(t.end), states.c_str());
                    for (const auto& [okey, o] : b.owned)
                        if (o.begin < 0x60 || o.full_end > 16384)
                            std::fprintf(stderr, "[texdump]   image %s %X/%u/%02X %ux%u blocks %X-%X (to %X), %s dirty %X-%X\n",
                                         o.depth ? "Z" : "C", unsigned(o.fbp), unsigned(o.fbw), unsigned(o.psm),
                                         unsigned(o.width), unsigned(o.height), unsigned(o.begin), unsigned(o.end),
                                         unsigned(o.full_end), o.gpu_newer ? "GPU newer," : "",
                                         unsigned(o.dirty_begin), unsigned(o.dirty_end));
                }
                ++dumped;
            }
        }
        if (count_decodes) {
            char key[96];
            std::snprintf(key, sizeof key, "psm %02X tbp %04X tbw %u %ux%u clutrows %d why %d%s known %d need %X",
                          unsigned(psm), unsigned(tbp), unsigned(tbw), unsigned(tw), unsigned(th),
                          int((clut_rows & ~b.clut_cpu) != 0), why, palette ? " palette" : "", int(need_known),
                          unsigned(need_pages));
            ++g_decode_kinds[key];
        }
        t.decoded_serial = b.serial;
        ++b.gpu_decodes;
    };
    // The GPU has newer blocks, or the CLUT rows it reads are only there:
    // decoded there without a look at the CPU's copy.
    if (!t.valid && (GpuHolds(b, t.begin, t.end) || (t.wrap_end && GpuHolds(b, 0, t.wrap_end)) ||
                     (!palette && (clut_rows & ~b.clut_cpu)))) {
        Timed timed(Section::GpuSync);
        const TimelineStep step("sync: a texture decoded on the GPU");
        gpu_holds_now = true;
        gpu_decode();
        t.replaced = t.pack_pending = false;
        b.Validated(&t);
        t.decoded = false;              // the CPU's copy was not read: no hash to compare
    }
    if (!t.valid) {
        Timed timed(Section::GpuSync);
        const TimelineStep step("sync: a texture from the CPU");
        // Its content hash stands for all of it: decoded whole (gpu_decode) - unless only the part sampled grew
        // (memory unchanged since the decode: the texels kept are right).
        if (!grow_only) redecode_part = false;
        b.cause = 0;
        const u64 before = b.downloads;
        Sync(b, t.begin, t.end, false);
        if (t.wrap_end) Sync(b, 0, t.wrap_end, false);
        if (b.downloads != before) b.texture_downloads[psm << 16 | tbw] += b.downloads - before;
        // Games send the same texels again and again (Burnout 3 its textures
        // every frame, through PATH3): decoded again only if its blocks hold
        // something else than when it was last decoded.
        u64 content = Hash(gs::TheGs().memory.data() + std::size_t(t.begin) * 256,
                           std::size_t(t.end - t.begin) * 256);
        if (t.wrap_end)
            content ^= Hash(gs::TheGs().memory.data(), std::size_t(t.wrap_end) * 256) * 0x9E3779B97F4A7C15ull;
        // Something else: decoded on the GPU all the same (texel by texel on
        // the CPU, the race's streamed textures were a twelfth of the GS
        // thread), the hash kept to compare the next time.
        if (!t.decoded || content != t.content || want_w > t.dec_w || want_h > t.dec_h) {
            if (t.decoded && content == t.content) {    // the same texels: the image's part kept, grown
                want_w = std::max(want_w, t.dec_w);
                want_h = std::max(want_h, t.dec_h);
            }
            t.replaced = t.pack_pending = false;
            // PS2HOST_TEXTURE_DUMP=<folder>: each texture the first time it
            // is decoded, named as a pack's replacement for it would be
            // (<name>.bmp; a pack wants it as .png or .dds) - to make one.
            static const char* dump_dir = std::getenv("PS2HOST_TEXTURE_DUMP");
            if (pack || dump_dir)
                t.pack_name = texture_pack::NameOf(gs::TheGs().memory, c.tex0, c.clamp, texa,
                                                   gs::TheGs().Clut().data());
            if (!pack || !Replace(b, t)) gpu_decode();
            else t.dec_w = tw, t.dec_h = th;        // a replacement stands for all of it
            static std::set<std::string> dumped;
            std::vector<u32> pixels;
            u32 w = 0, h = 0;
            if (dump_dir && !t.replaced && dumped.insert(t.pack_name.Text()).second &&
                b.renderer->ReadTexture(t.renderer_key, &pixels, &w, &h))
                WriteBmp(std::string(dump_dir) + "/" + t.pack_name.Text() + ".bmp", pixels, w, w, h, true);
            t.content = content;
            ++b.decodes;
        } else {
            ++b.unchanged;
            t.decoded_serial = b.serial;
        }
        b.Validated(&t);
        t.decoded = true;
    }
    const u64 renderer_key = t.renderer_key;
    // PS2HOST_GPU_PRIM_LOG=<first>-<last>: each primitive's texture there (diagnosis).
    static const std::pair<u64, u64> prim_log = [] {
        unsigned long long lo = 1, hi = 0;
        if (const char* e = std::getenv("PS2HOST_GPU_PRIM_LOG")) std::sscanf(e, "%llu-%llu", &lo, &hi);
        return std::make_pair(u64(lo), u64(hi));
    }();
    if (prim >= prim_log.first && prim <= prim_log.second) {
        std::fprintf(stderr, "[prim]   wrap_end %X wrap_full %X dec_wrap_end %X need_known %d need_pages %X tex1 %llX\n",
                     unsigned(t.wrap_end), unsigned(t.wrap_full), unsigned(t.dec_wrap_end), int(need_known),
                     unsigned(need_pages), (unsigned long long)c.tex1);
        for (std::size_t i = 0; i < vertex_count; ++i)
            std::fprintf(stderr, "[prim]   v%zu xy %.2f,%.2f stq %.6f %.6f %.6f -> texel %.3f,%.3f\n", i, vertices[i].x,
                         vertices[i].y, vertices[i].s, vertices[i].t, vertices[i].q,
                         vertices[i].s / vertices[i].q * tw, vertices[i].t / vertices[i].q * th);
        for (const Owned* o : t.over) {
            std::fprintf(stderr, "[prim]   over %X/%u/%02X begin %X drawn_serial %llu low:", unsigned(o->fbp),
                         unsigned(o->fbw), unsigned(o->psm), unsigned(o->begin), (unsigned long long)o->drawn_serial);
            for (u32 p = 0; p < Owned::kLowPages; ++p) std::fprintf(stderr, " %llu", (unsigned long long)o->low_page_serial[p]);
            std::fprintf(stderr, "\n");
        }
    }
    if (prim >= prim_log.first && prim <= prim_log.second)
        std::fprintf(stderr, "[prim] %llu tex0 %llX clamp %llX texa %llX key %llX renderer %llu palette %d why %d "
                     "dec %ux%u valid-before-decode? decoded_serial %llu serial %llu clut_loads %llu gpu_decodes %llu\n",
                     (unsigned long long)prim, (unsigned long long)c.tex0, (unsigned long long)c.clamp,
                     (unsigned long long)texa, (unsigned long long)key, (unsigned long long)t.renderer_key, int(palette),
                     why, unsigned(t.dec_w), unsigned(t.dec_h), (unsigned long long)t.decoded_serial,
                     (unsigned long long)b.serial, (unsigned long long)gs::TheGs().clut_loads(),
                     (unsigned long long)b.gpu_decodes);
    // Decoded in part: OnDraw's fast path checks each primitive against what the image holds.
    if (partial) *partial = !t.replaced && (t.dec_w < tw || t.dec_h < th) ? &t : nullptr;
    if (wrap_end_out) *wrap_end_out = t.wrap_end;
    // Keep the cache bounded: the least recently used go (4,096: a race keeps
    // over 2,048 about, and each one evicted came back decoded again).
    while (b.textures.size() > 4096) {
        const u64 old = b.texture_lru.back();
        b.texture_lru.pop_back();
        auto found = b.textures.find(old);
        if (found != b.textures.end()) {
            Mutate(kMutEvict);
            b.renderer->ReleaseTexture(found->second.renderer_key);
            b.IndexTexture(&found->second, false);
            b.textures.erase(found);
        }
    }
    (void)tw;
    return renderer_key;
}

// ------------------------------------------------------------------ both

ps2gpu::GsVertex Convert(const gs::Vertex& v, bool fst, u32 tw, u32 th) {
    ps2gpu::GsVertex out{};
    out.x = v.x;
    out.y = v.y;
    out.z = v.z;                    // clamped to the depth format by the caller
    out.r = v.r;
    out.g = v.g;
    out.b = v.b;
    out.a = v.a;
    out.fog = v.fog;
    if (fst) {
        // TW and TH are powers of two: dividing by 16 and by them is scaling
        // by a power of two, which the reciprocal does exactly.
        out.s = float(v.u) * (1.0f / float(16 * tw));
        out.t = float(v.v) * (1.0f / float(16 * th));
        out.q = 1.0f;
    } else {
        out.s = v.s;
        out.t = v.t;
        out.q = v.q;
    }
    return out;
}

/* A primitive as triangles: sprites as two (the second vertex's colour), lines
   and points as one pixel wide quads. */
std::size_t Triangles(const gs::Gs::DrawCall& call, bool fst, u32 tw, u32 th, ps2gpu::GsVertex* v, bool* gouraud) {
    const u32 type = u32(call.prim & 7);
    auto quad = [&](ps2gpu::GsVertex a, ps2gpu::GsVertex b) {
        ps2gpu::GsVertex ab = b, ba = b;
        ab.x = b.x; ab.y = a.y; ab.t = a.t;
        ba.x = a.x; ba.y = b.y; ba.s = a.s;
        a.r = b.r; a.g = b.g; a.b = b.b; a.a = b.a; a.fog = b.fog; a.z = b.z;   // flat: the second vertex's, Z too
        v[0] = a; v[1] = ab; v[2] = ba; v[3] = ab; v[4] = b; v[5] = ba;
        *gouraud = true;
        return std::size_t(6);
    };
    if (type == 6 && call.count >= 2)
        return quad(Convert(call.vertices[0], fst, tw, th), Convert(call.vertices[1], fst, tw, th));
    // Lines and points as the software GS draws them: a pixel per step along
    // the major axis, at the line's position rounded, both ends included. A
    // pixel n is sampled at n, so the band [p - 0.5, p + 0.5) across the line
    // holds exactly the rounded pixel, and the ends reach half a pixel past.
    if (type == 0 && call.count >= 1) {             // a point: its pixel
        ps2gpu::GsVertex a = Convert(call.vertices[call.count - 1], fst, tw, th), b = a;
        a.x -= 0.5f;
        a.y -= 0.5f;
        b.x += 0.5f;
        b.y += 0.5f;
        return quad(a, b);
    }
    if ((type == 1 || type == 2) && call.count >= 2) {   // a line: a band one pixel across
        ps2gpu::GsVertex a = Convert(call.vertices[0], fst, tw, th), b = Convert(call.vertices[1], fst, tw, th);
        const bool flat = std::fabs(b.x - a.x) >= std::fabs(b.y - a.y);
        float& a_major = flat ? a.x : a.y;
        float& b_major = flat ? b.x : b.y;
        const float reach = b_major >= a_major ? 0.5f : -0.5f;
        a_major -= reach;
        b_major += reach;
        ps2gpu::GsVertex a0 = a, a1 = a, b0 = b, b1 = b;
        if (flat) { a0.y -= 0.5f; b0.y -= 0.5f; a1.y += 0.5f; b1.y += 0.5f; }
        else { a0.x -= 0.5f; b0.x -= 0.5f; a1.x += 0.5f; b1.x += 0.5f; }
        v[0] = a0; v[1] = b0; v[2] = a1; v[3] = b0; v[4] = b1; v[5] = a1;
        return 6;
    }
    if (call.count >= 3) {
        for (int i = 0; i < 3; ++i) v[i] = Convert(call.vertices[i], fst, tw, th);
        return 3;
    }
    return 0;
}

void WriteBmp(const std::string& path, const std::vector<u32>& rgba, u32 stride, u32 w, u32 h, bool alpha);

/* PS2HOST_GPU_DUMP_FRAME: the target drawing just left, as it stands. */
void DumpPass(Bridge& s, const std::tuple<u32, u32, u32>& target) {
    const auto [fbp, fbw, psm] = target;
    std::vector<u32> pixels;
    u32 w = 0, h = 0;
    if (!s.renderer->ReadTarget(fbp, fbw, psm, &pixels, &w, &h)) return;
    char name[96];
    std::snprintf(name, sizeof name, "/pass_%03u_%X_%u_%02X.bmp", unsigned(s.pass), unsigned(fbp), unsigned(fbw),
                  unsigned(psm));
    WriteBmp(s.dump_passes + name, pixels, w, std::min(w, 640u), std::min(h, 512u));
}

/* A buffer starting inside another's drawing, at the same width and format
   and a whole row of pages down: that one's image, `offset` rows lower -
   drawn into and sampled at the internal resolution, not a second image
   reconciled with it through memory (at 1x). Burnout 3's splitscreen draws
   player 2's post effects into a frame based at the picture's row 224. */
/* `across`, when given: a target whole pages across into another too (its
   pixels that many to the right) - Burnout 3's glow draws into the second
   page column of its 128-wide strip buffer (D3 of D2). */
Owned* Containing(Bridge& b, u32 fbp, u32 fbw, u32 psm, u32* offset, u32* across = nullptr) {
    // PS2HOST_GPU_NO_CONTAINING=1: each buffer an image of its own (comparisons).
    static const bool off = std::getenv("PS2HOST_GPU_NO_CONTAINING") != nullptr;
    if (off) return nullptr;
    // Inside what a buffer of that width has drawn (in any format), not
    // only its memory: buffers laid end to end (Burnout 3's glow chain,
    // 128x128 each) stay apart.
    const u32 block = fbp * 32;
    bool inside = false;
    for (const auto& [key, o] : b.owned)
        if (!o.depth && o.fbw == fbw && o.begin <= block && block < o.drawn_end) {
            inside = true;
            break;
        }
    if (!inside) return nullptr;
    Owned* best = nullptr;
    for (auto& [key, o] : b.owned) {
        if (o.depth || o.fbw != fbw || o.psm != psm || o.fbp >= fbp) continue;
        const u32 pages_across = (fbp - o.fbp) % std::max(1u, fbw);
        if (pages_across && !across) continue;
        const u32 rows = (fbp - o.fbp) / std::max(1u, fbw) * (Is16(psm) ? 64 : 32);
        if (rows >= o.height || (best && o.fbp < best->fbp)) continue;
        best = &o;
        *offset = rows;
        if (across) *across = pages_across * 64;
    }
    static std::set<std::tuple<u32, u32, u32>> told;
    if (best && told.insert(std::make_tuple(fbp, fbw, psm)).second)
        std::fprintf(stderr, "[gpu] %X/%u/%02X drawn inside %X, %u rows down\n", unsigned(fbp), unsigned(fbw),
                     unsigned(psm), unsigned(best->fbp), unsigned(*offset));
    return best;
}

/* The pixel of an image a word of local memory is ((x | y << 16) + 1; 0
   none): an image's layout inverted, made once a layout (Shuffle). */
struct WordPixels {
    std::tuple<u32, u32, u32, u32, u32> key{~0u, 0, 0, 0, 0};
    std::vector<u32> pixel;
    const std::vector<u32>& For(u32 bp, u32 bw, u32 psm, u32 width, u32 height) {
        const auto k = std::make_tuple(bp, bw, psm, width, height);
        if (k == key) return pixel;
        key = k;
        pixel.assign(1u << 20, 0);
        for (u32 y = 0; y < height; ++y)
            for (u32 x = 0; x < width; ++x) {
                u32 nibble = 0;
                pixel[(gs::Memory::Address(bp, bw, x, y, psm, &nibble) >> 2) & ((1u << 20) - 1)] = (x | y << 16) + 1;
            }
        return pixel;
    }
};

/* The renderer's DrawTriangles calls (DrawOn), for the fast path's
   DrawTrianglesAgain: its state the last one drawn only if nothing has
   drawn since. */
u64 g_draws = 0;

void DrawOn(Bridge& s, const ps2gpu::GsDrawState& state, const ps2gpu::GsVertex* v, std::size_t n) {
    ++g_draws;
    s.renderer->DrawTriangles(state, v, n);
}

/* A sprite's Z into its own colour target's memory (OnDraw's z_as_colour),
   drawn into that target itself: a Z format's page is a 32-bit colour
   page with its 32x16 quarters swapped (the block index's bits 3 and 4), so
   each quarter of the sprite is drawn, as the Z's bytes, where its memory is
   in the target - not into an image of its own in the Z format, which the
   target and it then took turns to bring up to date through memory. The
   race's glow lays its threshold down so, 32 columns as colour and the 32
   next to them as Z: twenty such sprites a frame, each a write-back and a
   reload of both (~110 a frame). Only a sprite of one Z, the target's own
   32-bit image, the view and the target over the same pages - checked by
   each quarter's corners. False: drawn as before. */
bool ZIntoColour(Bridge& s, Owned& target, const ps2gpu::GsDrawState& state, u32 zpsm, const ps2gpu::GsVertex* v,
                 std::size_t n, int x_offset, int y_offset) {
    static const bool off = std::getenv("PS2HOST_GPU_Z_VIEWS") != nullptr;
    if (off || !state.sprite_prim || n != 6 || (zpsm != gs::PSMZ24 && zpsm != gs::PSMZ32)) return false;
    if (target.psm != gs::PSMCT32 || target.fbp != state.fbp || target.fbw != state.fbw) return false;
    for (std::size_t i = 1; i < n; ++i)
        if (v[i].z != v[0].z) return false;
    // In the Z's own view: the target's pixels moved back (Containing).
    float lx = v[0].x, hx = v[0].x, ly = v[0].y, hy = v[0].y;
    for (std::size_t i = 1; i < n; ++i) {
        lx = std::min(lx, v[i].x);
        hx = std::max(hx, v[i].x);
        ly = std::min(ly, v[i].y);
        hy = std::max(hy, v[i].y);
    }
    lx -= float(x_offset), hx -= float(x_offset), ly -= float(y_offset), hy -= float(y_offset);
    // The pixels it covers (their centres inside), inside the scissor.
    const int x0 = std::max(int(std::ceil(lx - 0.5f)), state.scissor_x0 - x_offset);
    const int x1 = std::min(int(std::ceil(hx - 0.5f)), state.scissor_x1 + 1 - x_offset);
    const int y0 = std::max(int(std::ceil(ly - 0.5f)), state.scissor_y0 - y_offset);
    const int y1 = std::min(int(std::ceil(hy - 0.5f)), state.scissor_y1 + 1 - y_offset);
    if (x0 < 0 || y0 < 0) return false;
    if (x1 <= x0 || y1 <= y0) return true;
    // Each quarter of a page it reaches: its corners where the target has
    // their memory, the same move for both - found once for each place (the
    // map of the target's words is remade for each target, and splitscreen's
    // two took turns: 15% of the GS thread).
    struct Piece { int x0, y0, x1, y1, dx, dy; };
    struct Found { bool ok; std::vector<Piece> pieces; };
    static std::map<std::tuple<u32, u32, u32, u32, u32, u32, int, int, int, int>, Found> found;
    const auto key = std::make_tuple(target.fbp, target.fbw, target.width | target.height << 16, state.zbp, state.fbw,
                                     zpsm, x0, y0, x1, y1);
    auto known = found.find(key);
    if (known == found.end()) {
        static WordPixels words;
        const std::vector<u32>& pixel =
            words.For(target.fbp * 32, target.fbw, gs::PSMCT32, target.width, target.height);
        auto where = [&](int x, int y, int* cx, int* cy) {
            u32 nibble = 0;
            const u32 p = pixel[(gs::Memory::Address(state.zbp * 32, state.fbw, u32(x), u32(y), zpsm, &nibble) >> 2) &
                                ((1u << 20) - 1)];
            if (!p) return false;
            *cx = int((p - 1) & 0xFFFF);
            *cy = int((p - 1) >> 16);
            return true;
        };
        Found f{true, {}};
        for (int ty = y0 / 16 * 16; ty < y1 && f.ok; ty += 16)
            for (int tx = x0 / 32 * 32; tx < x1 && f.ok; tx += 32) {
                Piece q{std::max(x0, tx), std::max(y0, ty), std::min(x1, tx + 32), std::min(y1, ty + 16), 0, 0};
                int ax, ay, bx, by;
                if (!where(q.x0, q.y0, &ax, &ay) || !where(q.x1 - 1, q.y1 - 1, &bx, &by)) {
                    f.ok = false;
                    break;
                }
                q.dx = ax - q.x0;
                q.dy = ay - q.y0;
                if (bx - (q.x1 - 1) != q.dx || by - (q.y1 - 1) != q.dy) f.ok = false;
                else f.pieces.push_back(q);
            }
        if (found.size() > 4096) found.clear();
        known = found.emplace(key, std::move(f)).first;
    }
    if (!known->second.ok) return false;
    const std::vector<Piece>& pieces = known->second.pieces;
    ps2gpu::GsDrawState zs;
    zs.fbp = target.fbp;
    zs.fbw = target.fbw;
    zs.psm = gs::PSMCT32;
    zs.width = target.width;
    zs.height = target.height;
    zs.scissor_x0 = 0;
    zs.scissor_y0 = 0;
    zs.scissor_x1 = int(target.width) - 1;
    zs.scissor_y1 = int(target.height) - 1;
    zs.gouraud = false;
    if (zpsm == gs::PSMZ24) zs.fbmsk = 0xFF000000u;
    const u32 z = v[0].z;
    for (const Piece& q : pieces) {
        ps2gpu::GsVertex corner{};
        corner.z = 0;
        corner.r = u8(z);
        corner.g = u8(z >> 8);
        corner.b = u8(z >> 16);
        corner.a = u8(z >> 24);
        corner.q = 1;
        const float ax = float(q.x0 + q.dx), ay = float(q.y0 + q.dy);
        const float bx = float(q.x1 + q.dx), by = float(q.y1 + q.dy);
        ps2gpu::GsVertex quad[6] = {corner, corner, corner, corner, corner, corner};
        quad[0].x = ax, quad[0].y = ay;
        quad[1].x = bx, quad[1].y = ay;
        quad[2].x = ax, quad[2].y = by;
        quad[3].x = bx, quad[3].y = ay;
        quad[4].x = bx, quad[4].y = by;
        quad[5].x = ax, quad[5].y = by;
        DrawOn(s, zs, quad, 6);
        Drew(s, target, u32(q.x0 + q.dx), u32(q.x1 + q.dx), u32(q.y1 + q.dy), u32(q.y0 + q.dy));
    }
    return true;
}

/* GsShuffle: a sprite into a 16-bit view of a 32-bit target, its texels a
   16-bit view of the depth buffer, copied 1:1, done on the target's image
   (ChannelShuffle) - not a 16-bit image of its own, its write-back and the
   target's reload. Burnout 3's fog puts its depth into the picture's alpha
   so, 40 sprites a frame: the 16-bit image, its write-back, the picture's,
   the depth buffer's and two reloads, all at the internal resolution, were
   most of the fog's cost. Only what it does exactly as the GS would: the
   texel unchanged (DECAL, TCC; no blend, or one that gives Cs; no fog), no
   test that could fail and no Z written, nearest texels at their centres,
   inside the texture; the view and the texture at the start of the target
   and the depth buffer or whole pages into them (Containing; Burnout 3's
   player 2); each pixel and texel inside their images, a Z24 texel's top
   byte (not in the image) never read - checked once for each sprite's place.
   False: drawn as it is. PS2HOST_GPU_NO_SHUFFLE: never. */
bool Shuffle(Bridge& s, const gs::Gs::DrawCall& call, const gs::Context& c, u32 frame_psm) {
    static const bool off = std::getenv("PS2HOST_GPU_NO_SHUFFLE") != nullptr;
    if (off || call.count < 2 || !s.renderer->CanShuffle()) return false;
    const u32 tex_psm = Bits(c.tex0, 20, 6);
    if (tex_psm != gs::PSMZ16 && tex_psm != gs::PSMZ16S) return false;
    if (!Bits(c.tex0, 34, 1) || Bits(c.tex0, 35, 2) != 1) return false;          // TCC, DECAL
    if ((call.prim >> 5) & 1 || !((call.prim >> 8) & 1)) return false;          // no fog; UV
    if (((call.prim >> 6) & 1) && !(Bits(c.alpha, 0, 2) == Bits(c.alpha, 2, 2) && Bits(c.alpha, 6, 2) == 0))
        return false;                                                           // blending to Cs alone
    const u64 test = c.test;
    if (Bits(test, 14, 1)) return false;                                        // DATE
    if (Bits(test, 0, 1) && !(Bits(test, 1, 3) == 1 || (Bits(test, 1, 3) == 0 && Bits(test, 12, 2) == 1)))
        return false;                                                           // ATST ALWAYS, or NEVER with FB_ONLY
    if (Bits(test, 16, 1) && Bits(test, 17, 2) != 1) return false;              // ZTST ALWAYS
    if (!Bits(c.zbuf, 32, 1)) return false;                                     // ZMSK
    if (Bits(c.tex1, 5, 4)) return false;                                       // MMAG, MMIN nearest
    const gs::Vertex& a = call.vertices[0];
    const gs::Vertex& b = call.vertices[1];
    const gs::Vertex& lo = a.x <= b.x ? a : b;
    const gs::Vertex& hi = a.x <= b.x ? b : a;
    const gs::Vertex& top = a.y <= b.y ? a : b;
    const gs::Vertex& bottom = a.y <= b.y ? b : a;
    if (lo.x != std::floor(lo.x) || hi.x != std::floor(hi.x) || top.y != std::floor(top.y) ||
        bottom.y != std::floor(bottom.y) || lo.x < 0 || top.y < 0)
        return false;
    const int x0 = int(lo.x), x1 = int(hi.x), y0 = int(top.y), y1 = int(bottom.y);
    if ((lo.u & 15) != 8 || (top.v & 15) != 8 || int(hi.u) - int(lo.u) != (x1 - x0) * 16 ||
        int(bottom.v) - int(top.v) != (y1 - y0) * 16)
        return false;
    const int du = int(lo.u >> 4) - x0, dv = int(top.v >> 4) - y0;
    // The pixels drawn: the sprite inside the scissor.
    const int qx0 = std::max(x0, int(Bits(c.scissor, 0, 11))), qx1 = std::min(x1, int(Bits(c.scissor, 16, 11)) + 1);
    const int qy0 = std::max(y0, int(Bits(c.scissor, 32, 11))), qy1 = std::min(y1, int(Bits(c.scissor, 48, 11)) + 1);
    if (qx1 <= qx0 || qy1 <= qy0) return true;                                  // nothing drawn
    const int tw = 1 << std::min(Bits(c.tex0, 26, 4), 10u), th = 1 << std::min(Bits(c.tex0, 30, 4), 10u);
    if (qx0 + du < 0 || qy0 + dv < 0 || qx1 + du > tw || qy1 + dv > th) return false;
    const u32 fbp = Bits(c.frame, 0, 9), fbw = std::max(1u, Bits(c.frame, 16, 6));
    const u32 tbp = Bits(c.tex0, 0, 14), tbw = std::max(1u, Bits(c.tex0, 14, 6));
    if (tbp % 32) return false;
    Owned* target = nullptr;
    if (auto at = s.owned.find(std::make_tuple(fbp, fbw, u32(gs::PSMCT32), false)); at != s.owned.end())
        target = &at->second;
    u32 down = 0;
    if (!target) target = Containing(s, fbp, fbw, gs::PSMCT32, &down);
    if (!target || !target->width) return false;
    // The depth buffer the texture starts in: at its ZBP, or whole pages in.
    Owned* depth = nullptr;
    for (auto& [k, o] : s.owned)
        if (o.depth && o.width && o.fbw == tbw && (o.psm == gs::PSMZ24 || o.psm == gs::PSMZ32) && o.fbp <= tbp / 32 &&
            (tbp / 32 - o.fbp) % tbw == 0 && (tbp / 32 - o.fbp) / tbw * 32 < o.height &&
            (!depth || o.fbp > depth->fbp))
            depth = &o;
    if (!depth) return false;
    Owned& t = *target;
    Owned& z = *depth;
    const u32 zpsm = z.psm;
    // Each pixel's word in the target's image, each texel's in the depth
    // buffer's, a Z24 texel's top half never read: once for each place.
    static std::map<std::tuple<u32, u32, u32, u32, u32, u32, int, int, int, int, int, int, u32, u32>, bool> verdicts;
    const auto key = std::make_tuple(fbp | t.fbp << 16, fbw, frame_psm, tbp | z.fbp << 16, tbw, tex_psm, qx0, qy0,
                                     qx1, qy1, du, dv, t.width | t.height << 16, z.width | z.height << 16 | zpsm << 28);
    auto known = verdicts.find(key);
    if (known == verdicts.end()) {
        static WordPixels target_words, depth_words;
        const std::vector<u32>& tw32 = target_words.For(t.fbp * 32, fbw, gs::PSMCT32, t.width, t.height);
        const std::vector<u32>& zw32 = depth_words.For(z.fbp * 32, tbw, zpsm, z.width, z.height);
        bool ok = true;
        for (int y = qy0; y < qy1 && ok; ++y)
            for (int x = qx0; x < qx1 && ok; ++x) {
                u32 nibble = 0;
                const u32 dst = gs::Memory::Address(fbp * 32, fbw, u32(x), u32(y), frame_psm, &nibble);
                const u32 src = gs::Memory::Address(tbp, tbw, u32(x + du), u32(y + dv), tex_psm, &nibble);
                ok = tw32[(dst >> 2) & ((1u << 20) - 1)] && zw32[(src >> 2) & ((1u << 20) - 1)] &&
                     !(zpsm == gs::PSMZ24 && (src & 2));
            }
        if (verdicts.size() > 4096) verdicts.clear();
        known = verdicts.emplace(key, ok).first;
    }
    if (!known->second) return false;
    const u32 fbmsk = u32(c.frame >> 32);
    u32 write16 = 0;
    for (u32 k = 0; k < 5; ++k) {
        if (!((fbmsk >> (3 + k)) & 1)) write16 |= 1u << k;
        if (!((fbmsk >> (11 + k)) & 1)) write16 |= 1u << (5 + k);
        if (!((fbmsk >> (19 + k)) & 1)) write16 |= 1u << (10 + k);
    }
    if (!(fbmsk >> 31)) write16 |= 0x8000;
    if (!write16) return true;
    Use(s, t.fbp, fbw, gs::PSMCT32, false, t.width, t.height, 1);       // (growing neither)
    Use(s, z.fbp, tbw, zpsm, true, z.width, z.height, 1);
    ps2gpu::GsShuffle sh;
    sh.fbp = t.fbp;
    sh.fbw = fbw;
    sh.view_fbp = fbp;
    sh.view_psm = frame_psm;
    sh.x0 = qx0;
    sh.y0 = qy0;
    sh.x1 = qx1;
    sh.y1 = qy1;
    sh.du = du;
    sh.dv = dv;
    sh.tbp = tbp;
    sh.tbw = tbw;
    sh.tex_psm = tex_psm;
    sh.zbp = z.fbp;
    sh.zwidth = z.width;
    sh.zheight = z.height;
    sh.write16 = write16;
    sh.ta0 = Bits(call.texa, 0, 8);
    sh.ta1 = Bits(call.texa, 32, 8);
    sh.aem = Bits(call.texa, 15, 1) != 0;
    sh.fba = Bits(c.fba, 0, 1) != 0;
    s.renderer->ChannelShuffle(sh);
    // The 32-bit pages the 16-bit ones are (a page each way: 64 by 64 16-bit
    // pixels, 64 by 32 32-bit ones).
    Drew(s, t, u32(qx0) / 64 * 64, (u32(qx1) + 63) / 64 * 64, down + (u32(qy1) + 63) / 64 * 32,
         down + u32(qy0) / 64 * 32);
    return true;
}

void OnDraw(const gs::Gs::DrawCall& call) {
    // Timed only past the fast path below: two clock reads a primitive were
    // a twentieth of the GS thread.
    std::optional<Timed> timed;
    Bridge& s = *g_bridge;
    const u32 type = u32(call.prim & 7);
    if (type == 7) return;
    if (!s.primary && type < 3) return;                 // the shadow compares what it has always drawn
    const gs::Context& c = *call.context;
    if (g_present_waits) {
        u32 fbp = 0, fbw = 0, psm = 0;
        if (!gs::TheGs().Displayed(&fbp, &fbw, &psm) || Bits(c.frame, 0, 9) != fbp) PresentWaiting();
    }
    // The frame and scissor as the registers have them; the draw's whole
    // state (a large struct) is made past the fast path below, which reuses
    // the one before.
    const u32 frame_psm = Bits(c.frame, 24, 6), frame_width = std::min(2048u, std::max(1u, Bits(c.frame, 16, 6)) * 64);
    // The rows a page holds twice over in a 16-bit format: a 16-bit view of a
    // 448-row picture is 896 rows (Burnout 3 writes its depth into the
    // picture's alpha through one - at 512 rows, the lower half went unwritten,
    // its haze and the sun's halo with it).
    const u32 frame_height = Is16(frame_psm) ? 1024 : 512;
    const int scissor_x0 = int(Bits(c.scissor, 0, 11)), scissor_x1 = int(Bits(c.scissor, 16, 11));
    const int scissor_y0 = int(Bits(c.scissor, 32, 11)), scissor_y1 = int(Bits(c.scissor, 48, 11));
    const bool fst = (call.prim >> 8) & 1;
    const u32 tw = 1u << std::min(Bits(c.tex0, 26, 4), 10u), th = 1u << std::min(Bits(c.tex0, 30, 4), 10u);
    // The primitive as triangles, and the pixels it can reach.
    ps2gpu::GsVertex v[6];
    bool gouraud = (call.prim >> 3) & 1;
    const std::size_t n = Triangles(call, fst, tw, th, v, &gouraud);
    if (!n) return;
    // The vertices' extremes, then rounded once (floor, ceil and the clamp
    // keep order: the same bounds as rounding each vertex).
    float low_x = float(frame_width), high_x = 0, low_y = float(frame_height), high_y = 0;
    for (std::size_t i = 0; i < n; ++i) {
        low_x = std::min(low_x, v[i].x);
        high_x = std::max(high_x, v[i].x);
        low_y = std::min(low_y, v[i].y);
        high_y = std::max(high_y, v[i].y);
    }
    u32 x0 = u32(std::clamp(std::floor(low_x), 0.0f, float(frame_width)));
    // Exclusive: the GS draws the pixels whose centres lie before the far edge.
    u32 x1 = u32(std::clamp(std::ceil(high_x), 0.0f, float(frame_width)));
    u32 y0 = u32(std::clamp(std::floor(low_y), 0.0f, float(frame_height)));
    u32 y1 = u32(std::clamp(std::ceil(high_y), 0.0f, float(frame_height)));
    y0 = std::max(y0, u32(std::max(scissor_y0, 0)));
    x0 = std::max(x0, u32(std::max(scissor_x0, 0)));
    x1 = std::min(x1, u32(scissor_x1 + 1));
    y1 = std::min(y1, u32(scissor_y1 + 1));
    if (s.primary) {
        // Wholly outside the scissor (or the buffer): nothing to draw, and no
        // image to bring up to date for it. Burnout 3's garage sends thousands
        // a frame to FRAME 0, FBW 1, whose colour and Z alias the same page.
        float lo_x = v[0].x, lo_y = v[0].y, hi_x = v[0].x, hi_y = v[0].y;
        for (std::size_t i = 1; i < n; ++i) {
            lo_x = std::min(lo_x, v[i].x);
            lo_y = std::min(lo_y, v[i].y);
            hi_x = std::max(hi_x, v[i].x);
            hi_y = std::max(hi_y, v[i].y);
        }
        const float left = float(std::max(scissor_x0, 0)), top = float(std::max(scissor_y0, 0));
        const float right = float(std::min(scissor_x1, int(frame_width) - 1));
        const float bottom = float(std::min(scissor_y1, int(frame_height) - 1));
        if (std::floor(hi_x) < left || std::ceil(lo_x) > right || std::floor(hi_y) < top || std::ceil(lo_y) > bottom)
            return;
        if (Bits(c.test, 16, 1) && Bits(c.test, 17, 2) == 0) return;    // ZTST NEVER: every pixel fails
    }
    if (s.primary && type == 6 && Is16(frame_psm) && ((call.prim >> 4) & 1) && Shuffle(s, call, c, frame_psm))
        return;
    const u32 rows = std::max(1u, y1);
    // The registers of the primitive before, and nothing changed since: its
    // state, target and depth buffer again (most of a frame's tens of
    // thousands of primitives; the lookups and checks were half the GS
    // thread's time).
    struct Fast {
        bool valid = false;
        u64 mutations = 0, clut_mutations = 0;
        const gs::Context* context = nullptr;
        u64 regs[12] = {};
        u32 rows = 0, zmax = 0;
        ps2gpu::GsDrawState state;
        Owned* target = nullptr;
        Owned* depth = nullptr;
        Owned* source = nullptr;    // a target sampled as the texture: fast only while it is not stale
        bool draws_colour = false;
        u32 y_offset = 0, t_offset = 0, th = 1;     // Containing's: the target's rows, the texture's
        u64 last_draw = ~0ull;      // g_draws after the last draw of this state
        const Cached* partial = nullptr;    // its texture decoded in part: fast only inside that part
    };
    static Fast fast;
    const u64 regs[12] = {call.prim, c.frame, c.scissor, c.tex0, c.tex1, c.clamp, c.alpha, c.test, c.zbuf,
                          call.texa, call.fogcol | (u64(call.pabe) << 63), c.miptbp1 ^ (c.miptbp2 * 31)};
    const auto inside_decoded = [&](const Cached* t) {
        u32 w = tw, h = th;
        DecodeExtent(c, tw, th, 1, v, n, &w, &h);
        return w <= t->dec_w && h <= t->dec_h;
    };
    // More rows than the cached primitive's, but in the same rows of pages of the target and of the depth buffer:
    // every check the slow path makes by rows goes through Blocks (target growth, texture overlap, colour / Z
    // overlap, the depth buffer's growth), so it would decide the same (2026-11-19: 4.5 M of the hangar's 34 M
    // primitives missed only so). Not where the target lies inside another (Containing's offset).
    // PS2HOST_GPU_FAST_ROWS=0: the old check (rows <= the cached primitive's).
    static const bool fast_rows = [] {
        const char* e = std::getenv("PS2HOST_GPU_FAST_ROWS");
        return !(e && e[0] == '0');
    }();
    const auto same_pages = [&] {
        if (!fast_rows || fast.y_offset) return false;
        const ps2gpu::GsDrawState& fs = fast.state;
        const u32 zpsm_fast = 0x30 | Bits(c.zbuf, 24, 4);
        return Blocks(fs.fbp, fs.fbw, fs.psm, rows) == Blocks(fs.fbp, fs.fbw, fs.psm, fast.rows) &&
               Blocks(fs.zbp, fs.fbw, zpsm_fast, rows) == Blocks(fs.zbp, fs.fbw, zpsm_fast, fast.rows);
    };
    if (s.primary && fast.valid && fast.mutations == g_mutations && fast.context == call.context &&
        (rows <= fast.rows || same_pages()) && std::memcmp(regs, fast.regs, sizeof regs) == 0 && !s.Dumping() &&
        (!fast.source || !fast.source->stale) && (!fast.partial || inside_decoded(fast.partial))) {
        ps2gpu::GsDrawState& st = fast.state;      // (the renderer copies what it keeps)
        st.gouraud = gouraud;
        for (std::size_t i = 0; i < n; ++i) v[i].z = std::min(v[i].z, fast.zmax);
        if (fast.y_offset) {
            for (std::size_t i = 0; i < n; ++i) v[i].y += float(fast.y_offset);
            y0 += fast.y_offset;
            y1 += fast.y_offset;
        }
        if (fast.t_offset)
            for (std::size_t i = 0; i < n; ++i) v[i].t += float(fast.t_offset) * v[i].q / float(fast.th);
        // The renderer's last draw this state's (but for gouraud) when
        // nothing has drawn since the one before this.
        if (fast.last_draw == g_draws) {
            ++g_draws;
            s.renderer->DrawTrianglesAgain(st, v, n);
        } else {
            DrawOn(s, st, v, n);
        }
        fast.last_draw = g_draws;
        if (fast.draws_colour) Drew(s, *fast.target, x0, x1, y1, y0);
        if (fast.depth) Drew(s, *fast.depth, x0, x1, y1, y0);
        ++g_fast_hits;
        return;
    }
    if (s.primary) {
        int why = 5;
        const bool clut_only = g_mutations - fast.mutations == g_clut_mutations - fast.clut_mutations;
        bool regs_same = true, tex0_clut_only = true;
        for (int i = 0; i < 12; ++i)
            if (regs[i] != fast.regs[i]) {
                regs_same = false;
                if (i != 3 || ((regs[i] ^ fast.regs[i]) & ((u64(1) << 37) - 1))) tex0_clut_only = false;
            }
        if (!fast.valid) why = 0;
        else if (!clut_only) why = 3;
        else if (fast.mutations != g_mutations && regs_same) why = 1;
        else if (fast.mutations != g_mutations && tex0_clut_only) why = 2;
        else if (!regs_same) why = 4;
        ++g_fast_misses[why];
        // Diagnosis 2 (2026-11-19): which registers differ ("other registers"), and whether one of the last
        // eight cacheable states would have matched (a small multi-entry fast path); the rest split by cause.
        if (why == 3) ++g_miss_site[g_last_mutation_site];
        if (why == 4) {
            int differ = 0;
            for (int i = 0; i < 12; ++i)
                if (regs[i] != fast.regs[i]) { ++g_miss_reg[i]; ++differ; }
            if (differ == 1) ++g_miss_single;
            for (const auto& r : g_fast_ring)
                if (r.mutations == g_mutations && r.context == call.context &&
                    std::memcmp(r.regs, regs, sizeof regs) == 0) { ++g_miss_ring_hit; break; }
        } else if (why == 5) {
            if (fast.context != call.context) ++g_miss_rest[0];
            else if (rows > fast.rows) ++g_miss_rest[1];
            else if (s.Dumping()) ++g_miss_rest[2];
            else if (fast.source && fast.source->stale) ++g_miss_rest[3];
            else ++g_miss_rest[4];
        }
    }
    fast.valid = false;
    timed.emplace(Section::GpuBridge);
    ps2gpu::GsDrawState state;
    state.fbp = Bits(c.frame, 0, 9);
    state.fbw = std::max(1u, Bits(c.frame, 16, 6));
    state.psm = frame_psm;
    state.fbmsk = u32(c.frame >> 32);
    state.width = frame_width;
    state.height = frame_height;
    state.scissor_x0 = scissor_x0;
    state.scissor_x1 = scissor_x1;
    state.scissor_y0 = scissor_y0;
    state.scissor_y1 = scissor_y1;
    state.gouraud = gouraud;
    state.textured = (call.prim >> 4) & 1;
    state.sprite = type == 6 && s.renderer->Scale().Upscaled();     // (at 1x it would only split batches)
    state.sprite_prim = type == 6;
    if (s.primary && s.Dumping()) {
        const auto key = std::make_tuple(state.fbp, state.fbw, state.psm);
        if (key != s.last_target) {
            if (std::get<0>(s.last_target) != ~0u) DumpPass(s, s.last_target);
            ++s.pass;
            std::fprintf(stderr, "[gpu-pass] %03u: frame %X/%u/%02X fbmsk %08X; prim %llX tex0 %llX clamp %llX; "
                                 "alpha %llX test %llX zbuf %llX\n",
                         unsigned(s.pass), unsigned(state.fbp), unsigned(state.fbw), unsigned(state.psm),
                         unsigned(state.fbmsk), static_cast<unsigned long long>(call.prim),
                         static_cast<unsigned long long>(c.tex0), static_cast<unsigned long long>(c.clamp),
                         static_cast<unsigned long long>(c.alpha), static_cast<unsigned long long>(c.test),
                         static_cast<unsigned long long>(c.zbuf));
            s.last_target = key;
        }
    }
    Owned* target = nullptr;
    Owned* tex_source = nullptr;
    const Cached* tex_partial = nullptr;    // the texture, decoded in part (CachedTexture)
    u32 tex_wrap_end = 0;                   // ...its part past the end of memory, from block 0 (CachedTexture)
    u32 y_offset = 0, t_offset = 0, x_offset = 0;
    u32 draw_rows = rows;
    u32 z_shift = 0;                    // the depth buffer's pages, moved with the target
    if (s.primary && !s.owned.count(std::make_tuple(state.fbp, state.fbw, state.psm, false))) {
        Owned* base = Containing(s, state.fbp, state.fbw, state.psm, &y_offset, &x_offset);
        // Pages across: the draw inside the base's width.
        if (base && x_offset && x1 + x_offset > base->fbw * 64) base = nullptr;
        // Its depth buffer as far into the base's (Burnout 3's player 2 has
        // its Z 224 rows into the picture's depth buffer too): there, or the
        // buffer stays apart - or, pages across (no depth buffer moves so),
        // a Z that only goes into the target's own memory (ZTST ALWAYS:
        // ZIntoColour) or none.
        if (base && Bits(c.test, 16, 1)) {
            const u32 pages = state.fbp - base->fbp, zbp = Bits(c.zbuf, 0, 9);
            const u32 zpsm_early = 0x30 | Bits(c.zbuf, 24, 4);
            const auto colour = Blocks(state.fbp, state.fbw, state.psm, rows);
            const auto z = Blocks(zbp, state.fbw, zpsm_early, rows);
            const bool z_inside = Bits(c.test, 17, 2) == 1 &&
                                  (Bits(c.zbuf, 32, 1) || (colour.first < z.second && z.first < colour.second) ||
                                   (base->begin < z.second && z.first < base->full_end));
            if (!x_offset && zbp >= pages && s.owned.count(std::make_tuple(zbp - pages, state.fbw, zpsm_early, true)))
                z_shift = pages;
            else if (!(x_offset && z_inside)) base = nullptr, y_offset = 0, x_offset = 0;
        }
        if (base && x_offset) {
            state.scissor_x0 += int(x_offset);
            state.scissor_x1 += int(x_offset);
            for (std::size_t i = 0; i < n; ++i) v[i].x += float(x_offset);
            x0 += x_offset;
            x1 += x_offset;
        }
        if (base) {
            state.fbp = base->fbp;
            state.scissor_y0 += int(y_offset);
            state.scissor_y1 += int(y_offset);
            for (std::size_t i = 0; i < n; ++i) v[i].y += float(y_offset);
            y0 += y_offset;
            y1 += y_offset;
            draw_rows = std::max(1u, y1);
        }
    }
    if (s.primary) target = &Use(s, state.fbp, state.fbw, state.psm, false, state.width, state.height, draw_rows);
    if (state.textured) {
        const u32 tex_psm = Bits(c.tex0, 20, 6), tbp_blocks = Bits(c.tex0, 0, 14);
        // A target read as PSMT8H (Burnout 3's glow and blur passes, ~900 a
        // picture in a race) indexes the GPU's CLUT by its alpha byte in the
        // shader (4): it was swizzled into GPU memory and decoded again each
        // time.
        const u32 tex_format = tex_psm == gs::PSMCT32 ? 0 : tex_psm == gs::PSMCT24 ? 1
                             : (tex_psm == gs::PSMCT16 || tex_psm == gs::PSMCT16S) ? 2
                             : (tex_psm == gs::PSMT8H && s.primary) ? 4 : 3;
        state.tw = tw;
        state.th = th;
        // A texture that is a target the GPU has drawn is sampled from it
        // (render to texture).
        if ((s.primary || s.targets_as_textures) && tex_format != 3 && tbp_blocks % 32 == 0) {
            const u32 candidates[2] = {tex_format == 2 ? tex_psm : gs::PSMCT32,
                                       tex_format == 2 ? tex_psm : tex_format == 4 ? gs::PSMCT32 : gs::PSMCT24};
            const u32 tex_fbw = Bits(c.tex0, 14, 6);
            for (u32 psm : candidates) {
                u32 tex_fbp = tbp_blocks / 32;
                // Inside another target (Containing): that one, its rows lower.
                if (s.primary && !s.owned.count(std::make_tuple(tex_fbp, tex_fbw, psm, false))) {
                    u32 offset = 0;
                    if (Owned* base = Containing(s, tex_fbp, tex_fbw, psm, &offset)) {
                        tex_fbp = base->fbp;
                        t_offset = offset;
                        for (std::size_t i = 0; i < n; ++i) v[i].t += float(offset) * v[i].q / float(th);
                    }
                }
                const bool known = s.primary ? s.owned.count(std::make_tuple(tex_fbp, tex_fbw, psm, false)) != 0
                                             : s.renderer->HasTarget(tex_fbp, tex_fbw, psm);
                if (!known) {
                    if (t_offset)
                        for (std::size_t i = 0; i < n; ++i) v[i].t -= float(t_offset) * v[i].q / float(th);
                    t_offset = 0;
                    continue;
                }
                if (s.primary) {
                    Owned& source = s.owned[std::make_tuple(tex_fbp, tex_fbw, psm, false)];
                    Pages sampled;
                    if (SampledPages(source, c.clamp, tw, th, v, n, &sampled)) Sample(s, source, sampled);
                    else Use(s, tex_fbp, tex_fbw, psm, false, source.width, source.height, 1);   // sampling grows nothing
                    tex_source = &source;
                }
                if (tex_format == 4 && b_clut_rows_cpu_only(s)) {
                    // The CLUT rows only the CPU has, over to the GPU's first.
                    s.renderer->ClutRows(gs::TheGs().Clut().data(), s.clut_cpu & ~s.clut_gpu);
                    s.clut_gpu |= s.clut_cpu;
                }
                state.texture_target = true;
                state.tex_fbp = tex_fbp;
                state.tex_fbw = tex_fbw;
                state.tex_target_psm = psm;
                break;
            }
            state.tex_format = tex_format;
            state.ta0 = Bits(call.texa, 0, 8);
            state.ta1 = Bits(call.texa, 32, 8);
            state.aem = Bits(call.texa, 15, 1) != 0;
        }
        if (!state.texture_target) {
            if (s.primary) {
                bool palette = false;
                state.texture = CachedTexture(s, c, call.texa, tw, th, v, n, &tex_partial, &palette, &tex_wrap_end);
                if (palette) {
                    // The CLUT as the GS model holds it, named by its entries' hash, to the renderer when it
                    // changed (a CLUT load bumps g_mutations: no fast primitive skips this). A load that left the
                    // CLUT as it was is not sent again: hashed, 1 KB through the render thread's queue and
                    // dropped there (the same version) for every draw of ZOE's, which loads a CLUT per draw.
                    static u64 sent_changes = ~0ull, sent_version = 0;
                    if (gs::TheGs().clut_changes() != sent_changes) {
                        sent_changes = gs::TheGs().clut_changes();
                        const auto& entries = gs::TheGs().Clut();
                        // Each CLUT's hash once, by its id (Gs::clut_id: the same id, the same entries) - ZOE
                        // alternates a few CLUTs, each change hashed 1 KB again (~1 % of the GS thread, 2026-11-19).
                        struct Known { u64 id = ~0ull, version = 0; };
                        static Known known[16];
                        const u64 id = gs::TheGs().clut_id();
                        Known& k = known[id % 16];
                        if (k.id != id) {
                            k.id = id;
                            k.version = Hash(reinterpret_cast<const unsigned char*>(entries.data()), 1024) | 1;
                        }
                        // The same entries as the CLUT sent last: the renderer has them (it drops it anyway).
                        if (k.version != sent_version) {
                            sent_version = k.version;
                            s.renderer->ClutVersion(sent_version, entries.data());
                        }
                    }
                    const u32 tpsm = Bits(c.tex0, 20, 6);
                    state.palette = true;
                    state.palette_eight = tpsm == gs::PSMT8 || tpsm == gs::PSMT8H;
                    state.csa = Bits(c.tex0, 56, 5);
                    state.clut_version = sent_version;
                    // The texels' alpha range: the rows it reads, where the GS model holds them (not the GPU only).
                    const u32 rows = ClutRowsOf(tpsm, state.csa);
                    if (rows && !(rows & ~s.clut_cpu)) {
                        static u64 ranged = ~0ull;
                        static int row_lo[16], row_hi[16];
                        if (gs::TheGs().clut_changes() != ranged) {
                            ranged = gs::TheGs().clut_changes();
                            const auto& entries = gs::TheGs().Clut();
                            for (u32 r = 0; r < 16; ++r) {
                                row_lo[r] = 255, row_hi[r] = 0;
                                for (u32 i = 0; i < 16; ++i) {
                                    const int a = int(entries[r * 16 + i] >> 24);
                                    row_lo[r] = std::min(row_lo[r], a), row_hi[r] = std::max(row_hi[r], a);
                                }
                            }
                        }
                        int lo = 255, hi = 0;
                        for (u32 r = 0; r < 16; ++r)
                            if (rows >> r & 1) lo = std::min(lo, row_lo[r]), hi = std::max(hi, row_hi[r]);
                        state.tex_alpha_lo = lo;
                        state.tex_alpha_hi = hi;
                    }
                }
            } else {
                // Decoded again when the texture's registers change, local memory
                // is uploaded into, or a draw since went into the texture's blocks.
                const auto key = std::make_tuple(c.tex0, c.clamp, call.texa, gs::TheGs().uploads());
                const u32 tbp = Bits(c.tex0, 0, 14);
                const u32 tex_blocks = std::max(1u, Bits(c.tex0, 14, 6) * 64 * th * PixelBits(tex_psm) / 8 / 256);
                if (key != s.texture_state || s.drawn_into(tbp, tex_blocks)) {
                    std::vector<u32> pixels;
                    u32 w = 0, h = 0;
#if !PS2HOST_NO_TIER_E      // (the shadow modes are tier E's: StartGpuRenderer refuses them without it)
                    gs::TheGs().DecodeTexture(&pixels, &w, &h);
#endif
                    // Named by the registers that place it: decoding it again
                    // replaces it rather than adding another.
                    s.texture_key = c.tex0 & 0x1FFFFFFFFFFFFFFFull;
                    s.renderer->UploadTexture(s.texture_key, pixels.data(), w, h);
                    s.texture_state = key;
                    s.since_decode.clear();
                }
                state.texture = s.texture_key;
            }
        }
        state.tfx = ps2gpu::TextureFunction(Bits(c.tex0, 35, 2));
        state.tcc = Bits(c.tex0, 34, 1) != 0;
        const int k = int(std::int32_t(Bits(c.tex1, 32, 12) << 20) >> 20);
        const u32 mmin = Bits(c.tex1, 6, 3);
        state.bilinear = k <= 0 ? Bits(c.tex1, 5, 1) != 0 : (mmin == 1 || mmin == 4 || mmin == 5);
        // Mipmapped, per pixel (gs.frag's Sample): the levels decoded with it
        // (CachedTexture). A texture of K alone that never minifies, as before.
        state.mip = s.primary && !state.texture_target ? gs::MipMode(c) : 0;
        state.wms = Bits(c.clamp, 0, 2);
        state.wmt = Bits(c.clamp, 2, 2);
        state.minu = Bits(c.clamp, 4, 10);
        state.maxu = Bits(c.clamp, 14, 10);
        state.minv = Bits(c.clamp, 24, 10);
        state.maxv = Bits(c.clamp, 34, 10);
        if (t_offset && state.wmt == 2) {       // REGION_CLAMP's rows, as far down
            state.minv += t_offset;
            state.maxv += t_offset;
        }
    }
    state.alpha_test = Bits(c.test, 0, 1) != 0;
    state.atst = Bits(c.test, 1, 3);
    state.aref = Bits(c.test, 4, 8);
    state.afail = Bits(c.test, 12, 2);
    state.date = Bits(c.test, 14, 1) != 0 && state.psm != gs::PSMCT24;
    state.datm = Bits(c.test, 15, 1);
    state.blend = (call.prim >> 6) & 1;
    state.a = ps2gpu::BlendColour(std::min(2u, Bits(c.alpha, 0, 2)));
    state.b = ps2gpu::BlendColour(std::min(2u, Bits(c.alpha, 2, 2)));
    state.c = ps2gpu::BlendAlpha(std::min(2u, Bits(c.alpha, 4, 2)));
    state.d = ps2gpu::BlendColour(std::min(2u, Bits(c.alpha, 6, 2)));
    state.fix = Bits(c.alpha, 32, 8);
    state.pabe = call.pabe;
    state.fog = (call.prim >> 5) & 1;
    state.fogcol = u32(call.fogcol & 0xFFFFFF);
    // Depth: ZTE with ZBUF.
    const u32 zpsm = 0x30 | Bits(c.zbuf, 24, 4);
    const u32 zmax = zpsm == gs::PSMZ32 ? 0xFFFFFFFFu : zpsm == gs::PSMZ24 ? 0xFFFFFFu : 0xFFFFu;
    state.depth = Bits(c.test, 16, 1) != 0;
    state.ztst = Bits(c.test, 17, 2);
    state.zbp = Bits(c.zbuf, 0, 9) - z_shift;
    state.depth_write = Bits(c.zbuf, 32, 1) == 0;
    // An alpha test failing every pixel (ATST NEVER) writes Z only for AFAIL
    // ZB_ONLY: otherwise none, whatever ZMSK says. Burnout 3's glow passes
    // (ATST NEVER, FB_ONLY, ZTST ALWAYS, ZMSK 0) were taken as writing their
    // depth buffer - over the glow images' own memory: each pass made those
    // stale, and reloaded the depth buffer (at 4x, a third of the GPU's time
    // went to moving pictures to and from its memory).
    if (state.alpha_test && state.atst == 0 && state.afail != 2) state.depth_write = false;
    // ZTST ALWAYS with ZMSK: the depth buffer plays no part. Burnout 3's race
    // blurs into scratch targets at its depth buffer's address with ZBUF left
    // there; treating that as a depth buffer would bring it up to date - a
    // write-back and a reload - for every one of their draws.
    if (state.depth && state.ztst == 1 && !state.depth_write) state.depth = false;
    // ZTST ALWAYS into a depth buffer over the colour target's own memory:
    // the GS writes each pixel's colour and its Z into the same memory, the Z
    // in its own layout - other words of it. As a depth buffer that is no
    // use; as memory it is: the race's glow passes lay their threshold grey
    // down this way, as colour and again as Z (the Z = the grey). So the Z
    // is drawn as colour into a view of that memory in the Z format, which
    // GPU memory reconciles with the colour like any two views.
    bool z_as_colour = false;
    if (s.primary && state.depth && state.ztst == 1) {
        const auto colour = Blocks(state.fbp, state.fbw, state.psm, rows);
        const auto z = Blocks(state.zbp, state.fbw, zpsm, rows);
        if (colour.first < z.second && z.first < colour.second) {
            state.depth = false;
            // An alpha test that fails every pixel writes Z only for AFAIL
            // ZB_ONLY (the race's glow passes: ATST NEVER, FB_ONLY - no Z).
            const bool none_pass = state.alpha_test && state.atst == 0;
            z_as_colour = state.depth_write && (!none_pass || state.afail == 2);
        }
    }
    Owned* depth = nullptr;
    if (state.depth) {
        if (s.primary) {
            depth = &Use(s, state.zbp, state.fbw, zpsm, true, state.width, 512, rows);   // (the renderer's kDepthRows)
        } else if (s.depth_seeded.insert(state.zbp).second) {
            // Each depth buffer starts the frame as local memory has it, so the
            // GPU tests against what the software GS drew.
            std::vector<u32> z(std::size_t(state.width) * state.height);
            const gs::Gs& g = gs::TheGs();
            for (u32 y = 0; y < state.height; ++y)
                for (u32 x = 0; x < state.width; ++x)
                    z[std::size_t(y) * state.width + x] = g.memory.Read(state.zbp * 32, state.fbw, x, y, zpsm);
            s.renderer->UploadDepth(state.zbp, state.width, state.height, z.data());
        }
    }

    for (std::size_t i = 0; i < n; ++i) v[i].z = std::min(v[i].z, zmax);
    // Not timed apart: tens of millions of primitives a race, and a timer's
    // two clock reads each cost as much as recording one.
    DrawOn(s, state, v, n);
    if (s.primary) {
        // FBMSK over every bit the format stores: the colour is not written.
        const u32 stored = state.psm == gs::PSMCT24 ? 0xFFFFFFu : Is16(state.psm) ? 0x80F8F8F8u : 0xFFFFFFFFu;
        const bool draws_colour = (state.fbmsk & stored) != stored;
        if (draws_colour) Drew(s, *target, x0, x1, y1, y0);
        if (depth && state.depth_write) Drew(s, *depth, x0, x1, y1, y0);
        // The next primitive may take the fast path - unless this one's
        // texture is a target, lies over what it draws (each primitive
        // makes it stale), or its Z goes in as colour.
        bool may = !z_as_colour && !x_offset && !s.Dumping();
        // A target that samples itself (feedback) sees each primitive's
        // drawing in the next: those go the whole way.
        if (state.texture_target && tex_source == target) may = false;
        if (may && state.textured && !state.texture_target) {
            const u32 tbp = Bits(c.tex0, 0, 14), tbw = Bits(c.tex0, 14, 6), tpsm = Bits(c.tex0, 20, 6);
            const u32 tend = std::min(16384u, tbp + std::max(1u, tbw * 64 * th * PixelBits(tpsm) / 8 / 256 + 32));
            // Over the rows a fast primitive can draw (it reaches no further
            // than this one's rows), not all the target could hold: the
            // race keeps textures past its depth buffer's 448 rows.
            const u32 colour_end = Blocks(state.fbp, state.fbw, state.psm, draw_rows).second;
            if (target->begin < tend && tbp < colour_end) may = false;
            if (depth && depth->begin < tend && tbp < Blocks(depth->fbp, depth->fbw, depth->psm, rows).second)
                may = false;
            // A texture past the end of memory reading on from block 0 into the target or depth buffer drawn:
            // each primitive checked on its own (CachedTexture: whether the pages its texels wrap to were drawn
            // into since it was decoded) - the next with the same registers may read others (ZOE's slots at
            // 0x3E90 / 0x3F81 over its frame buffer at block 0).
            if (tex_wrap_end && (target->begin < tex_wrap_end || (depth && depth->begin < tex_wrap_end))) may = false;
        }
        if (may) {
            fast.valid = true;
            fast.mutations = g_mutations;
            fast.clut_mutations = g_clut_mutations;
            {
                FastRingEntry& r = g_fast_ring[g_fast_ring_next++ & 7];
                r.mutations = g_mutations;
                r.context = call.context;
                std::memcpy(r.regs, regs, sizeof regs);
            }
            fast.context = call.context;
            std::memcpy(fast.regs, regs, sizeof regs);
            fast.rows = rows;
            fast.zmax = zmax;
            fast.state = state;
            fast.last_draw = g_draws;       // (the draw just made, above)
            fast.target = target;
            fast.depth = depth && state.depth_write ? depth : nullptr;
            fast.source = state.texture_target ? tex_source : nullptr;
        fast.partial = state.texture_target ? nullptr : tex_partial;
            fast.draws_colour = draws_colour;
            fast.y_offset = y_offset;
            fast.t_offset = t_offset;
            fast.th = th;
        }
        if (z_as_colour && z_shift == 0 &&
            ZIntoColour(s, *target, state, zpsm, v, n, int(x_offset), int(y_offset))) {
            // (drawn into the target where the Z's memory is)
        } else if (z_as_colour) {
            // The Z, as the bytes the Z format stores, into the Z-format view.
            ps2gpu::GsDrawState zs;
            zs.fbp = state.zbp;
            zs.fbw = state.fbw;
            zs.psm = zpsm;
            zs.width = state.width;
            zs.height = state.height;
            zs.scissor_x0 = state.scissor_x0 - int(x_offset);
            zs.scissor_y0 = state.scissor_y0;
            zs.scissor_x1 = state.scissor_x1 - int(x_offset);
            zs.scissor_y1 = state.scissor_y1;
            zs.gouraud = state.gouraud;
            if (zpsm == gs::PSMZ24) zs.fbmsk = 0xFF000000u;
            ps2gpu::GsVertex zv[6];
            for (std::size_t i = 0; i < n; ++i) {
                zv[i] = v[i];
                zv[i].x -= float(x_offset);         // (the Z's own view: not moved with the target)
                const u32 z = v[i].z;
                const u32 c = Is16(zpsm) ? From16(z & 0xFFFF) : z;
                zv[i].r = u8(c);
                zv[i].g = u8(c >> 8);
                zv[i].b = u8(c >> 16);
                zv[i].a = u8(c >> 24);
            }
            Owned& view = Use(s, zs.fbp, zs.fbw, zs.psm, false, zs.width, zs.height, rows);
            DrawOn(s, zs, zv, n);
            Drew(s, view, x0 - x_offset, x1 - x_offset, y1, y0);
        }
        return;
    }
    Extent& e = s.drawn[std::make_tuple(state.fbp, state.fbw, state.psm)];
    e.x1 = std::max(e.x1, x1);
    e.y1 = std::max(e.y1, y1);
    u32& blocks = s.since_decode[state.fbp * 32];
    blocks = std::max(blocks, std::max(1u, state.fbw * 64 * e.y1 * PixelBits(state.psm) / 8 / 256));
}

/* `alpha`: the pixels' own (else opaque). */
void WriteBmp(const std::string& path, const std::vector<u32>& rgba, u32 stride, u32 w, u32 h, bool alpha) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    const u32 data = w * h * 4, file = 54 + data;
    unsigned char header[54] = {'B', 'M'};
    auto put32 = [&](int at, u32 v) { for (int i = 0; i < 4; ++i) header[at + i] = (v >> (8 * i)) & 0xFF; };
    put32(2, file);
    put32(10, 54);
    put32(14, 40);
    put32(18, w);
    put32(22, u32(-std::int32_t(h)));                   // top-down
    header[26] = 1;
    header[28] = 32;
    put32(34, data);
    std::fwrite(header, 1, 54, f);
    for (u32 y = 0; y < h; ++y)
        for (u32 x = 0; x < w; ++x) {
            const u32 c = rgba[std::size_t(y) * stride + x];
            const unsigned char px[4] = {(unsigned char)(c >> 16), (unsigned char)(c >> 8), (unsigned char)c,
                                         alpha ? (unsigned char)(c >> 24) : (unsigned char)0xFF};
            std::fwrite(px, 1, 4, f);
        }
    std::fclose(f);
}

}  // namespace

/* The displayed buffer to the window from the GPU's target (the software
   picture where the GPU has none), on the thread the GS runs on. */
/* RENDER RESOLUTION (the ports' rule, porting guide 4; Orta's RECOMP OPTIONS): display sizes per shape
   (RenderSizes below), NATIVE first. A step the device cannot hold (its image limits, or images that failed to
   allocate at it) is refused. Supersampling (ANTIALIASING SSAA) multiplies both, at most 8x each way. */
namespace {
constexpr u32 kOne = ps2gpu::kScaleOne;
std::atomic<u32> g_unfit{~0u};      // the lowest scale (larger axis, 1/1024ths) whose images failed to allocate: it and above refused

/* RENDER RESOLUTION as display sizes (porting guide 4, the maintainer 2026-11-18: "nobody has a 1024x896
   display"): NATIVE (the game's own picture) first, then the usual screen sizes of the picture's shape - width and
   height each scaled to that size (PS2 pixels are not square: 512 columns for a 4:3 picture, the same 512 for a
   wide one). Each shape keeps its own choice (render_size_4:3, _16:9, _16:10, _21:9: "1920x1080" or "native"). Up
   to about 4K: 8x the columns, 5.4x the lines; a size the device cannot hold is not stepped to. */
struct RenderSize { u32 w = 0, h = 0; };            // 0 x 0: NATIVE
void Shown(u32* w, u32* h);                         // the shown picture's size (below)
u32 ShownLinesReal();

const char* ShapeName() {
    const float a = PictureAspect();
    return a < 1.4f ? "4:3" : a < 1.7f ? "16:10" : a < 2.0f ? "16:9" : "21:9";
}

std::vector<RenderSize> RenderSizes() {
    std::vector<RenderSize> all;
    const std::string shape = ShapeName();
    if (shape == "4:3") all = {{960, 720}, {1440, 1080}, {1920, 1440}, {2880, 2160}};
    else if (shape == "16:10") all = {{1280, 800}, {1920, 1200}, {2560, 1600}, {3840, 2400}};
    else if (shape == "16:9") all = {{1280, 720}, {1920, 1080}, {2560, 1440}, {3840, 2160}};
    else
        for (u32 lines : {1080u, 1440u, 2160u}) {     // 21:9: the width from the exact shape (2560, 3440, 5120)
            const u32 w = u32(float(lines) * PictureAspect() + 0.5f);
            all.push_back({w + (w & 1u), lines});
        }
    u32 sw = 0, sh = 0;
    Shown(&sw, &sh);
    std::vector<RenderSize> sizes{{0, 0}};
    for (const RenderSize& s : all)
        if (double(s.w) / sw <= 8.0 && double(s.h) / ShownLinesReal() <= 10.0) sizes.push_back(s);
    return sizes;
}

std::string SizeKey() { return std::string("render_size_") + ShapeName(); }

std::string SizeText(const RenderSize& s) {
    return s.w ? std::to_string(s.w) + "x" + std::to_string(s.h) : std::string("native");
}

// The saved size's place in RenderSizes (NATIVE where none or another is saved).
std::size_t RenderSizeIndex(const std::vector<RenderSize>& sizes) {
    const std::string v = Setting(SizeKey(), "native");
    for (std::size_t i = 0; i < sizes.size(); ++i)
        if (SizeText(sizes[i]) == v) return i;
    return 0;
}

// The lines a shown picture really has: 224 for a game drawing fields (ZOE US), else the shown height - a size's
// lines are the window's lines (1080 means 1080).
u32 ShownLinesReal() {
    u32 sw = 0, sh = 0;
    Shown(&sw, &sh);
    const u32 lines = gs::TheGs().ShownLines();
    return lines >= 64 && lines < sh ? lines : sh;
}

// The renderer's scale for a size (supersampling `ss` times it, at most 8x the columns and 10x the lines, the shape
// kept).
ps2gpu::GsScale ScaleOfSize(const RenderSize& s, u32 ss) {
    u32 sw = 0, sh = 0;
    Shown(&sw, &sh);
    double x = double(kOne) * ss, y = double(kOne) * ss;
    if (s.w) {
        x = double(s.w) * kOne / sw * ss;
        y = double(s.h) * kOne / ShownLinesReal() * ss;
    }
    if (x > 8.0 * kOne || y > 10.0 * kOne) {
        const double down = std::min(8.0 * kOne / x, 10.0 * kOne / y);
        x *= down;
        y *= down;
    }
    return ps2gpu::GsScale{std::max(kOne, u32(x + 0.5)), std::max(kOne, u32(y + 0.5))};
}

// The shown picture's size; 640 x 448 where DISPLAY says less than 64 pixels either way (none set yet).
void Shown(u32* w, u32* h) {
    gs::TheGs().ShownSize(w, h);
    if (*w < 64 || *h < 64) {
        *w = 640;
        *h = 448;
    }
}

// A GS replay's factor (GpuSetScale): the same both ways.
ps2gpu::GsScale ScaleFor(u32 q) { return ps2gpu::GsScale{std::max(kOne, q), std::max(kOne, q)}; }

bool RenderFits(const RenderSize& s) {
    const ps2gpu::GsScale k = ScaleOfSize(s, SupersamplingFactor());
    if (s.w && std::max(k.x, k.y) >= g_unfit.load()) return false;
    if (!g_bridge) return true;
    u32 w = 0, h = 0;
    Shown(&w, &h);
    // The widest target an image is drawn larger at (ScaleOf: under 1024 columns), the tallest 512 lines.
    return g_bridge->renderer->ScaleFits(k, std::max(w, 1023u), std::max(h, 512u));
}

std::atomic<u32> g_replay_scale{0};     // GpuSetScale's (a GS replay's): the setting's otherwise
ps2gpu::GsScale g_good;                 // the last scale that ran a frame without a failed image
}  // namespace

// An FSR preset's size (UPSCALING FSR QUALITY / BALANCED / PERFORMANCE, the maintainer 2026-11-20): the shown
// picture's size over the preset's ratio, in steps of 16 (a window dragged does not draw anew at each pixel);
// 0 x 0 for none or before the first picture.
RenderSize FsrPresetSize() {
    const float ratio = FsrPresetRatio();
    u32 w = 0, h = 0;
    if (ratio > 0.0f) WindowShownSize(&w, &h);
    if (!w || !h) return {};
    auto step = [&](u32 v) { return std::max(16u, (u32(float(v) / ratio + 8.0f) / 16u) * 16u); };
    return {step(w), step(h)};
}

ps2gpu::GsScale WantedScale() {
    // PS2HOST_REPLAY_SCALE_XY=<x>,<y> (diagnosis, a replay): the two factors as given - a wide picture's unequal ones
    static const char* const xy = std::getenv("PS2HOST_REPLAY_SCALE_XY");
    double sx = 0, sy = 0;
    if (xy && std::sscanf(xy, "%lf,%lf", &sx, &sy) == 2 && sx >= 1.0 && sy >= 1.0)
        return ps2gpu::GsScale{u32(sx * ps2gpu::kScaleOne + 0.5), u32(sy * ps2gpu::kScaleOne + 0.5)};
    if (const u32 q = g_replay_scale.load()) return ScaleFor(q);
    const RenderSize preset = FsrPresetSize();
    if (preset.w) return ScaleOfSize(preset, 1);
    const std::vector<RenderSize> sizes = RenderSizes();
    return ScaleOfSize(sizes[RenderSizeIndex(sizes)], SupersamplingFactor());
}

std::string RenderResolutionText() {
    const RenderSize preset = FsrPresetSize();
    if (preset.w) return "FSR " + std::to_string(preset.w) + "X" + std::to_string(preset.h);
    const std::vector<RenderSize> sizes = RenderSizes();
    const RenderSize& s = sizes[RenderSizeIndex(sizes)];
    if (s.w) return std::to_string(s.w) + "X" + std::to_string(s.h);
    u32 w = 0, h = 0;
    Shown(&w, &h);
    return "NATIVE " + std::to_string(w) + "X" + std::to_string(h);
}

bool StepRenderScale(int step) {
    const std::vector<RenderSize> sizes = RenderSizes();
    const long at = long(RenderSizeIndex(sizes)) + step;
    if (at < 0 || at >= long(sizes.size())) return false;          // the ends stop, as Orta's
    if (!RenderFits(sizes[std::size_t(at)])) return false;         // refused: the device cannot hold it
    SetSetting(SizeKey(), SizeText(sizes[std::size_t(at)]));
    return true;                    // applied between frames (the GS frame handler)
}

/* A new internal resolution: what the GPU drew goes to its local memory
   first, the renderer drops its targets and depth buffers, and every image
   comes back from memory at the new size as it is next used. */
void Rescale(Bridge& b, ps2gpu::GsScale scale) {
    for (auto& [key, o] : b.owned)
        if (o.gpu_newer) WriteBack(b, o);
    b.renderer->SetScale(scale);
    for (auto& [key, o] : b.owned) {
        o.stale = true;
        o.stale_beyond = false;
        o.why = 0;
        o.by16 = false;
        o.stale_row0 = 0;
        o.stale_row1 = ~0u;
        o.stale_pages.set();
        o.gpu_newer = false;
        o.dirty_begin = ~0u;
        o.dirty_end = 0;
        o.dirty_pages.reset();
        o.rect_dirty = false;
        o.checked = 0;
    }
    ++b.newer_epoch;
    Mutate(kMutNewerEpoch);
    std::fprintf(stderr, "[gpu] internal resolution: %.3fx %.3fx\n", scale.x / double(kOne), scale.y / double(kOne));
}

/* The picture handed over without waiting for the renderer's queue (DisplayTargetThen): the target's image
   presented from the renderer's thread when the queue gets there, the GS thread on to the next frame meanwhile. If
   the renderer had no such target after all (none drawn there yet: not owned by the GPU, it is seen here first), the
   next picture goes the waiting way, with the software picture where there is none - each frame after it handed
   over again. PS2HOST_PRESENT_WAIT: always the waiting way (agent L, 2026-11-21). */
static std::atomic<bool> g_present_missed{false};

void PresentGpuPicture(ps2gpu::Display& display, u64 due_ns) {
    u32 fbp = 0, fbw = 0, psm = 0;
    static const bool wait = std::getenv("PS2HOST_PRESENT_WAIT") != nullptr;
    if (g_bridge && gs::TheGs().Displayed(&fbp, &fbw, &psm)) {
        auto found = g_bridge->owned.find(std::make_tuple(fbp, fbw, psm, false));
        if (!wait && found != g_bridge->owned.end() && !found->second.stale && !g_present_missed.exchange(false)) {
            // What the picture's size takes of the GS's state, now; the rest on the renderer's thread.
            const ps2gpu::GsScale k = g_bridge->renderer->Scale();
            const u32 lines = std::min(gs::TheGs().ShownLines(), 448u);
            g_bridge->renderer->DisplayTargetThen(
                fbp, fbw, psm, [&display, k, lines, due_ns](bool ok, std::uint64_t image, int layout, u32 w, u32 h) {
                    if (!ok) {
                        g_present_missed = true;
                        return;
                    }
                    const u32 sw = std::min(w, ps2gpu::ScaleSize(640u, k.x)), sh = std::min(h, ps2gpu::ScaleSize(lines, k.y));
                    display.SetPictureCrop(k.x > kOne ? (k.x + 2 * kOne - 1) / (2 * kOne) : 0u,
                                           k.y > kOne ? (k.y + 2 * kOne - 1) / (2 * kOne) : 0u);
                    if (due_ns) display.PresentImageAt(image, layout, sw, sh, due_ns);
                    else display.PresentImage(image, layout, sw, sh);
                });
            return;
        }
        std::uint64_t image = 0;
        int layout = 0;
        u32 w = 0, h = 0;
        if (found != g_bridge->owned.end() && !found->second.stale &&
            g_bridge->renderer->DisplayTarget(fbp, fbw, psm, &image, &layout, &w, &h)) {
            // The target's own pixels, as many as the picture's 640x448 at the
            // internal resolution.
            const ps2gpu::GsScale k = g_bridge->renderer->Scale();
            const u32 lines = std::min(gs::TheGs().ShownLines(), 448u);    // 224 in field mode
            const u32 sw = std::min(w, ps2gpu::ScaleSize(640u, k.x)), sh = std::min(h, ps2gpu::ScaleSize(lines, k.y));
            // Drawn larger, the first PS2 column and row are only half drawn where a game's picture ends at 0.0
            // (ZOE: its scene's geometry stops there, the rest of the column an earlier picture - 2026-11-20
            // issue 11, a band down the left and top edges): half a PS2 pixel left out of what is shown.
            display.SetPictureCrop(k.x > kOne ? (k.x + 2 * kOne - 1) / (2 * kOne) : 0u,
                                   k.y > kOne ? (k.y + 2 * kOne - 1) / (2 * kOne) : 0u);
            if (due_ns) display.PresentImageAt(image, layout, sw, sh, due_ns);
            else display.PresentImage(image, layout, sw, sh);
            return;
        }
    }
    std::vector<u32> pixels;
    u32 w = 0, h = 0;
    if (gs::TheGs().Picture(&pixels, &w, &h)) display.Present(pixels.data(), w, h);
}

/* Texture packs (texture_pack.h; the maintainer 2026-11-17, in place of the boot animation): the "textures" folder
   beside the PC settings - the player's own folder, made at the first start so it can be found - holds one folder
   per pack, as a pack comes (a PCSX2 pack's SLUS-20148\replacements\... works as it is: only the files' names
   count). Files right in "textures" are a pack of their own ("textures"). The "texture_pack" setting names the one
   drawn, or "off"; unset (or "on", as the setting was before): the first pack by name. PS2HOST_TEXTURE_PACK: a
   folder used as the one pack (tests). Not for a scripted run (no PC settings): the game's own textures. */
namespace {
std::string g_textures_dir;                 // the "textures" folder ("" without PC settings)
std::vector<std::string> g_packs;           // its packs, by name
std::string g_pack;                         // the one open, "" none

std::string PackDir(const std::string& name) {
    namespace fs = std::filesystem;
    return name == "textures" ? g_textures_dir : (fs::u8path(g_textures_dir) / fs::u8path(name)).u8string();
}

void OpenPack(const std::string& name) {
    g_pack = name;
    if (name.empty()) {
        texture_pack::SetEnabled(false);
        return;
    }
    texture_pack::SetBlockCompression(g_bridge->renderer->BlockCompression());
    const std::size_t files = texture_pack::Open(PackDir(name));
    texture_pack::SetEnabled(files != 0);
    std::fprintf(stderr, "[texture pack] %s: %zu textures (Tab turns it on and off)\n", name.c_str(), files);
}
}  // namespace

std::vector<std::string> TexturePacks() { return g_packs; }
std::string TexturePack() { return g_pack.empty() ? "off" : g_pack; }

void SelectTexturePack(const std::string& name) {
    const bool known = std::find(g_packs.begin(), g_packs.end(), name) != g_packs.end();
    SetSetting("texture_pack", known ? name : "off");
    if (!g_bridge) return;
    OpenPack(known ? name : "");
}

void OpenTexturePack() {
    namespace fs = std::filesystem;
    if (const char* dir = std::getenv("PS2HOST_TEXTURE_PACK")) {
        texture_pack::SetBlockCompression(g_bridge->renderer->BlockCompression());
        const std::size_t files = texture_pack::Open(dir);
        texture_pack::SetEnabled(files != 0);
        std::fprintf(stderr, "[texture pack] %zu textures in %s\n", files, dir);
        return;
    }
    if (SettingsPath().empty()) return;
    g_textures_dir = (fs::path(SettingsPath()).parent_path() / "textures").u8string();
    std::error_code error;
    fs::create_directories(fs::u8path(g_textures_dir), error);
    g_packs.clear();
    bool loose = false;
    for (fs::directory_iterator it(fs::u8path(g_textures_dir), fs::directory_options::skip_permission_denied, error), end;
         !error && it != end; it.increment(error)) {
        if (it->is_directory(error)) g_packs.push_back(it->path().filename().u8string());
        else if (it->is_regular_file(error)) loose = true;
    }
    std::sort(g_packs.begin(), g_packs.end());
    if (loose) g_packs.insert(g_packs.begin(), "textures");
    std::string want = Setting("texture_pack", "on");
    if (want == "off") return OpenPack("");
    if (std::find(g_packs.begin(), g_packs.end(), want) == g_packs.end()) want = g_packs.empty() ? "" : g_packs[0];
    OpenPack(want);
}

bool StartGpuRenderer(std::string* error, GpuMode mode, bool show) {
#if PS2HOST_NO_TIER_E
    // The shadow modes check the GPU against the software GS: tier E, not in a build without it.
    if (mode != GpuMode::Primary) {
        if (error) *error = "--gpu-shadow is tier E's (this build has PS2HOST_TIER_E=OFF)";
        return false;
    }
#endif
    static Bridge bridge;
    // On the window's device when there is a window: one Vulkan device for both.
    ps2gpu::Gpu* device = WindowGpu();
    if (!device) {
        bridge.gpu = ps2gpu::CreateGpu(error);
        if (!bridge.gpu) return false;
        device = bridge.gpu.get();
    }
    ps2gpu::SetPipelineHints(g_pipeline_hints.data(), g_pipeline_hints.size());
    // The pipeline cache beside the PC settings - a player's only: a scripted
    // run's settings are beside its memory card, in %TEMP% (what every
    // game's runs there would share), and it makes every pipeline at once.
    // (PS2HOST_PLAYER_PIPELINES=1: a scripted run with the player's cache too - a benchmark measuring what a
    // player meets, not every pipeline made cold in the middle of a fight; 2026-11-22.)
    static const bool player_pipelines = std::getenv("PS2HOST_PLAYER_PIPELINES") != nullptr;
    if (!SettingsPath().empty() && (!VirtualClock() || player_pipelines))
        ps2gpu::SetPipelineCacheDir(std::filesystem::path(SettingsPath()).parent_path().u8string());
    ApplyShaderCompileSetting();        // SHADER COMPILE (RECOMP OPTIONS; a scripted run makes all at once anyway)
    ApplyAnisotropySetting();           // ANISOTROPIC FILTER (off unless chosen)
    bridge.renderer = ps2gpu::CreateGsRenderer(*device, error);
    if (!bridge.renderer) return false;
    // Vulkan's recording on a thread of its own, beside the GS thread's
    // (PS2HOST_GPU_THREAD=0: on the GS thread, as before).
    const char* threaded = std::getenv("PS2HOST_GPU_THREAD");
    if (SampleRenderThread()) ps2gpu::SetRenderThreadStart([] { StartSampler(); });   // --sample-render
    if (!threaded || std::strcmp(threaded, "0") != 0)
        bridge.renderer = ps2gpu::MakeThreadedGsRenderer(std::move(bridge.renderer));
    g_bridge = &bridge;
    OpenTexturePack();
    bridge.primary = mode == GpuMode::Primary;
    // The CPU's waits for the GPU, on the profiler's timeline, by why.
    ps2gpu::SetGpuWaitObserver([](const char* why, std::uint64_t from_ns, std::uint64_t to_ns) {
        timeline::Span(why, from_ns, to_ns);
    });
    if (const char* dump = std::getenv("PS2HOST_GPU_DUMP_FRAME")) bridge.dump_frame = std::atoll(dump);
    if (const char* dir = std::getenv("PS2HOST_GPU_DUMP_DIR")) bridge.dump_passes = dir;
    bridge.targets_as_textures = mode == GpuMode::ShadowRtt;
    gs::Gs& g = gs::TheGs();
    g.on_draw = OnDraw;
    if (bridge.primary) {
        g.rasterize = false;
        g.on_memory = [](u32 begin, u32 end, bool write) {
            g_bridge->cause = write ? 2 : 1;
            Sync(*g_bridge, begin, end, write);
        };
        g.on_write = [](u32 begin, u32 end, const u8* cover) {
            g_bridge->cause = 2;
            SyncWrite(*g_bridge, begin, end, cover);
        };
        g.on_clut_load = [](u64 tex0, u64 texa) { return LoadClut(*g_bridge, tex0, texa); };
        g.on_transfer_done = [] { ApplyPatch(*g_bridge); };
    }
    if (show || bridge.primary) {
        // The window shows the GPU's picture: the displayed buffer read back
        // from its target (the software picture where the GPU has none).
        SetPictureSource([](std::vector<u32>* pixels, u32* width, u32* height) {
            u32 fbp = 0, fbw = 0, psm = 0;
            if (!g_bridge || !gs::TheGs().Displayed(&fbp, &fbw, &psm)) return false;
            if (g_bridge->primary) {
                auto found = g_bridge->owned.find(std::make_tuple(fbp, fbw, psm, false));
                if (found == g_bridge->owned.end() || found->second.stale) return false;
            }
            std::vector<u32> rgba;
            u32 w = 0, h = 0;
            if (!g_bridge->renderer->ReadTarget(fbp, fbw, psm, &rgba, &w, &h)) return false;
            const u32 shown_w = std::min(w, 640u), shown_h = std::min(h, 448u);
            pixels->resize(std::size_t(shown_w) * shown_h);
            for (u32 y = 0; y < shown_h; ++y)
                for (u32 x = 0; x < shown_w; ++x) {
                    const u32 c = rgba[std::size_t(y) * w + x];
                    (*pixels)[std::size_t(y) * shown_w + x] = (c & 0xFF00FF00u) | ((c & 0xFF) << 16) | ((c >> 16) & 0xFF);
                }
            *width = shown_w;
            *height = shown_h;
            return true;
        });
    }
    if (bridge.primary) {
        // The end of each frame on the GS thread, at its place in the stream:
        // the frame to the GPU, and the window's picture straight from the
        // displayed target (no read-back) - at once, or once the stream has
        // moved past the displayed buffer (SetPresentAtBlankEnd); the EE goes
        // on meanwhile.
        SetGsFrameHandler([](u64 frame) {
            PresentWaiting();               // the last frame's, nothing having moved it on
            GpuShadowFrame(frame, std::string());
            // The internal resolution, between frames (the GS thread's).
            ps2gpu::GsScale scale = WantedScale();
            if (g_bridge->renderer->TakeScaleFailure()) {
                // Images that did not fit the device's memory at this scale: back to the last one that ran, the
                // factor and those above it refused from now on (RenderFits), the saved value undone.
                const ps2gpu::GsScale failed = g_bridge->renderer->Scale();
                g_unfit = std::min(g_unfit.load(), std::max(failed.x, failed.y));   // the scale, supersampling in it
                std::fprintf(stderr, "[gpu] internal resolution %.3fx %.3fx failed to allocate - back to %.3fx %.3fx\n",
                             failed.x / double(kOne), failed.y / double(kOne), g_good.x / double(kOne),
                             g_good.y / double(kOne));
                // The saved size back to the largest below the failed one (NATIVE at least).
                if (!g_replay_scale.load()) {
                    const std::vector<RenderSize> sizes = RenderSizes();
                    std::size_t keep = 0;
                    for (std::size_t i = 0; i < sizes.size(); ++i)
                        if (sizes[i].w && std::max(ScaleOfSize(sizes[i], SupersamplingFactor()).x,
                                                   ScaleOfSize(sizes[i], SupersamplingFactor()).y) < g_unfit.load())
                            keep = i;
                    SetSetting(SizeKey(), SizeText(sizes[keep]));
                }
                scale = g_good;
            } else {
                g_good = g_bridge->renderer->Scale();
            }
            if (scale != g_bridge->renderer->Scale()) Rescale(*g_bridge, scale);
            // The game's own pictures: a frame whose shown buffer differs from the last frame's is a new one (a
            // double-buffered game's flip) - GamePictures, what the game's speed is when it moves a step a picture
            // (the maintainer 2026-11-22: ZOE slows with its pictures while its blanks go on).
            {
                u32 shown_fbp = 0, shown_fbw = 0, shown_psm = 0;
                static u32 last_shown = ~0u;
                if (gs::TheGs().Displayed(&shown_fbp, &shown_fbw, &shown_psm) && shown_fbp != last_shown) {
                    last_shown = shown_fbp;
                    const u64 now = u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                            std::chrono::steady_clock::now().time_since_epoch()).count());
                    std::lock_guard<std::mutex> hold(g_picture_times_lock);
                    const u64 n = g_game_pictures.load(std::memory_order_relaxed);
                    if (n) {
                        const u64 gap = now - g_picture_times[(n - 1) & 255];
                        for (u64& longest : g_picture_longest) longest = std::max(longest, gap);
                    }
                    g_picture_times[n & 255] = now;
                    g_game_pictures.fetch_add(1, std::memory_order_relaxed);
                }
            }
            g_present_frame = frame;
            g_present_waits = true;
            if (!g_present_at_blank_end || VirtualClock()) PresentWaiting();
        });
    }
    std::fprintf(stderr, "gpu: drawing on %s %s\n", device->Describe().c_str(),
                 bridge.primary ? "instead of the software GS" : "beside the software GS");
    return true;
}

u64 GamePictures() { return g_game_pictures.load(std::memory_order_relaxed); }

int GamePicturesInLast(u64 window_ns) {
    const u64 now = u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count());
    std::lock_guard<std::mutex> hold(g_picture_times_lock);
    const u64 n = g_game_pictures.load(std::memory_order_relaxed);
    int count = 0;
    for (u64 i = n; i > 0 && n - i < 256 && now - g_picture_times[(i - 1) & 255] <= window_ns; --i) ++count;
    return count;
}

double TakeLongestGamePictureGap(int reader) {
    const u64 now = u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count());
    std::lock_guard<std::mutex> hold(g_picture_times_lock);
    const u64 n = g_game_pictures.load(std::memory_order_relaxed);
    u64 longest = std::exchange(g_picture_longest[reader & 1], 0);
    if (n) longest = std::max(longest, now - g_picture_times[(n - 1) & 255]);    // a wait going on now counts
    return double(longest) / 1e6;
}

void SetGamePictureRate(double per_second) { g_game_picture_rate = per_second; }
double GamePictureRate() { return g_game_picture_rate; }

bool GpuPrimary() { return g_bridge && g_bridge->primary; }

void GpuSetScale(double factor) {
    g_replay_scale = factor >= 1.0 ? u32(factor * kOne + 0.5) : 0u;
    if (!g_bridge) return;
    const ps2gpu::GsScale scale = WantedScale();
    if (scale != g_bridge->renderer->Scale()) Rescale(*g_bridge, scale);
}

void ApplyShaderCompileSetting() {
    const std::string v = Setting("shader_compile", "hybrid");
    ps2gpu::SetShaderCompileMode(v == "off"    ? ps2gpu::kShaderOff
                                 : v == "skip" ? ps2gpu::kShaderSkip
                                 : v == "wait" ? ps2gpu::kShaderWait
                                               : ps2gpu::kShaderHybrid);
}

bool GpuBuildingShaders() { return ps2gpu::BuildingShaders(); }

void ApplyAnisotropySetting() {
    const std::string v = Setting("anisotropic", "off");
    ps2gpu::SetAnisotropy(v == "off" ? 0 : std::atoi(v.c_str()));
}

void PresentWaiting() {
    if (!g_present_waits) return;
    g_present_waits = false;
    if (ps2gpu::Display* display = WindowDisplay()) {
        const bool pace = !VirtualClock() && FramePacingPaced();    // (FRAME PACING: PACED or IMMEDIATE)
        PresentGpuPicture(*display, pace ? FrameDue(g_present_frame) : 0);
    }
}

void SetPresentAtBlankEnd(bool on) { g_present_at_blank_end = on; }

void GpuCaptureBegin(u64 frame, const std::string& passes_dir) {
    if (!g_bridge) return;
    g_bridge->dump_passes = passes_dir;
    g_bridge->dump_frame = (long long)frame;
    g_bridge->dump_last = (long long)frame + 1;     // a game drawing a picture every other blank
}

std::string GpuCaptureScreen(const std::string& dir) {
    if (!g_bridge) return "no GPU";
    Bridge& s = *g_bridge;
    u32 fbp = 0, fbw = 0, psm = 0;
    gs::TheGs().Displayed(&fbp, &fbw, &psm);
    std::vector<u32> pixels;
    u32 w = 0, h = 0;
    char text[160];
    const ps2gpu::GsScale k = s.renderer->Scale();
    // The lines the picture has (224 for a game drawing fields: ZOE), each written twice then - the picture as the
    // window shows it, not squeezed into the top half of a 448-line one (his session 2026-11-20).
    const u32 lines = std::min(gs::TheGs().ShownLines(), 448u);
    const bool fields = lines >= 64 && lines < 300;
    auto write = [&](const std::string& path, u32 cw, u32 ch) {
        if (!fields) {
            WriteBmp(path, pixels, w, cw, ch);
            return;
        }
        std::vector<u32> doubled(std::size_t(cw) * ch * 2);
        for (u32 y = 0; y < ch; ++y)
            for (u32 r = 0; r < 2; ++r)
                std::memcpy(&doubled[(std::size_t(y) * 2 + r) * cw], &pixels[std::size_t(y) * w], std::size_t(cw) * 4);
        WriteBmp(path, doubled, cw, cw, ch * 2);
    };
    if (s.renderer->ReadTargetFull(fbp, fbw, psm, &pixels, &w, &h))
        write(dir + "/screen.bmp", std::min(w, ps2gpu::ScaleSize(640u, k.x)),
              std::min(h, ps2gpu::ScaleSize(fields ? lines : 448u, k.y)));
    if (s.renderer->ReadTarget(fbp, fbw, psm, &pixels, &w, &h))
        write(dir + "/screen_1x.bmp", std::min(w, 640u), std::min(h, fields ? lines : 448u));
    std::snprintf(text, sizeof text, "shown: FBP %X FBW %u PSM %02X; internal resolution %.3fx %.3fx", unsigned(fbp),
                  unsigned(fbw), unsigned(psm), k.x / double(kOne), k.y / double(kOne));
    return text;
}

void GpuShadowFrame(u64 frame, const std::string& dump_dir) {
    if (!g_bridge) return;
    Bridge& s = *g_bridge;
    if (s.primary) {
        if (s.Dumping() && std::get<0>(s.last_target) != ~0u) DumpPass(s, s.last_target);
        s.frame = frame + 1;
        // The frame's drawing to the GPU now, to run while the next is made,
        // rather than when something next waits on the GPU.
        {
            Timed timed(Section::GpuRender);
            s.renderer->Kick();
        }
        if (frame % 100 == 0)
            std::fprintf(stderr, "[gpu] frame %llu: %llu draws, %llu write-backs, %llu reloads, %llu decodes, %zu images, %zu textures\n",
                         static_cast<unsigned long long>(frame), static_cast<unsigned long long>(s.serial),
                         static_cast<unsigned long long>(s.write_backs), static_cast<unsigned long long>(s.reloads),
                         static_cast<unsigned long long>(s.decodes), s.owned.size(), s.textures.size());
        if (frame % 300 == 0 && std::getenv("PS2HOST_GPU_PROFILE"))
            std::fprintf(stderr, "[gpu] frame %llu: %s\n", static_cast<unsigned long long>(frame),
                         ps2gpu::GpuProfileReport().c_str());
        if (frame % 100 == 0 && std::getenv("PS2HOST_GPU_COUNT")) {
            std::fprintf(stderr, "[gpu]   last 100 frames: %s\n", ps2gpu::RendererCounters().c_str());
            static u64 last_partial[3] = {};
            const u64* p = s.partial_texels;
            std::fprintf(stderr, "[gpu]   GPU decodes: %llu, %.1f M texels of %.1f M whole (lazy decode)\n",
                         (unsigned long long)(p[0] - last_partial[0]), double(p[1] - last_partial[1]) / 1e6,
                         double(p[2] - last_partial[2]) / 1e6);
            std::memcpy(last_partial, p, sizeof last_partial);
            std::vector<std::pair<u64, std::string>> kinds;
            for (const auto& [k, n] : g_decode_kinds) kinds.emplace_back(n, k);
            std::sort(kinds.rbegin(), kinds.rend());
            for (std::size_t i = 0; i < kinds.size() && i < 12; ++i)
                std::fprintf(stderr, "[gpu]     GPU decodes %llu: %s\n", (unsigned long long)kinds[i].first, kinds[i].second.c_str());
            g_decode_kinds.clear();
        }
        static const bool gpu_log = std::getenv("PS2HOST_GPU_LOG") != nullptr;
        if (gpu_log) {
            std::fprintf(stderr, "[gpu]   KB to the CPU: %llu for textures, %llu for reads, %llu for writes;",
                         static_cast<unsigned long long>(s.downloads_by[0] / 4),
                         static_cast<unsigned long long>(s.downloads_by[1] / 4),
                         static_cast<unsigned long long>(s.downloads_by[2] / 4));
            for (const auto& [kind, blocks] : s.texture_downloads)
                std::fprintf(stderr, " psm %02X tbw %u: %llu KB", unsigned(kind >> 16), unsigned(kind & 0xFFFF),
                             static_cast<unsigned long long>(blocks / 4));
            std::fprintf(stderr, "\n");
        }
        if (gpu_log)
            for (const auto& [key, o] : s.owned)
                std::fprintf(stderr, "[gpu]   %s %X/%u/%02X blocks %X-%X drawn to %X, %ux%u; reloads new %llu memory %llu draw %llu write-back %llu\n",
                             o.depth ? "Z" : "C", unsigned(o.fbp), unsigned(o.fbw), unsigned(o.psm), unsigned(o.begin),
                             unsigned(o.end), unsigned(o.drawn_end), unsigned(o.x1), unsigned(o.y1),
                             static_cast<unsigned long long>(o.reloads[0]), static_cast<unsigned long long>(o.reloads[1]),
                             static_cast<unsigned long long>(o.reloads[2]), static_cast<unsigned long long>(o.reloads[3]));
        return;
    }
    if (!dump_dir.empty()) s.dump_dir = dump_dir;
    s.renderer->Flush();
    const gs::Gs& g = gs::TheGs();
    u32 shown_fbp = 0, shown_fbw = 0, shown_psm = 0;
    g.Displayed(&shown_fbp, &shown_fbw, &shown_psm);
    std::string line;
    u64 total = 0, close = 0;
    for (const auto& [key, extent] : s.drawn) {
        const auto [fbp, fbw, psm] = key;
        std::vector<u32> pixels;
        u32 w = 0, h = 0;
        if (!s.renderer->ReadTarget(fbp, fbw, psm, &pixels, &w, &h)) continue;
        const u32 x1 = std::min(extent.x1, w), y1 = std::min(extent.y1, h);
        const bool sixteen = psm == gs::PSMCT16 || psm == gs::PSMCT16S;
        const int slack = sixteen ? 8 : 2;
        u64 n = 0, ok = 0;
        for (u32 y = 0; y < y1; ++y)
            for (u32 x = 0; x < x1; ++x) {
                const u32 want = g.FramePixel(fbp, fbw, psm, x, y), got = pixels[std::size_t(y) * w + x];
                bool same = true;
                for (int shift = 0; shift < (psm == gs::PSMCT32 ? 32 : 24); shift += 8) {   // alpha too where it is stored
                    const int a = int((want >> shift) & 0xFF), b = int((got >> shift) & 0xFF);
                    if (std::abs(a - b) > slack) same = false;
                }
                ++n;
                ok += same;
            }
        total += n;
        close += ok;
        char text[96];
        std::snprintf(text, sizeof text, " %X/%u/%02X %ux%u %.1f%%", unsigned(fbp), unsigned(fbw), unsigned(psm),
                      unsigned(x1), unsigned(y1), n ? 100.0 * double(ok) / double(n) : 100.0);
        line += text;
        if (!s.dump_dir.empty() && x1 && y1) {
            char name[96];
            std::snprintf(name, sizeof name, "/gpu_%06llu_%X.bmp", static_cast<unsigned long long>(frame),
                          unsigned(fbp));
            WriteBmp(s.dump_dir + name, pixels, w, x1, y1);
            // The software GS's, beside it.
            std::vector<u32> reference(std::size_t(x1) * y1);
            for (u32 y = 0; y < y1; ++y)
                for (u32 x = 0; x < x1; ++x) reference[std::size_t(y) * x1 + x] = g.FramePixel(fbp, fbw, psm, x, y);
            std::snprintf(name, sizeof name, "/soft_%06llu_%X.bmp", static_cast<unsigned long long>(frame),
                          unsigned(fbp));
            WriteBmp(s.dump_dir + name, reference, x1, x1, y1);
        }
    }
    if (total) {
        ++s.frames;
        s.agreement_sum += double(close) / double(total);
        std::fprintf(stderr, "[gpu-shadow] frame %llu:%s (shown %X)\n", static_cast<unsigned long long>(frame),
                     line.c_str(), unsigned(shown_fbp));
    }
    if (!s.drawn.empty()) s.dump_dir.clear();
    s.drawn.clear();
    s.depth_seeded.clear();
}

std::string GpuShadowSummary() {
    if (!g_bridge) return "";
    char text[320];
    if (g_bridge->primary) {
        std::snprintf(text, sizeof text,
                      "gpu: %llu images into GPU local memory, %llu out of it; %llu KB to the CPU, %llu KB"
                      " from it; %llu textures decoded (%llu more found unchanged), %llu on the GPU;"
                      " %llu CLUTs loaded on the GPU",
                      static_cast<unsigned long long>(g_bridge->write_backs),
                      static_cast<unsigned long long>(g_bridge->reloads),
                      static_cast<unsigned long long>(g_bridge->downloads / 4),
                      static_cast<unsigned long long>(g_bridge->uploads / 4),
                      static_cast<unsigned long long>(g_bridge->decodes),
                      static_cast<unsigned long long>(g_bridge->unchanged),
                      static_cast<unsigned long long>(g_bridge->gpu_decodes),
                      static_cast<unsigned long long>(g_bridge->gpu_clut_loads));
        std::string summary = std::string(text) + "\ngpu: " + ps2gpu::SubmitStats();
        std::snprintf(text, sizeof text,
                      "\ngpu: fast path %llu primitives; missed: %llu none to repeat, %llu CLUT loads only, %llu CLUT"
                      " loads + TEX0's CLUT fields, %llu other changes, %llu other registers, %llu the rest",
                      static_cast<unsigned long long>(g_fast_hits), static_cast<unsigned long long>(g_fast_misses[0]),
                      static_cast<unsigned long long>(g_fast_misses[1]), static_cast<unsigned long long>(g_fast_misses[2]),
                      static_cast<unsigned long long>(g_fast_misses[3]), static_cast<unsigned long long>(g_fast_misses[4]),
                      static_cast<unsigned long long>(g_fast_misses[5]));
        summary += text;
        {
            static const char* const kRegNames[12] = {"prim", "frame", "scissor", "tex0", "tex1", "clamp", "alpha",
                                                      "test", "zbuf", "texa", "fogcol", "miptbp"};
            std::string line = "\ngpu: other registers differ:";
            for (int i = 0; i < 12; ++i)
                line += " " + std::string(kRegNames[i]) + " " + std::to_string(g_miss_reg[i]);
            line += "; one register only " + std::to_string(g_miss_single) + "; a ring of 8 would hit " +
                    std::to_string(g_miss_ring_hit) + "; the rest: context " + std::to_string(g_miss_rest[0]) +
                    ", more rows " + std::to_string(g_miss_rest[1]) + ", dumping " + std::to_string(g_miss_rest[2]) +
                    ", stale source " + std::to_string(g_miss_rest[3]) + ", partial texture " +
                    std::to_string(g_miss_rest[4]);
            static const char* const kSiteNames[kMutSites] = {
                "mark-stale", "write-back", "sync", "sync-write", "use-check", "drew-outside", "load-clut",
                "replacement", "new-texture", "texture-invalid", "clut-rows", "evict", "newer-epoch"};
            line += "\ngpu: mutation sites (all / last before an other-changes miss):";
            for (int i = 0; i < kMutSites; ++i)
                line += " " + std::string(kSiteNames[i]) + " " + std::to_string(g_mutation_site_count[i]) + "/" +
                        std::to_string(g_miss_site[i]);
            summary += line;
        }
        if (texture_pack::Count())
            summary += "\ngpu: texture pack of " + std::to_string(texture_pack::Count()) + " files, " +
                       std::to_string(g_bridge->replacements) + " textures replaced";
        return summary;
    }
    if (!g_bridge->frames) return "";
    std::snprintf(text, sizeof text, "gpu-shadow: %llu frames compared, %.1f%% of pixels agree on average",
                  static_cast<unsigned long long>(g_bridge->frames), 100.0 * g_bridge->agreement_sum / double(g_bridge->frames));
    return text;
}

#else

bool StartGpuRenderer(std::string* error, GpuMode, bool) {
    if (error) *error = "built without the GPU renderer (PS2HOST_VULKAN)";
    return false;
}
void GpuSetScale(double) {}
std::string RenderResolutionText() { return "640X448"; }
bool StepRenderScale(int) { return false; }
void GpuShadowFrame(gs::u64, const std::string&) {}
std::string GpuShadowSummary() { return ""; }
u64 GpuBusyMicroseconds() { return 0; }
std::string GpuProfileText() { return ""; }

#endif

}  // namespace ps2host
