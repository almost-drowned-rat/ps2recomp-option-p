/*
 * The port's window (--window): the GS's displayed picture, every vertical
 * blank, scaled to the window, and the keyboard the controller reads.
 *
 * With the GPU display built in (PS2HOST_VULKAN, engine/gpu) the window is an
 * SDL3 window presenting through Vulkan; without it, or when no Vulkan device
 * can be had, a plain Win32 window drawn with GDI. Both are pumped on the host's
 * thread at each frame (FrameDone).
 */

#include <algorithm>
#include <chrono>
#include <deque>
#include <thread>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <functional>
#include <vector>

#include "game_text.h"
#include "ps2host/gs.h"
#include "ps2host/timeline.h"
#include "ps2host/run.h"
#include "ps2host/settings.h"
#include "ps2host/texture_pack.h"
#include "ps2platform/keys.h"
#include "ps2platform/platform.h"

#ifdef PS2HOST_VULKAN
#include <SDL3/SDL_scancode.h>

#include "ps2gpu/display.h"
#endif

namespace ps2host {

namespace {

bool g_closed = false;

#ifdef PS2HOST_VULKAN
std::unique_ptr<ps2gpu::Display> g_display;

// The keys the controller reads, as the host's key codes (keys.h: Windows' virtual keys), by position.
/* A host key code as SDL's scancode: the key in that place on a US
   keyboard (SDL names keys by place). The punctuation's key codes are a
   US layout's. */
int Scancode(int k) {
    namespace key = ps2platform::key;
    switch (k) {
        case key::kBack: return SDL_SCANCODE_BACKSPACE;
        case key::kTab: return SDL_SCANCODE_TAB;
        case key::kReturn: return SDL_SCANCODE_RETURN;
        case key::kEscape: return SDL_SCANCODE_ESCAPE;      // the PC settings panel's
        case key::kSpace: return SDL_SCANCODE_SPACE;
        case key::kUp: return SDL_SCANCODE_UP;
        case key::kDown: return SDL_SCANCODE_DOWN;
        case key::kLeft: return SDL_SCANCODE_LEFT;
        case key::kRight: return SDL_SCANCODE_RIGHT;
        case key::kPrior: return SDL_SCANCODE_PAGEUP;
        case key::kNext: return SDL_SCANCODE_PAGEDOWN;
        case key::kHome: return SDL_SCANCODE_HOME;
        case key::kEnd: return SDL_SCANCODE_END;
        case key::kInsert: return SDL_SCANCODE_INSERT;
        case key::kDelete: return SDL_SCANCODE_DELETE;
        case key::kCapital: return SDL_SCANCODE_CAPSLOCK;
        case key::kLShift: return SDL_SCANCODE_LSHIFT;
        case key::kRShift: return SDL_SCANCODE_RSHIFT;
        case key::kLControl: return SDL_SCANCODE_LCTRL;
        case key::kRControl: return SDL_SCANCODE_RCTRL;
        case key::kLMenu: return SDL_SCANCODE_LALT;
        case key::kRMenu: return SDL_SCANCODE_RALT;
        case key::kNumpad0: return SDL_SCANCODE_KP_0;
        case key::kDecimal: return SDL_SCANCODE_KP_PERIOD;
        case key::kAdd: return SDL_SCANCODE_KP_PLUS;
        case key::kSubtract: return SDL_SCANCODE_KP_MINUS;
        case key::kMultiply: return SDL_SCANCODE_KP_MULTIPLY;
        case key::kDivide: return SDL_SCANCODE_KP_DIVIDE;
        case key::kOem1: return SDL_SCANCODE_SEMICOLON;
        case key::kOemPlus: return SDL_SCANCODE_EQUALS;
        case key::kOemComma: return SDL_SCANCODE_COMMA;
        case key::kOemMinus: return SDL_SCANCODE_MINUS;
        case key::kOemPeriod: return SDL_SCANCODE_PERIOD;
        case key::kOem2: return SDL_SCANCODE_SLASH;
        case key::kOem3: return SDL_SCANCODE_GRAVE;
        case key::kOem4: return SDL_SCANCODE_LEFTBRACKET;
        case key::kOem5: return SDL_SCANCODE_BACKSLASH;
        case key::kOem6: return SDL_SCANCODE_RIGHTBRACKET;
        case key::kOem7: return SDL_SCANCODE_APOSTROPHE;
        default: break;
    }
    if (k >= key::kF1 && k <= key::kF12) return SDL_SCANCODE_F1 + (k - key::kF1);
    if (k >= 'A' && k <= 'Z') return SDL_SCANCODE_A + (k - 'A');
    if (k >= '1' && k <= '9') return SDL_SCANCODE_1 + (k - '1');
    if (k == '0') return SDL_SCANCODE_0;
    if (k >= key::kNumpad1 && k <= key::kNumpad9) return SDL_SCANCODE_KP_1 + (k - key::kNumpad1);
    return -1;
}
#endif

// The fallback window (platform.h: Windows' GDI window): its messages, then the GS's picture scaled to it.
void PresentFallback() {
    ps2platform::PumpFallbackWindow(&g_closed);
    if (!ps2platform::FallbackWindowOpen()) return;
    std::vector<u32> pixels;
    u32 w = 0, h = 0;
    if (!gs::TheGs().Picture(&pixels, &w, &h)) return;
    ps2platform::DrawFallbackWindow(pixels.data(), w, h);
}

#ifdef PS2HOST_VULKAN
/* The "antialiasing" setting as the display takes it: supersampling's
   pictures (drawn larger, SupersamplingFactor) averaged down. */
ps2gpu::Display::AntiAliasing DisplayAntiAliasing(const std::string& mode) {
    using AA = ps2gpu::Display::AntiAliasing;
    return mode == "fxaa"                        ? AA::kFxaa
           : mode == "smaa"                      ? AA::kSmaa
           : mode == "ssaa2" || mode == "ssaa4" ? AA::kDownsample
                                                 : AA::kOff;
}
#endif

/* ASPECT RATIO (porting guide 4: 4:3 / 16:9 / 16:10 / 21:9, never stretched): only a game with its own wide
   picture (SetWidescreen - a projection + HUD patch, or the game's own setting) leaves 4:3. */
std::function<bool()> g_shows_wide;     // SetWidescreen's: the current picture is the wide one

/* The "aspect" setting as the display takes it: "4:3" (the default), "16:9", "16:10", or "21:9" - an ultrawide
   screen's exact shape while it lies in 2.30-2.42 (43:18 on 3440x1440, 64:27 on 2560x1080), else 64:27 (as
   Orta). "auto" (the window's shape, a stretch) and other shapes: 4:3. 4:3 too for a game with no wide picture. */
float SettingAspect() {
    if (!g_shows_wide) return 4.0f / 3.0f;
    const std::string aspect = Setting("aspect", "4:3");
    if (aspect == "16:9") return 16.0f / 9.0f;
    if (aspect == "16:10") return 16.0f / 10.0f;
    if (aspect == "21:9") {
        static float screen = 0.0f;     // the screen's, once known
#ifdef PS2HOST_VULKAN
        if (screen <= 0.0f && g_display) screen = g_display->ScreenAspect();
#endif
        return screen >= 2.30f && screen <= 2.42f ? screen : 64.0f / 27.0f;
    }
    return 4.0f / 3.0f;
}

}  // namespace

std::string g_title;                // the window's title as opened (SetWindowStatus adds to it)
void ApplyCursorArt();               // the game's pointer to a window opened after it was set

std::string& GameTitleText() {
    static std::string title;
    return title;
}

void SetGameTitle(const std::string& title) { GameTitleText() = title; }
const std::string& GameTitle() { return GameTitleText(); }

bool OpenWindow(const wchar_t* title, int width, int height) {
#ifdef PS2HOST_VULKAN
    std::string narrow;
    for (const wchar_t* p = title; *p; ++p) narrow += char(*p < 128 ? *p : '?');
    if (!GameTitle().empty()) narrow = GameTitle();
    g_title = narrow;
    std::string error;
    g_display = ps2gpu::CreateVulkanDisplay(narrow.c_str(), width, height, &error);
    if (g_display) {
        Note("window", "window: Vulkan on " + g_display->Describe());
        g_display->SetBorderless(Setting("fullscreen_mode", "fullscreen") == "borderless");
#ifdef __ANDROID__
        g_display->SetFullscreen(true);     // (Android: a window is the whole screen; windowed showed the system bars)
#else
        if (SettingOn("fullscreen")) g_display->SetFullscreen(true);
#endif
        ApplyCursorArt();
        g_display->SetPresentObserver([](std::uint64_t due_ns, std::uint64_t shown_ns, bool dropped) {
            // Shown on the display's own thread (its lane named so); dropped
            // on the thread handing it a newer one.
            thread_local bool named = false;
            if (!dropped && !named) {
                timeline::NameThread("present");
                named = true;
            }
            if (dropped) timeline::Dropped(due_ns);
            else timeline::Presented(due_ns, shown_ns);
        });
        if (SettingOn("vsync")) g_display->SetVsync(true);
        g_display->SetAspect(SettingAspect());
        g_display->SetAntiAliasing(DisplayAntiAliasing(Setting("antialiasing", "off")));
        g_display->SetUpscaling(Setting("upscaling", "linear").rfind("fsr", 0) == 0
                                    ? ps2gpu::Display::Upscaling::kFsr : ps2gpu::Display::Upscaling::kLinear);
        g_display->SetFsrSharpness(FsrSharpnessStops());
        g_display->SetBloom(BloomStrength());
        // Where the player left it (KeepWindowRect): its place and height, its width from the opening shape.
        int x = 0, y = 0, w = 0, h = 0;
        if (std::sscanf(Setting("window_rect", "").c_str(), "%d,%d,%d,%d", &x, &y, &w, &h) == 4 && h >= 240 &&
            !g_display->Fullscreen())
            g_display->SetWindowRect(x, y, int(float(h) * float(width) / float(height) + 0.5f), h);
        return true;
    }
    Note("window", "window: no Vulkan display (" + error + "); drawing with GDI");
#endif
    return ps2platform::OpenFallbackWindow(title, width, height);
}

bool WindowOpen() {
#ifdef PS2HOST_VULKAN
    if (g_display) return !g_closed;
#endif
    return ps2platform::FallbackWindowOpen();
}

bool WindowFocused() {
#ifdef PS2HOST_VULKAN
    if (g_display) return g_display->Focused();
#endif
    return ps2platform::FallbackWindowFocused();
}

bool WindowClosed() { return g_closed; }

// A game's own EXIT GAME: as if the window had been closed.
void QuitGame() { g_closed = true; }

namespace {
bool g_restart = false;
}  // namespace
// The run ends here too (run.h): a port's own RESTART NOW needs no QuitGame of its own.
void RequestRestart() {
    g_restart = true;
    QuitGame();
}
bool RestartRequested() { return g_restart; }

bool HostKeyDown(int key) {
    if (ScriptedKeyDown(key)) return true;          // --key
#ifdef PS2HOST_VULKAN
    if (g_display) return g_display->KeyDown(Scancode(key));
#endif
    return ps2platform::SystemKeyDown(key);
}

void HostTriggerEffects(const HostTriggerEffect& left, const HostTriggerEffect& right) {
#ifdef PS2HOST_VULKAN
    if (g_display) {
        auto effect = [](const HostTriggerEffect& e) {
            ps2gpu::Display::TriggerEffect t;
            t.kind = ps2gpu::Display::TriggerEffect::Kind(e.kind);
            t.start = e.start;
            t.force = e.force;
            t.frequency = e.frequency;
            return t;
        };
        g_display->SetTriggerEffects(effect(left), effect(right));
    }
#else
    (void)left, (void)right;
#endif
}

void ReleaseController() {
#ifdef PS2HOST_VULKAN
    if (g_display) g_display->ReleaseController();
#endif
}

void HostRumble(u8 large, u8 small) {
#ifdef PS2HOST_VULKAN
    if (g_display) g_display->SetRumble(u16(large * 257), u16(small * 257));
#else
    (void)large, (void)small;
#endif
}

bool TakeHostMouse(HostMouseState* state) {
    if (ScriptedMouse(state)) return true;      // --mouse
#ifdef PS2HOST_VULKAN
    ps2gpu::Display::MouseState m;
    if (g_display && g_display->TakeMouse(&m)) {
        state->over = m.over;
        state->x = m.x;
        state->y = m.y;
        state->held = m.held;
        state->pressed = m.pressed;
        state->wheel = m.wheel;
        state->moved = m.moved;
        return true;
    }
#endif
    (void)state;
    return false;
}

// The game's pointer (SetHostCursor), kept for a window opened after it is set.
struct CursorArt {
    std::vector<u32> rgba;
    u32 width = 0, height = 0, hot_x = 0, hot_y = 0;
};
CursorArt& TheCursorArt() {
    static CursorArt art;
    return art;
}

void ApplyCursorArt() {
#ifdef PS2HOST_VULKAN
    const CursorArt& a = TheCursorArt();
    if (g_display && !a.rgba.empty())
        g_display->SetCursorArt(std::vector<std::uint32_t>(a.rgba.begin(), a.rgba.end()), a.width, a.height,
                                a.hot_x, a.hot_y);
#endif
}

void SetHostCursor(const u32* rgba, u32 width, u32 height, u32 hot_x, u32 hot_y) {
    TheCursorArt() = CursorArt{std::vector<u32>(rgba, rgba + std::size_t(width) * height), width, height, hot_x, hot_y};
    ApplyCursorArt();
}

void ShowHostCursor(bool on) {
#ifdef PS2HOST_VULKAN
    if (g_display) g_display->ShowGameCursor(on);
#else
    (void)on;
#endif
}

#ifdef PS2HOST_VULKAN
namespace {
ControllerFamily Family(std::uint8_t family) {
    return family == ps2gpu::Display::kPlayStationController ? ControllerFamily::kPlayStation
           : family == ps2gpu::Display::kXboxController      ? ControllerFamily::kXbox
                                                             : ControllerFamily::kOther;
}
void Convert(const ps2gpu::Display::PadState& pad, HostPadState* state) {
    state->buttons = pad.buttons;
    for (int i = 0; i < 4; ++i) state->sticks[i] = pad.sticks[i];
    state->l2 = pad.l2;
    state->r2 = pad.r2;
    state->family = Family(pad.family);
}
}  // namespace
#endif

bool HostPad(HostPadState* state) {
#ifdef PS2HOST_VULKAN
    ps2gpu::Display::PadState pad;
    if (g_display && g_display->Pad(&pad)) {
        Convert(pad, state);
        return true;
    }
#endif
    (void)state;
    return false;   // the GDI window has none
}

std::vector<HostDevice> WindowControllers() {
    std::vector<HostDevice> list;
#ifdef PS2HOST_VULKAN
    if (g_display)
        for (const auto& c : g_display->Controllers()) list.push_back({c.id, c.name, Family(c.family)});
#endif
    return list;
}

bool WindowControllerPad(u32 id, HostPadState* state) {
#ifdef PS2HOST_VULKAN
    ps2gpu::Display::PadState pad;
    if (g_display && g_display->PadOf(id, &pad)) {
        Convert(pad, state);
        return true;
    }
#endif
    (void)id, (void)state;
    return false;
}

void HostRumbleOf(u32 id, u8 large, u8 small) {
#ifdef PS2HOST_VULKAN
    if (g_display) g_display->SetRumbleOf(id, u16(large * 257), u16(small * 257));
#else
    (void)id, (void)large, (void)small;
#endif
}

void HostTriggerTargets(const std::vector<u32>& ids) {
#ifdef PS2HOST_VULKAN
    if (g_display) g_display->SetTriggerTargets(std::vector<std::uint32_t>(ids.begin(), ids.end()));
#else
    (void)ids;
#endif
}

namespace {
// --gpu-show: where the window's picture comes from instead of the software GS.
std::function<bool(std::vector<u32>*, u32*, u32*)> g_picture_source;
}  // namespace

#ifdef PS2HOST_VULKAN
ps2gpu::Gpu* WindowGpu() { return g_display ? g_display->SharedGpu() : nullptr; }
#endif

void SetPictureSource(std::function<bool(std::vector<u32>*, u32*, u32*)> source) {
    g_picture_source = std::move(source);
}

namespace {
void RefreshOverlay();      // the overlay's lines and the FPS counter's, drawn
}  // namespace

// Alt+Enter changes fullscreen in the display's Pump: saved as the setting.
void KeepFullscreenSetting() {
#ifdef PS2HOST_VULKAN
    static int last = -1;
    const int now = g_display && g_display->Fullscreen() ? 1 : 0;
    if (last >= 0 && now != last) SetSettingOn("fullscreen", now != 0);
    last = now;
#endif
}

// The window's place and size saved once it has stood still for about a second (60 pumps unchanged) - the
// ports' rule: it opens there next time (OpenWindow).
void KeepWindowRect() {
#ifdef PS2HOST_VULKAN
    static int last[4] = {}, quiet = 0;
    static std::string saved = Setting("window_rect", "");
    int now[4] = {};
    if (!g_display || !g_display->WindowRect(&now[0], &now[1], &now[2], &now[3])) return;
    if (std::memcmp(now, last, sizeof now) != 0) {
        std::memcpy(last, now, sizeof now);
        quiet = 0;
        return;
    }
    if (++quiet != 60) return;
    const std::string text = std::to_string(now[0]) + "," + std::to_string(now[1]) + "," + std::to_string(now[2]) +
                             "," + std::to_string(now[3]);
    if (text != saved) {
        SetSetting("window_rect", text);
        saved = text;
    }
#endif
}

bool PumpWindow() {
#ifdef PS2HOST_VULKAN
    if (g_display) {
        if (!g_display->Pump()) g_closed = true;
        KeepFullscreenSetting();
        KeepWindowRect();
        if (g_display->TakeCapture()) RequestCapture();
        // A wide ASPECT RATIO: the game's wide picture fills the window; its 4:3 screens (menus, movies: the game
        // says which, SetWidescreen) stand 4:3 between bars - the window keeps its shape.
        // (Not while SELECT's menu holds the game: no 3D is drawn then, and the held wide picture went 4:3 between
        // bars, squeezed - his session 2026-11-20. It keeps the shape it had.)
        // (And for ten frames after it closes: the game's own sign of 3D comes back with its next frame.)
        static int settle = 0;
        if (SelectMenuOpen()) settle = 10;
        else if (settle > 0) --settle;
        if (g_shows_wide && settle == 0)
            g_display->SetPictureAspect(SettingAspect() > 4.0f / 3.0f + 0.01f && !g_shows_wide() ? 4.0f / 3.0f : 0.0f);
        UpdatePcMenu();                     // F9: the PC settings panel (pc_menu.cpp)
        UpdateSelectMenu();                 // Select: EXIT GAME / EXIT TO MENU (a port's EnableSelectMenu)
        // F1: the PERFORMANCE OVERLAY on or off; its figures redrawn twice a second.
        static std::chrono::steady_clock::time_point fps_drawn{};
        const auto now = std::chrono::steady_clock::now();
        if (g_display->TakeFpsToggle()) {       // (off <-> FPS + SPEED; the panel has the other kinds)
            SetSetting("performance_overlay", SettingPerformanceOverlay() == "off" ? "fps+speed" : "off");
            RefreshOverlay();
            fps_drawn = now;
        } else if (now - fps_drawn >= std::chrono::milliseconds(500)) {
            RefreshOverlay();
            fps_drawn = now;
        }
        // Tab: the texture pack on or off, said on the screen for two seconds.
        static std::chrono::steady_clock::time_point notice_until{};
        static bool notice = false;
        if (g_display->TakeTexturePackToggle() && texture_pack::Count()) {
            const bool on = !texture_pack::Enabled();
            texture_pack::SetEnabled(on);
            SetSettingOn("texture_pack", on);
            SetWindowOverlay({on ? "TEXTURE PACK ON  (Tab)" : "TEXTURE PACK OFF  (Tab)"});
            notice = true;
            notice_until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        }
        if (notice && std::chrono::steady_clock::now() >= notice_until) {
            notice = false;
            if (!PcMenuOpen()) SetWindowOverlay({});
        }
        return !g_closed;
    }
#endif
    if (ps2platform::FallbackWindowOpen()) PresentFallback();
    return !g_closed;                  // a game's EXIT GAME (QuitGame) with no window too
}

// SELECT's menu open: the game held still here (its frame, on the vblank path) - the window pumped, the last
// picture shown again with the menu, ~60 times a second - until it closes or the window does.
bool HoldForSelectMenu() {
    while (SelectMenuOpen()) {
        if (!PumpWindow()) return false;
#ifdef PS2HOST_VULKAN
        if (g_display) g_display->RepeatLast();
#endif
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    return !g_closed;
}

void SetWindowFullscreen(bool on) {
    SetSettingOn("fullscreen", on);
#ifdef PS2HOST_VULKAN
    if (g_display) g_display->SetFullscreen(on);
#endif
}

// DISPLAY MODE: "fullscreen" on/off is the state (Alt+Enter flips it), "fullscreen_mode" which fullscreen.
void SetWindowDisplayMode(const std::string& mode) {
    const bool borderless = mode == "borderless";
    if (mode != "windowed") SetSetting("fullscreen_mode", borderless ? "borderless" : "fullscreen");
#ifdef PS2HOST_VULKAN
    if (g_display && mode != "windowed") g_display->SetBorderless(borderless);
#endif
    SetWindowFullscreen(mode != "windowed");
}

std::string WindowDisplayMode() {
    if (!WindowFullscreen()) return "windowed";
    return Setting("fullscreen_mode", "fullscreen") == "borderless" ? "borderless" : "fullscreen";
}

namespace {

std::vector<std::string> g_overlay_lines;      // SetWindowOverlay's: the panel's, a notice's

// Draws the lines as the window's overlay (none when empty).
void DrawOverlay(const std::vector<std::string>& lines) {
#ifdef PS2HOST_VULKAN
    if (!g_display) return;
    if (lines.empty()) {
        g_display->SetOverlay({}, 0, 0);
        return;
    }
    // Text as the system draws it (platform.h), sized to the window (a 1080-line screen:
    // 22-pixel lines), white on a dark panel; a line starting with '>' in
    // yellow (the one chosen), the first (the title) blue.
    const std::uint32_t screen = std::max(480u, g_display->PixelHeight());
    const int line = std::max(14, int(screen / 48));
    std::vector<std::uint32_t> colours(lines.size());
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const bool chosen = !lines[i].empty() && lines[i][0] == '>';
        colours[i] = chosen ? 0xFFD23Cu : i == 0 ? 0x78BEFFu : 0xEBEBEBu;
    }
    std::vector<std::uint32_t> pixels;
    int width = 0, height = 0;
    if (!ps2platform::DrawTextPanel(lines, line, colours, 0x101420u, 1280, 720, &pixels, &width, &height)) return;
    g_display->SetOverlay(std::move(pixels), std::uint32_t(width), std::uint32_t(height));
#else
    (void)lines;
#endif
}

// The PERFORMANCE OVERLAY (F1, setting "performance_overlay"; off unless turned on): FPS = the pictures shown a
// second, FRAME TIME = the time a picture, SPEED = the game's own speed (GameSpeed: its blanks a real second, in
// % of the console's) - any of them, on a line under whatever else the overlay says.
void RefreshOverlay() {
    std::vector<std::string> lines = g_overlay_lines;
#ifdef PS2HOST_VULKAN
    if (g_display) {
        const bool fps_on = SettingOverlayShows("fps"), time_on = SettingOverlayShows("frametime");
        const bool speed_on = SettingOverlayShows("speed");
        std::string text;
        const auto add = [&text](const char* part) {
            if (!text.empty()) text += "  ";
            text += part;
        };
        char part[48];
        // All three from the game's own pictures (GamePictures: its flips, as ARMSX2's internal FPS) of the last
        // whole second - what the player sees, nothing averaged (the maintainer 2026-11-22):
        // FPS the pictures counted, SPEED them against the rate the game is made for (a game that moves a step a
        // picture is that much slower), FRAME TIME the longest wait between two (a wait going on counts).
        static int pictures = -1;
        static double frame_ms = -1;
        static auto measured = std::chrono::steady_clock::time_point{};
        static auto started = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        if (now - measured >= std::chrono::milliseconds(450)) {
            measured = now;
            const bool whole_second = now - started >= std::chrono::seconds(1) && GamePictures() > 0;
            pictures = whole_second ? GamePicturesInLast(1'000'000'000ull) : -1;
            frame_ms = GamePictures() > 1 ? TakeLongestGamePictureGap(0) : -1;
        }
        if (fps_on) {
            if (pictures >= 0) std::snprintf(part, sizeof part, "FPS %d", pictures);
            else std::snprintf(part, sizeof part, "FPS --");
            add(part);
        }
        if (time_on) {
            if (frame_ms >= 0) std::snprintf(part, sizeof part, "%.1f ms", frame_ms);
            else std::snprintf(part, sizeof part, "-- ms");
            add(part);
        }
        if (speed_on) {
            if (pictures >= 0) std::snprintf(part, sizeof part, "SPEED %.0f %%", 100.0 * pictures / GamePictureRate());
            else std::snprintf(part, sizeof part, "SPEED --");
            add(part);
        }
        if (!text.empty()) lines.push_back(text);
    }
    // SHADER NOTICE (setting "shader_notice", on unless turned off): while draws wait for, are left out for or
    // stand in for shaders being made (SHADER COMPILE).
    if (g_display && SettingOn("shader_notice", true) && GpuBuildingShaders()) lines.push_back("BUILDING SHADERS");
#endif
    static std::vector<std::string> shown;
    if (lines == shown) return;                 // drawn again only when it changes
    shown = lines;
    DrawOverlay(lines);
}

}  // namespace

std::string SettingPerformanceOverlay() {
    static const char* const kinds[] = {"off", "fps", "frametime", "speed", "fps+frametime", "fps+speed",
                                        "frametime+speed", "all"};
    const std::string v = Setting("performance_overlay", "");
    for (const char* k : kinds)
        if (v == k) return v;
    // Unset (or unknown): the FPS COUNTER's it replaced (2026-11-22).
    const std::string old = Setting("fps_counter", "off");
    return old == "on" ? "fps" : old == "frametime" ? "frametime" : old == "both" ? "fps+frametime" : "off";
}

bool SettingOverlayShows(const char* part) {
    const std::string v = SettingPerformanceOverlay();
    if (v == "all") return true;
    const std::string p = part;
    std::size_t at = 0;
    while (at <= v.size()) {
        const std::size_t end = std::min(v.find('+', at), v.size());
        if (v.compare(at, end - at, p) == 0) return true;
        at = end + 1;
    }
    return false;
}

void SetOverlayPart(const char* part, bool on) {
    std::string v;
    for (const char* p : {"fps", "frametime", "speed"}) {
        const bool shown = std::string(p) == part ? on : SettingOverlayShows(p);
        if (!shown) continue;
        if (!v.empty()) v += "+";
        v += p;
    }
    SetSetting("performance_overlay", v.empty() ? "off" : v == "fps+frametime+speed" ? "all" : v);
    RefreshOverlay();
}

void SetWindowOverlay(const std::vector<std::string>& lines) {
    g_overlay_lines = lines;
    RefreshOverlay();
}

void SetWindowStatus(const std::string& status) {
#ifdef PS2HOST_VULKAN
    if (g_display) g_display->SetTitle(status.empty() ? g_title : g_title + "  -  " + status);
#else
    (void)status;
#endif
}

void SetWindowAspect(const std::string& aspect) {
    SetSetting("aspect", aspect);
#ifdef PS2HOST_VULKAN
    if (g_display) g_display->SetAspect(SettingAspect());
#endif
}

void SetWindowAntiAliasing(const std::string& mode) {
    SetSetting("antialiasing", mode);
#ifdef PS2HOST_VULKAN
    if (g_display) g_display->SetAntiAliasing(DisplayAntiAliasing(mode));
#endif
}

void SetWindowUpscaling(const std::string& mode) {
    SetSetting("upscaling", mode);
#ifdef PS2HOST_VULKAN
    if (g_display) {
        g_display->SetUpscaling(mode.rfind("fsr", 0) == 0 ? ps2gpu::Display::Upscaling::kFsr
                                                         : ps2gpu::Display::Upscaling::kLinear);
        g_display->SetFsrSharpness(FsrSharpnessStops());
    }
#endif
}

// FSR SHARPNESS (the "fsr_sharpness" setting) in RCAS's stops: low 1.0, medium 0.5, high 0.25 (FSR's own until
// 2026-11-20, the default), max 0.
float FsrSharpnessStops() {
    const std::string v = Setting("fsr_sharpness", "high");
    return v == "low" ? 1.0f : v == "medium" ? 0.5f : v == "max" ? 0.0f : 0.25f;
}

void SetWindowFsrSharpness(const std::string& level) {
    SetSetting("fsr_sharpness", level);
#ifdef PS2HOST_VULKAN
    if (g_display) g_display->SetFsrSharpness(FsrSharpnessStops());
#endif
}

// FSR PRESET's ratio of the shown picture to the picture drawn (FSR's own: QUALITY 1.5, BALANCED 1.7,
// PERFORMANCE 2.0); 0 for none (CUSTOM: the chosen RENDER RESOLUTION; or UPSCALING bilinear).
float FsrPresetRatio() {
    if (Setting("upscaling", "linear").rfind("fsr", 0) != 0) return 0.0f;
    const std::string v = Setting("fsr_preset", "quality");
    return v == "balanced" ? 1.7f : v == "performance" ? 2.0f : 1.5f;
}

void WindowShownSize(std::uint32_t* width, std::uint32_t* height) {
    *width = *height = 0;
#ifdef PS2HOST_VULKAN
    if (g_display) g_display->ShownSize(width, height);
#endif
}

int BloomStrength() {
    if (!SettingOn("bloom", false)) return 0;
    return std::clamp(std::atoi(Setting("bloom_strength", "5").c_str()), 1, 10);
}

void ApplyBloomSetting() {
#ifdef PS2HOST_VULKAN
    if (g_display) g_display->SetBloom(BloomStrength());
#endif
}

u32 SupersamplingFactor() {
    const std::string mode = Setting("antialiasing", "off");
    return mode == "ssaa2" ? 2 : mode == "ssaa4" ? 4 : 1;
}

void SetGameLabel(const std::string& text, float x, float y, float height, u32 colour) {
    if (text.empty()) SetGameTexts({});
    else SetGameTexts({GameText{text, x, y, height, colour, false}});
}

// PS2HOST_GAME_TEXT_DUMP=<folder> (diagnosis, also without a window): each new set of texts written as a picture
// (text_<n>.tga, 32-bit, transparent where nothing is) and a line naming it with the game's blank - laid over the
// --frames pictures to see a port's own menu page in a scripted run.
void DumpGameTexts(const std::vector<std::uint32_t>& pixels, u32 width, u32 height) {
    static const char* folder = std::getenv("PS2HOST_GAME_TEXT_DUMP");
    static int n = 0;
    if (!folder) return;
    const std::string path = std::string(folder) + "/text_" + std::to_string(n++) + ".tga";
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    const unsigned char head[18] = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, (unsigned char)(width & 0xFF),
                                    (unsigned char)(width >> 8), (unsigned char)(height & 0xFF),
                                    (unsigned char)(height >> 8), 32, 0x28};      // top-left origin, 8 alpha bits
    std::fwrite(head, 1, sizeof head, f);
    std::vector<unsigned char> row(std::size_t(width) * 4);
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            const std::uint32_t p = pixels.empty() ? 0 : pixels[std::size_t(y) * width + x];
            row[x * 4 + 0] = (unsigned char)(p & 0xFF);
            row[x * 4 + 1] = (unsigned char)((p >> 8) & 0xFF);
            row[x * 4 + 2] = (unsigned char)((p >> 16) & 0xFF);
            row[x * 4 + 3] = (unsigned char)(p >> 24);
        }
        std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
    std::fprintf(stderr, "[text] %s at %.3f s\n", path.c_str(), double(LastVblankNs()) / 1e9);
}

namespace {
void ShowGameTexts(const std::vector<GameText>& texts);
std::vector<GameText> g_menu_texts, g_subtitle_texts;      // the two layers (SetGameTexts, SetGameSubtitleTexts)
void ShowLayers() {
    std::vector<GameText> all = g_menu_texts;
    all.insert(all.end(), g_subtitle_texts.begin(), g_subtitle_texts.end());
    ShowGameTexts(all);
}
}  // namespace

void SetGameTexts(const std::vector<GameText>& texts) {
    g_menu_texts = texts;
    ShowLayers();
}

void SetGameSubtitleTexts(const std::vector<GameText>& texts) {
    if (texts.empty() && g_subtitle_texts.empty()) return;
    g_subtitle_texts = texts;
    ShowLayers();
}

namespace {
void ShowGameTexts(const std::vector<GameText>& texts) {
    static const bool dump = std::getenv("PS2HOST_GAME_TEXT_DUMP") != nullptr;
#ifdef PS2HOST_VULKAN
    if (!g_display && !dump) return;
#else
    if (!dump) return;
#endif
    // Drawn into one transparent picture over the whole game picture (kLines lines; columns for the picture's shape
    // as shown, so its pixels are square), only when the lines or the shape change.
    const float shown = g_shows_wide && g_shows_wide() ? SettingAspect() : 4.0f / 3.0f;
    static std::vector<GameText> last;
    static float last_shown = 0;
    auto same = [&] {
        if (texts.size() != last.size() || shown != last_shown) return false;
        for (std::size_t i = 0; i < texts.size(); ++i) {
            const GameText &a = texts[i], &b = last[i];
            if (a.text != b.text || a.x != b.x || a.y != b.y || a.height != b.height || a.colour != b.colour ||
                a.right != b.right || a.centre != b.centre || a.stretch != b.stretch || a.width != b.width || a.mask != b.mask)
                return false;
        }
        return true;
    };
    if (same()) return;
    last = texts;
    last_shown = shown;
    // 448 lines (a PS2 frame's): the letters as soft as the game's own scaled up beside them (1152 looked too
    // sharp next to them, the maintainer 2026-11-20).
    constexpr u32 kLines = 448;
    const u32 columns = u32(std::lround(kLines * shown));
    std::vector<std::uint32_t> pixels;
    if (!texts.empty() && !DrawGameTexts(texts, columns, kLines, &pixels)) return;
    if (dump) DumpGameTexts(pixels, columns, kLines);
#ifdef PS2HOST_VULKAN
    if (!g_display) return;
    if (texts.empty()) g_display->SetPictureLabel({}, 0, 0, 0, 0, 0, 0);
    else g_display->SetPictureLabel(std::move(pixels), columns, kLines, 0.0f, 0.0f, 1.0f, 1.0f);
#endif
}
}  // namespace

void SetWidescreen(std::function<bool()> shows_wide) {
    g_shows_wide = std::move(shows_wide);
#ifdef PS2HOST_VULKAN
    if (g_display) g_display->SetAspect(SettingAspect());
#endif
}

bool WidescreenSupported() { return bool(g_shows_wide); }

float WideAspect() { return SettingAspect(); }

int FieldOfViewExtra() {
    if (!g_shows_wide) return 0;
    return std::clamp(std::atoi(Setting("field_of_view", "0").c_str()), 0, 30) / 5 * 5;
}

float PictureAspect() {
    const float aspect = SettingAspect();
    if (aspect > 0.0f) return aspect;
#ifdef PS2HOST_VULKAN
    if (g_display) {
        const float window = g_display->WindowAspect();
        if (window > 0.0f) return window;
    }
#endif
    return 4.0f / 3.0f;
}

void SetWindowVsync(bool on) {
    SetSettingOn("vsync", on);
#ifdef PS2HOST_VULKAN
    if (g_display) g_display->SetVsync(on);
#endif
}

bool WindowVsync() {
#ifdef PS2HOST_VULKAN
    if (g_display) return g_display->Vsync();
#endif
    return SettingOn("vsync");
}

bool WindowFullscreen() {
#ifdef PS2HOST_VULKAN
    if (g_display) return g_display->Fullscreen();
#endif
    return SettingOn("fullscreen");
}

ps2gpu::Display* WindowDisplay() {
#ifdef PS2HOST_VULKAN
    return g_display.get();
#else
    return nullptr;
#endif
}

void PresentFrame() {
#ifdef PS2HOST_VULKAN
    if (g_display) {
        if (!g_display->Pump()) {
            g_closed = true;
            return;
        }
        KeepFullscreenSetting();
        std::vector<u32> pixels;
        u32 w = 0, h = 0;
        const bool got = (g_picture_source && g_picture_source(&pixels, &w, &h)) || gs::TheGs().Picture(&pixels, &w, &h);
        if (got && !g_display->Present(pixels.data(), w, h)) g_closed = true;
        return;
    }
#endif
    if (ps2platform::FallbackWindowOpen()) PresentFallback();
}

}  // namespace ps2host
