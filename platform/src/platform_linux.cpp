/*
 * The platform layer on Linux (include/ps2platform/platform.h, keys.h): the
 * same calls as src/platform_win32.cpp, each with the system's own way of
 * doing it - fibers by a small register switch (no signal mask per switch as
 * swapcontext has), futex waits, clock_nanosleep, /proc for the CPU times,
 * SDL3 for the dialogs. There is no fallback window (SDL3 and Vulkan are the
 * way); the overlay's text is drawn by stb_truetype with the engine's
 * Liberation Sans Bold (engine/data/fonts), built in by platform/CMakeLists.txt.
 */

#include <SDL3/SDL.h>

#include <dirent.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <pthread.h>
#include <sched.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <set>
#include <sstream>
#include <thread>
#include <utility>

#if defined(__x86_64__)
#include <cpuid.h>
#endif

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

#include "ps2platform/keys.h"
#include "ps2platform/platform.h"

extern "C" char** environ;

namespace ps2platform {

// ---- Fibers. A Fiber holds its saved stack pointer; a switch saves the callee-saved registers, MXCSR and the
// x87 control word on the old stack and takes them from the new one (the System V ABI's callee-saved state).
struct Fiber {
    void* sp = nullptr;             // the saved stack pointer while not running
    void* stack = nullptr;          // its own stack (null: a thread's own)
    std::size_t stack_bytes = 0;
};

namespace {
thread_local Fiber* g_current = nullptr;
}  // namespace

extern "C" void ps2platform_fiber_switch(void** save_sp, void* new_sp);
extern "C" void ps2platform_fiber_start();

#if defined(__x86_64__)
asm(R"(
    .text
    .globl ps2platform_fiber_switch
    .type ps2platform_fiber_switch, @function
ps2platform_fiber_switch:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    subq $8, %rsp
    stmxcsr (%rsp)
    fnstcw 4(%rsp)
    movq %rsp, (%rdi)
    movq %rsi, %rsp
    ldmxcsr (%rsp)
    fldcw 4(%rsp)
    addq $8, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    ret
    .size ps2platform_fiber_switch, .-ps2platform_fiber_switch

    .globl ps2platform_fiber_start
    .type ps2platform_fiber_start, @function
ps2platform_fiber_start:
    movq %r13, %rdi
    andq $-16, %rsp
    callq *%r12
    ud2
    .size ps2platform_fiber_start, .-ps2platform_fiber_start
    .section .note.GNU-stack,"",@progbits
    .text
)");
#elif defined(__aarch64__)
asm(R"(
    .text
    .globl ps2platform_fiber_switch
    .type ps2platform_fiber_switch, %function
ps2platform_fiber_switch:
    sub sp, sp, #176
    stp x19, x20, [sp, #0]
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x26, [sp, #48]
    stp x27, x28, [sp, #64]
    stp x29, x30, [sp, #80]
    stp d8, d9, [sp, #96]
    stp d10, d11, [sp, #112]
    stp d12, d13, [sp, #128]
    stp d14, d15, [sp, #144]
    mrs x9, fpcr
    str x9, [sp, #160]
    mov x9, sp
    str x9, [x0]
    mov sp, x1
    ldp x19, x20, [sp, #0]
    ldp x21, x22, [sp, #16]
    ldp x23, x24, [sp, #32]
    ldp x25, x26, [sp, #48]
    ldp x27, x28, [sp, #64]
    ldp x29, x30, [sp, #80]
    ldp d8, d9, [sp, #96]
    ldp d10, d11, [sp, #112]
    ldp d12, d13, [sp, #128]
    ldp d14, d15, [sp, #144]
    ldr x9, [sp, #160]
    msr fpcr, x9
    add sp, sp, #176
    ret
    .size ps2platform_fiber_switch, .-ps2platform_fiber_switch

    .globl ps2platform_fiber_start
    .type ps2platform_fiber_start, %function
ps2platform_fiber_start:
    mov x0, x20
    blr x19
    brk #0
    .size ps2platform_fiber_start, .-ps2platform_fiber_start
    .section .note.GNU-stack,"",%progbits
    .text
)");
#else
#error "platform_linux.cpp: a fiber switch for this processor"
#endif

Fiber* ThreadAsFiber() {
    if (!g_current) g_current = new Fiber();
    return g_current;
}

Fiber* CreateFiber(std::size_t stack_bytes, FiberEntry entry, void* parameter) {
    stack_bytes = std::max<std::size_t>((stack_bytes + 4095) & ~std::size_t(4095), 64 * 1024);
    void* stack = std::aligned_alloc(4096, stack_bytes);
    if (!stack) return nullptr;
    auto* fiber = new Fiber();
    fiber->stack = stack;
    fiber->stack_bytes = stack_bytes;
    auto top = (reinterpret_cast<std::uintptr_t>(stack) + stack_bytes) & ~std::uintptr_t(15);
    auto* slot = reinterpret_cast<std::uint64_t*>(top);
#if defined(__x86_64__)
    // From the saved stack pointer up: MXCSR + x87 control word, r15 r14 r13 r12 rbx rbp, the return address
    // (the start routine), and a zero where its caller's return address would be (the stack 16-aligned at its
    // call, as the ABI has it). r12 = the entry, r13 = its parameter. The floating-point state the creator's.
    std::uint32_t mxcsr = 0;
    std::uint16_t fpcw = 0;
    asm volatile("stmxcsr %0" : "=m"(mxcsr));
    asm volatile("fnstcw %0" : "=m"(fpcw));
    slot -= 9;
    slot[0] = std::uint64_t(mxcsr) | std::uint64_t(fpcw) << 32;
    slot[1] = 0;                                                  // r15
    slot[2] = 0;                                                  // r14
    slot[3] = reinterpret_cast<std::uint64_t>(parameter);         // r13
    slot[4] = reinterpret_cast<std::uint64_t>(entry);             // r12
    slot[5] = 0;                                                  // rbx
    slot[6] = 0;                                                  // rbp
    slot[7] = reinterpret_cast<std::uint64_t>(&ps2platform_fiber_start);
    slot[8] = 0;
#else
    // x19 = the entry, x20 = its parameter, x30 (the return) = the start routine, the creator's FPCR.
    std::uint64_t fpcr = 0;
    asm volatile("mrs %0, fpcr" : "=r"(fpcr));
    slot -= 22;                                                   // 176 bytes
    std::memset(slot, 0, 176);
    slot[0] = reinterpret_cast<std::uint64_t>(entry);
    slot[1] = reinterpret_cast<std::uint64_t>(parameter);
    slot[11] = reinterpret_cast<std::uint64_t>(&ps2platform_fiber_start);
    slot[20] = fpcr;
#endif
    fiber->sp = slot;
    return fiber;
}

void SwitchFiber(Fiber* to) {
    Fiber* from = ThreadAsFiber();
    if (to == from) return;
    g_current = to;
    ps2platform_fiber_switch(&from->sp, to->sp);
}

void DeleteFiber(Fiber* fiber) {
    if (!fiber) return;
    std::free(fiber->stack);
    delete fiber;
}

// ---- Waiting.
namespace {
long Futex(void* address, int op, std::uint32_t value, const timespec* timeout) {
    return syscall(SYS_futex, address, op, value, timeout, nullptr, 0);
}
}  // namespace

void WaitOnValue(std::atomic<std::uint32_t>& value, std::uint32_t seen, std::uint32_t timeout_ms) {
    static_assert(sizeof(std::atomic<std::uint32_t>) == sizeof(std::uint32_t), "an atomic as its value");
    timespec timeout{};
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_nsec = long(timeout_ms % 1000) * 1000000L;
    Futex(&value, FUTEX_WAIT_PRIVATE, seen, timeout_ms == 0xFFFFFFFFu ? nullptr : &timeout);
}

void WakeAll(std::atomic<std::uint32_t>& value) { Futex(&value, FUTEX_WAKE_PRIVATE, INT_MAX, nullptr); }

// steady_clock is CLOCK_MONOTONIC (libstdc++ and libc++ alike): slept to the time itself, no drift.
void SleepUntil(std::chrono::steady_clock::time_point when) {
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(when.time_since_epoch()).count();
    if (ns <= 0) return;
    timespec at{};
    at.tv_sec = time_t(ns / 1000000000LL);
    at.tv_nsec = long(ns % 1000000000LL);
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &at, nullptr) == EINTR) {
    }
}

// ---- The process's real time: the timer slack (50 us by default) down to 1 us; threads started later
// inherit it.
void FineTimers() { prctl(PR_SET_TIMERSLACK, 1000UL, 0, 0, 0); }

// ---- Threads. A nice value lower than the process's own needs a privilege (CAP_SYS_NICE, rtkit): tried, and
// left as it is where refused, as Windows' own calls fail quietly.
void SetThreadPriority(Priority priority) {
    int nice_value = 0;
    switch (priority) {
        case Priority::kNormal: nice_value = 0; break;
        case Priority::kAboveNormal: nice_value = -2; break;
        case Priority::kHighest: nice_value = -5; break;
        case Priority::kTimeCritical: nice_value = -10; break;
    }
    setpriority(PRIO_PROCESS, id_t(syscall(SYS_gettid)), nice_value);
    prctl(PR_SET_TIMERSLACK, 1000UL, 0, 0, 0);
}

void NoPowerThrottling() {}

namespace {
struct ThreadHandle {
    pid_t tid;
    clockid_t clock;
};
}  // namespace

void* NameThread(const char* name) {
    char short_name[16] = {};
    std::strncpy(short_name, name, 15);         // the kernel's limit: 15 characters
    pthread_setname_np(pthread_self(), short_name);
    auto* handle = new ThreadHandle{pid_t(syscall(SYS_gettid)), CLOCK_THREAD_CPUTIME_ID};
    if (pthread_getcpuclockid(pthread_self(), &handle->clock) != 0) handle->clock = CLOCK_THREAD_CPUTIME_ID;
    return handle;
}

bool ThreadCpuNs(void* thread, std::uint64_t* total_ns, std::uint64_t* kernel_ns) {
    if (!thread) return false;
    const auto* handle = static_cast<const ThreadHandle*>(thread);
    timespec t{};
    if (clock_gettime(handle->clock, &t) != 0) return false;
    *total_ns = std::uint64_t(t.tv_sec) * 1000000000ull + std::uint64_t(t.tv_nsec);
    // The kernel's share: /proc's stime (field 15), in clock ticks.
    *kernel_ns = 0;
    char path[64];
    std::snprintf(path, sizeof path, "/proc/self/task/%d/stat", int(handle->tid));
    if (std::FILE* f = std::fopen(path, "r")) {
        char text[1024] = {};
        const std::size_t n = std::fread(text, 1, sizeof text - 1, f);
        std::fclose(f);
        text[n] = 0;
        if (const char* p = std::strrchr(text, ')')) {
            unsigned long long utime = 0, stime = 0;
            // after "pid (comm)": state ppid pgrp session tty tpgid flags minflt cminflt majflt cmajflt utime stime
            if (std::sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &utime, &stime) == 2) {
                const long hz = sysconf(_SC_CLK_TCK);
                if (hz > 0) *kernel_ns = std::min<std::uint64_t>(*total_ns, stime * (1000000000ull / std::uint64_t(hz)));
            }
        }
    }
    return true;
}

std::uint64_t ProcessCpuNs() {
    timespec t{};
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t) != 0) return 0;
    return std::uint64_t(t.tv_sec) * 1000000000ull + std::uint64_t(t.tv_nsec);
}

// ---- Cores.
int ProcessCores() {
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof set, &set) == 0) {
        const int n = CPU_COUNT(&set);
        if (n > 0) return n;
    }
    return int(std::thread::hardware_concurrency());
}

// Linux's affinity is a thread's: every thread the process has now (new ones inherit their creator's).
bool HoldToCores(std::uint64_t mask) {
    if (!mask) return false;
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int i = 0; i < 64; ++i)
        if (mask >> i & 1) CPU_SET(i, &set);
    bool any = false;
    if (DIR* tasks = opendir("/proc/self/task")) {
        while (dirent* e = readdir(tasks)) {
            const int tid = std::atoi(e->d_name);
            if (tid > 0 && sched_setaffinity(pid_t(tid), sizeof set, &set) == 0) any = true;
        }
        closedir(tasks);
    }
    return any || sched_setaffinity(0, sizeof set, &set) == 0;
}

// ---- Files and the process.
std::filesystem::path ExePath() {
    char exe[4096] = {};
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0) return {};
    exe[n] = 0;
    return std::filesystem::path(exe);
}

bool Exists(const std::string& path) {
    struct stat s;
    return stat(path.c_str(), &s) == 0;
}

int CompareNoCase(const char* a, const char* b) { return strcasecmp(a, b); }

void AttachTerminal() {}        // a Linux program always writes to the terminal it was started from

void RedirectOutput(const std::string& log) {
    const int file = open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (file >= 0) {
        dup2(file, STDOUT_FILENO);
        dup2(file, STDERR_FILENO);
        close(file);
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // a run ended by force loses nothing
    std::setvbuf(stderr, nullptr, _IONBF, 0);
}

namespace {
std::string FirstLine(const char* path) {
    std::ifstream in(path);
    std::string line;
    std::getline(in, line);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' ')) line.pop_back();
    return line;
}

// A field of /etc/os-release (its quotes taken off), "" if none.
std::string OsRelease(const char* key) {
    for (const char* path : {"/etc/os-release", "/usr/lib/os-release"}) {
        std::ifstream in(path);
        std::string line;
        const std::string want = std::string(key) + "=";
        while (std::getline(in, line)) {
            if (line.rfind(want, 0) != 0) continue;
            std::string v = line.substr(want.size());
            if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'')) v = v.substr(1, v.size() - 2);
            return v;
        }
    }
    return {};
}

// A line of /proc/meminfo in kB, 0 if none.
unsigned long long MemInfoKb(const char* key) {
    std::ifstream in("/proc/meminfo");
    std::string name;
    unsigned long long kb = 0;
    std::string unit;
    while (in >> name >> kb) {
        std::getline(in, unit);
        if (name == std::string(key) + ":") return kb;
    }
    return 0;
}
}  // namespace

std::vector<std::string> SystemReport() {
    std::vector<std::string> lines;
    char line[512];
    // The OS: the distribution's name, the kernel, the session (Wayland / X11) and the desktop.
    utsname u{};
    uname(&u);
    std::string distro = OsRelease("PRETTY_NAME");
    if (distro.empty()) distro = "Linux";
    std::snprintf(line, sizeof line, "OS: %s (%s %s %s)", distro.c_str(), u.sysname, u.release, u.machine);
    lines.push_back(line);
    {
        const char* session = std::getenv("XDG_SESSION_TYPE");
        const char* desktop = std::getenv("XDG_CURRENT_DESKTOP");
        std::string text = std::string("session: ") + (session ? session : "?") + ", desktop " +
                           (desktop ? desktop : "?");
        if (std::getenv("WAYLAND_DISPLAY")) text += ", WAYLAND_DISPLAY set";
        if (std::getenv("DISPLAY")) text += ", DISPLAY set";
        if (const char* appimage = std::getenv("APPIMAGE")) text += std::string(", AppImage ") + appimage;
        if (std::getenv("STEAM_COMPAT_DATA_PATH") || std::getenv("SteamAppId")) text += ", under Steam";
        lines.push_back(text);
    }
    // The processor: its name, cores and threads.
    std::string name;
#if defined(__x86_64__)
    {
        unsigned info[4] = {};
        char brand[49] = {};
        if (__get_cpuid(0x80000000u, &info[0], &info[1], &info[2], &info[3]) && info[0] >= 0x80000004u) {
            for (unsigned i = 0; i < 3; ++i) {
                __get_cpuid(0x80000002u + i, &info[0], &info[1], &info[2], &info[3]);
                std::memcpy(brand + i * 16, info, 16);
            }
        }
        name = brand;
    }
#endif
    std::set<std::pair<int, int>> cores;     // (package, core)
    {
        std::ifstream in("/proc/cpuinfo");
        std::string l;
        int package = 0;
        while (std::getline(in, l)) {
            const auto colon = l.find(':');
            if (colon == std::string::npos) continue;
            std::string key = l.substr(0, colon);
            while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
            const std::string value = colon + 2 <= l.size() ? l.substr(colon + 2) : "";
            if (key == "physical id") package = std::atoi(value.c_str());
            else if (key == "core id") cores.insert({package, std::atoi(value.c_str())});
            else if (name.empty() && key == "model name") name = value;
        }
    }
    while (!name.empty() && name.back() == ' ') name.pop_back();
    while (!name.empty() && name.front() == ' ') name.erase(0, 1);
    const long threads = sysconf(_SC_NPROCESSORS_ONLN);
    std::snprintf(line, sizeof line, "CPU: %s, %u cores, %ld threads", name.empty() ? "?" : name.c_str(),
                  unsigned(cores.empty() ? std::size_t(threads) : cores.size()), threads);
    lines.push_back(line);
    const unsigned long long total = MemInfoKb("MemTotal"), avail = MemInfoKb("MemAvailable");
    if (total) {
        std::snprintf(line, sizeof line, "RAM: %.1f GB, %.1f GB free", double(total) / 1048576.0,
                      double(avail) / 1048576.0);
        lines.push_back(line);
    }
    // The power plan: the platform profile (power-profiles-daemon / ACPI) and the first core's governor.
    {
        std::string profile = FirstLine("/sys/firmware/acpi/platform_profile");
        const std::string governor = FirstLine("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor");
        const std::string epp = FirstLine("/sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference");
        std::string text = "power plan: " + (profile.empty() ? std::string("platform profile ?") : profile);
        if (!governor.empty()) text += ", governor " + governor;
        if (!epp.empty()) text += ", EPP " + epp;
        lines.push_back(text);
    }
    return lines;
}

// The same program with the same arguments, a new process (as CreateProcess with GetCommandLine): the caller
// then ends this one.
bool RestartSelf() {
    std::vector<std::string> args;
    {
        std::ifstream in("/proc/self/cmdline", std::ios::binary);
        std::string arg;
        while (std::getline(in, arg, '\0')) args.push_back(arg);
    }
    const std::string exe = ExePath().string();
    if (args.empty() || exe.empty()) return false;
    std::vector<char*> argv;
    for (std::string& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    const pid_t child = fork();
    if (child < 0) {
        std::fprintf(stderr, "[port] restart failed (%d)\n", errno);
        return false;
    }
    if (child == 0) {
        // Detached from this process (setsid), its descriptors kept but for those marked close-on-exec.
        setsid();
        execve(exe.c_str(), argv.data(), environ);
        _exit(127);
    }
    return true;
}

void ExitNow(int code) { _exit(code); }

std::tm LocalTime(std::time_t when) {
    std::tm local{};
    localtime_r(&when, &local);
    return local;
}

// ---- Dialogs: SDL3's (the desktop's own dialog by its portal, zenity / kdialog where there is none).
void ShowMessage(const char* title, const std::string& text, bool error) {
    if (!SDL_ShowSimpleMessageBox(error ? SDL_MESSAGEBOX_ERROR : SDL_MESSAGEBOX_WARNING, title, text.c_str(), nullptr))
        std::fprintf(stderr, "%s: %s\n", title, text.c_str());
}

namespace {
struct PickState {
    std::atomic<bool> done{false};
    std::string file;
};

void SDLCALL PickDone(void* user, const char* const* files, int) {
    auto* state = static_cast<PickState*>(user);
    if (files && files[0]) state->file = files[0];
    state->done.store(true);
}
}  // namespace

std::string PickFile(const char* title, const char* filter) {
    // Win32's filter ("Name\0*.a;*.b\0...\0\0") as SDL's: ("Name", "a;b").
    std::vector<std::string> names, patterns;
    for (const char* p = filter; p && *p;) {
        const char* name = p;
        p += std::strlen(p) + 1;
        if (!*p) break;
        std::string pattern;
        for (std::string part : [&] {
                 std::vector<std::string> parts;
                 std::stringstream s(p);
                 std::string item;
                 while (std::getline(s, item, ';')) parts.push_back(item);
                 return parts;
             }()) {
            if (part.rfind("*.", 0) == 0) part = part.substr(2);
            if (part == "*" || part.empty()) part = "*";
            if (!pattern.empty()) pattern += ";";
            pattern += part;
        }
        names.push_back(name);
        patterns.push_back(pattern);
        p += std::strlen(p) + 1;
    }
    std::vector<SDL_DialogFileFilter> filters;
    for (std::size_t i = 0; i < names.size(); ++i) filters.push_back({names[i].c_str(), patterns[i].c_str()});
    const bool started = SDL_WasInit(SDL_INIT_VIDEO) != 0 || SDL_InitSubSystem(SDL_INIT_VIDEO);
    if (!started) return {};
    PickState state;
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_TITLE_STRING, title);
    SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_FILTERS_POINTER, filters.data());
    SDL_SetNumberProperty(props, SDL_PROP_FILE_DIALOG_NFILTERS_NUMBER, Sint64(filters.size()));
    SDL_ShowFileDialogWithProperties(SDL_FILEDIALOG_OPENFILE, PickDone, &state, props);
    while (!state.done.load()) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
        }
        SDL_Delay(10);
    }
    SDL_DestroyProperties(props);
    return state.file;
}

// ---- Text for the overlay: the engine's Liberation Sans Bold by stb_truetype (Windows: GDI's Consolas).
namespace {
const unsigned char kPanelFont[] = {
#include "liberation_sans_bold.inc"
};
}  // namespace

bool DrawTextPanel(const std::vector<std::string>& lines, int line_px, const std::vector<std::uint32_t>& colours,
                   std::uint32_t panel, int max_width, int max_height, std::vector<std::uint32_t>* pixels, int* width,
                   int* height) {
    static stbtt_fontinfo font;
    static const bool ok = stbtt_InitFont(&font, kPanelFont, stbtt_GetFontOffsetForIndex(kPanelFont, 0)) != 0;
    pixels->clear();
    if (!ok) return false;
    const float scale = stbtt_ScaleForPixelHeight(&font, float(line_px - 4));
    int ascent = 0, descent = 0, gap = 0;
    stbtt_GetFontVMetrics(&font, &ascent, &descent, &gap);
    auto text_width = [&](const std::string& text) {
        float x = 0;
        for (std::size_t i = 0; i < text.size(); ++i) {
            int advance = 0, lsb = 0;
            stbtt_GetCodepointHMetrics(&font, (unsigned char)text[i], &advance, &lsb);
            x += float(advance) * scale;
            if (i + 1 < text.size())
                x += float(stbtt_GetCodepointKernAdvance(&font, (unsigned char)text[i], (unsigned char)text[i + 1])) *
                     scale;
        }
        return int(x + 0.5f);
    };
    int w = 0;
    for (const std::string& text : lines) w = std::max(w, text_width(text));
    w = std::min(max_width, w + line_px);
    const int h = std::min(max_height, int(lines.size()) * line_px + line_px / 2);
    if (w <= 0 || h <= 0) return false;
    pixels->assign(std::size_t(w) * std::size_t(h), 0xFF000000u | (panel & 0xFFFFFFu));
    std::vector<unsigned char> glyph;
    for (std::size_t li = 0; li < lines.size(); ++li) {
        const std::uint32_t colour = li < colours.size() ? colours[li] : 0xEBEBEBu;
        const int base = int(li) * line_px + line_px / 4 + int(float(ascent) * scale + 0.5f);
        float x = float(line_px / 2);
        const std::string& text = lines[li];
        for (std::size_t i = 0; i < text.size(); ++i) {
            const int cp = (unsigned char)text[i];
            int advance = 0, lsb = 0;
            stbtt_GetCodepointHMetrics(&font, cp, &advance, &lsb);
            int x0, y0, x1, y1;
            stbtt_GetCodepointBitmapBox(&font, cp, scale, scale, &x0, &y0, &x1, &y1);
            const int gw = x1 - x0, gh = y1 - y0;
            if (gw > 0 && gh > 0) {
                glyph.assign(std::size_t(gw) * std::size_t(gh), 0);
                stbtt_MakeCodepointBitmap(&font, glyph.data(), gw, gh, gw, scale, scale, cp);
                const int ox = int(x + 0.5f) + x0, oy = base + y0;
                for (int gy = 0; gy < gh; ++gy) {
                    const int py = oy + gy;
                    if (py < 0 || py >= h) continue;
                    for (int gx = 0; gx < gw; ++gx) {
                        const int px = ox + gx;
                        if (px < 0 || px >= w) continue;
                        const unsigned a = glyph[std::size_t(gy) * std::size_t(gw) + std::size_t(gx)];
                        if (!a) continue;
                        std::uint32_t& p = (*pixels)[std::size_t(py) * std::size_t(w) + std::size_t(px)];
                        auto mix = [&](int shift) {
                            const unsigned bg = (p >> shift) & 0xFF, fg = (colour >> shift) & 0xFF;
                            return ((fg * a + bg * (255 - a) + 127) / 255) << shift;
                        };
                        p = 0xFF000000u | mix(16) | mix(8) | mix(0);
                    }
                }
            }
            x += float(advance) * scale;
            if (i + 1 < text.size())
                x += float(stbtt_GetCodepointKernAdvance(&font, cp, (unsigned char)text[i + 1])) * scale;
        }
    }
    *width = w;
    *height = h;
    return true;
}

// ---- The fallback window: none on Linux (SDL3 and Vulkan are the way; the port says why it has no window).
bool OpenFallbackWindow(const wchar_t*, int, int) { return false; }
bool FallbackWindowOpen() { return false; }
bool FallbackWindowFocused() { return false; }
void PumpFallbackWindow(bool*) {}
void DrawFallbackWindow(const std::uint32_t*, std::uint32_t, std::uint32_t) {}

// ---- Keys (keys.h). A set-1 scan code is a key's place; its key code is the one a US layout has there - the
// same place as window.cpp's Scancode() takes it back to, so a binding is the key in that place on every layout
// (README: "by key position, the same on every layout"). Its name is the one the layout in use gives it (SDL).
namespace {
struct ScanKey {
    std::uint16_t scan;
    int vk;
    SDL_Scancode sdl;
};
namespace k = key;
const ScanKey kScanKeys[] = {
    {0x01, k::kEscape, SDL_SCANCODE_ESCAPE},
    {0x02, '1', SDL_SCANCODE_1}, {0x03, '2', SDL_SCANCODE_2}, {0x04, '3', SDL_SCANCODE_3},
    {0x05, '4', SDL_SCANCODE_4}, {0x06, '5', SDL_SCANCODE_5}, {0x07, '6', SDL_SCANCODE_6},
    {0x08, '7', SDL_SCANCODE_7}, {0x09, '8', SDL_SCANCODE_8}, {0x0A, '9', SDL_SCANCODE_9},
    {0x0B, '0', SDL_SCANCODE_0}, {0x0C, k::kOemMinus, SDL_SCANCODE_MINUS}, {0x0D, k::kOemPlus, SDL_SCANCODE_EQUALS},
    {0x0E, k::kBack, SDL_SCANCODE_BACKSPACE}, {0x0F, k::kTab, SDL_SCANCODE_TAB},
    {0x10, 'Q', SDL_SCANCODE_Q}, {0x11, 'W', SDL_SCANCODE_W}, {0x12, 'E', SDL_SCANCODE_E},
    {0x13, 'R', SDL_SCANCODE_R}, {0x14, 'T', SDL_SCANCODE_T}, {0x15, 'Y', SDL_SCANCODE_Y},
    {0x16, 'U', SDL_SCANCODE_U}, {0x17, 'I', SDL_SCANCODE_I}, {0x18, 'O', SDL_SCANCODE_O},
    {0x19, 'P', SDL_SCANCODE_P}, {0x1A, k::kOem4, SDL_SCANCODE_LEFTBRACKET}, {0x1B, k::kOem6, SDL_SCANCODE_RIGHTBRACKET},
    {0x1C, k::kReturn, SDL_SCANCODE_RETURN}, {0x1D, k::kLControl, SDL_SCANCODE_LCTRL},
    {0x1E, 'A', SDL_SCANCODE_A}, {0x1F, 'S', SDL_SCANCODE_S}, {0x20, 'D', SDL_SCANCODE_D},
    {0x21, 'F', SDL_SCANCODE_F}, {0x22, 'G', SDL_SCANCODE_G}, {0x23, 'H', SDL_SCANCODE_H},
    {0x24, 'J', SDL_SCANCODE_J}, {0x25, 'K', SDL_SCANCODE_K}, {0x26, 'L', SDL_SCANCODE_L},
    {0x27, k::kOem1, SDL_SCANCODE_SEMICOLON}, {0x28, k::kOem7, SDL_SCANCODE_APOSTROPHE},
    {0x29, k::kOem3, SDL_SCANCODE_GRAVE}, {0x2A, k::kLShift, SDL_SCANCODE_LSHIFT},
    {0x2B, k::kOem5, SDL_SCANCODE_BACKSLASH},
    {0x2C, 'Z', SDL_SCANCODE_Z}, {0x2D, 'X', SDL_SCANCODE_X}, {0x2E, 'C', SDL_SCANCODE_C},
    {0x2F, 'V', SDL_SCANCODE_V}, {0x30, 'B', SDL_SCANCODE_B}, {0x31, 'N', SDL_SCANCODE_N},
    {0x32, 'M', SDL_SCANCODE_M}, {0x33, k::kOemComma, SDL_SCANCODE_COMMA}, {0x34, k::kOemPeriod, SDL_SCANCODE_PERIOD},
    {0x35, k::kOem2, SDL_SCANCODE_SLASH}, {0x36, k::kRShift, SDL_SCANCODE_RSHIFT},
    {0x37, k::kMultiply, SDL_SCANCODE_KP_MULTIPLY}, {0x38, k::kLMenu, SDL_SCANCODE_LALT},
    {0x39, k::kSpace, SDL_SCANCODE_SPACE}, {0x3A, k::kCapital, SDL_SCANCODE_CAPSLOCK},
    {0x3B, k::kF1 + 0, SDL_SCANCODE_F1}, {0x3C, k::kF1 + 1, SDL_SCANCODE_F2}, {0x3D, k::kF1 + 2, SDL_SCANCODE_F3},
    {0x3E, k::kF1 + 3, SDL_SCANCODE_F4}, {0x3F, k::kF1 + 4, SDL_SCANCODE_F5}, {0x40, k::kF1 + 5, SDL_SCANCODE_F6},
    {0x41, k::kF1 + 6, SDL_SCANCODE_F7}, {0x42, k::kF1 + 7, SDL_SCANCODE_F8}, {0x43, k::kF1 + 8, SDL_SCANCODE_F9},
    {0x44, k::kF1 + 9, SDL_SCANCODE_F10}, {0x45, 0x90, SDL_SCANCODE_NUMLOCKCLEAR},
    {0x46, 0x91, SDL_SCANCODE_SCROLLLOCK},
    {0x47, k::kNumpad0 + 7, SDL_SCANCODE_KP_7}, {0x48, k::kNumpad0 + 8, SDL_SCANCODE_KP_8},
    {0x49, k::kNumpad0 + 9, SDL_SCANCODE_KP_9}, {0x4A, k::kSubtract, SDL_SCANCODE_KP_MINUS},
    {0x4B, k::kNumpad0 + 4, SDL_SCANCODE_KP_4}, {0x4C, k::kNumpad0 + 5, SDL_SCANCODE_KP_5},
    {0x4D, k::kNumpad0 + 6, SDL_SCANCODE_KP_6}, {0x4E, k::kAdd, SDL_SCANCODE_KP_PLUS},
    {0x4F, k::kNumpad0 + 1, SDL_SCANCODE_KP_1}, {0x50, k::kNumpad0 + 2, SDL_SCANCODE_KP_2},
    {0x51, k::kNumpad0 + 3, SDL_SCANCODE_KP_3}, {0x52, k::kNumpad0, SDL_SCANCODE_KP_0},
    {0x53, k::kDecimal, SDL_SCANCODE_KP_PERIOD}, {0x56, 0xE2, SDL_SCANCODE_NONUSBACKSLASH},
    {0x57, k::kF1 + 10, SDL_SCANCODE_F11}, {0x58, k::kF12, SDL_SCANCODE_F12},
    {0xE01C, k::kReturn, SDL_SCANCODE_KP_ENTER}, {0xE01D, k::kRControl, SDL_SCANCODE_RCTRL},
    {0xE035, k::kDivide, SDL_SCANCODE_KP_DIVIDE}, {0xE038, k::kRMenu, SDL_SCANCODE_RALT},
    {0xE047, k::kHome, SDL_SCANCODE_HOME}, {0xE048, k::kUp, SDL_SCANCODE_UP}, {0xE049, k::kPrior, SDL_SCANCODE_PAGEUP},
    {0xE04B, k::kLeft, SDL_SCANCODE_LEFT}, {0xE04D, k::kRight, SDL_SCANCODE_RIGHT},
    {0xE04F, k::kEnd, SDL_SCANCODE_END}, {0xE050, k::kDown, SDL_SCANCODE_DOWN}, {0xE051, k::kNext, SDL_SCANCODE_PAGEDOWN},
    {0xE052, k::kInsert, SDL_SCANCODE_INSERT}, {0xE053, k::kDelete, SDL_SCANCODE_DELETE},
};

const ScanKey* FindScan(std::uint16_t scan_code) {
    for (const ScanKey& s : kScanKeys)
        if (s.scan == scan_code) return &s;
    return nullptr;
}
}  // namespace

int KeyOfScanCode(std::uint16_t scan_code) {
    const ScanKey* s = FindScan(scan_code);
    return s ? s->vk : 0;
}

bool ScanCodeName(std::uint16_t scan_code, std::string* name) {
    const ScanKey* s = FindScan(scan_code);
    if (!s || !SDL_WasInit(SDL_INIT_VIDEO)) return false;
    const SDL_Keycode code = SDL_GetKeyFromScancode(s->sdl, SDL_KMOD_NONE, false);
    const char* text = SDL_GetKeyName(code);
    if (!text || !*text) return false;
    *name = text;
    return true;
}

// Only the fallback window reads keys this way, and Linux has none.
bool SystemKeyDown(int) { return false; }

}  // namespace ps2platform
