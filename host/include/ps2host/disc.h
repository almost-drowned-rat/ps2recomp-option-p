/*
 * The game disc, read from the user's own image of it: 2048-byte sectors and the
 * ISO 9660 file system on them, as the CD/DVD services the game calls (through
 * the IOP) need them. Nothing from the disc is part of a port.
 *
 * An image is an .iso (2048-byte sectors, a DVD's usual form) or a raw CD image
 * (.bin of 2352-byte sectors: sync, header, Mode 1 data at 16 or Mode 2 data at
 * 24), opened directly or through the .cue sheet naming it, or a CHD (MAME's
 * compressed hunks, chdman createdvd / createcd; read through libchdr, BSD-3).
 */

#ifndef PS2HOST_DISC_H
#define PS2HOST_DISC_H

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "ps2rt/types.h"

namespace ps2host {

constexpr u32 kSectorSize = 2048;
constexpr u32 kRawSectorSize = 2352;

class Disc {
public:
    struct File {
        u32 lsn = 0;        // first sector
        u32 size = 0;       // bytes
    };

    ~Disc();

    /* Open an image file: an .iso, a raw .bin, or a .cue naming one - or an UNPACKED disc: a folder (or its
       disc.idx) Unpack wrote. */
    bool Open(const std::string& path);
    /* An unpacked disc is open (a folder, not an image). */
    bool IsUnpacked() const { return unpacked_; }

    /* ---- Unpacked discs (the maintainer 2026-11-21: the launcher unpacks the player's discs into the game's
       folder once; everything reads from there after - the images needed only for that). A folder holds the
       disc's files under their own paths and disc.idx: "ps2-unpacked-disc 1", "sectors <n>", "cd <0|1>", then
       per run "run <lsn> <count> <bytes> <path>" - sectors lsn.. from that file (its bytes; beyond them zeros).
       Sectors in no run read as zeros. */
    struct IsoFile {
        std::string path;   // "ZOE.DAT", "MOVIE/OP.PSS" (as on the disc, no version)
        u32 lsn = 0;
        u32 size = 0;
    };
    /* The ISO 9660 file system's files (the primary volume's tree). */
    std::vector<IsoFile> Files();
    /* Write this disc into `folder`: every file under its path plus every other sector that is not all zeros
       (`only` null: the whole disc, nothing lost - a disc read from the folder equals the image) - or only the
       sectors of the runs in `only` ({lsn, count}; a whole file's under its name, part of one as
       "<path>.<first sector>.part", the rest as "_sectors/<lsn>.bin"). disc.idx is written last: a folder
       without it is no disc. `progress(done, total)` (bytes) now and then, may be null. */
    bool Unpack(const std::string& folder, const std::vector<std::pair<u32, u32>>* only,
                void (*progress)(unsigned long long done, unsigned long long total, void* user) = nullptr,
                void* user = nullptr);
    /* Recording: every sector range this disc is read at (Read, ReadPattern, a map taking sectors from it) noted
       - what a port needs of a disc (ZOE's Japanese voices: Unpack(folder, &Recorded())). */
    void Record(bool on) { recording_ = on; }
    const std::vector<std::pair<u32, u32>>& Recorded() const { return recorded_; }
    /* Use an image held in memory instead (the host's own tests): 2048-byte
       sectors, or raw 2352-byte ones when it starts with a sector's sync. */
    void Use(std::vector<u8> image);
    bool IsOpen() const { return file_ != nullptr || !memory_.empty() || chd_ != nullptr || unpacked_; }
    /* A CD rather than a DVD: a raw image, or an .iso small enough for a CD. */
    bool IsCd() const;

    /* Read `count` sectors' 2048 data bytes from `lsn`; false past the end. */
    bool Read(u32 lsn, u32 count, u8* out);

    /* Sectors served from elsewhere - a multi-version port's other disc (ZOE's Japanese voices from the JP image,
       M9), a rewritten table: from `lsn` on, `count` sectors read from `source` at its `source_lsn`, or the
       `bytes` given (padded to whole sectors with zeros). They may lie past this image's end (the disc then
       reaches as far). A later map over the same sectors wins; ClearMaps drops them all. */
    void MapSectors(u32 lsn, u32 count, Disc* source, u32 source_lsn);
    void MapBytes(u32 lsn, std::vector<u8> bytes);
    void ClearMaps();
    /* (Maps may change while the game reads - a port's SELECT / debug line remapping a scene: one lock for all of
       them, recursive, a map's source read within it.) The first sector of the last Read / ReadPattern. */
    u32 LastReadLsn() const { return last_lsn_; }
    /* The disc's sectors: the image's, or as far as a map reaches. */
    u32 Sectors() const;
    /* Read sectors as sceCdRead's data pattern asks: 2048 bytes (0), 2328 (1:
       Mode 2 data with the form 2 area) or 2340 (2: header, subheader, data).
       An .iso has no raw bytes: the header is made, the subheader zero. */
    bool ReadPattern(u32 lsn, u32 count, u32 pattern, u8* out);
    static u32 PatternSize(u32 pattern) { return pattern == 1 ? 2328 : pattern == 2 ? 2340 : kSectorSize; }

    /* Look a path up - "\\DATA\\CARS.BIN;1" as the SDK writes it; separators may
       be either slash, the version suffix is optional, case does not matter. */
    bool Find(const std::string& path, File* out);
    /* A whole file's bytes (a path as Find takes it); false if it is not there. */
    bool ReadFile(const std::string& path, std::vector<u8>* out);
    /* The executable the console boots: SYSTEM.CNF's BOOT2 line, as a path on
       the disc ("\\SLUS_210.50;1"); "" when the image has none (not a PS2 disc). */
    std::string BootPath();

private:
    void DetectFormat();
    bool ReadBytes(u64 offset, u64 length, u8* out);
    bool ReadOwn(u32 lsn, u32 count, u8* out);          // the image's own sectors
    struct SectorMap {
        u32 lsn = 0, count = 0;
        Disc* source = nullptr;
        u32 source_lsn = 0;
        std::vector<u8> bytes;
    };
    const SectorMap* MapOf(u32 lsn) const;
    std::vector<SectorMap> maps_;

    std::FILE* file_ = nullptr;
    std::vector<u8> memory_;
    // A CHD: its file (libchdr's chd_file), a hunk's bytes, a unit's (2048 a DVD's sector, 2448 a CD's frame:
    // 2352 + 96 subcode - read as 2352-byte raw sectors), the hunk last read kept.
    void* chd_ = nullptr;
    u32 chd_hunk_bytes_ = 0, chd_unit_bytes_ = 0, chd_sector_bytes_ = 0;
    std::vector<u8> chd_hunk_;
    u32 chd_hunk_index_ = ~0u;
    bool ReadChd(u64 offset, u64 length, u8* out);
    void CloseImage();
    u64 size_ = 0;
    u32 data_offset_ = 0;       // within a raw sector: 16 (Mode 1), 24 (Mode 2); 0 for an .iso
    u32 sectors_ = 0;
    u32 last_lsn_ = 0;          // LastReadLsn
    // An unpacked disc: its runs (by lsn), each file opened when first read.
    struct Run {
        u32 lsn = 0, count = 0;
        unsigned long long bytes = 0;
        std::string path;
        std::FILE* file = nullptr;
    };
    bool OpenUnpacked(const std::string& folder);
    bool ReadUnpacked(u32 lsn, u32 count, u8* out);
    bool unpacked_ = false, unpacked_cd_ = false;
    std::vector<Run> runs_;
    bool recording_ = false;
    std::vector<std::pair<u32, u32>> recorded_;
    void Note(u32 lsn, u32 count) {
        if (recording_ && count) recorded_.push_back({lsn, count});
    }
};

/* The disc the running game reads. */
Disc& GuestDisc();

/* A port's second disc unpacked (`game.exe --unpack-extra <image> <folder> --disc <its own disc>`, the launcher's
   step): only what the port reads of it - ZOE's Japanese voices (dub.cpp records the sectors its voices need and
   unpacks those). The port's function: the image, the folder; true when written. */
using ExtraDiscUnpacker = bool (*)(const std::string& image, const std::string& folder);
void SetExtraDiscUnpacker(ExtraDiscUnpacker unpacker);
ExtraDiscUnpacker GetExtraDiscUnpacker();
/* A progress file beside an unpacking folder ("<folder>.progress": "<done> <total>" bytes), for the launcher. */
void WriteUnpackProgress(unsigned long long done, unsigned long long total, void* folder_progress_path);

}  // namespace ps2host

#endif  /* PS2HOST_DISC_H */
