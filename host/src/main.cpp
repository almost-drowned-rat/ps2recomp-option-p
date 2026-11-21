/*
 * Every game port's entry point. The game supplies its translated functions (the
 * table engine/tools/port.py generates) and its overrides; the rest is shared.
 */

#include <cctype>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "ps2host/crash_report.h"
#include "ps2host/disc.h"
#include "ps2host/host.h"
#include "ps2host/run.h"
#include "ps2host/settings.h"
#include "ps2host/vif.h"
#include "ps2platform/platform.h"

#ifdef __ANDROID__
#include <SDL3/SDL_main.h>     // main() is SDL_main: SDL's activity calls it
#endif

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#endif

extern const ps2host::FunctionEntry kGuestFunctions[];
extern const std::size_t kGuestFunctionCount;
extern const ps2host::OverlayEntry kGuestOverlays[];
extern const std::size_t kGuestOverlayCount;
extern const ps2host::RelocatableEntry kGuestRelocatables[];
extern const std::size_t kGuestRelocatableCount;
#ifdef PS2HOST_PGO_GENERATE
extern "C" int __llvm_profile_write_file(void);   // PS2HOST_PGO=generate (LLVM's profile runtime)
#endif

/* The game's host implementations of guest functions (its port's overrides.cpp). */
void RegisterGameOverrides();

namespace {

/* The port is built for AVX2 (engine/host/CMakeLists.txt; the VU's lanes and
   the rest take its three-operand forms): a processor without it is told so
   rather than stopped by an illegal instruction somewhere. */
bool HasAvx2() {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86)) && defined(__AVX2__)
    int info[4];
    __cpuid(info, 0);
    if (info[0] < 7) return false;
    __cpuid(info, 1);
    const bool osxsave = (info[2] >> 27) & 1, avx = (info[2] >> 28) & 1;
    if (!osxsave || !avx) return false;
    if ((_xgetbv(0) & 6) != 6) return false;        // the OS saves the YMM registers
    __cpuidex(info, 7, 0);
    return (info[1] >> 5) & 1;
#else
    return true;
#endif
}

std::string g_log;     // the log a run started without arguments writes
std::filesystem::path g_here;      // ...its folder (the game's), for the crash reports

#ifdef PS2HOST_GAME_EXECUTABLE
/* Whether `path` is an image of the game's disc: the executable it boots is
   the one the port was translated from (game.json's). */
bool IsGameDisc(const std::string& path) {
    ps2host::Disc disc;
    if (!disc.Open(path)) return false;
    std::string boot = disc.BootPath();
    const std::size_t semicolon = boot.find(';');
    if (semicolon != std::string::npos) boot.resize(semicolon);
    while (!boot.empty() && (boot[0] == '\\' || boot[0] == '/')) boot.erase(0, 1);
    return ps2platform::CompareNoCase(boot.c_str(), PS2HOST_GAME_EXECUTABLE) == 0;
}

/* The player's disc image, from a file dialog; "" if they cancel. */
std::string PickDisc() {
    return ps2platform::PickFile("Choose your " PS2HOST_GAME_NAME " disc image",
                                 "Disc images (*.iso; *.chd; *.cue; *.bin)\0*.iso;*.chd;*.cue;*.bin\0All files (*.*)\0*.*\0");
}
#endif

/* Started without arguments - from Explorer, a shortcut - the port plays its
   game from the player's own disc image: the one chosen before (kept in the
   PC settings, beside the executable), or asked for and checked to be the
   game's disc; the memory card a directory beside it too. In a window, with
   sound, on the GPU, with no time limit (the default is for test runs).
   An empty list: the player gave none. */
std::vector<std::string> PlayArguments(const char* self) {
#ifdef PS2HOST_GAME_EXECUTABLE
    const std::filesystem::path exe = ps2platform::ExePath();
    const std::filesystem::path here = exe.parent_path();
    ps2host::LoadSettings((here / "pc_settings.ini").string());     // where --memcard puts them
    std::string disc = ps2host::Setting("disc");
    // None chosen yet, or gone (moved): an image of the game's disc beside the executable or in its "game"
    // folder (the launcher's, as option-c's) is taken without asking.
    // (The launcher's unpacked disc first: game\disc - the images are needed only to unpack it.)
    if (!IsGameDisc(disc) && IsGameDisc((here / "game" / "disc").string())) disc = (here / "game" / "disc").string();
    if (!IsGameDisc(disc)) {
        for (const std::filesystem::path& folder : {here, here / "game"}) {
            std::error_code error;
            for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
                std::error_code is_dir;
                if (entry.is_directory(is_dir) && std::filesystem::exists(entry.path() / "disc.idx", is_dir) &&
                    IsGameDisc(entry.path().string())) {
                    disc = entry.path().string();
                    break;
                }
                std::string ext = entry.path().extension().string();
                for (char& ch : ext) ch = char(tolower(static_cast<unsigned char>(ch)));
                if ((ext == ".iso" || ext == ".chd" || ext == ".bin" || ext == ".cue") &&
                    IsGameDisc(entry.path().string())) {
                    disc = entry.path().string();
                    break;
                }
            }
            if (IsGameDisc(disc)) break;
        }
    }
    while (!IsGameDisc(disc)) {
        if (!disc.empty() && ps2platform::Exists(disc))
            ps2platform::ShowMessage(PS2HOST_GAME_NAME, "This is not an image of the " PS2HOST_GAME_NAME " disc "
                                     "(" PS2HOST_GAME_EXECUTABLE "):\n\n" + disc, false);
        disc = PickDisc();
        if (disc.empty()) return {};
    }
    if (disc != ps2host::Setting("disc")) ps2host::SetSetting("disc", disc);
    // No terminal to read: what the run says goes to a log in data\logs, stdout's and stderr's lines in the
    // order they were written, unbuffered (a run ended by force loses nothing). The "log_keep" setting (1, the
    // default, or 3): the last runs' logs kept as <name>.1.log, <name>.2.log (the maintainer 2026-11-20).
    const std::filesystem::path logs = here / "data" / "logs";
    std::error_code made;
    std::filesystem::create_directories(logs, made);
    const std::string stem = exe.stem().string();
    g_log = "data\\logs\\" + stem + ".log";
    g_here = here;
    const int keep = ps2host::Setting("log_keep") == "3" ? 3 : 1;
    for (int n = 1; n <= 2; ++n) {
        std::error_code gone;
        if (n >= keep) std::filesystem::remove(logs / (stem + "." + std::to_string(n) + ".log"), gone);
    }
    if (keep > 1) {
        std::error_code moved;
        std::filesystem::remove(logs / (stem + ".2.log"), moved);
        std::filesystem::rename(logs / (stem + ".1.log"), logs / (stem + ".2.log"), moved);
        std::filesystem::rename(logs / (stem + ".log"), logs / (stem + ".1.log"), moved);
    }
    ps2platform::RedirectOutput((logs / (stem + ".log")).string());
    std::fprintf(stderr, "[port] disc %s\n", disc.c_str());
    return {self, "--disc", disc, "--memcard", (here / "memcard").string(), "--window", "--gpu",
            "--audio", "--timeout", "315360000"};     // ten years
#else
    return {self};
#endif
}

}  // namespace

int main(int argc, char** argv) {
    // A port built as a windowed program has no console of its own. Started from a terminal its output goes to
    // that one; redirected (a file, a pipe: the tools' runs), where it was sent.
    ps2platform::AttachTerminal();
#ifdef PS2HOST_GAME_TITLE
    const ps2host::ReportGame report{PS2HOST_GAME_TITLE, PS2HOST_GAME_VERSION, PS2HOST_GAME_TARGET,
                                     PS2HOST_GAME_GITHUB};
    // A crashed run's report (ps2host/crash_report.h: this program started again by the crash's filter).
    if (ps2host::IsCrashReportHelper(argc, argv)) return ps2host::CrashReportHelper(argc, argv, report);
#endif
    // The launcher's unpacking (the maintainer 2026-11-21: the discs unpacked into the game's folder once, read
    // from there after): `--unpack-disc <image> <folder>` - the whole disc, nothing lost; `--unpack-extra <image>
    // <folder> --disc <its own disc>` - a second disc, only what the port reads of it. Progress in
    // "<folder>.progress". Exit 0: written.
    if (argc >= 4 && (std::string(argv[1]) == "--unpack-disc" || std::string(argv[1]) == "--unpack-extra")) {
        const std::string image = argv[2], folder = argv[3];
        std::string progress = folder + ".progress";
        std::error_code error;
        std::filesystem::remove_all(folder, error);
        if (std::string(argv[1]) == "--unpack-disc") {
            ps2host::Disc disc;
            if (!disc.Open(image)) {
                std::fprintf(stderr, "[unpack] cannot open %s\n", image.c_str());
                return 2;
            }
            const bool ok = disc.Unpack(folder, nullptr, ps2host::WriteUnpackProgress, &progress);
            std::fprintf(stderr, "[unpack] %s -> %s: %s\n", image.c_str(), folder.c_str(), ok ? "done" : "FAILED");
            return ok ? 0 : 1;
        }
        const std::filesystem::path here = ps2platform::ExePath().parent_path();
        ps2host::LoadSettings((here / "pc_settings.ini").string());
        for (int i = 4; i + 1 < argc; ++i)
            if (std::string(argv[i]) == "--disc" && !ps2host::GuestDisc().Open(argv[i + 1])) {
                std::fprintf(stderr, "[unpack] cannot open the game's disc %s\n", argv[i + 1]);
                return 2;
            }
        RegisterGameOverrides();
        const ps2host::ExtraDiscUnpacker unpack = ps2host::GetExtraDiscUnpacker();
        const bool ok = unpack && unpack(image, folder);
        std::fprintf(stderr, "[unpack] %s -> %s: %s\n", image.c_str(), folder.c_str(), ok ? "done" : "FAILED");
        return ok ? 0 : 1;
    }
    std::vector<std::string> play;
    std::vector<char*> play_argv;
    if (argc == 1) {
        play = PlayArguments(argv[0]);
        if (play.empty()) return 0;
#ifdef PS2HOST_GAME_TITLE
        ps2host::ArmCrashReports(g_here, g_log, report);     // (a player's run: crashes and error stops reported)
#endif
        for (std::string& arg : play) play_argv.push_back(arg.data());
        play_argv.push_back(nullptr);
        argc = int(play.size());
        argv = play_argv.data();
    }
    if (!HasAvx2()) {
        std::fprintf(stderr, "This build needs a processor with AVX2 (Intel Haswell, AMD Excavator or later).\n");
        return 70;
    }
#ifdef PS2HOST_GAME_TITLE
    ps2host::SetGameTitle(PS2HOST_GAME_TITLE);      // the window's (game.json's "title")
#endif
#ifdef PS2HOST_GAME_VERSION
    std::fprintf(stderr, "[port] %s, version %s\n", PS2HOST_GAME_TITLE, PS2HOST_GAME_VERSION);
#endif
    ps2host::RegisterFunctions(kGuestFunctions, kGuestFunctionCount);
    ps2host::RegisterOverlays(kGuestOverlays, kGuestOverlayCount);
    ps2host::RegisterRelocatables(kGuestRelocatables, kGuestRelocatableCount);
    RegisterGameOverrides();
    const int code = ps2host::Main(argc, argv);
    // RECOMP OPTIONS' RESTART NOW: the same executable again, as it was started (its command line, the
    // environment as it is) - the settings saved by now.
    if (ps2host::RestartRequested()) ps2platform::RestartSelf();
    if (!play.empty() && code != 0) {         // closing the window ends a run with 0
        std::fflush(nullptr);
        const std::string text = "The game stopped because of an error. What happened is in " +
                                 g_log + " in the game's folder.";
        if (!ps2host::ReportStop(code))       // (a player's run: a crash report and the message with it)
            ps2platform::ShowMessage(ps2host::GameTitle().empty() ? "PS2 port" : ps2host::GameTitle().c_str(), text,
                                     true);
    }
    // What must be finished is (the report written, the settings and the
    // memory card saved as they changed, the WAV's header here); the rest -
    // the window, the GPU device, the threads - goes with the process.
    // Taken down one by one at exit, the Vulkan device's could hang
    // (vkDestroyDevice with the driver's threads and the renderer's images
    // still about), leaving a window that never closed.
    ps2host::CloseAudio();
    ps2host::ReleaseController();
    ps2host::PrintVifStats();
    std::fflush(nullptr);
#ifdef PS2HOST_PGO_GENERATE
    __llvm_profile_write_file();            // (the profile's own writer runs at exit, which this skips)
#endif
    ps2platform::ExitNow(code);
}
