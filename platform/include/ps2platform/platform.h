/*
 * The operating system under the host and the GPU display: everything of the
 * host's that is not standard C++ (or SDL3, which already hides each
 * platform's window, input and sound) goes through here, so that a port for
 * another system (Linux, Android: docs/option_p/portability_handoff.md) adds
 * one file - src/platform_<system>.cpp - and leaves the host as it is.
 *
 * Windows is the only implementation (src/platform_win32.cpp). What has no
 * counterpart elsewhere says so; what the Windows build alone has (the
 * sampler and the crash report of profile.cpp, the processor check of
 * cpu_check.cpp, the GDI window, WASAPI's sound, Winsock) is listed in the
 * hand-off document with the way the other systems do it.
 *
 * The calls the scheduler and the timers make each frame (SwitchFiber,
 * WaitOnValue, SleepUntil) are as thin as the Win32 calls they wrap: a fiber
 * is the system's own handle, nothing added on a switch.
 */

#ifndef PS2PLATFORM_PLATFORM_H
#define PS2PLATFORM_PLATFORM_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

namespace ps2platform {

// ---- Fibers: the guest's threads and coroutines, switched by hand (threads.cpp).
// A fiber's entry; it never returns (it switches away for the last time).
using FiberEntry = void (*)(void* parameter);
struct Fiber;   // the system's own (Windows: the fiber's address)
// The calling thread as a fiber (the scheduler's own), once per thread.
Fiber* ThreadAsFiber();
// A new fiber with a stack of `stack_bytes`, starting at entry(parameter) when first switched to; null if none.
Fiber* CreateFiber(std::size_t stack_bytes, FiberEntry entry, void* parameter);
// Run `to` from where it stopped; returns when something switches back to this one.
void SwitchFiber(Fiber* to);
// A fiber that is not running.
void DeleteFiber(Fiber* fiber);

// ---- Waiting.
// Asleep until `value` is not `seen` any more (or `timeout_ms` passed): Windows' WaitOnAddress, a futex elsewhere.
void WaitOnValue(std::atomic<std::uint32_t>& value, std::uint32_t seen, std::uint32_t timeout_ms);
// Every thread waiting on `value` woken.
void WakeAll(std::atomic<std::uint32_t>& value);
// Asleep until `when`, to a tenth of a millisecond or so (Windows: a high-resolution waitable timer, per thread;
// elsewhere clock_nanosleep is that precise already); sleep_until where there is no such timer.
void SleepUntil(std::chrono::steady_clock::time_point when);

// ---- The process's real time (StartRealTimeThreads).
// Sleeps to the millisecond (Windows' timer resolution, 15.6 ms otherwise), kept with the window behind others,
// and the process never throttled for power. Nothing to do where sleeps are fine already.
void FineTimers();

// ---- Threads.
enum class Priority { kNormal, kAboveNormal, kHighest, kTimeCritical };
// The calling thread's priority (Linux: a nice value or SCHED_FIFO where allowed; Android: the thread's priority).
void SetThreadPriority(Priority priority);
// The calling thread never slowed down for power (Windows' power throttling; nothing elsewhere).
void NoPowerThrottling();
// A name for the calling thread: debuggers, profilers - and the per-thread CPU time below (ThreadCpuNs).
// Returns a handle for ThreadCpuNs, or null where there is none.
void* NameThread(const char* name);
// The CPU time (user + kernel, ns) of a thread NameThread named; false where it cannot be had.
bool ThreadCpuNs(void* thread, std::uint64_t* total_ns, std::uint64_t* kernel_ns);
// The CPU time every thread of the process has had (ns); 0 where it cannot be had.
std::uint64_t ProcessCpuNs();

// ---- Cores.
// The logical cores the process may run on (its affinity), hardware_concurrency where that cannot be had.
int ProcessCores();
// The process held to the logical cores in `mask` (bit n: core n); false if not done.
bool HoldToCores(std::uint64_t mask);

// ---- Files and the process.
// The running executable's own path; its folder holds what belongs to it (the .pdb, an -sse4 build).
std::filesystem::path ExePath();
inline std::filesystem::path ExeDir() { return ExePath().parent_path(); }
// The port's root: where it keeps its settings, saves, game\, data\, boot.mpg. The executable's folder - or, when
// that folder is named "data" (any case), its parent: a release's top folder holds only <game>_launcher, the game
// <game> lives in data\ (the maintainer's rule 2026-11-21, all ports and platforms).
inline std::filesystem::path RootDir() {
    const std::filesystem::path dir = ExeDir();
    std::string name = dir.filename().string();
    for (char& ch : name) ch = char(ch >= 'A' && ch <= 'Z' ? ch + 32 : ch);
    return name == "data" && dir.has_parent_path() ? dir.parent_path() : dir;
}
// Whether a file or folder is there.
bool Exists(const std::string& path);
// Case-insensitive ASCII compare (0: equal).
int CompareNoCase(const char* a, const char* b);
// A program built windowed (no console of its own) started from a terminal: its output to that terminal.
void AttachTerminal();
// stdout and stderr to `log` (truncated), unbuffered: a windowed run's log beside the game.
void RedirectOutput(const std::string& log);
// The machine for a log's head, a line each (the maintainer 2026-11-20, issue 10): the OS and its version (Wine's
// and Proton's under Wine), the processor, its threads, the memory, the power plan.
std::vector<std::string> SystemReport();
// The same program started again with the same command line (RECOMP OPTIONS' RESTART NOW); false if it failed.
bool RestartSelf();
// The process ended at once with `code`, nothing torn down (main.cpp says why).
[[noreturn]] void ExitNow(int code);
// The local time, as std::localtime would give it, but safe across threads.
std::tm LocalTime(std::time_t when);

// ---- Dialogs (the player's PC, before the window opens).
// A message box with OK; `error` for the error icon, a warning's otherwise.
void ShowMessage(const char* title, const std::string& text, bool error);
// An open-file dialog: `filter` in Win32's form ("Name\0*.a;*.b\0...\0\0"); "" if cancelled.
std::string PickFile(const char* title, const char* filter);

// ---- Sound out (audio.cpp's audio thread): float stereo at `rate`, pulled by render(user, out, frames) on the
// calling thread until *stop; *opened set to 1 once a device plays, -1 if none could be opened. Windows
// (src/audio_win32.cpp): WASAPI shared mode, waveOut where that cannot open (or `wave_only`). Elsewhere: SDL3's
// audio stream, pulled the same way.
struct AudioOutput {
    int rate = 48000;
    void (*render)(void* user, float* out, unsigned frames) = nullptr;
    void* user = nullptr;
    const std::atomic<bool>* stop = nullptr;
    std::atomic<int>* opened = nullptr;
    unsigned target_ms = 0;             // for the log line
    bool wave_only = false;             // PS2HOST_AUDIO_WAVEOUT
};
void RunAudioOutput(const AudioOutput& output);

// ---- Text for the window's overlay (the PC settings panel, notices; window.cpp).
// `lines` drawn in a bold fixed-pitch font `line_px` pixels a line, line i in colours[i] (0xRRGGBB), on a panel of
// `panel` (0xRRGGBB), the panel as wide as the longest line and at most max_width x max_height: opaque 0xAARRGGBB
// pixels (B in the low byte), rows top down. False where the system has no font to draw with (then: a font
// rasteriser, stb_truetype with a bundled font, in the other system's file).
bool DrawTextPanel(const std::vector<std::string>& lines, int line_px, const std::vector<std::uint32_t>& colours,
                   std::uint32_t panel, int max_width, int max_height, std::vector<std::uint32_t>* pixels, int* width,
                   int* height);

// ---- The display's vertical blank (FRAME PACING's DISPLAY source, the maintainer 2026-11-22; option-c's
// host_vblank as the model): the guest's blank on the display's own instead of a 59.94 Hz timer, so a picture
// stays exactly n refreshes on the screen. Linux: DRM's wait for the vblank on the first active CRTC; Android: the
// Choreographer's frame callbacks; Windows: D3DKMTWaitForVerticalBlankEvent (gdi32) on the primary display.
// Open once, then one thread waits; *steady_ns = the blank's time on std::chrono::steady_clock. False: none on this
// system or display (or it stopped coming) - the caller keeps its timer.
bool OpenDisplayVblank();
bool WaitDisplayVblank(std::uint64_t* steady_ns);

// ---- The fallback window (no Vulkan device to be had): the picture drawn by the system itself.
// Windows: a GDI window. Elsewhere none - false, and the port runs without a window (SDL3 and Vulkan are the way).
bool OpenFallbackWindow(const wchar_t* title, int width, int height);
bool FallbackWindowOpen();
bool FallbackWindowFocused();
// Its messages handled: *closed set when the player closed it (FallbackWindowOpen false from then on).
void PumpFallbackWindow(bool* closed);
// `pixels` (w x h, 0x00RRGGBB, rows top down) scaled to the window.
void DrawFallbackWindow(const std::uint32_t* pixels, std::uint32_t w, std::uint32_t h);

}  // namespace ps2platform

#endif
