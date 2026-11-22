/*
 * The port's display: a window showing the console's picture, and the keyboard,
 * game controller and window events that come with it.
 *
 * Display is what the host talks to; VulkanDisplay (SDL3 + Vulkan) is the one
 * implementation so far. Stage one presents a finished picture - the software
 * GS's - scaled to the window; a GPU GS renderer will draw into the same
 * swapchain later.
 */

#ifndef PS2GPU_DISPLAY_H
#define PS2GPU_DISPLAY_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ps2gpu {

class Display {
public:
    virtual ~Display() = default;

    /* Shows a picture: 32-bit pixels, BGRA in memory (B first), rows top-down,
       scaled to the window. False when the window is gone. */
    virtual bool Present(const std::uint32_t* pixels, std::uint32_t width, std::uint32_t height) = 0;

    /* Shows a picture the GPU already has: the top-left width x height of a
       VkImage (on the display's own device, as its 64-bit handle), in
       `layout` (a VkImageLayout) and left in it, copied to the window on the
       GPU - no read-back, no upload. Whatever drew into it must have been
       submitted to the queue before. False when the window is gone, or a
       display cannot. */
    virtual bool PresentImage(std::uint64_t image, int layout, std::uint32_t width, std::uint32_t height) {
        (void)image, (void)layout, (void)width, (void)height;
        return false;
    }

    /* PresentImage, the picture shown at `due` (a steady_clock time, in
       nanoseconds since its epoch) rather than now: the image is copied at
       once (it may be drawn over next), and a thread of the display's shows
       it then - so pictures reach the screen evenly however unevenly they
       were finished. The newest picture wins: one not yet shown when the
       next comes is never shown. */
    virtual bool PresentImageAt(std::uint64_t image, int layout, std::uint32_t width, std::uint32_t height,
                                std::uint64_t due_ns) {
        (void)due_ns;
        return PresentImage(image, layout, width, height);
    }

    /* Handles the window's events; false once it has been closed. Alt+Enter
       toggles fullscreen. */
    virtual bool Pump() = 0;

    /* How evenly pictures went to the screen since the last call (--frame-times):
       presents, and the time between them - mean, standard deviation and
       longest, in milliseconds. */
    struct Pacing { std::uint64_t presents = 0; double mean_ms = 0, jitter_ms = 0, longest_ms = 0; };
    virtual Pacing TakePacing() { return {}; }
    /* Pictures shown a second, over the last half second or so: the FPS
       counter's (F1), apart from TakePacing so it takes nothing from it. */
    virtual double PresentsPerSecond() const { return 0.0; }
    /* Pictures shown since the start, and the longest time between two since the last call (ms; taken: reset) -
       the per-second log's (GAME SPEED's, 2026-11-22: 60 FPS without a drop). */
    virtual std::uint64_t PresentCount() const { return 0; }
    virtual double TakeLongestPresentGap() { return 0.0; }
    /* The same for the overlay's FRAME TIME (its own, so the log and the overlay take nothing from each other). */
    virtual double TakeLongestFrame() { return 0.0; }
    /* Pictures shown in the last whole second, counted (the overlay's FPS; -1: not yet a second). */
    virtual int PresentsLastSecond() const { return -1; }

    /* Vertical sync: each picture waits for the screen's refresh (no
       tearing, FIFO), or goes at once (IMMEDIATE, or MAILBOX where that is
       all there is). */
    virtual void SetVsync(bool on) { (void)on; }
    virtual bool Vsync() const { return false; }

    /* The window's height in the screen's pixels (0 when not known). */
    virtual std::uint32_t PixelHeight() const { return 0; }

    /* Fullscreen (the desktop's resolution) or a window. */
    virtual void SetFullscreen(bool on) { (void)on; }
    virtual bool Fullscreen() const { return false; }
    /* Which fullscreen (DISPLAY MODE): off - the display switched to a refresh the game's 60 frames divide evenly
       where the desktop's does not (120 Hz on a 144 Hz display); on - borderless, the desktop's own mode left as
       it is (no mode switch, alt-tab at once). Applied at once when fullscreen, else at the next. */
    virtual void SetBorderless(bool on) { (void)on; }
    /* The window's place and size on the desktop (false in fullscreen, or none), and set again (a place whose
       middle is on no display is centred) - the ports' rule: the window comes back where the player left it. */
    virtual bool WindowRect(int* x, int* y, int* width, int* height) const {
        (void)x, (void)y, (void)width, (void)height;
        return false;
    }
    virtual void SetWindowRect(int x, int y, int width, int height) { (void)x, (void)y, (void)width, (void)height; }
    /* The picture's shape on the screen, width over height: 4:3 as a
       television showed it (the default), 16:9, or 0 - the window's own
       shape, filled. Letterboxed or pillarboxed in the window otherwise. */
    virtual void SetAspect(float aspect) { (void)aspect; }
    /* The shape the picture is shown at inside the window, where it differs from SetAspect's (the window's): a
       game in 16:9 showing a 4:3 screen (a menu, a movie) as 4:3 between bars. 0: SetAspect's. */
    virtual void SetPictureAspect(float aspect) { (void)aspect; }
    /* The screen's own shape (its desktop mode), 0 when not known: ASPECT RATIO 21:9 takes an ultrawide
       screen's exact one. */
    virtual float ScreenAspect() const { return 0.0f; }
    /* Anti-aliasing: the picture shown as drawn (the default); its edges
       smoothed (FXAA, or SMAA's three passes) before it is scaled to the
       window; or, drawn larger than shown (supersampling), averaged down to
       the shown size rather than scaled. */
    enum class AntiAliasing { kOff, kFxaa, kDownsample, kSmaa };
    virtual void SetAntiAliasing(AntiAliasing mode) { (void)mode; }
    /* Upscaling: a picture drawn smaller than it is shown, brought up to the
       shown size - filtered bilinearly (the default), or by FSR 1 (AMD's
       FidelityFX Super Resolution: its edge-adaptive upscale and sharpening,
       after anti-aliasing). A picture as large as shown or larger is shown
       as before either way. */
    enum class Upscaling { kLinear, kFsr };
    virtual void SetUpscaling(Upscaling mode) { (void)mode; }
    /* The picture's left `x` columns and top `y` rows left out of what is shown (PresentImage, PresentImageAt):
       a picture drawn larger whose first PS2 column / row is only half drawn (gpu_bridge's PresentGpuPicture). */
    virtual void SetPictureCrop(std::uint32_t x, std::uint32_t y) { (void)x, (void)y; }
    /* The last picture given (PresentImage / PresentImageAt) shown again now, with the overlay as it is: a game
       held still behind a menu (the host's SELECT menu). False when there is none. */
    virtual bool RepeatLast() { return false; }
    /* FSR's sharpening (RCAS) in its stops: 0 the sharpest, each stop half as sharp (FSR SHARPNESS). */
    virtual void SetFsrSharpness(float stops) { (void)stops; }
    /* The picture's size in the window as last shown (its letterbox's inside), 0 x 0 before the first: FSR's
       presets draw at a fraction of it. */
    virtual void ShownSize(std::uint32_t* width, std::uint32_t* height) const { *width = *height = 0; }
    /* BLOOM (porting guide 4b, a PC enhancement, off by default): the finished picture's bright parts spread
       as a soft glow, before it is scaled to the window - 0 off, else BLOOM STRENGTH 1-10. */
    virtual void SetBloom(int strength) { (void)strength; }
    /* A label blended into the picture (a port's line in the game's own menu): `rgba` 0xAARRGGBB pixels (straight
       alpha, rows top down), shown in the rectangle x0, y0 - x1, y1 of the picture (fractions 0..1) before it is
       scaled to the window. Empty pixels: none. */
    virtual void SetPictureLabel(std::vector<std::uint32_t> rgba, std::uint32_t width, std::uint32_t height, float x0,
                                 float y0, float x1, float y1) {
        (void)rgba; (void)width; (void)height; (void)x0; (void)y0; (void)x1; (void)y1;
    }
    /* The window's shape in the screen's pixels, width over height (0 when
       not known). */
    virtual float WindowAspect() const { return 0.0f; }
    /* A panel over the picture, at the window's top left: BGRA pixels
       (opaque), rows top-down; none when empty. */
    virtual void SetOverlay(std::vector<std::uint32_t> pixels, std::uint32_t width, std::uint32_t height) {
        (void)pixels, (void)width, (void)height;
    }
    /* The window's title (taken up by the window's own thread, in Pump). */
    virtual void SetTitle(const std::string& title) { (void)title; }
    /* F12, or L3 with R3 on a controller, since the last call: a capture asked for. */
    virtual bool TakeCapture() { return false; }
    /* Tab since the last call: the texture pack turned on or off. */
    virtual bool TakeTexturePackToggle() { return false; }
    /* F1 since the last call: the FPS counter turned on or off. */
    virtual bool TakeFpsToggle() { return false; }

    /* Whether the window has the keyboard, and whether a key is down - by SDL
       scancode (SDL_SCANCODE_*), which names keys by position. */
    virtual bool Focused() const = 0;
    virtual bool KeyDown(int scancode) const = 0;

    /* The first game controller, as a DualShock 2 reads: false when none is
       plugged in. */
    struct PadState {
        std::uint16_t buttons = 0;      // bit n: DualShock 2 input n held (libpad's order:
                                        // Select L3 R3 Start Up Right Down Left L2 R2 L1 R1
                                        // Triangle Circle Cross Square)
        std::uint8_t sticks[4] = {0x80, 0x80, 0x80, 0x80};   // right X, Y, left X, Y; 0x80 centred
        std::uint8_t l2 = 0, r2 = 0;    // the triggers' travel, 0-255
        std::uint8_t family = 0;        // whose it is (its buttons' names): ControllerFamily
    };
    enum ControllerFamily : std::uint8_t { kOtherController, kPlayStationController, kXboxController };
    virtual bool Pad(PadState* state) const { (void)state; return false; }
    /* Every controller plugged in, in the order they were (SDL's id, the
       name it gives, its family) - and one's own state by its id: false when
       it is gone. */
    struct ControllerInfo {
        std::uint32_t id = 0;
        std::string name;
        std::uint8_t family = 0;
    };
    virtual std::vector<ControllerInfo> Controllers() const { return {}; }
    virtual bool PadOf(std::uint32_t id, PadState* state) const { (void)id, (void)state; return false; }
    /* The controller's motors, 0-65535 each (the low-frequency, large one;
       the high-frequency, small one), until set again - 0, 0 stops them.
       Taken up by the window's own thread (Pump). SetRumble sets every
       controller's, SetRumbleOf one's. */
    virtual void SetRumble(std::uint16_t large, std::uint16_t small) { (void)large, (void)small; }
    virtual void SetRumbleOf(std::uint32_t id, std::uint16_t large, std::uint16_t small) {
        (void)id, (void)large, (void)small;
    }
    /* The controllers SetTriggerEffects is for: none named, every one. */
    virtual void SetTriggerTargets(std::vector<std::uint32_t> ids) { (void)ids; }
    /* A controller's adaptive triggers (a DualSense's; others have none):
       off, a resistance from `start` (0-255 of the travel) of `force`
       (0-255), or a vibration from `start` of `force` at `frequency` (Hz),
       until set again. Taken up by the window's own thread (Pump), and
       again when a controller is plugged in. */
    struct TriggerEffect {
        enum Kind : std::uint8_t { kOff, kResistance, kVibration } kind = kOff;
        std::uint8_t start = 0, force = 0, frequency = 0;
        bool operator==(const TriggerEffect& o) const {
            return kind == o.kind && start == o.start && force == o.force && frequency == o.frequency;
        }
    };
    virtual void SetTriggerEffects(TriggerEffect left, TriggerEffect right) { (void)left, (void)right; }
    /* The controller left as it was found, at once (the game is ending):
       its motors stopped, its triggers' effects off. */
    virtual void ReleaseController() {}

    /* The mouse over the window, in the picture's terms: x and y from 0 to 1
       across the game's picture as it is shown (inside its letterbox; outside
       it they go past 0 or 1). `held` the buttons down (bit 0 left, 1 middle,
       2 right), `pressed` those that went down since the last call, `wheel`
       the notches turned since (up positive), `moved` whether it moved. */
    struct MouseState {
        bool over = false;              // the pointer is in the window
        float x = 0.0f, y = 0.0f;
        std::uint32_t held = 0, pressed = 0;
        float wheel = 0.0f;
        bool moved = false;
    };
    virtual bool TakeMouse(MouseState* state) { (void)state; return false; }
    /* The game's own mouse pointer: RGBA pixels (rows top-down), the point
       that clicks at (hot_x, hot_y); scaled with the window. Shown while
       ShowGameCursor(true) - otherwise the system's (hidden in fullscreen). */
    virtual void SetCursorArt(std::vector<std::uint32_t> rgba, std::uint32_t width, std::uint32_t height,
                              std::uint32_t hot_x, std::uint32_t hot_y) {
        (void)rgba, (void)width, (void)height, (void)hot_x, (void)hot_y;
    }
    virtual void ShowGameCursor(bool on) { (void)on; }

    /* Told each picture PresentImageAt was given: shown (when it was due, when
       it was), or dropped unshown (shown_ns 0) - on the presenting thread. */
    using PresentObserver = std::function<void(std::uint64_t due_ns, std::uint64_t shown_ns, bool dropped)>;
    virtual void SetPresentObserver(PresentObserver observer) { (void)observer; }

    /* The GPU and driver, for the report. */
    virtual std::string Describe() const = 0;

    /* The window's device as a Gpu, for a GsRenderer to draw with: one
       Vulkan instance and device for both (owned by the display). */
    virtual class Gpu* SharedGpu() = 0;
};

/* A window with a Vulkan swapchain; null, with `error` saying why, when there is
   no Vulkan device or window to be had. */
std::unique_ptr<Display> CreateVulkanDisplay(const char* title, int width, int height, std::string* error);

}  // namespace ps2gpu

#endif  /* PS2GPU_DISPLAY_H */
