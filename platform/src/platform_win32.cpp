/*
 * The platform layer on Windows (include/ps2platform/platform.h, keys.h): the
 * Win32 calls the host and the GPU display made in place before 2026-11-17,
 * moved here unchanged, with what each is for.
 */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include <timeapi.h>
#include <intrin.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <thread>

#include "ps2platform/keys.h"
#include "ps2platform/platform.h"

#if defined(_MSC_VER)
#pragma comment(lib, "synchronization.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "comdlg32.lib")
#endif

namespace ps2platform {

// ---- Fibers. A Fiber* is the fiber's own address: nothing between the scheduler and SwitchToFiber.
// (One calling convention on x64 and ARM64: a FiberEntry is a fiber's start routine as it is.)
static_assert(sizeof(void*) == 8, "the 64-bit Windows calling convention: FiberEntry is LPFIBER_START_ROUTINE");

Fiber* ThreadAsFiber() { return static_cast<Fiber*>(ConvertThreadToFiber(nullptr)); }

Fiber* CreateFiber(std::size_t stack_bytes, FiberEntry entry, void* parameter) {
    return static_cast<Fiber*>(::CreateFiber(stack_bytes, reinterpret_cast<LPFIBER_START_ROUTINE>(entry), parameter));
}

void SwitchFiber(Fiber* to) { SwitchToFiber(to); }

void DeleteFiber(Fiber* fiber) { ::DeleteFiber(fiber); }

// ---- Waiting.
void WaitOnValue(std::atomic<std::uint32_t>& value, std::uint32_t seen, std::uint32_t timeout_ms) {
    static_assert(sizeof(std::atomic<std::uint32_t>) == sizeof(std::uint32_t), "an atomic as its value");
    WaitOnAddress(&value, &seen, sizeof seen, timeout_ms);
}

void WakeAll(std::atomic<std::uint32_t>& value) { WakeByAddressAll(&value); }

/* A high-resolution waitable timer (Windows 10 1803 on), not Sleep's
   millisecond steps that can overshoot by more; one per thread. */
void SleepUntil(std::chrono::steady_clock::time_point when) {
    thread_local HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                                       TIMER_ALL_ACCESS);
    const auto left = when - std::chrono::steady_clock::now();
    if (left <= std::chrono::steady_clock::duration::zero()) return;
    if (timer) {
        LARGE_INTEGER due;
        due.QuadPart = -std::max<long long>(1, std::chrono::duration_cast<std::chrono::nanoseconds>(left).count() / 100);
        if (SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0)) {
            WaitForSingleObject(timer, INFINITE);
            return;
        }
    }
    std::this_thread::sleep_until(when);
}

// ---- The process's real time.
void FineTimers() {
    timeBeginPeriod(1);             // sleeps to the millisecond, not Windows' 15.6 ms
    // ...even with the window hidden behind others: Windows 11 drops a
    // process's timer resolution then (a 200 us sleep took 15 ms, a blank
    // seen 14 ms late), unless told not to; and the process not throttled.
    PROCESS_POWER_THROTTLING_STATE process{};
    process.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    process.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    process.StateMask = 0;
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &process, sizeof process);
}

// ---- Threads.
void SetThreadPriority(Priority priority) {
    int level = THREAD_PRIORITY_NORMAL;
    switch (priority) {
        case Priority::kNormal: level = THREAD_PRIORITY_NORMAL; break;
        case Priority::kAboveNormal: level = THREAD_PRIORITY_ABOVE_NORMAL; break;
        case Priority::kHighest: level = THREAD_PRIORITY_HIGHEST; break;
        case Priority::kTimeCritical: level = THREAD_PRIORITY_TIME_CRITICAL; break;
    }
    ::SetThreadPriority(GetCurrentThread(), level);
}

void NoPowerThrottling() {
    THREAD_POWER_THROTTLING_STATE throttling{};
    throttling.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
    throttling.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
    throttling.StateMask = 0;               // never throttled
    SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &throttling, sizeof throttling);
}

void* NameThread(const char* name) {
    wchar_t wide[64] = {};
    for (int i = 0; i < 63 && name[i]; ++i) wide[i] = wchar_t(name[i]);
    SetThreadDescription(GetCurrentThread(), wide);
    HANDLE self = nullptr;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &self,
                    THREAD_QUERY_LIMITED_INFORMATION, FALSE, 0);
    return self;
}

namespace {
std::uint64_t Ticks(const FILETIME& t) { return std::uint64_t(t.dwHighDateTime) << 32 | t.dwLowDateTime; }
}  // namespace

bool ThreadCpuNs(void* thread, std::uint64_t* total_ns, std::uint64_t* kernel_ns) {
    FILETIME created, exited, kernel, user;
    if (!thread || !GetThreadTimes(static_cast<HANDLE>(thread), &created, &exited, &kernel, &user)) return false;
    *total_ns = (Ticks(kernel) + Ticks(user)) * 100;
    *kernel_ns = Ticks(kernel) * 100;
    return true;
}

std::uint64_t ProcessCpuNs() {
    FILETIME created, exited, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0;
    return (Ticks(kernel) + Ticks(user)) * 100;
}

// ---- Cores.
int ProcessCores() {
    DWORD_PTR process = 0, system = 0;
    int cores = int(std::thread::hardware_concurrency());
    if (GetProcessAffinityMask(GetCurrentProcess(), &process, &system) && process) {
        cores = 0;
        for (DWORD_PTR m = process; m; m &= m - 1) ++cores;
    }
    return cores;
}

bool HoldToCores(std::uint64_t mask) {
    return mask && SetProcessAffinityMask(GetCurrentProcess(), DWORD_PTR(mask));
}

// ---- Files and the process.
std::filesystem::path ExePath() {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    return std::filesystem::path(exe);
}

bool Exists(const std::string& path) { return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

int CompareNoCase(const char* a, const char* b) { return _stricmp(a, b); }

void AttachTerminal() {
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out != nullptr && out != INVALID_HANDLE_VALUE) return;
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;
    std::FILE* stream = nullptr;
    freopen_s(&stream, "CONOUT$", "w", stdout);
    freopen_s(&stream, "CONOUT$", "w", stderr);
}

/* Opened once, shared (fopen's would lock the file to one stream), and given
   to both: their lines in the order they were written. */
void RedirectOutput(const std::string& log) {
    std::FILE* stream = nullptr;
    freopen_s(&stream, "NUL", "w", stdout);    // streams with descriptors, in a windowed program
    freopen_s(&stream, "NUL", "w", stderr);
    const int file = _open(log.c_str(), _O_WRONLY | _O_CREAT | _O_TRUNC | _O_TEXT, _S_IREAD | _S_IWRITE);
    if (file >= 0) {
        _dup2(file, _fileno(stdout));
        _dup2(file, _fileno(stderr));
        _close(file);
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // a run ended by force loses nothing
    std::setvbuf(stderr, nullptr, _IONBF, 0);
}

namespace {
// A string value of HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion ("" if none).
std::string WindowsVersionValue(const char* name) {
    char text[256] = {};
    DWORD size = sizeof text;
    if (RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", name, RRF_RT_REG_SZ,
                     nullptr, text, &size) != ERROR_SUCCESS)
        return {};
    return text;
}

std::string Narrow(const wchar_t* text) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(std::size_t(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), n, nullptr, nullptr);
    return out;
}
}  // namespace

std::vector<std::string> SystemReport() {
    std::vector<std::string> lines;
    char line[512];
    // The OS: Windows' own numbers (RtlGetVersion: GetVersionEx lies to programs without a manifest) and name.
    const HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof version;
    if (ntdll) {
        if (auto get = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"))) get(&version);
    }
    // (ProductName stays "Windows 10 ..." on Windows 11: build 22000 on is 11.)
    std::string product = WindowsVersionValue("ProductName");
    if (version.dwBuildNumber >= 22000 && product.rfind("Windows 10", 0) == 0) product.replace(8, 2, "11");
    std::snprintf(line, sizeof line, "OS: %s %s (Windows %lu.%lu build %lu)", product.c_str(),
                  WindowsVersionValue("DisplayVersion").c_str(), static_cast<unsigned long>(version.dwMajorVersion),
                  static_cast<unsigned long>(version.dwMinorVersion), static_cast<unsigned long>(version.dwBuildNumber));
    lines.push_back(line);
    // Under Wine: its version, its build (Proton's names itself there) and the system under it; Steam's Proton
    // by the variables it sets.
    if (ntdll) {
        using WineVersionFn = const char*(CDECL*)();
        using WineHostFn = void(CDECL*)(const char**, const char**);
        if (auto wine = reinterpret_cast<WineVersionFn>(GetProcAddress(ntdll, "wine_get_version"))) {
            std::string text = std::string("Wine: ") + wine();
            if (auto build = reinterpret_cast<WineVersionFn>(GetProcAddress(ntdll, "wine_get_build_id")))
                text += std::string(", build ") + build();
            if (auto host = reinterpret_cast<WineHostFn>(GetProcAddress(ntdll, "wine_get_host_version"))) {
                const char* sysname = nullptr;
                const char* release = nullptr;
                host(&sysname, &release);
                if (sysname) text += std::string(", on ") + sysname + (release ? std::string(" ") + release : "");
            }
            lines.push_back(text);
            const char* steam = std::getenv("STEAM_COMPAT_DATA_PATH");
            const char* tools = std::getenv("STEAM_COMPAT_TOOL_PATHS");
            if (steam || tools) {
                std::string proton = "Proton: under Steam";
                // The tool's "version" file (a Unix path: Wine's Z: drive).
                if (tools) {
                    std::string first = tools;
                    first = first.substr(0, first.find(':'));
                    std::string path = "Z:" + first + "/version";
                    for (char& ch : path) if (ch == '/') ch = '\\';
                    if (std::FILE* f = std::fopen(path.c_str(), "r")) {
                        char text[128] = {};
                        if (std::fgets(text, sizeof text, f)) {
                            std::string v = text;
                            while (!v.empty() && (v.back() == '\n' || v.back() == '\r')) v.pop_back();
                            proton += ", " + v;
                        }
                        std::fclose(f);
                    }
                    proton += " (" + first + ")";
                }
                lines.push_back(proton);
            }
        }
    }
    // The processor: its name, cores and threads.
    int info[4] = {};
    char brand[49] = {};
    __cpuid(info, int(0x80000000));
    if (unsigned(info[0]) >= 0x80000004u) {
        for (int i = 0; i < 3; ++i) {
            __cpuid(info, int(0x80000002 + i));
            std::memcpy(brand + i * 16, info, 16);
        }
    }
    std::string name = brand;
    while (!name.empty() && name.back() == ' ') name.pop_back();
    while (!name.empty() && name.front() == ' ') name.erase(0, 1);
    DWORD bytes = 0;
    unsigned cores = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &bytes);
    std::vector<unsigned char> buffer(bytes);
    if (bytes && GetLogicalProcessorInformationEx(RelationProcessorCore,
                                                  reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data()),
                                                  &bytes)) {
        for (DWORD at = 0; at < bytes;) {
            const auto* entry = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() + at);
            ++cores;
            at += entry->Size;
        }
    }
    std::snprintf(line, sizeof line, "CPU: %s, %u cores, %lu threads", name.empty() ? "?" : name.c_str(), cores,
                  static_cast<unsigned long>(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)));
    lines.push_back(line);
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof memory;
    if (GlobalMemoryStatusEx(&memory)) {
        std::snprintf(line, sizeof line, "RAM: %.1f GB, %.1f GB free", double(memory.ullTotalPhys) / 1073741824.0,
                      double(memory.ullAvailPhys) / 1073741824.0);
        lines.push_back(line);
    }
    // The power plan (powrprof, loaded here: none under some Wines).
    if (HMODULE power = LoadLibraryA("powrprof.dll")) {
        using ActiveFn = DWORD(WINAPI*)(HKEY, GUID**);
        using NameFn = DWORD(WINAPI*)(HKEY, const GUID*, const GUID*, const GUID*, UCHAR*, DWORD*);
        auto active = reinterpret_cast<ActiveFn>(GetProcAddress(power, "PowerGetActiveScheme"));
        auto read = reinterpret_cast<NameFn>(GetProcAddress(power, "PowerReadFriendlyName"));
        GUID* scheme = nullptr;
        const DWORD got = active && read ? active(nullptr, &scheme) : ERROR_PROC_NOT_FOUND;
        if (got == ERROR_SUCCESS && scheme) {
            DWORD size = 0;                                 // its size first (ERROR_MORE_DATA otherwise)
            read(nullptr, scheme, nullptr, nullptr, nullptr, &size);
            std::vector<wchar_t> text(size / sizeof(wchar_t) + 2, L'\0');
            size = DWORD(text.size() * sizeof(wchar_t));
            const DWORD named = read(nullptr, scheme, nullptr, nullptr, reinterpret_cast<UCHAR*>(text.data()), &size);
            if (named == ERROR_SUCCESS) {
                lines.push_back("power plan: " + Narrow(text.data()));
            } else {
                std::snprintf(line, sizeof line, "power plan: {%08lX-...} (its name: error %lu)",
                              static_cast<unsigned long>(scheme->Data1), static_cast<unsigned long>(named));
                lines.push_back(line);
            }
            LocalFree(scheme);
        } else {
            std::snprintf(line, sizeof line, "power plan: unknown (error %lu)", static_cast<unsigned long>(got));
            lines.push_back(line);
        }
        FreeLibrary(power);
    } else {
        lines.push_back("power plan: unknown (no powrprof)");
    }
    return lines;
}

bool RestartSelf() {
    std::wstring command = GetCommandLineW();
    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) {
        std::fprintf(stderr, "[port] restart failed (%lu)\n", static_cast<unsigned long>(GetLastError()));
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

void ExitNow(int code) {
    TerminateProcess(GetCurrentProcess(), UINT(code));
    std::_Exit(code);
}

std::tm LocalTime(std::time_t when) {
    std::tm local{};
    localtime_s(&local, &when);
    return local;
}

// ---- Dialogs.
void ShowMessage(const char* title, const std::string& text, bool error) {
    MessageBoxA(nullptr, text.c_str(), title, MB_OK | (error ? MB_ICONERROR : MB_ICONWARNING));
}

std::string PickFile(const char* title, const char* filter) {
    char file[MAX_PATH] = {};
    OPENFILENAMEA dialog = {};
    dialog.lStructSize = sizeof dialog;
    dialog.lpstrFilter = filter;
    dialog.lpstrFile = file;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrTitle = title;
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    return GetOpenFileNameA(&dialog) ? std::string(file) : std::string();
}

// ---- Text for the overlay: GDI's Consolas, bold, antialiased.
bool DrawTextPanel(const std::vector<std::string>& lines, int line_px, const std::vector<std::uint32_t>& colours,
                   std::uint32_t panel, int max_width, int max_height, std::vector<std::uint32_t>* pixels, int* width,
                   int* height) {
    auto rgb = [](std::uint32_t c) { return RGB((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF); };
    pixels->clear();
    HDC dc = CreateCompatibleDC(nullptr);
    HFONT font = CreateFontA(-(line_px - 4), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
    HGDIOBJ old_font = SelectObject(dc, font);
    int w = 0;
    for (const std::string& text : lines) {
        SIZE size{};
        GetTextExtentPoint32A(dc, text.c_str(), int(text.size()), &size);
        w = std::max(w, int(size.cx));
    }
    w = std::min(max_width, w + line_px);
    const int h = std::min(max_height, int(lines.size()) * line_px + line_px / 2);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof info.bmiHeader;
    info.bmiHeader.biWidth = w;
    info.bmiHeader.biHeight = -h;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bitmap && bits) {
        HGDIOBJ old_bitmap = SelectObject(dc, bitmap);
        const RECT all{0, 0, w, h};
        HBRUSH brush = CreateSolidBrush(rgb(panel));
        FillRect(dc, &all, brush);
        DeleteObject(brush);
        SetBkMode(dc, TRANSPARENT);
        for (std::size_t i = 0; i < lines.size(); ++i) {
            SetTextColor(dc, rgb(i < colours.size() ? colours[i] : 0xEBEBEBu));
            TextOutA(dc, line_px / 2, int(i) * line_px + line_px / 4, lines[i].c_str(), int(lines[i].size()));
        }
        GdiFlush();
        pixels->assign(static_cast<const std::uint32_t*>(bits), static_cast<const std::uint32_t*>(bits) + w * h);
        for (std::uint32_t& p : *pixels) p |= 0xFF000000u;
        SelectObject(dc, old_bitmap);
        DeleteObject(bitmap);
    }
    SelectObject(dc, old_font);
    DeleteObject(font);
    DeleteDC(dc);
    *width = w;
    *height = h;
    return true;
}

// ---- The fallback window: a plain Win32 window drawn with GDI.
namespace {
HWND g_window = nullptr;
bool g_close_seen = false;

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM w, LPARAM l) {
    switch (message) {
        case WM_CLOSE:
            g_close_seen = true;
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            g_window = nullptr;
            return 0;
        default:
            return DefWindowProcW(window, message, w, l);
    }
}
}  // namespace

bool OpenFallbackWindow(const wchar_t* title, int width, int height) {
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"ps2host";
    RegisterClassW(&wc);
    RECT r = {0, 0, width, height};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    g_window = CreateWindowW(L"ps2host", title, WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT,
                             CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr,
                             wc.hInstance, nullptr);
    return g_window != nullptr;
}

bool FallbackWindowOpen() { return g_window != nullptr; }

bool FallbackWindowFocused() { return g_window && GetForegroundWindow() == g_window; }

void PumpFallbackWindow(bool* closed) {
    g_close_seen = false;
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (g_close_seen) *closed = true;
}

void DrawFallbackWindow(const std::uint32_t* pixels, std::uint32_t w, std::uint32_t h) {
    if (!g_window) return;
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = LONG(w);
    info.bmiHeader.biHeight = -LONG(h);                 // top-down rows
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    RECT client;
    GetClientRect(g_window, &client);
    HDC dc = GetDC(g_window);
    SetStretchBltMode(dc, HALFTONE);
    StretchDIBits(dc, 0, 0, client.right, client.bottom, 0, 0, LONG(w), LONG(h), pixels, &info,
                  DIB_RGB_COLORS, SRCCOPY);
    ReleaseDC(g_window, dc);
}

// ---- Keys (keys.h): the names are Windows' own numbers.
static_assert(key::kBack == VK_BACK && key::kTab == VK_TAB && key::kReturn == VK_RETURN && key::kEscape == VK_ESCAPE &&
              key::kSpace == VK_SPACE && key::kPrior == VK_PRIOR && key::kNext == VK_NEXT && key::kEnd == VK_END &&
              key::kHome == VK_HOME && key::kLeft == VK_LEFT && key::kUp == VK_UP && key::kRight == VK_RIGHT &&
              key::kDown == VK_DOWN && key::kInsert == VK_INSERT && key::kDelete == VK_DELETE &&
              key::kCapital == VK_CAPITAL && key::kNumpad0 == VK_NUMPAD0 && key::kNumpad1 == VK_NUMPAD1 &&
              key::kNumpad9 == VK_NUMPAD9 && key::kMultiply == VK_MULTIPLY && key::kAdd == VK_ADD &&
              key::kSubtract == VK_SUBTRACT && key::kDecimal == VK_DECIMAL && key::kDivide == VK_DIVIDE &&
              key::kF1 == VK_F1 && key::kF9 == VK_F9 && key::kF12 == VK_F12 && key::kLShift == VK_LSHIFT &&
              key::kRShift == VK_RSHIFT && key::kLControl == VK_LCONTROL && key::kRControl == VK_RCONTROL &&
              key::kLMenu == VK_LMENU && key::kRMenu == VK_RMENU && key::kOem1 == VK_OEM_1 &&
              key::kOemPlus == VK_OEM_PLUS && key::kOemComma == VK_OEM_COMMA && key::kOemMinus == VK_OEM_MINUS &&
              key::kOemPeriod == VK_OEM_PERIOD && key::kOem2 == VK_OEM_2 && key::kOem3 == VK_OEM_3 &&
              key::kOem4 == VK_OEM_4 && key::kOem5 == VK_OEM_5 && key::kOem6 == VK_OEM_6 && key::kOem7 == VK_OEM_7,
              "keys.h's numbers are Windows' virtual keys");

int KeyOfScanCode(std::uint16_t scan_code) { return int(MapVirtualKeyW(scan_code, MAPVK_VSC_TO_VK_EX)); }

bool ScanCodeName(std::uint16_t scan_code, std::string* name) {
    wchar_t wide[64] = {};
    const LONG lparam = LONG((scan_code & 0xFF) << 16) | ((scan_code & 0xE000) == 0xE000 ? (1L << 24) : 0);
    if (GetKeyNameTextW(lparam, wide, 64) <= 0) return false;
    name->clear();
    for (const wchar_t* p = wide; *p; ++p) *name += (*p < 128 ? char(*p) : '?');
    return true;
}

bool SystemKeyDown(int key) { return (GetAsyncKeyState(key) & 0x8000) != 0; }

}  // namespace ps2platform
