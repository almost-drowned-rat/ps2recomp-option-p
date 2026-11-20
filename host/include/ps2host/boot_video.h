/*
 * The optional boot animation of a port, hidden (maintainer 2026-11-17 for the PS2 ports: no BOOT ANIMATION line
 * in RECOMP OPTIONS, no media folder): a boot.mpg (MPEG-1 video + MP2 audio, 1920x1080 at 60 fps) in the game's
 * own folder, beside the executable, plays in the window before the game starts and cannot be skipped; without the
 * file nothing plays - the file is the switch. Decoded on the CPU with pl_mpeg (MIT, host/third_party/pl_mpeg),
 * its sound through SDL3, its pictures to the window (Display::Present).
 *
 * Nothing here writes, converts or deletes a file. A boot.mp4 / boot.mkv without a boot.mpg is only reported.
 */
#ifndef PS2HOST_BOOT_VIDEO_H
#define PS2HOST_BOOT_VIDEO_H

#include <string>

namespace ps2host {

/* The boot file: boot.mpg beside the executable. "found": it is there; "convert": only a boot.mp4 / .mkv is;
   "none": nothing. */
enum class BootVideoState { kFound, kConvert, kNone };
BootVideoState FindBootVideo(std::string* path = nullptr);

/* Plays boot.mpg in the window to its end (or until the window closes) when the file is there. True when it
   played. Blocks; called before the game starts. */
bool PlayBootVideo();

}  // namespace ps2host

#endif  /* PS2HOST_BOOT_VIDEO_H */
