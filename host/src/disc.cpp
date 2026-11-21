#include "ps2host/disc.h"

#include "libchdr/cdrom.h"
#include "libchdr/chd.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>

namespace ps2host {

namespace {

u32 Le32(const u8* p) { return u32(p[0]) | u32(p[1]) << 8 | u32(p[2]) << 16 | u32(p[3]) << 24; }

std::string Upper(std::string s) {
    for (char& ch : s) ch = char(std::toupper(static_cast<unsigned char>(ch)));
    return s;
}

// A directory record's name without its version (";1") and trailing dot.
std::string Bare(std::string name) {
    const auto semicolon = name.find(';');
    if (semicolon != std::string::npos) name.resize(semicolon);
    if (!name.empty() && name.back() == '.') name.pop_back();
    return Upper(name);
}

}  // namespace

Disc::~Disc() { CloseImage(); }

void Disc::CloseImage() {
    if (file_) std::fclose(file_);
    file_ = nullptr;
    for (Run& r : runs_)
        if (r.file) std::fclose(r.file);
    runs_.clear();
    unpacked_ = unpacked_cd_ = false;
    if (chd_) chd_close(static_cast<chd_file*>(chd_));
    chd_ = nullptr;
    chd_hunk_.clear();
    chd_hunk_index_ = ~0u;
}

namespace {

// The image a .cue sheet names in its first FILE line, beside the sheet.
std::string CueImage(const std::string& cue_path) {
    std::FILE* cue = std::fopen(cue_path.c_str(), "rb");
    if (!cue) return "";
    std::string text;
    char chunk[4096];
    for (std::size_t n; (n = std::fread(chunk, 1, sizeof chunk, cue)) > 0;) text.append(chunk, n);
    std::fclose(cue);
    for (std::size_t at = 0; at < text.size();) {
        std::size_t end = text.find_first_of("\r\n", at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        at = end + 1;
        line.erase(0, line.find_first_not_of(" \t"));
        if (Upper(line.substr(0, 5)) != "FILE ") continue;
        std::string name = line.substr(5);
        name.erase(0, name.find_first_not_of(" \t"));
        if (!name.empty() && name[0] == '"') {
            const std::size_t close = name.find('"', 1);
            name = name.substr(1, close == std::string::npos ? std::string::npos : close - 1);
        } else {
            name = name.substr(0, name.find_first_of(" \t"));
        }
        const std::size_t slash = cue_path.find_last_of("/\\");
        return slash == std::string::npos ? name : cue_path.substr(0, slash + 1) + name;
    }
    return "";
}

const u8 kSync[12] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};

u8 ToBcd(u32 v) { return u8((v / 10) << 4 | (v % 10)); }

}  // namespace

bool Disc::Open(const std::string& path) {
    CloseImage();
    memory_.clear();
    std::string image = path;
    {   // an unpacked disc: its folder, or its disc.idx
        std::error_code error;
        if (!path.empty() && std::filesystem::is_directory(path, error)) return OpenUnpacked(path);
        if (image.size() >= 8 && Upper(image.substr(image.size() - 8)) == "DISC.IDX")
            return OpenUnpacked(std::filesystem::path(path).parent_path().string());
    }
    if (image.size() > 4 && Upper(image.substr(image.size() - 4)) == ".CHD") {
        chd_file* chd = nullptr;
        if (chd_open(image.c_str(), CHD_OPEN_READ, nullptr, &chd) != CHDERR_NONE || !chd) return false;
        const chd_header* h = chd_get_header(chd);
        if (!h || !h->hunkbytes || !h->unitbytes || h->hunkbytes % h->unitbytes) {
            chd_close(chd);
            return false;
        }
        chd_ = chd;
        chd_hunk_bytes_ = h->hunkbytes;
        chd_unit_bytes_ = h->unitbytes;
        // A CD's frame (2448: 2352 + 96 subcode) read as a raw sector; a DVD's unit is its 2048-byte sector.
        chd_sector_bytes_ = chd_unit_bytes_ == CD_FRAME_SIZE ? kRawSectorSize : chd_unit_bytes_;
        size_ = (h->logicalbytes / chd_unit_bytes_) * chd_sector_bytes_;
        chd_hunk_.resize(chd_hunk_bytes_);
        DetectFormat();
        return true;
    }
    if (image.size() > 4 && Upper(image.substr(image.size() - 4)) == ".CUE") {
        image = CueImage(path);
        if (image.empty()) return false;
    }
    file_ = std::fopen(image.c_str(), "rb");
    if (!file_) return false;
    _fseeki64(file_, 0, SEEK_END);
    size_ = u64(_ftelli64(file_));
    DetectFormat();
    return true;
}

void Disc::Use(std::vector<u8> image) {
    memory_ = std::move(image);
    size_ = memory_.size();
    DetectFormat();
}

// A raw image: every sector starts with the sync pattern, and sector 16 - the
// volume descriptor, on every PS2 disc - says the mode.
void Disc::DetectFormat() {
    data_offset_ = 0;
    u8 head[16];
    if (size_ >= 17 * u64(kRawSectorSize) && ReadBytes(16 * u64(kRawSectorSize), 16, head) &&
        std::memcmp(head, kSync, 12) == 0 && (head[15] == 1 || head[15] == 2))
        data_offset_ = head[15] == 1 ? 16 : 24;
    sectors_ = u32(size_ / (data_offset_ ? kRawSectorSize : kSectorSize));
}

bool Disc::IsCd() const {
    if (unpacked_) return unpacked_cd_;
    // A CD holds at most about 360,000 sectors (80 minutes); a DVD image is raw-less.
    return data_offset_ != 0 || sectors_ <= 360000;
}

// A CHD's bytes as the plain image would hold them: each sector's unit found in its hunk (decompressed once, the
// last kept - reads go on sector by sector).
bool Disc::ReadChd(u64 offset, u64 length, u8* out) {
    while (length) {
        const u64 sector = offset / chd_sector_bytes_, inner = offset % chd_sector_bytes_;
        const u64 take = std::min<u64>(length, chd_sector_bytes_ - inner);
        const u64 source = sector * chd_unit_bytes_ + inner;
        const u32 hunk = u32(source / chd_hunk_bytes_), at = u32(source % chd_hunk_bytes_);
        if (hunk != chd_hunk_index_) {
            if (chd_read(static_cast<chd_file*>(chd_), hunk, chd_hunk_.data()) != CHDERR_NONE) {
                chd_hunk_index_ = ~0u;
                return false;
            }
            chd_hunk_index_ = hunk;
        }
        std::memcpy(out, &chd_hunk_[at], std::size_t(take));
        out += take;
        offset += take;
        length -= take;
    }
    return true;
}

bool Disc::ReadBytes(u64 offset, u64 length, u8* out) {
    if (offset + length > size_) return false;
    if (chd_) return ReadChd(offset, length, out);
    if (!memory_.empty()) {
        std::memcpy(out, memory_.data() + offset, size_t(length));
        return true;
    }
    if (!file_ || _fseeki64(file_, s64(offset), SEEK_SET) != 0) return false;
    return std::fread(out, 1, size_t(length), file_) == length;
}

namespace {
std::recursive_mutex g_maps_lock;           // every Disc's maps (disc.h): changed while the game reads
}  // namespace

void Disc::ClearMaps() {
    std::lock_guard<std::recursive_mutex> hold(g_maps_lock);
    maps_.clear();
}

void Disc::MapSectors(u32 lsn, u32 count, Disc* source, u32 source_lsn) {
    if (!count || !source) return;
    std::lock_guard<std::recursive_mutex> hold(g_maps_lock);
    SectorMap m;
    m.lsn = lsn;
    m.count = count;
    m.source = source;
    m.source_lsn = source_lsn;
    maps_.push_back(std::move(m));
    source->Note(source_lsn, count);            // (what a port needs of that disc: Record)
}

void Disc::MapBytes(u32 lsn, std::vector<u8> bytes) {
    if (bytes.empty()) return;
    SectorMap m;
    m.lsn = lsn;
    m.count = u32((bytes.size() + kSectorSize - 1) / kSectorSize);
    bytes.resize(std::size_t(m.count) * kSectorSize, 0);
    m.bytes = std::move(bytes);
    std::lock_guard<std::recursive_mutex> hold(g_maps_lock);
    maps_.push_back(std::move(m));
}

u32 Disc::Sectors() const {
    std::lock_guard<std::recursive_mutex> hold(g_maps_lock);
    u32 end = sectors_;
    for (const SectorMap& m : maps_) end = std::max(end, m.lsn + m.count);
    return end;
}

// The map serving a sector, the latest first; null: the image's own.
const Disc::SectorMap* Disc::MapOf(u32 lsn) const {
    for (auto it = maps_.rbegin(); it != maps_.rend(); ++it)
        if (lsn >= it->lsn && lsn - it->lsn < it->count) return &*it;
    return nullptr;
}

bool Disc::Read(u32 lsn, u32 count, u8* out) {
    last_lsn_ = lsn;
    Note(lsn, count);
    if (maps_.empty()) return ReadOwn(lsn, count, out);
    std::lock_guard<std::recursive_mutex> hold(g_maps_lock);
    if (u64(lsn) + count > Sectors()) return false;
    // Run by run: the image's own sectors read together, a mapped one from its map.
    for (u32 i = 0; i < count;) {
        const SectorMap* m = MapOf(lsn + i);
        u8* dst = out + std::size_t(i) * kSectorSize;
        if (m) {
            const u32 at = lsn + i - m->lsn;
            if (m->source) {
                if (!m->source->Read(m->source_lsn + at, 1, dst)) return false;
            } else {
                std::memcpy(dst, &m->bytes[std::size_t(at) * kSectorSize], kSectorSize);
            }
            ++i;
            continue;
        }
        u32 run = 1;
        while (i + run < count && !MapOf(lsn + i + run)) ++run;
        if (!ReadOwn(lsn + i, run, dst)) return false;
        i += run;
    }
    return true;
}

bool Disc::ReadOwn(u32 lsn, u32 count, u8* out) {
    if (u64(lsn) + count > sectors_) return false;
    if (unpacked_) return ReadUnpacked(lsn, count, out);
    if (!data_offset_) return ReadBytes(u64(lsn) * kSectorSize, u64(count) * kSectorSize, out);
    // Raw: read the whole run at once, then keep each sector's data.
    std::vector<u8> raw(std::size_t(count) * kRawSectorSize);
    if (!ReadBytes(u64(lsn) * kRawSectorSize, raw.size(), raw.data())) return false;
    for (u32 i = 0; i < count; ++i)
        std::memcpy(out + std::size_t(i) * kSectorSize, &raw[std::size_t(i) * kRawSectorSize + data_offset_],
                    kSectorSize);
    return true;
}

bool Disc::ReadPattern(u32 lsn, u32 count, u32 pattern, u8* out) {
    if (pattern != 1 && pattern != 2) return Read(lsn, count, out);
    last_lsn_ = lsn;
    Note(lsn, count);
    std::lock_guard<std::recursive_mutex> hold(g_maps_lock);
    if (u64(lsn) + count > Sectors()) return false;
    const u32 size = PatternSize(pattern);
    // 2340 starts at the header (offset 12), 2328 after the subheader (24).
    const u32 from = pattern == 2 ? 12 : 24;
    std::vector<u8> raw(kRawSectorSize);
    for (u32 i = 0; i < count; ++i) {
        u8* dst = out + std::size_t(i) * size;
        if (data_offset_ && !MapOf(lsn + i)) {              // (a mapped sector: made around its data, below)
            if (!ReadBytes(u64(lsn + i) * kRawSectorSize, kRawSectorSize, raw.data())) return false;
            std::memcpy(dst, &raw[from], size);
            continue;
        }
        // An .iso: a Mode 2 Form 1 sector made around its data.
        std::memset(raw.data(), 0, raw.size());
        const u32 address = lsn + i + 150;          // the 2-second lead-in
        raw[12] = ToBcd(address / 75 / 60);
        raw[13] = ToBcd(address / 75 % 60);
        raw[14] = ToBcd(address % 75);
        raw[15] = 2;
        raw[18] = raw[22] = 0x08;                   // subheader submode: data
        if (!Read(lsn + i, 1, &raw[24])) return false;
        std::memcpy(dst, &raw[from], size);
    }
    return true;
}

bool Disc::Find(const std::string& path, File* out) {
    // The primary volume descriptor, sector 16, holds the root directory's record.
    std::vector<u8> sector(kSectorSize);
    if (!Read(16, 1, sector.data()) || sector[0] != 1 || std::memcmp(&sector[1], "CD001", 5) != 0)
        return false;
    File directory{Le32(&sector[156 + 2]), Le32(&sector[156 + 10])};
    std::string rest = path;
    for (char& ch : rest) if (ch == '/') ch = '\\';
    while (!rest.empty()) {
        while (!rest.empty() && rest[0] == '\\') rest.erase(0, 1);
        if (rest.empty()) break;
        const auto end = rest.find('\\');
        const std::string part = Bare(rest.substr(0, end));
        rest = end == std::string::npos ? "" : rest.substr(end);
        // Scan the directory's records for this part.
        const u32 sectors = (directory.size + kSectorSize - 1) / kSectorSize;
        std::vector<u8> data(size_t(sectors) * kSectorSize);
        if (!Read(directory.lsn, sectors, data.data())) return false;
        bool found = false;
        for (u32 at = 0; at < directory.size;) {
            const u8 length = data[at];
            if (length == 0) {                  // records never cross a sector boundary
                at = (at / kSectorSize + 1) * kSectorSize;
                continue;
            }
            const u8 name_length = data[at + 32];
            const std::string name(reinterpret_cast<const char*>(&data[at + 33]), name_length);
            if (name_length > 1 || (name_length == 1 && name[0] > 1)) {
                if (Bare(name) == part) {
                    directory = File{Le32(&data[at + 2]), Le32(&data[at + 10])};
                    found = true;
                    break;
                }
            }
            at += length;
        }
        if (!found) return false;
    }
    *out = directory;
    return true;
}

bool Disc::ReadFile(const std::string& path, std::vector<u8>* out) {
    File file;
    if (!Find(path, &file)) return false;
    out->assign(size_t(file.size + kSectorSize - 1) / kSectorSize * kSectorSize, 0);
    if (!Read(file.lsn, u32(out->size() / kSectorSize), out->data())) return false;
    out->resize(file.size);
    return true;
}

std::string Disc::BootPath() {
    std::vector<u8> cnf;
    if (!ReadFile("SYSTEM.CNF", &cnf)) return "";
    const std::string text(cnf.begin(), cnf.end());
    for (std::size_t at = 0; at < text.size();) {
        std::size_t end = text.find_first_of("\r\n", at);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(at, end - at);
        at = end + 1;
        // BOOT2 = cdrom0:\SLUS_210.50;1
        const std::size_t equals = line.find('=');
        if (equals == std::string::npos) continue;
        std::string key = line.substr(0, equals);
        key.erase(key.find_last_not_of(" \t") + 1);
        if (Upper(key) != "BOOT2") continue;
        std::string value = line.substr(equals + 1);
        value.erase(0, value.find_first_not_of(" \t"));
        value.erase(value.find_last_not_of(" \t") + 1);
        const std::size_t colon = value.find(':');
        return colon == std::string::npos ? value : value.substr(colon + 1);
    }
    return "";
}

// ---- Unpacked discs (disc.h).

namespace {
const char kIndexHead[] = "ps2-unpacked-disc 1";
}  // namespace

bool Disc::OpenUnpacked(const std::string& folder) {
    std::FILE* f = std::fopen((std::filesystem::path(folder) / "disc.idx").string().c_str(), "rb");
    if (!f) return false;
    std::string text;
    char chunk[4096];
    for (std::size_t n; (n = std::fread(chunk, 1, sizeof chunk, f)) > 0;) text.append(chunk, n);
    std::fclose(f);
    std::vector<Run> runs;
    u32 sectors = 0;
    bool cd = false, head = false;
    for (std::size_t at = 0; at < text.size();) {
        std::size_t end = text.find_first_of("\r\n", at);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(at, end - at);
        at = end + 1;
        if (line == kIndexHead) {
            head = true;
        } else if (line.rfind("sectors ", 0) == 0) {
            sectors = u32(std::strtoul(line.c_str() + 8, nullptr, 10));
        } else if (line.rfind("cd ", 0) == 0) {
            cd = line.size() > 3 && line[3] == '1';
        } else if (line.rfind("run ", 0) == 0) {
            Run r;
            char* p = nullptr;
            r.lsn = u32(std::strtoul(line.c_str() + 4, &p, 10));
            r.count = u32(std::strtoul(p, &p, 10));
            r.bytes = std::strtoull(p, &p, 10);
            if (!p || *p != ' ') return false;
            r.path = (std::filesystem::path(folder) / std::filesystem::path(std::string(p + 1))).string();
            runs.push_back(std::move(r));
        }
    }
    if (!head || !sectors) return false;
    std::sort(runs.begin(), runs.end(), [](const Run& a, const Run& b) { return a.lsn < b.lsn; });
    runs_ = std::move(runs);
    unpacked_ = true;
    unpacked_cd_ = cd;
    sectors_ = sectors;
    size_ = u64(sectors) * kSectorSize;
    data_offset_ = 0;
    return true;
}

bool Disc::ReadUnpacked(u32 lsn, u32 count, u8* out) {
    for (u32 i = 0; i < count;) {
        const u32 at = lsn + i;
        auto next = std::upper_bound(runs_.begin(), runs_.end(), at, [](u32 v, const Run& r) { return v < r.lsn; });
        u8* dst = out + std::size_t(i) * kSectorSize;
        if (next != runs_.begin() && at - std::prev(next)->lsn < std::prev(next)->count) {
            Run& r = *std::prev(next);
            const u32 take = std::min(count - i, r.lsn + r.count - at);
            const u64 from = u64(at - r.lsn) * kSectorSize, want = u64(take) * kSectorSize;
            const u64 have = from >= r.bytes ? 0 : std::min<u64>(want, r.bytes - from);
            std::memset(dst + have, 0, std::size_t(want - have));
            if (have) {
                if (!r.file) r.file = std::fopen(r.path.c_str(), "rb");
                if (!r.file || _fseeki64(r.file, s64(from), SEEK_SET) != 0 ||
                    std::fread(dst, 1, std::size_t(have), r.file) != have)
                    return false;
            }
            i += take;
            continue;
        }
        // no run here: zeros up to the next one
        const u32 gap = next == runs_.end() ? count - i : std::min(count - i, next->lsn - at);
        std::memset(dst, 0, std::size_t(gap) * kSectorSize);
        i += gap;
    }
    return true;
}

std::vector<Disc::IsoFile> Disc::Files() {
    std::vector<IsoFile> out;
    std::vector<u8> pvd(kSectorSize);
    if (!ReadOwn(16, 1, pvd.data()) || pvd[0] != 1 || std::memcmp(&pvd[1], "CD001", 5) != 0) return out;
    struct Dir {
        u32 lsn, size;
        std::string prefix;
        int depth;
    };
    std::vector<Dir> todo{{Le32(&pvd[156 + 2]), Le32(&pvd[156 + 10]), "", 0}};
    std::vector<std::pair<u32, u32>> seen;
    while (!todo.empty()) {
        const Dir d = todo.back();
        todo.pop_back();
        if (d.depth > 16 || std::find(seen.begin(), seen.end(), std::make_pair(d.lsn, d.size)) != seen.end()) continue;
        seen.push_back({d.lsn, d.size});
        const u32 sectors = (d.size + kSectorSize - 1) / kSectorSize;
        if (!sectors || u64(d.lsn) + sectors > sectors_) continue;
        std::vector<u8> data(std::size_t(sectors) * kSectorSize);
        if (!ReadOwn(d.lsn, sectors, data.data())) continue;
        for (u32 at = 0; at < d.size;) {
            const u8 length = data[at];
            if (length == 0) {                      // records never cross a sector boundary
                at = (at / kSectorSize + 1) * kSectorSize;
                continue;
            }
            if (at + length > data.size() || length < 34) break;
            const u8 name_length = data[at + 32];
            std::string name(reinterpret_cast<const char*>(&data[at + 33]), name_length);
            const bool dir = (data[at + 25] & 2) != 0;
            if (name_length > 1 || (name_length == 1 && name[0] > 1)) {
                const auto semicolon = name.find(';');
                if (semicolon != std::string::npos) name.resize(semicolon);
                if (!dir && !name.empty() && name.back() == '.') name.pop_back();
                if (dir)
                    todo.push_back({Le32(&data[at + 2]), Le32(&data[at + 10]), d.prefix + name + "/", d.depth + 1});
                else
                    out.push_back({d.prefix + name, Le32(&data[at + 2]), Le32(&data[at + 10])});
            }
            at += length;
        }
    }
    std::sort(out.begin(), out.end(), [](const IsoFile& a, const IsoFile& b) { return a.lsn < b.lsn; });
    return out;
}

bool Disc::Unpack(const std::string& folder, const std::vector<std::pair<u32, u32>>* only,
                  void (*progress)(unsigned long long, unsigned long long, void*), void* user) {
    namespace fs = std::filesystem;
    const std::vector<IsoFile> files = Files();
    struct Piece {
        u32 lsn, count;
        unsigned long long bytes;
        std::string rel;
    };
    std::vector<Piece> pieces;
    auto sectors_piece = [&](u32 lsn, u32 count) {
        pieces.push_back({lsn, count, u64(count) * kSectorSize, "_sectors/" + std::to_string(lsn) + ".bin"});
    };
    auto file_end = [](const IsoFile& f) { return f.lsn + (f.size + kSectorSize - 1) / kSectorSize; };
    if (!only) {
        // Every file, then every sector outside them that holds anything: nothing lost.
        auto scan_gap = [&](u32 from, u32 to) -> bool {
            std::vector<u8> chunk(std::size_t(512) * kSectorSize);
            u32 run_at = 0, run_n = 0;
            for (u32 at = from; at < to;) {
                const u32 n = std::min<u32>(512, to - at);
                if (!ReadOwn(at, n, chunk.data())) return false;
                for (u32 k = 0; k < n; ++k) {
                    const u8* sec = &chunk[std::size_t(k) * kSectorSize];
                    if (std::all_of(sec, sec + kSectorSize, [](u8 b) { return b == 0; })) continue;
                    if (run_n && run_at + run_n == at + k) {
                        ++run_n;
                    } else {
                        if (run_n) sectors_piece(run_at, run_n);
                        run_at = at + k;
                        run_n = 1;
                    }
                }
                at += n;
            }
            if (run_n) sectors_piece(run_at, run_n);
            return true;
        };
        u32 cur = 0;
        for (const IsoFile& f : files) {
            if (f.lsn >= sectors_) continue;
            if (f.lsn > cur && !scan_gap(cur, f.lsn)) return false;
            if (f.size && file_end(f) > cur) {
                if (f.lsn >= cur) pieces.push_back({f.lsn, std::min(file_end(f), sectors_) - f.lsn, f.size, f.path});
                else sectors_piece(cur, std::min(file_end(f), sectors_) - cur);     // (overlapping files: the rest)
            }
            cur = std::max(cur, std::min(file_end(f), sectors_));
        }
        if (cur < sectors_ && !scan_gap(cur, sectors_)) return false;
    } else {
        // The ranges asked for, merged, cut at the files' bounds.
        std::vector<std::pair<u32, u32>> ranges;
        for (const auto& r : *only)
            if (r.second) ranges.push_back({r.first, r.first + r.second});
        std::sort(ranges.begin(), ranges.end());
        std::vector<std::pair<u32, u32>> merged;
        for (const auto& r : ranges) {
            if (!merged.empty() && r.first <= merged.back().second)
                merged.back().second = std::max(merged.back().second, r.second);
            else
                merged.push_back(r);
        }
        for (auto range : merged) {
            const u32 a = range.first, b = std::min(range.second, sectors_);
            for (u32 at = a; at < b;) {
                const IsoFile* in = nullptr;
                const IsoFile* next = nullptr;
                for (const IsoFile& f : files) {
                    if (f.size && at >= f.lsn && at < file_end(f)) {
                        in = &f;
                        break;
                    }
                    if (f.lsn > at && (!next || f.lsn < next->lsn)) next = &f;
                }
                if (!in) {
                    const u32 to = next ? std::min(b, next->lsn) : b;
                    sectors_piece(at, to - at);
                    at = to;
                    continue;
                }
                const u32 to = std::min(b, file_end(*in));
                if (at == in->lsn && to == file_end(*in)) {
                    pieces.push_back({at, to - at, in->size, in->path});
                } else {
                    const u64 from = u64(at - in->lsn) * kSectorSize;
                    pieces.push_back({at, to - at, std::min<u64>(u64(to - at) * kSectorSize, in->size - from),
                                      in->path + "." + std::to_string(at - in->lsn) + ".part"});
                }
                at = to;
            }
        }
    }
    unsigned long long total = 0, done = 0;
    for (const Piece& p : pieces) total += p.bytes;
    std::error_code error;
    fs::create_directories(folder, error);
    fs::remove(fs::path(folder) / "disc.idx", error);
    std::vector<u8> chunk(std::size_t(2048) * kSectorSize);
    std::string index = std::string(kIndexHead) + "\nsectors " + std::to_string(sectors_) + "\ncd " +
                        (IsCd() ? "1" : "0") + "\n";
    for (const Piece& p : pieces) {
        const fs::path to = fs::path(folder) / fs::path(p.rel);
        fs::create_directories(to.parent_path(), error);
        std::FILE* out = std::fopen(to.string().c_str(), "wb");
        if (!out) return false;
        unsigned long long left = p.bytes;
        for (u32 at = p.lsn; left;) {
            const u32 n = u32(std::min<u64>(2048, (left + kSectorSize - 1) / kSectorSize));
            if (!ReadOwn(at, n, chunk.data())) {
                std::fclose(out);
                return false;
            }
            const std::size_t put = std::size_t(std::min<u64>(left, u64(n) * kSectorSize));
            if (std::fwrite(chunk.data(), 1, put, out) != put) {
                std::fclose(out);
                return false;
            }
            left -= put;
            done += put;
            at += n;
            if (progress) progress(done, total, user);
        }
        if (std::fclose(out) != 0) return false;
        index += "run " + std::to_string(p.lsn) + " " + std::to_string(p.count) + " " + std::to_string(p.bytes) + " " +
                 p.rel + "\n";
    }
    const fs::path idx = fs::path(folder) / "disc.idx", tmp = fs::path(folder) / "disc.idx.part";
    std::FILE* f = std::fopen(tmp.string().c_str(), "wb");
    if (!f) return false;
    const bool wrote = std::fwrite(index.data(), 1, index.size(), f) == index.size();
    if (std::fclose(f) != 0 || !wrote) return false;
    fs::rename(tmp, idx, error);
    return !error;
}

namespace {
ExtraDiscUnpacker g_extra_unpacker = nullptr;
}  // namespace

void SetExtraDiscUnpacker(ExtraDiscUnpacker unpacker) { g_extra_unpacker = unpacker; }
ExtraDiscUnpacker GetExtraDiscUnpacker() { return g_extra_unpacker; }

void WriteUnpackProgress(unsigned long long done, unsigned long long total, void* path) {
    static unsigned long long last = 0;
    if (done != total && done - last < (16ull << 20) && done > last) return;      // every 16 MB
    last = done;
    const std::string& p = *static_cast<const std::string*>(path);
    if (std::FILE* f = std::fopen(p.c_str(), "wb")) {
        std::fprintf(f, "%llu %llu\n", done, total);
        std::fclose(f);
    }
}

Disc& GuestDisc() {
    static Disc disc;
    return disc;
}

}  // namespace ps2host
