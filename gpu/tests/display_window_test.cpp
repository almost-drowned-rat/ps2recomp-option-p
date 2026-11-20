/*
 * The display's window tests (2026-11-18, ZOE US session with the maintainer), with a real window:
 *  1. ASPECT RATIO changes keep the window's height: 4:3 <-> 16:9 six times, then back - each change shrank the
 *     window before (SDL_SetWindowAspectRatio fitted it inside its current size first).
 *  2. The window's own thread: Windows' move loop (WM_SYSCOMMAND SC_MOVE by keyboard, ended with Escape - the
 *     loop a drag runs) holds the thread that takes the window's events; this thread presents
 *     on through it. Before, Pump ran here and the whole program stood until the loop ended (an 11 s stall while
 *     the maintainer resized ZOE's window). With PS2HOST_WINDOW_THREAD=0 (the old way) part 2 must FAIL - the
 *     proof the loop was entered.
 * Exit status 0 when both hold. Windows only (part 2); the window shows for about four seconds.
 */

#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include <SDL3/SDL.h>

#include "ps2gpu/display.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace {
constexpr const char* kTitle = "ps2gpu window test";

bool PresentFrame(ps2gpu::Display& display, int frame) {
    const std::uint32_t w = 640, h = 448;
    static std::vector<std::uint32_t> pixels(std::size_t(w) * h);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x)
            pixels[y * w + x] = 0xFF000000u | ((x + frame * 4) & 0xFF) << 16 | (y & 0xFF) << 8 | 0x40;
    return display.Pump() && display.Present(pixels.data(), w, h);
}

// Presents for `ms`, the window thread taking its events meanwhile.
int PresentFor(ps2gpu::Display& display, int ms, int* frame) {
    int done = 0;
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < until) {
        if (!PresentFrame(display, (*frame)++)) return -1;
        ++done;
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    return done;
}
}  // namespace

int main(int, char**) {
    std::string error;
    auto display = ps2gpu::CreateVulkanDisplay(kTitle, 960, 720, &error);
    if (!display) {
        std::printf("display: %s\n", error.c_str());
        return 1;
    }
    int frame = 0, failed = 0;
    PresentFor(*display, 500, &frame);

    // 1. Aspect changes.
    int x = 0, y = 0, w0 = 0, h0 = 0;
    display->WindowRect(&x, &y, &w0, &h0);
    std::printf("aspect: start %dx%d\n", w0, h0);
    for (int i = 0; i < 6; ++i) {
        display->SetAspect(i % 2 == 0 ? 16.0f / 9.0f : 4.0f / 3.0f);
        PresentFor(*display, 150, &frame);
        int w = 0, h = 0;
        display->WindowRect(&x, &y, &w, &h);
        std::printf("aspect: %s -> %dx%d\n", i % 2 == 0 ? "16:9" : "4:3", w, h);
        if (h < h0 - 2 || h > h0 + 2) {
            std::printf("FAIL: the height changed (%d, was %d)\n", h, h0);
            ++failed;
        }
    }

#ifdef _WIN32
    // 2. Windows' move loop on the window's thread.
    HWND hwnd = FindWindowA(nullptr, kTitle);
    if (!hwnd) {
        std::printf("FAIL: window not found by its title\n");
        ++failed;
    } else {
        // Windows' own move loop, by keyboard (the window menu's "Move"): the loop a drag runs. The window menu
        // posted to a window in the background opened nothing (2026-11-18: the old way passed too) - the window
        // in front first. The loop puts the pointer on the window; Escape ends it where it began.
        SetForegroundWindow(hwnd);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        PostMessageA(hwnd, WM_SYSCOMMAND, SC_MOVE, 0);
        // Escape from a thread of its own: the old way holds this one inside the loop until it comes.
        std::thread escape([hwnd] {
            std::this_thread::sleep_for(std::chrono::milliseconds(1700));
            PostMessageA(hwnd, WM_KEYDOWN, VK_ESCAPE, 0);
            PostMessageA(hwnd, WM_KEYUP, VK_ESCAPE, 0);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const auto began = std::chrono::steady_clock::now();
        const int presented = PresentFor(*display, 1500, &frame);
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
        escape.join();
        std::printf("move loop: %d presents in %.2f s\n", presented, seconds);
        // ~16 ms a present: 1.5 s gives about 90; a held thread would give one or none in 1.5 s, or run long.
        if (presented < 40 || seconds > 2.0) {
            std::printf("FAIL: presenting stood during the move loop\n");
            ++failed;
        }
        PresentFor(*display, 300, &frame);
    }
#endif
    std::printf(failed ? "window tests: %d FAILED\n" : "window tests: all passed\n", failed);
    return failed ? 1 : 0;
}
