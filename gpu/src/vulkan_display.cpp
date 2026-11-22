/*
 * VulkanDisplay: an SDL3 window and a Vulkan swapchain presenting a picture.
 *
 * Each frame the picture goes into a host-visible staging buffer, is copied to
 * an image, and blitted - scaled, 4:3, letterboxed - onto the swapchain image,
 * which is cleared to black first. One frame is in flight: the fence is waited
 * before the staging buffer is written again. The swapchain presents in
 * MAILBOX mode where the driver has it (never blocking the emulation thread),
 * else IMMEDIATE, else FIFO, and is recreated when the window changes size.
 */

#include "ps2gpu/display.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <volk.h>

#include "gpu_internal.h"
#include "AreaTex.h"             // gpu/third_party/smaa
#include "SearchTex.h"
#include "bloom_comp.h"
#include "label_comp.h"
#include "downsample_comp.h"
#include "fxaa_comp.h"
#include "fsr_easu_comp.h"
#include "fsr_rcas_comp.h"
#include "smaa_blend_comp.h"
#include "smaa_edges_comp.h"
#include "smaa_weights_comp.h"
#include "ps2platform/platform.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ps2gpu {

namespace {

class VulkanDisplay final : public Display {
public:
    ~VulkanDisplay() override;
    bool Init(const char* title, int width, int height);
    bool Present(const std::uint32_t* pixels, std::uint32_t width, std::uint32_t height) override;
    bool PresentImage(std::uint64_t image, int layout, std::uint32_t width, std::uint32_t height) override;
    bool PresentImageAt(std::uint64_t image, int layout, std::uint32_t width, std::uint32_t height,
                        std::uint64_t due_ns) override;
    bool Pump() override;
    void SetFullscreen(bool on) override;
    bool Fullscreen() const override;
    void SetBorderless(bool on) override;
    bool WindowRect(int* x, int* y, int* width, int* height) const override {
        if (!window_ || Fullscreen()) return false;
        return SDL_GetWindowPosition(window_, x, y) && SDL_GetWindowSize(window_, width, height);
    }
    void SetWindowRect(int x, int y, int width, int height) override {
        if (!window_ || width <= 0 || height <= 0) return;
        if (!OnWindowThread()) {
            std::lock_guard<std::mutex> hold(requests_lock_);
            rect_request_ = {true, x, y, width, height};
            return;
        }
        SDL_SetWindowSize(window_, width, height);
        const SDL_Point middle{x + width / 2, y + height / 2};
        if (SDL_GetDisplayForPoint(&middle) != 0) SDL_SetWindowPosition(window_, x, y);
        else SDL_SetWindowPosition(window_, int(SDL_WINDOWPOS_CENTERED), int(SDL_WINDOWPOS_CENTERED));
    }
    /* The window's own thread (2026-11-18: the maintainer resized ZOE's window and the game stood 11 s - Windows
       holds the thread that takes a window's events in its own loop while it is dragged or resized, and that was
       the EE's, at every vertical blank). The window is made, its events taken and its SDL calls made on a thread
       of its own (WindowLoop); Pump only says whether it is still open. PS2HOST_WINDOW_THREAD=0: the old way. */
    bool StartOnOwnThread(const char* title, int width, int height);
    void WindowLoop(std::string title, int width, int height);
    bool PumpEvents();
    bool OnWindowThread() const { return !threaded_ || std::this_thread::get_id() == window_thread_id_; }
    bool threaded_ = false;
    std::thread window_thread_;
    std::thread::id window_thread_id_{};
    std::atomic<bool> window_stop_{false};
    std::atomic<bool> window_gone_{false};      // the window thread destroyed the window (its last act)
    std::mutex requests_lock_;                  // requests to the window thread from others
    struct RectRequest {
        bool wanted = false;
        int x = 0, y = 0, width = 0, height = 0;
    } rect_request_;
    int fullscreen_request_ = -1;               // -1 none, 0 windowed, 1 fullscreen
    void TakeRequests();
    // ASPECT RATIO's window size (Pump): the height the player gave it, kept across changes of the aspect
    // (2026-11-18: each change shrank the window - SDL_SetWindowAspectRatio fits the window inside its current
    // size first, so every change took off height or width).
    int player_height_ = 0;
    int our_width_ = 0, our_height_ = 0;        // the size we last set (its resize event is not the player's)
    void SetVsync(bool on) override;
    bool TakeCapture() override {
        const bool asked = capture_;
        capture_ = false;
        return asked;
    }
    bool capture_ = false;
    bool TakeTexturePackToggle() override {
        const bool asked = texture_pack_;
        texture_pack_ = false;
        return asked;
    }
    bool texture_pack_ = false;
    bool TakeFpsToggle() override {
        const bool asked = fps_toggle_;
        fps_toggle_ = false;
        return asked;
    }
    bool fps_toggle_ = false;
    double PresentsPerSecond() const override { return fps_.load(); }
    std::atomic<double> fps_{0.0};
    std::chrono::steady_clock::time_point fps_since_{};
    std::uint64_t fps_presents_ = 0;
    void SetAspect(float aspect) override { aspect_.store(aspect); }
    void SetAntiAliasing(AntiAliasing mode) override { anti_aliasing_.store(int(mode)); }
    void SetUpscaling(Upscaling mode) override { upscaling_.store(int(mode)); }
    void SetFsrSharpness(float stops) override { fsr_sharpness_.store(stops); }
    void SetPictureCrop(std::uint32_t x, std::uint32_t y) override {
        crop_x_.store(x);
        crop_y_.store(y);
    }
    bool RepeatLast() override {
        std::uint64_t image = 0;
        int layout = 0;
        std::uint32_t width = 0, height = 0;
        {
            std::lock_guard<std::mutex> hold(last_lock_);
            image = last_image_, layout = last_layout_, width = last_width_, height = last_height_;
        }
        return image && PresentImage(image, layout, width, height);
    }
    void Remember(std::uint64_t image, int layout, std::uint32_t width, std::uint32_t height) {
        std::lock_guard<std::mutex> hold(last_lock_);
        last_image_ = image, last_layout_ = layout, last_width_ = width, last_height_ = height;
    }
    void ShownSize(std::uint32_t* width, std::uint32_t* height) const override {
        *width = shown_seen_.load() ? std::uint32_t(shown_w_.load()) : 0u;
        *height = shown_seen_.load() ? std::uint32_t(shown_h_.load()) : 0u;
    }
    void SetBloom(int strength) override { bloom_.store(std::clamp(strength, 0, 10)); }
    void SetPictureLabel(std::vector<std::uint32_t> rgba, std::uint32_t width, std::uint32_t height, float x0,
                         float y0, float x1, float y1) override {
        std::lock_guard<std::mutex> hold(label_lock_);
        if (rgba.size() != std::size_t(width) * height || width > kLabelMaxW || height > kLabelMaxH) rgba.clear();
        if (rgba == label_pixels_ && width == label_w_ && height == label_h_ && x0 == label_rect_[0] &&
            y0 == label_rect_[1] && x1 == label_rect_[2] && y1 == label_rect_[3])
            return;
        label_pixels_ = std::move(rgba);
        label_w_ = width;
        label_h_ = height;
        label_rect_[0] = x0;
        label_rect_[1] = y0;
        label_rect_[2] = x1;
        label_rect_[3] = y1;
        ++label_generation_;
    }
    void SetOverlay(std::vector<std::uint32_t> pixels, std::uint32_t width, std::uint32_t height) override {
        std::lock_guard<std::mutex> hold(overlay_lock_);
        overlay_ = std::move(pixels);
        overlay_width_ = width;
        overlay_height_ = height;
        ++overlay_generation_;
    }
    /* The overlay into the swapchain image (TRANSFER_DST) being made. */
    void DrawOverlay(VkImage target);
    std::mutex overlay_lock_;
    std::vector<std::uint32_t> overlay_;
    std::uint32_t overlay_width_ = 0, overlay_height_ = 0;
    std::uint64_t overlay_generation_ = 1;      // a SetOverlay's
    static constexpr VkDeviceSize kOverlayBytes = VkDeviceSize(1280) * 720 * 4;
    VkBuffer overlay_buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory overlay_memory_ = VK_NULL_HANDLE;
    void* overlay_mapped_ = nullptr;
    void SetTitle(const std::string& title) override {
        std::lock_guard<std::mutex> hold(title_lock_);
        title_ = title;
        title_changed_ = true;
    }
    std::mutex title_lock_;
    std::string title_;
    bool title_changed_ = false;
    float WindowAspect() const override {
        int w = 0, h = 0;
        if (!window_ || !SDL_GetWindowSizeInPixels(window_, &w, &h) || w <= 0 || h <= 0) return 0.0f;
        return float(w) / float(h);
    }
    void SetPictureAspect(float aspect) override { picture_aspect_.store(aspect > 0.0f ? aspect : 0.0f); }
    float ScreenAspect() const override {
        const SDL_DisplayID id = window_ ? SDL_GetDisplayForWindow(window_) : SDL_GetPrimaryDisplay();
        const SDL_DisplayMode* mode = id ? SDL_GetDesktopDisplayMode(id) : nullptr;
        if (!mode || mode->w <= 0 || mode->h <= 0) return 0.0f;
        return float(mode->w) / float(mode->h);
    }
    std::atomic<float> aspect_{4.0f / 3.0f};
    std::atomic<float> picture_aspect_{0.0f};   // SetPictureAspect's; 0: aspect_'s
    float window_aspect_ = -1.0f;      // the aspect the window is held to (Pump, the window's thread); -1: not yet
    std::uint32_t PixelHeight() const override {
        int w = 0, h = 0;
        if (!window_ || !SDL_GetWindowSizeInPixels(window_, &w, &h)) return 0;
        return std::uint32_t(h > 0 ? h : 0);
    }
    bool Vsync() const override { return vsync_; }
    Pacing TakePacing() override;
    bool Focused() const override;
    bool KeyDown(int scancode) const override;
    bool Pad(PadState* state) const override;
    std::string Describe() const override { return describe_; }
    Gpu* SharedGpu() override;
    const std::string& error() const { return error_; }

private:
    bool Check(VkResult result, const char* what);
    bool CreateSwapchain();
    void DestroySwapchain();
    bool EnsurePicture(std::uint32_t width, std::uint32_t height);
    void DestroyPicture();
    std::uint32_t FindMemory(std::uint32_t bits, VkMemoryPropertyFlags wanted) const;
    // PresentImageAt's: pictures copied and waiting, and the thread that
    // shows them when they are due.
    bool EnsurePaced(std::uint32_t width, std::uint32_t height);
    void DestroyPaced();
    void PresentLoop();

    SDL_Window* window_ = nullptr;
    // Every controller plugged in, in the order they were: opened and closed
    // by the window's thread (Pump), read from the EE's - under pads_lock_.
    struct Gamepad {
        SDL_Gamepad* pad = nullptr;
        std::uint32_t id = 0;
        std::uint32_t rumble = 0;           // as wanted: large << 16 | small
        std::uint32_t rumble_sent = 0;
        std::uint64_t rumble_renewed = 0;   // when SDL was last told (ms): its rumble lasts a while
        bool triggers_unsent = true;        // (plugged in since they were sent)
        TriggerEffect triggers_sent[2];
    };
    mutable std::mutex pads_lock_;
    std::vector<Gamepad> pads_;
    std::vector<std::uint32_t> trigger_targets_;        // SetTriggerTargets: none, every controller
    const Gamepad* FindPad(std::uint32_t id) const {
        for (const Gamepad& g : pads_)
            if (g.id == id) return &g;
        return nullptr;
    }
public:
    void SetRumble(std::uint16_t large, std::uint16_t small) override {
        std::lock_guard<std::mutex> hold(pads_lock_);
        for (Gamepad& g : pads_) g.rumble = std::uint32_t(large) << 16 | small;
    }
    void SetRumbleOf(std::uint32_t id, std::uint16_t large, std::uint16_t small) override {
        std::lock_guard<std::mutex> hold(pads_lock_);
        for (Gamepad& g : pads_)
            if (g.id == id) g.rumble = std::uint32_t(large) << 16 | small;
    }
    void SetTriggerEffects(TriggerEffect left, TriggerEffect right) override {
        std::lock_guard<std::mutex> hold(triggers_lock_);
        triggers_[0] = left;
        triggers_[1] = right;
    }
    void SetTriggerTargets(std::vector<std::uint32_t> ids) override {
        std::lock_guard<std::mutex> hold(triggers_lock_);
        trigger_targets_ = std::move(ids);
    }
    std::vector<ControllerInfo> Controllers() const override;
    bool PadOf(std::uint32_t id, PadState* state) const override;
    void ReleaseController() override;
    std::mutex triggers_lock_;
    TriggerEffect triggers_[2];                         // left, right: wanted
    void SendTriggers(SDL_Gamepad* pad, const TriggerEffect& left, const TriggerEffect& right);
    bool TakeMouse(MouseState* state) override {
        std::lock_guard<std::mutex> hold(mouse_lock_);
        *state = mouse_;
        mouse_.pressed = 0;
        mouse_.wheel = 0.0f;
        mouse_.moved = false;
        return true;
    }
    void SetCursorArt(std::vector<std::uint32_t> rgba, std::uint32_t width, std::uint32_t height,
                      std::uint32_t hot_x, std::uint32_t hot_y) override {
        std::lock_guard<std::mutex> hold(mouse_lock_);
        cursor_art_ = std::move(rgba);
        cursor_width_ = width;
        cursor_height_ = height;
        cursor_hot_x_ = hot_x;
        cursor_hot_y_ = hot_y;
        cursor_size_ = 0;                   // made again at the window's size
    }
    void ShowGameCursor(bool on) override { cursor_wanted_.store(on); }
    void SetPresentObserver(PresentObserver observer) override {
        std::lock_guard<std::mutex> hold(paced_lock_);
        observer_ = std::move(observer);
    }
private:
    PresentObserver observer_;              // SetPresentObserver (under paced_lock_)
    // The mouse (TakeMouse), from the window's events; the picture's place in
    // the window, in pixels, as the last present put it.
    std::mutex mouse_lock_;
    MouseState mouse_;
    float mouse_px_ = 0.0f, mouse_py_ = 0.0f;   // the pointer, in the window's pixels
    std::atomic<std::int32_t> shown_x0_{0}, shown_y0_{0}, shown_w_{1}, shown_h_{1};
    void MouseAt(float x, float y);
    // The game's pointer: its art, and the cursor made from it at a size.
    std::vector<std::uint32_t> cursor_art_;
    std::uint32_t cursor_width_ = 0, cursor_height_ = 0, cursor_hot_x_ = 0, cursor_hot_y_ = 0;
    std::uint32_t cursor_size_ = 0;
    SDL_Cursor* cursor_ = nullptr;
    std::atomic<bool> cursor_wanted_{false};
    int cursor_showing_ = -1;               // what the window shows: -1 not yet set, 0 the system's, 1 the game's
    bool fullscreen_cursor_stale_ = false;
    bool borderless_ = false;               // SetBorderless: fullscreen on the desktop's own mode
    void UpdateCursor();
    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    /* Android: the app in the background (Home, Back, another app) - its window's surface taken away: the swapchain
       and the Vulkan surface are let go and nothing is presented until it comes back (Pump; under present_lock_) */
    bool background_ = false;
    bool extent_from_window_ = false;          // the swapchain sized by SDL's window (a turned panel: CreateSwapchain)
    std::atomic<bool> foreground_{false};     // back from the background: a new surface at the next present
    static bool SDLCALL LifeCycle(void* self, SDL_Event* e);
    VkPhysicalDevice gpu_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    std::uint32_t family_ = 0;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D extent_{};
    std::vector<VkImage> images_;
    std::vector<VkSemaphore> rendered_;     // one per swapchain image
    VkCommandPool pool_ = VK_NULL_HANDLE;
    // Presents in flight: each its command buffer, acquire semaphore and
    // fence, so a present waits for the one three before it, not the last
    // (with one, every present waited for all the GPU had been given - the
    // GS thread, which presents, in step with the GPU once a vertical blank).
    static constexpr int kInFlight = 3;
    VkCommandBuffer cmds_[kInFlight] = {};
    VkSemaphore acquireds_[kInFlight] = {};
    VkFence fences_[kInFlight] = {};
    /* Signalled as the acquired image is free (BeginPresent waits for it on
       the CPU): the present's submission never waits on the queue for the
       display - the queue the renderer shares, its work held behind each
       present's wait for a refresh (at 144 Hz the GS thread waited on its
       fences most of a loading screen, kept to 144 frames a second). */
    VkFence acquire_fences_[kInFlight] = {};
    /* The overlay's copy each slot's present reads (DrawOverlay): which of
       SetOverlay's panels it holds. One copy for all would be overwritten
       while an earlier present still read it - a panel changing twice a
       second (the FPS counter) would tear. */
    std::uint64_t overlay_slot_generation_[kInFlight] = {};
    /* PS2HOST_GPU_PROFILE: two timestamps per present slot, then two per paced copy
       (g_display_present_ns, g_display_copy_ns). */
    VkQueryPool stamps_ = VK_NULL_HANDLE;
    double stamp_ns_ = 0;
    bool stamp_tried_ = false;
    bool present_stamped_[kInFlight] = {};
    bool copy_stamped_[5] = {};
    void EnsureStamps();
    void TakeStamp(std::uint32_t first, std::atomic<std::uint64_t>& into);
    int slot_ = 0;
    // The current present's (slot_'s).
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    VkSemaphore acquired_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    VkPresentModeKHR mode_ = VK_PRESENT_MODE_FIFO_KHR;
    std::atomic<bool> vsync_{false};        // FIFO when set (SetVsync)

    VkBuffer staging_ = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory_ = VK_NULL_HANDLE;
    void* mapped_ = nullptr;
    VkImage picture_ = VK_NULL_HANDLE;
    VkDeviceMemory picture_memory_ = VK_NULL_HANDLE;
    std::uint32_t picture_width_ = 0, picture_height_ = 0;

    // Set by Pump (the main thread), read where pictures are presented (the
    // GS thread, when it presents).
    std::atomic<bool> closed_{false}, stale_{false};
    /* Acquires the next swapchain image and begins the command buffer:
       false (with *ok) when there is nothing to present into. */
    bool BeginPresent(std::uint32_t* index, bool* ok);
    /* Clears the swapchain image, blits `source` (TRANSFER_SRC) to it 4:3,
       submits and presents. */
    bool FinishPresent(std::uint32_t index, VkImage source, std::uint32_t width, std::uint32_t height);
    /* Ends the command buffer, submits it and presents the image. */
    bool Submit(std::uint32_t index);
    bool dual_source_ = false;
    // ANTI-ALIASING (SetAntiAliasing): the picture copied into post_in_, and
    // smoothed by fxaa.comp into post_out_ (its size), or brought down to the
    // shown size by downsample.comp (supersampling's) - which goes to the
    // window instead.
    std::atomic<int> anti_aliasing_{int(AntiAliasing::kOff)};
    VkImage post_in_ = VK_NULL_HANDLE, post_out_ = VK_NULL_HANDLE;
    VkDeviceMemory post_in_memory_ = VK_NULL_HANDLE, post_out_memory_ = VK_NULL_HANDLE;
    VkImageView post_in_view_ = VK_NULL_HANDLE, post_out_view_ = VK_NULL_HANDLE;
    VkSampler post_sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout post_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool post_descriptors_ = VK_NULL_HANDLE;
    VkDescriptorSet post_set_ = VK_NULL_HANDLE;
    VkPipelineLayout post_layout_ = VK_NULL_HANDLE;
    VkPipeline fxaa_pipeline_ = VK_NULL_HANDLE, downsample_pipeline_ = VK_NULL_HANDLE;
    std::uint32_t post_in_width_ = 0, post_in_height_ = 0, post_out_width_ = 0, post_out_height_ = 0;
    bool post_failed_ = false;              // the passes could not be made: pictures shown as drawn
    // UPSCALING: FSR 1 (SetUpscaling) - the picture copied into fsr_image_[0] (its size), EASU into [1] and
    // RCAS into [2] (both the shown size: fsr.comp), [2] blitted 1:1. A set each pass: a sampled image, a
    // storage image (fxaa.comp's layout).
    std::atomic<int> upscaling_{int(Upscaling::kLinear)};
    std::atomic<float> fsr_sharpness_{0.25f};      // RCAS's stops (SetFsrSharpness)
    std::atomic<bool> shown_seen_{false};          // a picture shown (ShownSize)
    std::atomic<std::uint32_t> crop_x_{0}, crop_y_{0};     // SetPictureCrop's
    std::mutex last_lock_;                                  // RepeatLast's picture: the last one given
    std::uint64_t last_image_ = 0;
    int last_layout_ = 0;
    std::uint32_t last_width_ = 0, last_height_ = 0;
    static constexpr int kFsrImages = 3;
    VkImage fsr_image_[kFsrImages] = {};
    VkDeviceMemory fsr_memory_[kFsrImages] = {};
    VkImageView fsr_view_[kFsrImages] = {};
    VkSampler fsr_sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout fsr_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool fsr_descriptors_ = VK_NULL_HANDLE;
    VkDescriptorSet fsr_sets_[2] = {};
    VkPipelineLayout fsr_layout_ = VK_NULL_HANDLE;
    VkPipeline fsr_pipelines_[2] = {};      // EASU, RCAS
    std::uint32_t fsr_in_width_ = 0, fsr_in_height_ = 0, fsr_out_width_ = 0, fsr_out_height_ = 0;
    bool fsr_failed_ = false;               // could not be made: the bilinear blit instead
    bool EnsureFsr(std::uint32_t in_width, std::uint32_t in_height, std::uint32_t out_width, std::uint32_t out_height);
    // BLOOM (SetBloom; bloom.comp): the picture copied into [0] (its size), its bright parts averaged down into
    // [1] (a quarter each way), blurred across into [2] and down into [1], then added to [0] into [3] (its
    // size, blitted on). A set each pass: two sampled images and a storage image.
    std::atomic<int> bloom_{0};             // 0 off, else BLOOM STRENGTH 1-10
    static constexpr int kBloomImages = 4;
    VkImage bloom_image_[kBloomImages] = {};
    VkDeviceMemory bloom_memory_[kBloomImages] = {};
    VkImageView bloom_view_[kBloomImages] = {};
    VkSampler bloom_sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout bloom_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool bloom_descriptors_ = VK_NULL_HANDLE;
    VkDescriptorSet bloom_sets_[4] = {};
    VkPipelineLayout bloom_layout_ = VK_NULL_HANDLE;
    VkPipeline bloom_pipeline_ = VK_NULL_HANDLE;
    std::uint32_t bloom_width_ = 0, bloom_height_ = 0;
    bool bloom_failed_ = false;             // could not be made: the picture shown without it
    bool EnsureBloom(std::uint32_t width, std::uint32_t height);
    // SetPictureLabel's (label.comp): the picture copied into [0], the label's own image [2] (uploaded from a
    // staging buffer when it changes), [0] with the label blended into [1] (blitted on).
    // (Up to a whole picture's worth: a port's menu page is one label over the game's picture, SetGameTexts -
    // 597 x 448 at 4:3, 1062 at 21:9; room for sharper ones.)
    static constexpr std::uint32_t kLabelMaxW = 2048, kLabelMaxH = 1152;
    std::mutex label_lock_;
    std::vector<std::uint32_t> label_pixels_;
    std::uint32_t label_w_ = 0, label_h_ = 0;
    float label_rect_[4] = {};
    std::uint64_t label_generation_ = 0, label_uploaded_ = 0;
    VkImage label_image_[3] = {};
    VkDeviceMemory label_memory_[3] = {};
    VkImageView label_view_[3] = {};
    std::uint32_t label_size_w_ = 0, label_size_h_ = 0, label_tex_w_ = 0, label_tex_h_ = 0;
    VkBuffer label_staging_ = VK_NULL_HANDLE;
    VkDeviceMemory label_staging_memory_ = VK_NULL_HANDLE;
    void* label_staging_mapped_ = nullptr;
    VkSampler label_sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout label_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool label_descriptors_ = VK_NULL_HANDLE;
    VkDescriptorSet label_set_ = VK_NULL_HANDLE;
    VkPipelineLayout label_layout_ = VK_NULL_HANDLE;
    VkPipeline label_pipeline_ = VK_NULL_HANDLE;
    bool label_failed_ = false;
    bool MakeLabelImage(int which, std::uint32_t w, std::uint32_t h, VkFormat format, VkImageUsageFlags usage);
    void WriteLabelSet();
    /* `source` (TRANSFER_SRC) with the label blended into label_image_[1] (left TRANSFER_SRC); false when there is
       no label (or it could not be made): the picture as it is. */
    // (width / height: the picture's in, the label's result's out - at the label's own size when it covers the
    // whole picture and is larger: a menu page's letters are not squeezed into a 224-line picture first.)
    bool RecordLabel(VkImage source, std::uint32_t& width, std::uint32_t& height);
    std::uint32_t label_out_w_ = 0, label_out_h_ = 0;
    void DestroyLabel();
    // PS2HOST_DISPLAY_SHOT=<n>:<file.bmp> (development): the n-th present's picture as it is blitted to the
    // window (after anti-aliasing, BLOOM, FSR), read back and written once - post effects checked by eye.
    std::uint64_t shot_presents_ = 0;
    VkBuffer shot_buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory shot_memory_ = VK_NULL_HANDLE;
    std::uint32_t shot_width_ = 0, shot_height_ = 0;
    bool shot_rgba_ = false;                // the source's byte order (the post passes' RGBA, else BGRA)
    void ShotRecord(VkImage source, std::uint32_t width, std::uint32_t height, bool rgba);
    void ShotWrite();                       // after the present's submission
    void DestroyBloomImages();
    /* `source` (TRANSFER_SRC, width x height) with its glow into bloom_image_[3] (left TRANSFER_SRC). */
    void RecordBloom(VkImage source, std::uint32_t width, std::uint32_t height);
    void DestroyFsr();
    /* `source` (TRANSFER_SRC, width x height) through EASU and RCAS into fsr_image_[2] (left TRANSFER_SRC). */
    void RecordFsr(VkImage source, std::uint32_t width, std::uint32_t height);
    /* The pipelines (once) and the images at these sizes; false when there are none. */
    bool EnsurePost(std::uint32_t in_width, std::uint32_t in_height, std::uint32_t out_width,
                    std::uint32_t out_height);
    void DestroyPostImages();
    /* `source` (TRANSFER_SRC, width x height) through `pipeline` into post_out_ (left
       TRANSFER_SRC), in cmd_. */
    void RecordPost(VkImage source, std::uint32_t width, std::uint32_t height, VkPipeline pipeline);
    // SMAA: post_in_ through its edges and blending weights (images of the
    // picture's size) into post_out_ - smaa_edges, smaa_weights and
    // smaa_blend.comp, each with a set of its own (three samplers, an
    // output image); its area and search tables uploaded once, from a
    // staging buffer kept until the end.
    enum { kSmaaEdges, kSmaaWeights, kSmaaArea, kSmaaSearch, kSmaaImages };
    VkImage smaa_image_[kSmaaImages] = {};
    VkDeviceMemory smaa_memory_[kSmaaImages] = {};
    VkImageView smaa_view_[kSmaaImages] = {};
    VkSampler smaa_point_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout smaa_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool smaa_descriptors_ = VK_NULL_HANDLE;
    VkDescriptorSet smaa_sets_[3] = {};
    VkPipelineLayout smaa_layout_ = VK_NULL_HANDLE;
    VkPipeline smaa_pipelines_[3] = {};
    VkBuffer smaa_staging_ = VK_NULL_HANDLE;
    VkDeviceMemory smaa_staging_memory_ = VK_NULL_HANDLE;
    bool smaa_tables_sent_ = false, smaa_failed_ = false;
    std::uint32_t smaa_width_ = 0, smaa_height_ = 0;
    VkImageView smaa_for_in_ = VK_NULL_HANDLE;      // post_in_'s view its sets were written with
    bool MakeSmaaImage(int which, VkFormat format, std::uint32_t width, std::uint32_t height, VkImageUsageFlags usage);
    /* The passes (once), their tables and images at this size; false when there are none. */
    bool EnsureSmaa(std::uint32_t width, std::uint32_t height);
    /* As RecordPost, through SMAA's three passes. */
    void RecordSmaa(VkImage source, std::uint32_t width, std::uint32_t height);
    void DestroySmaa();
    std::string describe_, error_;
    // PresentImageAt's pictures: copies of the frames' targets, five, each
    // its copy's command buffer and fence (the GS thread's own pool). Three
    // were too few where the GPU is the limit (an Odin 2 in ZOE's boss
    // fight): pictures came while all three waited or were shown, and the
    // oldest lost its turn - 303 of 1,651 frames dropped, 160 with five
    // (shown 45 -> 50 a second); agent L, 2026-11-22.
    static constexpr int kPaced = 5;
    struct PacedPicture {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkCommandBuffer copy = VK_NULL_HANDLE;
        VkFence copied = VK_NULL_HANDLE;
        std::uint32_t width = 0, height = 0;
        std::uint64_t due_ns = 0;
    };
    PacedPicture paced_[kPaced];
    static_assert(kPaced == sizeof(copy_stamped_) / sizeof(copy_stamped_[0]), "copy_stamped_: one a paced picture");
    std::uint32_t paced_width_ = 0, paced_height_ = 0;
    VkCommandPool copy_pool_ = VK_NULL_HANDLE;
    std::mutex paced_lock_;                 // ready_, taken_, stop_
    std::condition_variable paced_wake_;
    std::deque<int> ready_;                 // pictures waiting to be shown, oldest first
    int taken_ = -1;                        // the one being shown
    bool stop_ = false;
    std::thread present_thread_;
    std::mutex present_lock_;               // the swapchain and cmds_: the present thread, or Present
    // Present times (TakePacing): intervals since the last take.
    std::mutex pacing_lock_;
    std::chrono::steady_clock::time_point last_present_{};
    std::uint64_t presents_ = 0;
    double interval_sum_ = 0, interval_squares_ = 0, interval_longest_ = 0;
    std::unique_ptr<VulkanGpu> shared_;
};

Gpu* VulkanDisplay::SharedGpu() {
    if (!shared_) {
        auto gpu = std::make_unique<VulkanGpu>();
        std::string error;
        if (!gpu->InitShared(instance_, gpu_, device_, family_, queue_, dual_source_, describe_, &error))
            return nullptr;
        shared_ = std::move(gpu);
    }
    return shared_.get();
}

namespace {
/* A GPU's preference: PS2HOST_GPU=<part of its name> (any case: "intel",
   "4070") first - a laptop's integrated GPU measured beside its discrete
   one - then a discrete GPU. */
int DeviceScore(const VkPhysicalDeviceProperties& p) {
    int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
    if (const char* want = std::getenv("PS2HOST_GPU")) {
        std::string name = p.deviceName, part = want;
        for (char& ch : name) ch = char(std::tolower(static_cast<unsigned char>(ch)));
        for (char& ch : part) ch = char(std::tolower(static_cast<unsigned char>(ch)));
        if (!part.empty() && name.find(part) != std::string::npos) score += 10;
    }
    return score;
}
}  // namespace

bool VulkanDisplay::Check(VkResult result, const char* what) {
    if (result == VK_SUCCESS) return true;
    char text[160];
    std::snprintf(text, sizeof text, "%s failed (VkResult %d)", what, int(result));
    error_ = text;
    return false;
}

bool VulkanDisplay::Init(const char* title, int width, int height) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        error_ = std::string("SDL_Init: ") + SDL_GetError();
        return false;
    }
    // Controllers: optional (the keyboard still plays), and read whichever
    // window has focus - the console's pad does not care either. With
    // PS2HOST_PAD_FOCUS=1, only while this window has it: two instances on
    // one PC each take the pad in turn.
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, std::getenv("PS2HOST_PAD_FOCUS") ? "0" : "1");
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        std::fprintf(stderr, "[window] no game controllers: %s\n", SDL_GetError());
    } else {
        int joysticks = 0, gamepads = 0;
        // Wireless controllers can take a moment to be found.
        for (int waited = 0; waited < 1000 && joysticks == 0; waited += 50) {
            SDL_PumpEvents();
            SDL_free(SDL_GetJoysticks(&joysticks));
            if (joysticks == 0) SDL_Delay(50);
        }
        SDL_free(SDL_GetGamepads(&gamepads));
        std::fprintf(stderr, "[window] %d controller(s) plugged in, %d of them with a known layout\n", joysticks,
                     gamepads);
    }
    if (!Check(LoadVulkan(), "volkInitialize (is there a Vulkan driver?)")) return false;
    window_ = SDL_CreateWindow(title, width, height, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    if (!window_) {
        error_ = std::string("SDL_CreateWindow: ") + SDL_GetError();
        return false;
    }

    // The instance, with the extensions SDL needs for its surface.
    Uint32 count = 0;
    const char* const* extensions = SDL_Vulkan_GetInstanceExtensions(&count);
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = title;
    app.apiVersion = VK_API_VERSION_1_3;      // the GS renderer draws on this device too
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = count;
    ici.ppEnabledExtensionNames = extensions;
    if (!Check(vkCreateInstance(&ici, nullptr, &instance_), "vkCreateInstance")) return false;
    volkLoadInstance(instance_);
#ifdef __ANDROID__
    SDL_AddEventWatch(LifeCycle, this);
#endif
    if (!SDL_Vulkan_CreateSurface(window_, instance_, nullptr, &surface_)) {
        error_ = std::string("SDL_Vulkan_CreateSurface: ") + SDL_GetError();
        return false;
    }

    // A GPU with a queue that draws and presents; a discrete one first.
    std::uint32_t gpus = 0;
    vkEnumeratePhysicalDevices(instance_, &gpus, nullptr);
    std::vector<VkPhysicalDevice> list(gpus);
    vkEnumeratePhysicalDevices(instance_, &gpus, list.data());
    int best = -1;
    for (VkPhysicalDevice candidate : list) {
        std::uint32_t families = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, nullptr);
        std::vector<VkQueueFamilyProperties> props(families);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, props.data());
        for (std::uint32_t f = 0; f < families; ++f) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(candidate, f, surface_, &present);
            if (!(props[f].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !present) continue;
            VkPhysicalDeviceProperties p;
            vkGetPhysicalDeviceProperties(candidate, &p);
            const int score = DeviceScore(p);
            if (score > best) {
                best = score;
                gpu_ = candidate;
                family_ = f;
            }
            break;
        }
    }
    if (!gpu_) {
        error_ = "no Vulkan device can draw and present to the window";
        return false;
    }
    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(gpu_, &p);
    char text[320];
    std::snprintf(text, sizeof text, "%s (Vulkan %u.%u.%u, driver %u)", p.deviceName,
                  VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion),
                  VK_API_VERSION_PATCH(p.apiVersion), p.driverVersion);
    describe_ = text;

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = family_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    std::vector<const char*> device_extensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    // What the GS renderer needs, for when it draws on this device
    // (SharedGpu): dynamic rendering, and dual-source blending if there is any.
    VkPhysicalDeviceFeatures supported;
    vkGetPhysicalDeviceFeatures(gpu_, &supported);
    dual_source_ = supported.dualSrcBlend == VK_TRUE;
    VkPhysicalDeviceVulkan13Features v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    v13.dynamicRendering = VK_TRUE;
    v13.synchronization2 = VK_TRUE;
    v13.shaderDemoteToHelperInvocation = VK_TRUE;      // the shaders' discard (gpu.cpp)
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features.features.dualSrcBlend = supported.dualSrcBlend;
    features.features.logicOp = supported.logicOp;
    features.features.textureCompressionBC = supported.textureCompressionBC;     // texture packs' DDS
    features.pNext = &v13;
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT f3{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
    const bool dynamic_blend = OfferDynamicBlend(gpu_, &f3, &device_extensions);
    if (dynamic_blend) v13.pNext = &f3;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &features;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = std::uint32_t(device_extensions.size());
    dci.ppEnabledExtensionNames = device_extensions.data();
    if (!Check(vkCreateDevice(gpu_, &dci, nullptr, &device_), "vkCreateDevice")) return false;
    g_dynamic_blend = dynamic_blend;
    // Device functions stay the loader's: see gpu.cpp.
    vkGetDeviceQueue(device_, family_, 0, &queue_);

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = family_;
    if (!Check(vkCreateCommandPool(device_, &pci, nullptr, &pool_), "vkCreateCommandPool")) return false;
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool_;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = kInFlight;
    if (!Check(vkAllocateCommandBuffers(device_, &cai, cmds_), "vkAllocateCommandBuffers")) return false;
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (int i = 0; i < kInFlight; ++i) {
        if (!Check(vkCreateSemaphore(device_, &sci, nullptr, &acquireds_[i]), "vkCreateSemaphore")) return false;
        if (!Check(vkCreateFence(device_, &fci, nullptr, &fences_[i]), "vkCreateFence")) return false;
        VkFenceCreateInfo unsignalled{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (!Check(vkCreateFence(device_, &unsignalled, nullptr, &acquire_fences_[i]), "vkCreateFence")) return false;
    }
    cmd_ = cmds_[0];
    acquired_ = acquireds_[0];
    fence_ = fences_[0];
    return CreateSwapchain();
}

bool VulkanDisplay::CreateSwapchain() {
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu_, surface_, &caps);
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window_, &w, &h);
    extent_ = caps.currentExtent.width != 0xFFFFFFFFu ? caps.currentExtent
              : VkExtent2D{std::uint32_t(w), std::uint32_t(h)};
    // A display mounted turned (Android: the Odin 2 Portal's panel is portrait, the game always landscape): the
    // surface reports a 90/270 degree transform. The picture is drawn upright at the window's own size (SDL's, in
    // pixels - the surface's extent can be either way round) and the compositor turns it (preTransform identity):
    // with the reported transform it came out turned, with a swapped extent squashed (Odin, 2026-11-21).
    VkSurfaceTransformFlagBitsKHR transform = caps.currentTransform;
    if (transform != VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR &&
        (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)) {
        if (w > 0 && h > 0) extent_ = VkExtent2D{std::uint32_t(w), std::uint32_t(h)};
        transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
        extent_from_window_ = true;
    } else {
        extent_from_window_ = false;
    }
    if (extent_.width == 0 || extent_.height == 0) return true;      // minimised: nothing to show
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        error_ = "the swapchain cannot be a transfer destination";
        return false;
    }

    std::uint32_t n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu_, surface_, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu_, surface_, &n, formats.data());
    // An 8-bit UNORM picture in the plain sRGB colour space (the one Windows' drivers offer): a desktop with HDR or
    // wide-gamut output (KDE / GNOME on Wayland) lists B8G8R8A8_UNORM with other colour spaces too - extended sRGB,
    // pass-through - which the compositor shows brighter (agent L, 2026-11-21: the picture too bright on KDE).
    VkSurfaceFormatKHR chosen = formats.empty() ? VkSurfaceFormatKHR{} : formats[0];
    int rank = 0;
    for (const auto& f : formats) {
        const bool srgb = f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        const int r = f.format == VK_FORMAT_B8G8R8A8_UNORM ? (srgb ? 4 : 2)
                      : f.format == VK_FORMAT_R8G8B8A8_UNORM ? (srgb ? 3 : 1) : 0;
        if (r > rank) rank = r, chosen = f;
    }
    format_ = chosen.format;
    static bool told = false;
    if (!told) {
        told = true;
        std::fprintf(stderr, "[display] swapchain format %d, colour space %d (of %u offered)\n", int(chosen.format),
                     int(chosen.colorSpace), unsigned(formats.size()));
    }

    vkGetPhysicalDeviceSurfacePresentModesKHR(gpu_, surface_, &n, nullptr);
    std::vector<VkPresentModeKHR> modes(n);
    vkGetPhysicalDeviceSurfacePresentModesKHR(gpu_, surface_, &n, modes.data());
    mode_ = VK_PRESENT_MODE_FIFO_KHR;            // every device has it
    if (!vsync_)
        for (VkPresentModeKHR m : {VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_MAILBOX_KHR})
            if (std::find(modes.begin(), modes.end(), m) != modes.end()) mode_ = m;

    VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sci.surface = surface_;
    sci.minImageCount = std::max(caps.minImageCount, 2u);
    if (caps.maxImageCount) sci.minImageCount = std::min(sci.minImageCount + 1, caps.maxImageCount);
    sci.imageFormat = format_;
    sci.imageColorSpace = chosen.colorSpace;
    sci.imageExtent = extent_;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = transform;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode = mode_;
    sci.clipped = VK_TRUE;
    sci.oldSwapchain = swapchain_;
    VkSwapchainKHR created = VK_NULL_HANDLE;
    if (!Check(vkCreateSwapchainKHR(device_, &sci, nullptr, &created), "vkCreateSwapchainKHR")) return false;
    DestroySwapchain();
    swapchain_ = created;
    vkGetSwapchainImagesKHR(device_, swapchain_, &n, nullptr);
    images_.resize(n);
    vkGetSwapchainImagesKHR(device_, swapchain_, &n, images_.data());
    VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    rendered_.resize(n);
    for (auto& s : rendered_)
        if (!Check(vkCreateSemaphore(device_, &semaphore, nullptr, &s), "vkCreateSemaphore")) return false;
    stale_ = false;
    return true;
}

void VulkanDisplay::DestroySwapchain() {
    if (!device_) return;
    {
        std::lock_guard<std::mutex> hold(g_queue_lock);
        vkDeviceWaitIdle(device_);
    }
    for (VkSemaphore s : rendered_) vkDestroySemaphore(device_, s, nullptr);
    rendered_.clear();
    images_.clear();
    if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
}

std::uint32_t VulkanDisplay::FindMemory(std::uint32_t bits, VkMemoryPropertyFlags wanted) const {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(gpu_, &mp);
    for (std::uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & wanted) == wanted) return i;
    return ~0u;
}

void VulkanDisplay::DestroyPicture() {
    if (!device_) return;
    if (mapped_) vkUnmapMemory(device_, staging_memory_);
    mapped_ = nullptr;
    if (staging_) vkDestroyBuffer(device_, staging_, nullptr);
    if (staging_memory_) vkFreeMemory(device_, staging_memory_, nullptr);
    if (picture_) vkDestroyImage(device_, picture_, nullptr);
    if (picture_memory_) vkFreeMemory(device_, picture_memory_, nullptr);
    staging_ = VK_NULL_HANDLE;
    staging_memory_ = picture_memory_ = VK_NULL_HANDLE;
    picture_ = VK_NULL_HANDLE;
    picture_width_ = picture_height_ = 0;
}

bool VulkanDisplay::EnsurePicture(std::uint32_t width, std::uint32_t height) {
    if (width == picture_width_ && height == picture_height_ && picture_) return true;
    {
        std::lock_guard<std::mutex> hold(g_queue_lock);
        vkDeviceWaitIdle(device_);
    }
    DestroyPicture();
    const VkDeviceSize bytes = VkDeviceSize(width) * height * 4;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = bytes;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (!Check(vkCreateBuffer(device_, &bci, nullptr, &staging_), "vkCreateBuffer")) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device_, staging_, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = FindMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!Check(vkAllocateMemory(device_, &mai, nullptr, &staging_memory_), "vkAllocateMemory (staging)"))
        return false;
    vkBindBufferMemory(device_, staging_, staging_memory_, 0);
    vkMapMemory(device_, staging_memory_, 0, bytes, 0, &mapped_);

    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_B8G8R8A8_UNORM;
    ici.extent = {width, height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!Check(vkCreateImage(device_, &ici, nullptr, &picture_), "vkCreateImage")) return false;
    vkGetImageMemoryRequirements(device_, picture_, &req);
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = FindMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!Check(vkAllocateMemory(device_, &mai, nullptr, &picture_memory_), "vkAllocateMemory (picture)"))
        return false;
    vkBindImageMemory(device_, picture_, picture_memory_, 0);
    picture_width_ = width;
    picture_height_ = height;
    return true;
}

void Barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
             VkAccessFlags src, VkAccessFlags dst, VkPipelineStageFlags src_stage,
             VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = from;
    b.newLayout = to;
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

/* Android's life cycle (an event watch: SDL sends these from Android's own thread, at once - the event loop may be
   blocked while paused): Home / Back / another app take the window's surface away, so the swapchain and the Vulkan
   surface are let go first; back in the foreground the next present makes new ones (the picture stayed black). */
bool SDLCALL VulkanDisplay::LifeCycle(void* self, SDL_Event* e) {
    VulkanDisplay* d = static_cast<VulkanDisplay*>(self);
    if (e->type == SDL_EVENT_WILL_ENTER_BACKGROUND || e->type == SDL_EVENT_DID_ENTER_BACKGROUND) {
        std::lock_guard<std::mutex> hold(d->present_lock_);
        if (!d->background_) {
            d->background_ = true;
            d->foreground_ = false;
            d->DestroySwapchain();
            if (d->surface_) vkDestroySurfaceKHR(d->instance_, d->surface_, nullptr);
            d->surface_ = VK_NULL_HANDLE;
            std::fprintf(stderr, "[display] in the background: the surface let go\n");
        }
    } else if (e->type == SDL_EVENT_DID_ENTER_FOREGROUND) {
        d->foreground_ = true;
    }
    return true;
}

bool VulkanDisplay::BeginPresent(std::uint32_t* index, bool* ok) {
    *ok = true;
    if (closed_) {
        *ok = false;
        return false;
    }
    if (background_ && foreground_.exchange(false)) {  // (Android: back - the window's new surface)
        if (SDL_Vulkan_CreateSurface(window_, instance_, nullptr, &surface_)) {
            background_ = false;
            stale_ = true;
            std::fprintf(stderr, "[display] back in the foreground: a new surface\n");
        } else {
            surface_ = VK_NULL_HANDLE;
            foreground_ = true;                     // (its window not there yet: again at the next present)
        }
    }
    if (background_ || !surface_) return false;        // (Android, in the background: nothing to show on)
    if (extent_from_window_ && swapchain_) {
        // Sized by the window, not the surface: a new window size (Android at the start: 1920x914 with the bars, then
        // 1920x1080 once they are hidden) need not give OUT_OF_DATE - a swapchain of the first size would stay.
        // Checked each present (a cached value of SDL's), besides the resize event.
        int w = 0, h = 0;
        if (SDL_GetWindowSizeInPixels(window_, &w, &h) && w > 0 && h > 0 &&
            (std::uint32_t(w) != extent_.width || std::uint32_t(h) != extent_.height))
            stale_ = true;
    }
    if (stale_ && !CreateSwapchain()) {
        *ok = false;
        return false;
    }
    if (!swapchain_ || extent_.width == 0 || extent_.height == 0) return false;
    slot_ = (slot_ + 1) % kInFlight;
    cmd_ = cmds_[slot_];
    acquired_ = acquireds_[slot_];
    fence_ = fences_[slot_];
    vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX);
    VkFence acquire_fence = acquire_fences_[slot_];
    vkResetFences(device_, 1, &acquire_fence);
    VkResult r = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, acquired_, acquire_fence, index);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        stale_ = true;
        return false;
    }
    if (r != VK_SUBOPTIMAL_KHR && !Check(r, "vkAcquireNextImageKHR")) {
        *ok = false;
        return false;
    }
    vkWaitForFences(device_, 1, &acquire_fence, VK_TRUE, UINT64_MAX);
    vkResetFences(device_, 1, &fence_);
    vkResetCommandBuffer(cmd_, 0);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd_, &begin);
    EnsureStamps();
    if (stamps_) {
        // The slot's last present has run (its fence waited for above).
        if (present_stamped_[slot_]) {
            TakeStamp(2 * std::uint32_t(slot_), g_display_present_ns);
            g_display_presents.fetch_add(1, std::memory_order_relaxed);
        }
        present_stamped_[slot_] = false;
        vkCmdResetQueryPool(cmd_, stamps_, 2 * std::uint32_t(slot_), 2);
        vkCmdWriteTimestamp(cmd_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, stamps_, 2 * std::uint32_t(slot_));
    }
    return true;
}

void VulkanDisplay::EnsureStamps() {
    if (stamp_tried_) return;
    stamp_tried_ = true;
    if (!std::getenv("PS2HOST_GPU_PROFILE")) return;
    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(gpu_, &p);
    if (p.limits.timestampPeriod <= 0) return;
    VkQueryPoolCreateInfo qci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qci.queryCount = 2 * (kInFlight + kPaced);
    if (vkCreateQueryPool(device_, &qci, nullptr, &stamps_) != VK_SUCCESS) {
        stamps_ = VK_NULL_HANDLE;
        return;
    }
    stamp_ns_ = p.limits.timestampPeriod;
}

void VulkanDisplay::TakeStamp(std::uint32_t first, std::atomic<std::uint64_t>& into) {
    std::uint64_t ticks[2] = {};
    if (vkGetQueryPoolResults(device_, stamps_, first, 2, sizeof ticks, ticks, sizeof ticks[0],
                              VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && ticks[1] > ticks[0])
        into.fetch_add(std::uint64_t(double(ticks[1] - ticks[0]) * stamp_ns_), std::memory_order_relaxed);
}

void VulkanDisplay::DrawOverlay(VkImage target) {
    std::lock_guard<std::mutex> hold(overlay_lock_);
    if (overlay_.empty() || !overlay_width_ || !overlay_height_) return;
    if (!overlay_buffer_) {
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = kOverlayBytes * kInFlight;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        if (vkCreateBuffer(device_, &bci, nullptr, &overlay_buffer_) != VK_SUCCESS) return;
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device_, overlay_buffer_, &req);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = FindMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (vkAllocateMemory(device_, &mai, nullptr, &overlay_memory_) != VK_SUCCESS) return;
        vkBindBufferMemory(device_, overlay_buffer_, overlay_memory_, 0);
        vkMapMemory(device_, overlay_memory_, 0, kOverlayBytes * kInFlight, 0, &overlay_mapped_);
    }
    const std::uint32_t w = std::min(overlay_width_, extent_.width > 16 ? extent_.width - 16 : 0u);
    const std::uint32_t h = std::min(overlay_height_, extent_.height > 16 ? extent_.height - 16 : 0u);
    if (!w || !h || VkDeviceSize(overlay_width_) * overlay_height_ * 4 > kOverlayBytes) return;
    // This slot's copy (BeginPresent waited for its fence: no present reads it).
    const VkDeviceSize offset = kOverlayBytes * VkDeviceSize(slot_);
    if (overlay_slot_generation_[slot_] != overlay_generation_) {
        std::memcpy(static_cast<char*>(overlay_mapped_) + offset, overlay_.data(),
                    std::size_t(overlay_width_) * overlay_height_ * 4);
        overlay_slot_generation_[slot_] = overlay_generation_;
    }
    // After the picture's blit into the same image: two transfer writes over the same pixels need a barrier to keep
    // their order (Turnip ran them together and the blit covered the FPS counter; the Adreno driver kept the order)
    VkImageMemoryBarrier after{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    after.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    after.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    after.oldLayout = after.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    after.srcQueueFamilyIndex = after.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    after.image = target;
    after.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &after);
    VkBufferImageCopy copy{};
    copy.bufferOffset = offset;
    copy.bufferRowLength = overlay_width_;
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageOffset = {16, 16, 0};
    copy.imageExtent = {w, h, 1};
    vkCmdCopyBufferToImage(cmd_, overlay_buffer_, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
}

void VulkanDisplay::DestroyPostImages() {
    if (!device_) return;
    if (post_in_view_) vkDestroyImageView(device_, post_in_view_, nullptr);
    if (post_out_view_) vkDestroyImageView(device_, post_out_view_, nullptr);
    if (post_in_) vkDestroyImage(device_, post_in_, nullptr);
    if (post_out_) vkDestroyImage(device_, post_out_, nullptr);
    if (post_in_memory_) vkFreeMemory(device_, post_in_memory_, nullptr);
    if (post_out_memory_) vkFreeMemory(device_, post_out_memory_, nullptr);
    post_in_view_ = post_out_view_ = VK_NULL_HANDLE;
    post_in_ = post_out_ = VK_NULL_HANDLE;
    post_in_memory_ = post_out_memory_ = VK_NULL_HANDLE;
    post_in_width_ = post_in_height_ = post_out_width_ = post_out_height_ = 0;
}

bool VulkanDisplay::EnsurePost(std::uint32_t width, std::uint32_t height, std::uint32_t out_width,
                               std::uint32_t out_height) {
    if (post_failed_) return false;
    if (width == post_in_width_ && height == post_in_height_ && out_width == post_out_width_ &&
        out_height == post_out_height_ && post_out_)
        return true;
    post_failed_ = true;                    // until it is all made
    if (!fxaa_pipeline_) {
        VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (!Check(vkCreateSampler(device_, &sci, nullptr, &post_sampler_), "vkCreateSampler (FXAA)")) return false;
        VkDescriptorSetLayoutBinding bindings[2] = {};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        lci.bindingCount = 2;
        lci.pBindings = bindings;
        if (!Check(vkCreateDescriptorSetLayout(device_, &lci, nullptr, &post_set_layout_), "FXAA set layout"))
            return false;
        const VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1},
                                               {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}};
        VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dpi.maxSets = 1;
        dpi.poolSizeCount = 2;
        dpi.pPoolSizes = sizes;
        if (!Check(vkCreateDescriptorPool(device_, &dpi, nullptr, &post_descriptors_), "FXAA descriptor pool"))
            return false;
        VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dai.descriptorPool = post_descriptors_;
        dai.descriptorSetCount = 1;
        dai.pSetLayouts = &post_set_layout_;
        if (!Check(vkAllocateDescriptorSets(device_, &dai, &post_set_), "FXAA descriptor set")) return false;
        const VkPushConstantRange size{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &post_set_layout_;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges = &size;
        if (!Check(vkCreatePipelineLayout(device_, &pli, nullptr, &post_layout_), "FXAA pipeline layout"))
            return false;
        const struct { const std::uint32_t* code; std::size_t bytes; VkPipeline* pipeline; } shaders[2] = {
            {spirv_fxaa_comp, sizeof spirv_fxaa_comp, &fxaa_pipeline_},
            {spirv_downsample_comp, sizeof spirv_downsample_comp, &downsample_pipeline_},
        };
        for (const auto& shader : shaders) {
            VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            smi.codeSize = shader.bytes;
            smi.pCode = shader.code;
            VkShaderModule module = VK_NULL_HANDLE;
            if (!Check(vkCreateShaderModule(device_, &smi, nullptr, &module), "anti-aliasing shader")) return false;
            VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            cpi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            cpi.stage.module = module;
            cpi.stage.pName = "main";
            cpi.layout = post_layout_;
            const VkResult made = vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &cpi, nullptr, shader.pipeline);
            vkDestroyShaderModule(device_, module, nullptr);
            if (!Check(made, "anti-aliasing pipeline")) return false;
        }
    }
    // Earlier presents may still read the old images.
    {
        std::lock_guard<std::mutex> hold(g_queue_lock);
        vkDeviceWaitIdle(device_);
    }
    DestroyPostImages();
    struct Made { VkImage* image; VkDeviceMemory* memory; VkImageView* view; VkImageUsageFlags usage;
                  std::uint32_t width, height; };
    const Made made[2] = {
        {&post_in_, &post_in_memory_, &post_in_view_, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
         width, height},
        {&post_out_, &post_out_memory_, &post_out_view_, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
         out_width, out_height},
    };
    for (const Made& m : made) {
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_R8G8B8A8_UNORM;
        ici.extent = {m.width, m.height, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = m.usage;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (!Check(vkCreateImage(device_, &ici, nullptr, m.image), "vkCreateImage (FXAA)")) return false;
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(device_, *m.image, &req);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = FindMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (!Check(vkAllocateMemory(device_, &mai, nullptr, m.memory), "vkAllocateMemory (FXAA)")) return false;
        vkBindImageMemory(device_, *m.image, *m.memory, 0);
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = *m.image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = VK_FORMAT_R8G8B8A8_UNORM;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (!Check(vkCreateImageView(device_, &vci, nullptr, m.view), "vkCreateImageView (FXAA)")) return false;
    }
    const VkDescriptorImageInfo in{post_sampler_, post_in_view_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorImageInfo out{VK_NULL_HANDLE, post_out_view_, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet writes[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
    writes[0].dstSet = writes[1].dstSet = post_set_;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].pImageInfo = &in;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[1].pImageInfo = &out;
    vkUpdateDescriptorSets(device_, 2, writes, 0, nullptr);
    post_in_width_ = width;
    post_in_height_ = height;
    post_out_width_ = out_width;
    post_out_height_ = out_height;
    post_failed_ = false;
    return true;
}

void VulkanDisplay::RecordPost(VkImage source, std::uint32_t width, std::uint32_t height, VkPipeline pipeline) {
    // The picture as RGBA (a blit converts the display's BGRA), to be sampled;
    // the last present's reads of both images done before they are written.
    Barrier(cmd_, post_in_, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkImageBlit region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = region.srcSubresource;
    region.srcOffsets[1] = {std::int32_t(width), std::int32_t(height), 1};
    region.dstOffsets[1] = region.srcOffsets[1];
    vkCmdBlitImage(cmd_, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, post_in_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &region, VK_FILTER_NEAREST);
    Barrier(cmd_, post_in_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    Barrier(cmd_, post_out_, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, post_layout_, 0, 1, &post_set_, 0, nullptr);
    struct { float texel[2]; std::int32_t size[2]; } size{{1.0f / float(width), 1.0f / float(height)},
                                                         {std::int32_t(post_out_width_), std::int32_t(post_out_height_)}};
    vkCmdPushConstants(cmd_, post_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof size, &size);
    vkCmdDispatch(cmd_, (post_out_width_ + 7) / 8, (post_out_height_ + 7) / 8, 1);
    Barrier(cmd_, post_out_, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT,
            VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
}

void VulkanDisplay::DestroyFsr() {
    if (!device_) return;
    for (int i = 0; i < kFsrImages; ++i) {
        if (fsr_view_[i]) vkDestroyImageView(device_, fsr_view_[i], nullptr);
        if (fsr_image_[i]) vkDestroyImage(device_, fsr_image_[i], nullptr);
        if (fsr_memory_[i]) vkFreeMemory(device_, fsr_memory_[i], nullptr);
        fsr_view_[i] = VK_NULL_HANDLE;
        fsr_image_[i] = VK_NULL_HANDLE;
        fsr_memory_[i] = VK_NULL_HANDLE;
    }
    fsr_in_width_ = fsr_in_height_ = fsr_out_width_ = fsr_out_height_ = 0;
    for (VkPipeline& p : fsr_pipelines_) {
        if (p) vkDestroyPipeline(device_, p, nullptr);
        p = VK_NULL_HANDLE;
    }
    if (fsr_layout_) vkDestroyPipelineLayout(device_, fsr_layout_, nullptr);
    if (fsr_descriptors_) vkDestroyDescriptorPool(device_, fsr_descriptors_, nullptr);
    if (fsr_set_layout_) vkDestroyDescriptorSetLayout(device_, fsr_set_layout_, nullptr);
    if (fsr_sampler_) vkDestroySampler(device_, fsr_sampler_, nullptr);
    fsr_layout_ = VK_NULL_HANDLE;
    fsr_descriptors_ = VK_NULL_HANDLE;
    fsr_set_layout_ = VK_NULL_HANDLE;
    fsr_sampler_ = VK_NULL_HANDLE;
    fsr_sets_[0] = fsr_sets_[1] = VK_NULL_HANDLE;
}

bool VulkanDisplay::EnsureFsr(std::uint32_t width, std::uint32_t height, std::uint32_t out_width,
                              std::uint32_t out_height) {
    if (fsr_failed_) return false;
    if (width == fsr_in_width_ && height == fsr_in_height_ && out_width == fsr_out_width_ &&
        out_height == fsr_out_height_ && fsr_image_[2])
        return true;
    fsr_failed_ = true;                     // until it is all made
    if (!fsr_pipelines_[0]) {
        VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (!Check(vkCreateSampler(device_, &sci, nullptr, &fsr_sampler_), "vkCreateSampler (FSR)")) return false;
        VkDescriptorSetLayoutBinding bindings[2] = {};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        lci.bindingCount = 2;
        lci.pBindings = bindings;
        if (!Check(vkCreateDescriptorSetLayout(device_, &lci, nullptr, &fsr_set_layout_), "FSR set layout"))
            return false;
        const VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2},
                                               {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2}};
        VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dpi.maxSets = 2;
        dpi.poolSizeCount = 2;
        dpi.pPoolSizes = sizes;
        if (!Check(vkCreateDescriptorPool(device_, &dpi, nullptr, &fsr_descriptors_), "FSR descriptor pool"))
            return false;
        const VkDescriptorSetLayout layouts[2] = {fsr_set_layout_, fsr_set_layout_};
        VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dai.descriptorPool = fsr_descriptors_;
        dai.descriptorSetCount = 2;
        dai.pSetLayouts = layouts;
        if (!Check(vkAllocateDescriptorSets(device_, &dai, fsr_sets_), "FSR descriptor sets")) return false;
        const VkPushConstantRange params{VK_SHADER_STAGE_COMPUTE_BIT, 0, 32};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &fsr_set_layout_;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges = &params;
        if (!Check(vkCreatePipelineLayout(device_, &pli, nullptr, &fsr_layout_), "FSR pipeline layout")) return false;
        const struct { const std::uint32_t* code; std::size_t bytes; } shaders[2] = {
            {spirv_fsr_easu_comp, sizeof spirv_fsr_easu_comp}, {spirv_fsr_rcas_comp, sizeof spirv_fsr_rcas_comp}};
        for (int i = 0; i < 2; ++i) {
            VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            smi.codeSize = shaders[i].bytes;
            smi.pCode = shaders[i].code;
            VkShaderModule module = VK_NULL_HANDLE;
            if (!Check(vkCreateShaderModule(device_, &smi, nullptr, &module), "FSR shader")) return false;
            VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            cpi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            cpi.stage.module = module;
            cpi.stage.pName = "main";
            cpi.layout = fsr_layout_;
            const VkResult made = vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &cpi, nullptr, &fsr_pipelines_[i]);
            vkDestroyShaderModule(device_, module, nullptr);
            if (!Check(made, "FSR pipeline")) return false;
        }
    }
    // Earlier presents may still read the old images.
    {
        std::lock_guard<std::mutex> hold(g_queue_lock);
        vkDeviceWaitIdle(device_);
    }
    for (int i = 0; i < kFsrImages; ++i) {
        if (fsr_view_[i]) vkDestroyImageView(device_, fsr_view_[i], nullptr);
        if (fsr_image_[i]) vkDestroyImage(device_, fsr_image_[i], nullptr);
        if (fsr_memory_[i]) vkFreeMemory(device_, fsr_memory_[i], nullptr);
        fsr_view_[i] = VK_NULL_HANDLE;
        fsr_image_[i] = VK_NULL_HANDLE;
        fsr_memory_[i] = VK_NULL_HANDLE;
    }
    const struct { VkImageUsageFlags usage; std::uint32_t width, height; } made[kFsrImages] = {
        {VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, width, height},
        {VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, out_width, out_height},
        {VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, out_width, out_height},
    };
    for (int i = 0; i < kFsrImages; ++i) {
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_R8G8B8A8_UNORM;
        ici.extent = {made[i].width, made[i].height, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = made[i].usage;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (!Check(vkCreateImage(device_, &ici, nullptr, &fsr_image_[i]), "vkCreateImage (FSR)")) return false;
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(device_, fsr_image_[i], &req);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = FindMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (!Check(vkAllocateMemory(device_, &mai, nullptr, &fsr_memory_[i]), "vkAllocateMemory (FSR)")) return false;
        vkBindImageMemory(device_, fsr_image_[i], fsr_memory_[i], 0);
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = fsr_image_[i];
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = VK_FORMAT_R8G8B8A8_UNORM;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (!Check(vkCreateImageView(device_, &vci, nullptr, &fsr_view_[i]), "vkCreateImageView (FSR)")) return false;
    }
    // EASU: [0] sampled -> [1]; RCAS: [1] sampled -> [2].
    for (int pass = 0; pass < 2; ++pass) {
        const VkDescriptorImageInfo in{fsr_sampler_, fsr_view_[pass], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        const VkDescriptorImageInfo out{VK_NULL_HANDLE, fsr_view_[pass + 1], VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet writes[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET},
                                          {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
        writes[0].dstSet = writes[1].dstSet = fsr_sets_[pass];
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo = &in;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[1].pImageInfo = &out;
        vkUpdateDescriptorSets(device_, 2, writes, 0, nullptr);
    }
    fsr_in_width_ = width;
    fsr_in_height_ = height;
    fsr_out_width_ = out_width;
    fsr_out_height_ = out_height;
    fsr_failed_ = false;
    return true;
}

void VulkanDisplay::RecordFsr(VkImage source, std::uint32_t width, std::uint32_t height) {
    // The picture as RGBA (a blit converts the display's BGRA), to be sampled; the last present's reads of all
    // three images done before they are written.
    Barrier(cmd_, fsr_image_[0], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkImageBlit region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = region.srcSubresource;
    region.srcOffsets[1] = {std::int32_t(width), std::int32_t(height), 1};
    region.dstOffsets[1] = region.srcOffsets[1];
    vkCmdBlitImage(cmd_, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, fsr_image_[0],
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_NEAREST);
    Barrier(cmd_, fsr_image_[0], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    Barrier(cmd_, fsr_image_[1], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    struct { float in_view[2], in_size[2], out_size[2], sharpness, pad; } params{
        {float(width), float(height)}, {float(width), float(height)},
        {float(fsr_out_width_), float(fsr_out_height_)}, fsr_sharpness_.load(), 0.0f};
    const std::uint32_t gx = (fsr_out_width_ + 15) / 16, gy = (fsr_out_height_ + 15) / 16;
    vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, fsr_pipelines_[0]);
    vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, fsr_layout_, 0, 1, &fsr_sets_[0], 0, nullptr);
    vkCmdPushConstants(cmd_, fsr_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof params, &params);
    vkCmdDispatch(cmd_, gx, gy, 1);
    Barrier(cmd_, fsr_image_[1], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    Barrier(cmd_, fsr_image_[2], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, fsr_pipelines_[1]);
    vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, fsr_layout_, 0, 1, &fsr_sets_[1], 0, nullptr);
    vkCmdPushConstants(cmd_, fsr_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof params, &params);
    vkCmdDispatch(cmd_, gx, gy, 1);
    Barrier(cmd_, fsr_image_[2], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT);
}

void VulkanDisplay::DestroyBloomImages() {
    if (!device_) return;
    for (int i = 0; i < kBloomImages; ++i) {
        if (bloom_view_[i]) vkDestroyImageView(device_, bloom_view_[i], nullptr);
        if (bloom_image_[i]) vkDestroyImage(device_, bloom_image_[i], nullptr);
        if (bloom_memory_[i]) vkFreeMemory(device_, bloom_memory_[i], nullptr);
        bloom_view_[i] = VK_NULL_HANDLE;
        bloom_image_[i] = VK_NULL_HANDLE;
        bloom_memory_[i] = VK_NULL_HANDLE;
    }
    bloom_width_ = bloom_height_ = 0;
}

bool VulkanDisplay::EnsureBloom(std::uint32_t width, std::uint32_t height) {
    if (bloom_failed_) return false;
    if (width == bloom_width_ && height == bloom_height_ && bloom_image_[3]) return true;
    bloom_failed_ = true;                   // until it is all made
    if (!bloom_pipeline_) {
        VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (!Check(vkCreateSampler(device_, &sci, nullptr, &bloom_sampler_), "vkCreateSampler (bloom)")) return false;
        VkDescriptorSetLayoutBinding bindings[3] = {};
        for (std::uint32_t i = 0; i < 3; ++i) {
            bindings[i].binding = i;
            bindings[i].descriptorType = i == 1 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        lci.bindingCount = 3;
        lci.pBindings = bindings;
        if (!Check(vkCreateDescriptorSetLayout(device_, &lci, nullptr, &bloom_set_layout_), "bloom set layout"))
            return false;
        const VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8},
                                               {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4}};
        VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dpi.maxSets = 4;
        dpi.poolSizeCount = 2;
        dpi.pPoolSizes = sizes;
        if (!Check(vkCreateDescriptorPool(device_, &dpi, nullptr, &bloom_descriptors_), "bloom descriptor pool"))
            return false;
        const VkDescriptorSetLayout layouts[4] = {bloom_set_layout_, bloom_set_layout_, bloom_set_layout_,
                                                  bloom_set_layout_};
        VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dai.descriptorPool = bloom_descriptors_;
        dai.descriptorSetCount = 4;
        dai.pSetLayouts = layouts;
        if (!Check(vkAllocateDescriptorSets(device_, &dai, bloom_sets_), "bloom descriptor sets")) return false;
        const VkPushConstantRange params{VK_SHADER_STAGE_COMPUTE_BIT, 0, 24};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &bloom_set_layout_;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges = &params;
        if (!Check(vkCreatePipelineLayout(device_, &pli, nullptr, &bloom_layout_), "bloom pipeline layout"))
            return false;
        VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        smi.codeSize = sizeof spirv_bloom_comp;
        smi.pCode = spirv_bloom_comp;
        VkShaderModule module = VK_NULL_HANDLE;
        if (!Check(vkCreateShaderModule(device_, &smi, nullptr, &module), "bloom shader")) return false;
        VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cpi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        cpi.stage.module = module;
        cpi.stage.pName = "main";
        cpi.layout = bloom_layout_;
        const VkResult made = vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &cpi, nullptr, &bloom_pipeline_);
        vkDestroyShaderModule(device_, module, nullptr);
        if (!Check(made, "bloom pipeline")) return false;
    }
    // Earlier presents may still read the old images.
    {
        std::lock_guard<std::mutex> hold(g_queue_lock);
        vkDeviceWaitIdle(device_);
    }
    DestroyBloomImages();
    const std::uint32_t qw = std::max(1u, (width + 3) / 4), qh = std::max(1u, (height + 3) / 4);
    const struct { VkImageUsageFlags usage; std::uint32_t width, height; } made[kBloomImages] = {
        {VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, width, height},
        {VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, qw, qh},
        {VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, qw, qh},
        {VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, width, height},
    };
    for (int i = 0; i < kBloomImages; ++i) {
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_R8G8B8A8_UNORM;
        ici.extent = {made[i].width, made[i].height, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = made[i].usage;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (!Check(vkCreateImage(device_, &ici, nullptr, &bloom_image_[i]), "vkCreateImage (bloom)")) return false;
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(device_, bloom_image_[i], &req);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = FindMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (!Check(vkAllocateMemory(device_, &mai, nullptr, &bloom_memory_[i]), "vkAllocateMemory (bloom)"))
            return false;
        vkBindImageMemory(device_, bloom_image_[i], bloom_memory_[i], 0);
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = bloom_image_[i];
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = VK_FORMAT_R8G8B8A8_UNORM;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (!Check(vkCreateImageView(device_, &vci, nullptr, &bloom_view_[i]), "vkCreateImageView (bloom)"))
            return false;
    }
    // Per pass: (source, target, glow) - bright [0] -> [1], across [1] -> [2], down [2] -> [1], added [0]+[1] -> [3].
    const int wiring[4][3] = {{0, 1, 0}, {1, 2, 1}, {2, 1, 2}, {0, 3, 1}};
    for (int pass = 0; pass < 4; ++pass) {
        const VkDescriptorImageInfo source{bloom_sampler_, bloom_view_[wiring[pass][0]], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        const VkDescriptorImageInfo target{VK_NULL_HANDLE, bloom_view_[wiring[pass][1]], VK_IMAGE_LAYOUT_GENERAL};
        const VkDescriptorImageInfo glow{bloom_sampler_, bloom_view_[wiring[pass][2]], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        const VkDescriptorImageInfo* infos[3] = {&source, &target, &glow};
        VkWriteDescriptorSet writes[3] = {};
        for (std::uint32_t i = 0; i < 3; ++i) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = bloom_sets_[pass];
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = i == 1 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[i].pImageInfo = infos[i];
        }
        vkUpdateDescriptorSets(device_, 3, writes, 0, nullptr);
    }
    bloom_width_ = width;
    bloom_height_ = height;
    bloom_failed_ = false;
    return true;
}

void VulkanDisplay::RecordBloom(VkImage source, std::uint32_t width, std::uint32_t height) {
    const std::uint32_t qw = std::max(1u, (width + 3) / 4), qh = std::max(1u, (height + 3) / 4);
    struct { float texel[2]; std::int32_t size[2]; std::int32_t mode; float strength; } params{};
    const auto run = [&](int pass, std::uint32_t in_w, std::uint32_t in_h, std::uint32_t out_w, std::uint32_t out_h) {
        params.texel[0] = 1.0f / float(in_w);
        params.texel[1] = 1.0f / float(in_h);
        params.size[0] = std::int32_t(out_w);
        params.size[1] = std::int32_t(out_h);
        params.mode = pass;
        params.strength = float(bloom_.load());
        vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, bloom_layout_, 0, 1, &bloom_sets_[pass], 0, nullptr);
        vkCmdPushConstants(cmd_, bloom_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof params, &params);
        vkCmdDispatch(cmd_, (out_w + 7) / 8, (out_h + 7) / 8, 1);
    };
    const auto written = [&](VkImage image) {      // a pass's target, read by the next ones
        Barrier(cmd_, image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    };
    // The picture as RGBA (a blit converts the display's BGRA); the last present's reads of the images done
    // before they are written.
    Barrier(cmd_, bloom_image_[0], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkImageBlit region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = region.srcSubresource;
    region.srcOffsets[1] = {std::int32_t(width), std::int32_t(height), 1};
    region.dstOffsets[1] = region.srcOffsets[1];
    vkCmdBlitImage(cmd_, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, bloom_image_[0],
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_NEAREST);
    Barrier(cmd_, bloom_image_[0], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, bloom_pipeline_);
    for (int i : {1, 2})
        Barrier(cmd_, bloom_image_[i], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    run(0, width, height, qw, qh);          // bright parts, a quarter each way
    written(bloom_image_[1]);
    run(1, qw, qh, qw, qh);                 // across
    written(bloom_image_[2]);
    Barrier(cmd_, bloom_image_[1], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
            VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    run(2, qw, qh, qw, qh);                 // down
    written(bloom_image_[1]);
    Barrier(cmd_, bloom_image_[3], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    run(3, width, height, width, height);   // added to the picture
    Barrier(cmd_, bloom_image_[3], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT);
}

// PS2HOST_DISPLAY_SHOT=<n>[,<n>...]:<file.bmp>: the presents to read back; with more than one, each written as
// <file>_<n>.bmp.
static const std::vector<unsigned long long>& ShotPresents() {
    static const std::vector<unsigned long long> list = [] {
        std::vector<unsigned long long> v;
        const char* e = std::getenv("PS2HOST_DISPLAY_SHOT");
        while (e && *e && *e != ':') {
            char* end = nullptr;
            const unsigned long long n = std::strtoull(e, &end, 10);
            if (end == e) break;
            v.push_back(n);
            e = *end == ',' ? end + 1 : end;
        }
        return v;
    }();
    return list;
}

void VulkanDisplay::ShotRecord(VkImage source, std::uint32_t width, std::uint32_t height, bool rgba) {
    const auto& list = ShotPresents();
    if (list.empty() || shot_buffer_) return;
    ++shot_presents_;
    if (std::find(list.begin(), list.end(), shot_presents_) == list.end() || !width || !height) return;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = VkDeviceSize(width) * height * 4;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (vkCreateBuffer(device_, &bci, nullptr, &shot_buffer_) != VK_SUCCESS) {
        shot_buffer_ = VK_NULL_HANDLE;
        return;
    }
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device_, shot_buffer_, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = FindMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(device_, &mai, nullptr, &shot_memory_) != VK_SUCCESS) {
        vkDestroyBuffer(device_, shot_buffer_, nullptr);
        shot_buffer_ = VK_NULL_HANDLE;
        return;
    }
    vkBindBufferMemory(device_, shot_buffer_, shot_memory_, 0);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {width, height, 1};
    vkCmdCopyImageToBuffer(cmd_, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, shot_buffer_, 1, &copy);
    shot_width_ = width;
    shot_height_ = height;
    shot_rgba_ = rgba;
}

void VulkanDisplay::ShotWrite() {
    vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX);
    const char* want = std::getenv("PS2HOST_DISPLAY_SHOT");
    const char* path = want ? std::strchr(want, ':') : nullptr;
    void* mapped = nullptr;
    if (path && vkMapMemory(device_, shot_memory_, 0, VK_WHOLE_SIZE, 0, &mapped) == VK_SUCCESS) {
        // A 32-bit BMP, rows top-down (a negative height), bytes as BMP keeps them (B, G, R, A).
        const std::uint32_t w = shot_width_, h = shot_height_, bytes = w * h * 4;
        std::vector<std::uint8_t> pixels(static_cast<const std::uint8_t*>(mapped),
                                         static_cast<const std::uint8_t*>(mapped) + bytes);
        vkUnmapMemory(device_, shot_memory_);
        if (shot_rgba_)
            for (std::uint32_t i = 0; i < bytes; i += 4) std::swap(pixels[i], pixels[i + 2]);
        std::uint8_t header[54] = {'B', 'M'};
        const auto put = [&](int at, std::uint32_t v) {
            for (int i = 0; i < 4; ++i) header[at + i] = std::uint8_t(v >> (8 * i));
        };
        put(2, 54 + bytes);
        put(10, 54);
        put(14, 40);
        put(18, w);
        put(22, std::uint32_t(-std::int32_t(h)));
        header[26] = 1;
        header[28] = 32;
        put(34, bytes);
        std::string file = path + 1;
        if (ShotPresents().size() > 1) {
            const std::size_t dot = file.rfind('.');
            const std::string n = "_" + std::to_string(shot_presents_);
            file = dot == std::string::npos ? file + n : file.substr(0, dot) + n + file.substr(dot);
        }
        if (std::FILE* f = std::fopen(file.c_str(), "wb")) {
            std::fwrite(header, 1, sizeof header, f);
            std::fwrite(pixels.data(), 1, pixels.size(), f);
            std::fclose(f);
            std::fprintf(stderr, "[display] present %llu: %ux%u written to %s\n",
                         static_cast<unsigned long long>(shot_presents_), unsigned(w), unsigned(h), file.c_str());
        }
    }
    vkDestroyBuffer(device_, shot_buffer_, nullptr);
    vkFreeMemory(device_, shot_memory_, nullptr);
    shot_buffer_ = VK_NULL_HANDLE;
    shot_memory_ = VK_NULL_HANDLE;
}

bool VulkanDisplay::MakeLabelImage(int which, std::uint32_t w, std::uint32_t h, VkFormat format,
                                   VkImageUsageFlags usage) {
    if (label_view_[which]) vkDestroyImageView(device_, label_view_[which], nullptr);
    if (label_image_[which]) vkDestroyImage(device_, label_image_[which], nullptr);
    if (label_memory_[which]) vkFreeMemory(device_, label_memory_[which], nullptr);
    label_view_[which] = VK_NULL_HANDLE;
    label_image_[which] = VK_NULL_HANDLE;
    label_memory_[which] = VK_NULL_HANDLE;
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format;
    ici.extent = {w, h, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!Check(vkCreateImage(device_, &ici, nullptr, &label_image_[which]), "vkCreateImage (label)")) return false;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device_, label_image_[which], &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = FindMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!Check(vkAllocateMemory(device_, &mai, nullptr, &label_memory_[which]), "vkAllocateMemory (label)")) return false;
    vkBindImageMemory(device_, label_image_[which], label_memory_[which], 0);
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = label_image_[which];
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return Check(vkCreateImageView(device_, &vci, nullptr, &label_view_[which]), "vkCreateImageView (label)");
}

void VulkanDisplay::WriteLabelSet() {
    const VkDescriptorImageInfo source{label_sampler_, label_view_[0], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorImageInfo target{VK_NULL_HANDLE, label_view_[1], VK_IMAGE_LAYOUT_GENERAL};
    const VkDescriptorImageInfo label{label_sampler_, label_view_[2], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorImageInfo* infos[3] = {&source, &target, &label};
    VkWriteDescriptorSet writes[3] = {};
    for (std::uint32_t i = 0; i < 3; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = label_set_;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = i == 1 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = infos[i];
    }
    vkUpdateDescriptorSets(device_, 3, writes, 0, nullptr);
}

bool VulkanDisplay::RecordLabel(VkImage source, std::uint32_t& width, std::uint32_t& height) {
    std::unique_lock<std::mutex> hold(label_lock_);
    if (label_pixels_.empty() || label_failed_ || !width || !height) return false;
    label_failed_ = true;                   // until it is all made
    if (!label_pipeline_) {
        VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (!Check(vkCreateSampler(device_, &sci, nullptr, &label_sampler_), "vkCreateSampler (label)")) return false;
        VkDescriptorSetLayoutBinding bindings[3] = {};
        for (std::uint32_t i = 0; i < 3; ++i) {
            bindings[i].binding = i;
            bindings[i].descriptorType = i == 1 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        lci.bindingCount = 3;
        lci.pBindings = bindings;
        if (!Check(vkCreateDescriptorSetLayout(device_, &lci, nullptr, &label_set_layout_), "label set layout")) return false;
        const VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2},
                                               {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}};
        VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dpi.maxSets = 1;
        dpi.poolSizeCount = 2;
        dpi.pPoolSizes = sizes;
        if (!Check(vkCreateDescriptorPool(device_, &dpi, nullptr, &label_descriptors_), "label descriptor pool")) return false;
        VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dai.descriptorPool = label_descriptors_;
        dai.descriptorSetCount = 1;
        dai.pSetLayouts = &label_set_layout_;
        if (!Check(vkAllocateDescriptorSets(device_, &dai, &label_set_), "label descriptor set")) return false;
        const VkPushConstantRange params{VK_SHADER_STAGE_COMPUTE_BIT, 0, 24};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &label_set_layout_;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges = &params;
        if (!Check(vkCreatePipelineLayout(device_, &pli, nullptr, &label_layout_), "label pipeline layout")) return false;
        VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        smi.codeSize = sizeof spirv_label_comp;
        smi.pCode = spirv_label_comp;
        VkShaderModule module = VK_NULL_HANDLE;
        if (!Check(vkCreateShaderModule(device_, &smi, nullptr, &module), "label shader")) return false;
        VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cpi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        cpi.stage.module = module;
        cpi.stage.pName = "main";
        cpi.layout = label_layout_;
        const VkResult made = vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &cpi, nullptr, &label_pipeline_);
        vkDestroyShaderModule(device_, module, nullptr);
        if (!Check(made, "label pipeline")) return false;
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = VkDeviceSize(kLabelMaxW) * kLabelMaxH * 4 * kInFlight;     // a part for each frame in flight
        bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        if (!Check(vkCreateBuffer(device_, &bci, nullptr, &label_staging_), "vkCreateBuffer (label)")) return false;
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device_, label_staging_, &req);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = FindMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (!Check(vkAllocateMemory(device_, &mai, nullptr, &label_staging_memory_), "vkAllocateMemory (label)")) return false;
        vkBindBufferMemory(device_, label_staging_, label_staging_memory_, 0);
        vkMapMemory(device_, label_staging_memory_, 0, VK_WHOLE_SIZE, 0, &label_staging_mapped_);
    }
    // The images at the picture's size and the label's (made again when either changes; earlier presents may
    // still read the old ones), the label's pixels uploaded when they changed.
    const bool whole = label_rect_[0] <= 0.001f && label_rect_[1] <= 0.001f && label_rect_[2] >= 0.999f &&
                       label_rect_[3] >= 0.999f;
    const std::uint32_t out_w = whole ? std::max(width, label_w_) : width;
    const std::uint32_t out_h = whole ? std::max(height, label_h_) : height;
    const bool resize = width != label_size_w_ || height != label_size_h_ || out_w != label_out_w_ ||
                        out_h != label_out_h_ || !label_image_[1];
    const bool retex = label_w_ != label_tex_w_ || label_h_ != label_tex_h_ || !label_image_[2];
    const bool upload = label_generation_ != label_uploaded_;
    // New images wait for the GPU (earlier frames may read the old ones); new pixels do not - they go through this
    // frame's own part of the staging buffer, and the copy's barrier orders it after the frames before (a label
    // changing every frame - a menu's words sliding in - made each of those frames wait for an idle GPU: a hitch).
    if (resize || retex) {
        std::lock_guard<std::mutex> queue(g_queue_lock);
        vkDeviceWaitIdle(device_);
    }
    if (resize) {
        if (!MakeLabelImage(0, width, height, VK_FORMAT_R8G8B8A8_UNORM,
                            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT) ||
            !MakeLabelImage(1, out_w, out_h, VK_FORMAT_R8G8B8A8_UNORM,
                            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
            return false;
        label_size_w_ = width;
        label_size_h_ = height;
        label_out_w_ = out_w;
        label_out_h_ = out_h;
    }
    if (retex) {
        if (!MakeLabelImage(2, label_w_, label_h_, VK_FORMAT_B8G8R8A8_UNORM,
                            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT))
            return false;
        label_tex_w_ = label_w_;
        label_tex_h_ = label_h_;
    }
    if (resize || retex) WriteLabelSet();
    if (upload || retex) {
        const VkDeviceSize part = VkDeviceSize(kLabelMaxW) * kLabelMaxH * 4 * VkDeviceSize(slot_);
        std::memcpy(static_cast<unsigned char*>(label_staging_mapped_) + part, label_pixels_.data(),
                    label_pixels_.size() * 4);
        Barrier(cmd_, label_image_[2], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy{};
        copy.bufferOffset = part;
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {label_w_, label_h_, 1};
        vkCmdCopyBufferToImage(cmd_, label_staging_, label_image_[2], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        Barrier(cmd_, label_image_[2], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        label_uploaded_ = label_generation_;
    }
    struct { float rect[4]; std::int32_t size[2]; } params{
        {label_rect_[0], label_rect_[1], label_rect_[2], label_rect_[3]}, {std::int32_t(out_w), std::int32_t(out_h)}};
    hold.unlock();
    // The picture as RGBA (a blit converts the display's BGRA), to be sampled.
    Barrier(cmd_, label_image_[0], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkImageBlit region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = region.srcSubresource;
    region.srcOffsets[1] = {std::int32_t(width), std::int32_t(height), 1};
    region.dstOffsets[1] = region.srcOffsets[1];
    vkCmdBlitImage(cmd_, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, label_image_[0],
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_NEAREST);
    Barrier(cmd_, label_image_[0], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    Barrier(cmd_, label_image_[1], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, label_pipeline_);
    vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, label_layout_, 0, 1, &label_set_, 0, nullptr);
    vkCmdPushConstants(cmd_, label_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof params, &params);
    vkCmdDispatch(cmd_, (out_w + 7) / 8, (out_h + 7) / 8, 1);
    Barrier(cmd_, label_image_[1], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT);
    label_failed_ = false;
    width = out_w;                          // the picture goes on at the label's size
    height = out_h;
    return true;
}

void VulkanDisplay::DestroyLabel() {
    if (!device_) return;
    for (int i = 0; i < 3; ++i) {
        if (label_view_[i]) vkDestroyImageView(device_, label_view_[i], nullptr);
        if (label_image_[i]) vkDestroyImage(device_, label_image_[i], nullptr);
        if (label_memory_[i]) vkFreeMemory(device_, label_memory_[i], nullptr);
    }
    if (label_staging_) vkDestroyBuffer(device_, label_staging_, nullptr);
    if (label_staging_memory_) vkFreeMemory(device_, label_staging_memory_, nullptr);
    if (label_pipeline_) vkDestroyPipeline(device_, label_pipeline_, nullptr);
    if (label_layout_) vkDestroyPipelineLayout(device_, label_layout_, nullptr);
    if (label_descriptors_) vkDestroyDescriptorPool(device_, label_descriptors_, nullptr);
    if (label_set_layout_) vkDestroyDescriptorSetLayout(device_, label_set_layout_, nullptr);
    if (label_sampler_) vkDestroySampler(device_, label_sampler_, nullptr);
}

bool VulkanDisplay::MakeSmaaImage(int which, VkFormat format, std::uint32_t width, std::uint32_t height,
                                  VkImageUsageFlags usage) {
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format;
    ici.extent = {width, height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!Check(vkCreateImage(device_, &ici, nullptr, &smaa_image_[which]), "vkCreateImage (SMAA)")) return false;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device_, smaa_image_[which], &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = FindMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!Check(vkAllocateMemory(device_, &mai, nullptr, &smaa_memory_[which]), "vkAllocateMemory (SMAA)")) return false;
    vkBindImageMemory(device_, smaa_image_[which], smaa_memory_[which], 0);
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = smaa_image_[which];
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return Check(vkCreateImageView(device_, &vci, nullptr, &smaa_view_[which]), "vkCreateImageView (SMAA)");
}

void VulkanDisplay::DestroySmaa() {
    if (!device_) return;
    for (int i = 0; i < kSmaaImages; ++i) {
        if (smaa_view_[i]) vkDestroyImageView(device_, smaa_view_[i], nullptr);
        if (smaa_image_[i]) vkDestroyImage(device_, smaa_image_[i], nullptr);
        if (smaa_memory_[i]) vkFreeMemory(device_, smaa_memory_[i], nullptr);
        smaa_view_[i] = VK_NULL_HANDLE;
        smaa_image_[i] = VK_NULL_HANDLE;
        smaa_memory_[i] = VK_NULL_HANDLE;
    }
    for (VkPipeline& p : smaa_pipelines_)
        if (p) vkDestroyPipeline(device_, p, nullptr), p = VK_NULL_HANDLE;
    if (smaa_layout_) vkDestroyPipelineLayout(device_, smaa_layout_, nullptr);
    if (smaa_descriptors_) vkDestroyDescriptorPool(device_, smaa_descriptors_, nullptr);
    if (smaa_set_layout_) vkDestroyDescriptorSetLayout(device_, smaa_set_layout_, nullptr);
    if (smaa_point_) vkDestroySampler(device_, smaa_point_, nullptr);
    if (smaa_staging_) vkDestroyBuffer(device_, smaa_staging_, nullptr);
    if (smaa_staging_memory_) vkFreeMemory(device_, smaa_staging_memory_, nullptr);
    smaa_layout_ = VK_NULL_HANDLE;
    smaa_descriptors_ = VK_NULL_HANDLE;
    smaa_set_layout_ = VK_NULL_HANDLE;
    smaa_point_ = VK_NULL_HANDLE;
    smaa_staging_ = VK_NULL_HANDLE;
    smaa_staging_memory_ = VK_NULL_HANDLE;
}

bool VulkanDisplay::EnsureSmaa(std::uint32_t width, std::uint32_t height) {
    if (smaa_failed_) return false;
    if (width == smaa_width_ && height == smaa_height_ && smaa_for_in_ == post_in_view_ && smaa_image_[kSmaaEdges])
        return true;
    smaa_failed_ = true;                    // until it is all made
    if (!smaa_pipelines_[0]) {
        VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (!Check(vkCreateSampler(device_, &sci, nullptr, &smaa_point_), "vkCreateSampler (SMAA)")) return false;
        VkDescriptorSetLayoutBinding bindings[4] = {};
        for (std::uint32_t i = 0; i < 4; ++i) {
            bindings[i].binding = i;
            bindings[i].descriptorType = i < 3 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        lci.bindingCount = 4;
        lci.pBindings = bindings;
        if (!Check(vkCreateDescriptorSetLayout(device_, &lci, nullptr, &smaa_set_layout_), "SMAA set layout"))
            return false;
        const VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 9},
                                               {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3}};
        VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dpi.maxSets = 3;
        dpi.poolSizeCount = 2;
        dpi.pPoolSizes = sizes;
        if (!Check(vkCreateDescriptorPool(device_, &dpi, nullptr, &smaa_descriptors_), "SMAA descriptor pool"))
            return false;
        const VkDescriptorSetLayout layouts[3] = {smaa_set_layout_, smaa_set_layout_, smaa_set_layout_};
        VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dai.descriptorPool = smaa_descriptors_;
        dai.descriptorSetCount = 3;
        dai.pSetLayouts = layouts;
        if (!Check(vkAllocateDescriptorSets(device_, &dai, smaa_sets_), "SMAA descriptor sets")) return false;
        const VkPushConstantRange metrics{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &smaa_set_layout_;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges = &metrics;
        if (!Check(vkCreatePipelineLayout(device_, &pli, nullptr, &smaa_layout_), "SMAA pipeline layout")) return false;
        const struct { const std::uint32_t* code; std::size_t bytes; } shaders[3] = {
            {spirv_smaa_edges_comp, sizeof spirv_smaa_edges_comp},
            {spirv_smaa_weights_comp, sizeof spirv_smaa_weights_comp},
            {spirv_smaa_blend_comp, sizeof spirv_smaa_blend_comp},
        };
        for (int i = 0; i < 3; ++i) {
            VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            smi.codeSize = shaders[i].bytes;
            smi.pCode = shaders[i].code;
            VkShaderModule module = VK_NULL_HANDLE;
            if (!Check(vkCreateShaderModule(device_, &smi, nullptr, &module), "SMAA shader")) return false;
            VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            cpi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            cpi.stage.module = module;
            cpi.stage.pName = "main";
            cpi.layout = smaa_layout_;
            const VkResult made = vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &cpi, nullptr, &smaa_pipelines_[i]);
            vkDestroyShaderModule(device_, module, nullptr);
            if (!Check(made, "SMAA pipeline")) return false;
        }
        // The tables: their images, and their bytes in a staging buffer
        // (RecordSmaa copies them over the first time).
        const VkImageUsageFlags table = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        if (!MakeSmaaImage(kSmaaArea, VK_FORMAT_R8G8_UNORM, AREATEX_WIDTH, AREATEX_HEIGHT, table) ||
            !MakeSmaaImage(kSmaaSearch, VK_FORMAT_R8_UNORM, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, table))
            return false;
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = sizeof areaTexBytes + sizeof searchTexBytes;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        if (!Check(vkCreateBuffer(device_, &bci, nullptr, &smaa_staging_), "SMAA staging buffer")) return false;
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device_, smaa_staging_, &req);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = FindMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (!Check(vkAllocateMemory(device_, &mai, nullptr, &smaa_staging_memory_), "SMAA staging memory")) return false;
        vkBindBufferMemory(device_, smaa_staging_, smaa_staging_memory_, 0);
        void* mapped = nullptr;
        if (!Check(vkMapMemory(device_, smaa_staging_memory_, 0, bci.size, 0, &mapped), "SMAA staging map")) return false;
        std::memcpy(mapped, areaTexBytes, sizeof areaTexBytes);
        std::memcpy(static_cast<char*>(mapped) + sizeof areaTexBytes, searchTexBytes, sizeof searchTexBytes);
        vkUnmapMemory(device_, smaa_staging_memory_);
    }
    // Earlier presents may still read the old images.
    {
        std::lock_guard<std::mutex> hold(g_queue_lock);
        vkDeviceWaitIdle(device_);
    }
    for (int which : {int(kSmaaEdges), int(kSmaaWeights)}) {
        if (smaa_view_[which]) vkDestroyImageView(device_, smaa_view_[which], nullptr);
        if (smaa_image_[which]) vkDestroyImage(device_, smaa_image_[which], nullptr);
        if (smaa_memory_[which]) vkFreeMemory(device_, smaa_memory_[which], nullptr);
        smaa_view_[which] = VK_NULL_HANDLE;
        smaa_image_[which] = VK_NULL_HANDLE;
        smaa_memory_[which] = VK_NULL_HANDLE;
        if (!MakeSmaaImage(which, VK_FORMAT_R8G8B8A8_UNORM, width, height,
                           VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT))
            return false;
    }
    // Each pass's set: its inputs (sampled; one unused stands for the
    // others) and its output.
    const VkImageLayout read = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    const VkDescriptorImageInfo colour{post_sampler_, post_in_view_, read};
    const VkDescriptorImageInfo edges{post_sampler_, smaa_view_[kSmaaEdges], read};
    const VkDescriptorImageInfo weights{post_sampler_, smaa_view_[kSmaaWeights], read};
    const VkDescriptorImageInfo area{post_sampler_, smaa_view_[kSmaaArea], read};
    const VkDescriptorImageInfo search{smaa_point_, smaa_view_[kSmaaSearch], read};
    const VkDescriptorImageInfo out_edges{VK_NULL_HANDLE, smaa_view_[kSmaaEdges], VK_IMAGE_LAYOUT_GENERAL};
    const VkDescriptorImageInfo out_weights{VK_NULL_HANDLE, smaa_view_[kSmaaWeights], VK_IMAGE_LAYOUT_GENERAL};
    const VkDescriptorImageInfo out_picture{VK_NULL_HANDLE, post_out_view_, VK_IMAGE_LAYOUT_GENERAL};
    const VkDescriptorImageInfo* inputs[3][4] = {{&colour, &colour, &colour, &out_edges},
                                                 {&edges, &area, &search, &out_weights},
                                                 {&colour, &weights, &colour, &out_picture}};
    VkWriteDescriptorSet writes[12] = {};
    for (int set = 0; set < 3; ++set)
        for (std::uint32_t b = 0; b < 4; ++b) {
            VkWriteDescriptorSet& w = writes[set * 4 + b];
            w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet = smaa_sets_[set];
            w.dstBinding = b;
            w.descriptorCount = 1;
            w.descriptorType = b < 3 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            w.pImageInfo = inputs[set][b];
        }
    vkUpdateDescriptorSets(device_, 12, writes, 0, nullptr);
    smaa_width_ = width;
    smaa_height_ = height;
    smaa_for_in_ = post_in_view_;
    smaa_failed_ = false;
    return true;
}

void VulkanDisplay::RecordSmaa(VkImage source, std::uint32_t width, std::uint32_t height) {
    // The picture as RGBA, to be sampled (as RecordPost).
    Barrier(cmd_, post_in_, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkImageBlit region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = region.srcSubresource;
    region.srcOffsets[1] = {std::int32_t(width), std::int32_t(height), 1};
    region.dstOffsets[1] = region.srcOffsets[1];
    vkCmdBlitImage(cmd_, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, post_in_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &region, VK_FILTER_NEAREST);
    Barrier(cmd_, post_in_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    if (!smaa_tables_sent_) {
        const struct { int which; VkDeviceSize offset; std::uint32_t w, h; } tables[2] = {
            {kSmaaArea, 0, AREATEX_WIDTH, AREATEX_HEIGHT},
            {kSmaaSearch, sizeof areaTexBytes, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT}};
        for (const auto& t : tables) {
            Barrier(cmd_, smaa_image_[t.which], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy copy{};
            copy.bufferOffset = t.offset;
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {t.w, t.h, 1};
            vkCmdCopyBufferToImage(cmd_, smaa_staging_, smaa_image_[t.which], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                   &copy);
            Barrier(cmd_, smaa_image_[t.which], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        }
        smaa_tables_sent_ = true;
    }
    const float metrics[4] = {1.0f / float(width), 1.0f / float(height), float(width), float(height)};
    const VkImage outputs[3] = {smaa_image_[kSmaaEdges], smaa_image_[kSmaaWeights], post_out_};
    for (int pass = 0; pass < 3; ++pass) {
        Barrier(cmd_, outputs[pass], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, smaa_pipelines_[pass]);
        vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, smaa_layout_, 0, 1, &smaa_sets_[pass], 0, nullptr);
        vkCmdPushConstants(cmd_, smaa_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof metrics, metrics);
        vkCmdDispatch(cmd_, (width + 7) / 8, (height + 7) / 8, 1);
        if (pass < 2)                       // read by the next pass
            Barrier(cmd_, outputs[pass], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    }
    Barrier(cmd_, post_out_, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT,
            VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
}

bool VulkanDisplay::FinishPresent(std::uint32_t index, VkImage source, std::uint32_t width, std::uint32_t height) {
    const VkImage target = images_[index];
    Barrier(cmd_, target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    const VkClearColorValue black{};
    const VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd_, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &all);
    {   // the clear before the picture's blit over it: ordered by a barrier (Turnip ran them together - black bands)
        VkImageMemoryBarrier cleared{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        cleared.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        cleared.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        cleared.oldLayout = cleared.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        cleared.srcQueueFamilyIndex = cleared.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        cleared.image = target;
        cleared.subresourceRange = all;
        vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &cleared);
    }
    // 4:3 as a television shows it, 16:9, or the window's own shape (SetAspect),
    // centred in the window.
    std::int32_t w = std::int32_t(extent_.width), h = std::int32_t(extent_.height);
    std::int32_t dw = w, dh = h;
    const float picture = picture_aspect_.load();
    const float aspect = picture > 0.0f ? picture : aspect_.load();
    if (aspect > 0.0f) {
        if (float(w) > float(h) * aspect) dw = std::int32_t(float(h) * aspect + 0.5f);
        else dh = std::int32_t(float(w) / aspect + 0.5f);
    }
    const std::int32_t x0 = (w - dw) / 2, y0 = (h - dh) / 2;
    shown_x0_.store(x0);
    shown_y0_.store(y0);
    shown_w_.store(std::max(dw, 1));
    shown_seen_.store(true);
    shown_h_.store(std::max(dh, 1));
    // Anti-aliasing: FXAA at the picture's size; supersampling's picture,
    // larger than shown, averaged down to the shown size (then copied 1:1).
    const AntiAliasing mode = AntiAliasing(anti_aliasing_.load());
    const bool larger = width > std::uint32_t(dw) || height > std::uint32_t(dh);
    if (mode == AntiAliasing::kFxaa && EnsurePost(width, height, width, height)) {
        RecordPost(source, width, height, fxaa_pipeline_);
        source = post_out_;
    } else if (mode == AntiAliasing::kSmaa && EnsurePost(width, height, width, height) && EnsureSmaa(width, height)) {
        RecordSmaa(source, width, height);
        source = post_out_;
    } else if (mode == AntiAliasing::kDownsample && larger && dw > 0 && dh > 0 &&
               EnsurePost(width, height, std::uint32_t(dw), std::uint32_t(dh))) {
        RecordPost(source, width, height, downsample_pipeline_);
        source = post_out_;
        width = std::uint32_t(dw);
        height = std::uint32_t(dh);
    }
    // BLOOM: the bright parts' glow added, after the anti-aliasing, at the picture's size (before it is scaled).
    bool rgba = source == post_out_;
    if (bloom_.load() > 0 && width && height && EnsureBloom(width, height)) {
        RecordBloom(source, width, height);
        source = bloom_image_[3];
        rgba = true;
    }
    // A port's label in the picture (SetPictureLabel): after the post effects, before the scaling.
    if (RecordLabel(source, width, height)) {
        source = label_image_[1];
        rgba = true;
    }
    // UPSCALING: FSR 1 for a picture smaller than shown (each way at most as large, one smaller), after the
    // anti-aliasing; then copied 1:1. Bilinear (the blit's) otherwise, or where its images could not be made.
    if (Upscaling(upscaling_.load()) == Upscaling::kFsr && dw > 0 && dh > 0 && width <= std::uint32_t(dw) &&
        height <= std::uint32_t(dh) && (width < std::uint32_t(dw) || height < std::uint32_t(dh)) &&
        EnsureFsr(width, height, std::uint32_t(dw), std::uint32_t(dh))) {
        RecordFsr(source, width, height);
        source = fsr_image_[2];
        width = std::uint32_t(dw);
        height = std::uint32_t(dh);
        rgba = true;
    }
    ShotRecord(source, width, height, rgba);
    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = {std::int32_t(width), std::int32_t(height), 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[0] = {x0, y0, 0};
    blit.dstOffsets[1] = {x0 + dw, y0 + dh, 1};
    vkCmdBlitImage(cmd_, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    DrawOverlay(target);
    Barrier(cmd_, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    return true;
}

bool VulkanDisplay::Present(const std::uint32_t* pixels, std::uint32_t width, std::uint32_t height) {
    if (!width || !height) return !closed_;
    std::lock_guard<std::mutex> hold(present_lock_);
    std::uint32_t index = 0;
    bool ok = true;
    if (!BeginPresent(&index, &ok)) return ok;
    if (!EnsurePicture(width, height)) {
        vkEndCommandBuffer(cmd_);
        return false;
    }
    // One staging buffer and picture for all: what an earlier present still
    // copies from goes first.
    for (int i = 0; i < kInFlight; ++i)
        if (i != slot_) vkWaitForFences(device_, 1, &fences_[i], VK_TRUE, UINT64_MAX);
    std::memcpy(mapped_, pixels, std::size_t(width) * height * 4);
    Barrier(cmd_, picture_, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {width, height, 1};
    vkCmdCopyBufferToImage(cmd_, staging_, picture_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    Barrier(cmd_, picture_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT);
    FinishPresent(index, picture_, width, height);
    return Submit(index);
}

bool VulkanDisplay::PresentImage(std::uint64_t image, int layout, std::uint32_t width, std::uint32_t height) {
    if (!width || !height || !image) return !closed_;
    Remember(image, layout, width, height);
    std::lock_guard<std::mutex> hold(present_lock_);
    std::uint32_t index = 0;
    bool ok = true;
    if (!BeginPresent(&index, &ok)) return ok;
    const VkImage source = reinterpret_cast<VkImage>(image);
    const VkImageLayout kept = VkImageLayout(layout);
    // Whatever drew it was submitted before on this queue: a barrier from
    // all of it, and back to the layout its renderer has it in.
    Barrier(cmd_, source, kept, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_MEMORY_WRITE_BIT,
            VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    const std::uint32_t cx = std::min(crop_x_.load(), width / 8), cy = std::min(crop_y_.load(), height / 8);
    if ((cx || cy) && EnsurePicture(width - cx, height - cy)) {
        // SetPictureCrop's edge left out: the rest copied into the picture image first (the queue's earlier
        // reads of it - a present before - go first by the barrier's scope).
        Barrier(cmd_, picture_, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkImageBlit region{};
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.dstSubresource = region.srcSubresource;
        region.srcOffsets[0] = {std::int32_t(cx), std::int32_t(cy), 0};
        region.srcOffsets[1] = {std::int32_t(width), std::int32_t(height), 1};
        region.dstOffsets[1] = {std::int32_t(width - cx), std::int32_t(height - cy), 1};
        vkCmdBlitImage(cmd_, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, picture_,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_NEAREST);
        Barrier(cmd_, picture_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT);
        FinishPresent(index, picture_, width - cx, height - cy);
    } else {
        FinishPresent(index, source, width, height);
    }
    Barrier(cmd_, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, kept, VK_ACCESS_TRANSFER_READ_BIT,
            VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    return Submit(index);
}

bool VulkanDisplay::EnsurePaced(std::uint32_t width, std::uint32_t height) {
    if (width == paced_width_ && height == paced_height_ && paced_[0].image) return true;
    {
        std::unique_lock<std::mutex> hold(paced_lock_);
        ready_.clear();
        paced_wake_.wait(hold, [&] { return taken_ < 0; });   // none being shown
    }
    {
        std::lock_guard<std::mutex> hold(g_queue_lock);
        vkDeviceWaitIdle(device_);
    }
    DestroyPaced();
    if (!copy_pool_) {
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = family_;
        if (!Check(vkCreateCommandPool(device_, &pci, nullptr, &copy_pool_), "vkCreateCommandPool")) return false;
    }
    for (PacedPicture& p : paced_) {
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_B8G8R8A8_UNORM;
        ici.extent = {width, height, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (!Check(vkCreateImage(device_, &ici, nullptr, &p.image), "vkCreateImage (paced)")) return false;
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(device_, p.image, &req);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = FindMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (!Check(vkAllocateMemory(device_, &mai, nullptr, &p.memory), "vkAllocateMemory (paced)")) return false;
        vkBindImageMemory(device_, p.image, p.memory, 0);
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = copy_pool_;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        if (!Check(vkAllocateCommandBuffers(device_, &cai, &p.copy), "vkAllocateCommandBuffers (paced)")) return false;
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (!Check(vkCreateFence(device_, &fci, nullptr, &p.copied), "vkCreateFence (paced)")) return false;
    }
    paced_width_ = width;
    paced_height_ = height;
    if (!present_thread_.joinable()) present_thread_ = std::thread([this] { PresentLoop(); });
    return true;
}

void VulkanDisplay::DestroyPaced() {
    if (!device_) return;
    for (PacedPicture& p : paced_) {
        if (p.copied) vkDestroyFence(device_, p.copied, nullptr);
        if (p.copy) vkFreeCommandBuffers(device_, copy_pool_, 1, &p.copy);
        if (p.image) vkDestroyImage(device_, p.image, nullptr);
        if (p.memory) vkFreeMemory(device_, p.memory, nullptr);
        p = PacedPicture{};
    }
    paced_width_ = paced_height_ = 0;
}

bool VulkanDisplay::PresentImageAt(std::uint64_t image, int layout, std::uint32_t width, std::uint32_t height,
                                   std::uint64_t due_ns) {
    if (!width || !height || !image) return !closed_;
    if (closed_) return false;
    Remember(image, layout, width, height);
    if (!EnsurePaced(width, height)) return false;
    // A picture neither waiting to be shown nor being shown - or, with all
    // of them waiting, the oldest waiting (its turn is lost: the shown
    // picture keeps no more than two frames behind). What the GPU still
    // reads of it (an earlier present's blit, submitted before) goes first,
    // by the queue's order and the copy's barrier.
    int slot = -1;
    {
        std::lock_guard<std::mutex> hold(paced_lock_);
        for (int i = 0; i < kPaced && slot < 0; ++i)
            if (i != taken_ && std::find(ready_.begin(), ready_.end(), i) == ready_.end()) slot = i;
        if (slot < 0) {
            slot = ready_.front();
            ready_.pop_front();
            if (observer_) observer_(paced_[slot].due_ns, 0, true);     // its turn lost
        }
    }
    PacedPicture& p = paced_[slot];
    vkWaitForFences(device_, 1, &p.copied, VK_TRUE, UINT64_MAX);    // its command buffer, from last time
    vkResetFences(device_, 1, &p.copied);
    vkResetCommandBuffer(p.copy, 0);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(p.copy, &begin);
    // (stamps_ is made by the first BeginPresent, before any paced copy can be taken back)
    const std::uint32_t copy_stamp = 2 * std::uint32_t(kInFlight + slot);
    if (stamps_) {
        if (copy_stamped_[slot]) TakeStamp(copy_stamp, g_display_copy_ns);
        copy_stamped_[slot] = false;
        vkCmdResetQueryPool(p.copy, stamps_, copy_stamp, 2);
        vkCmdWriteTimestamp(p.copy, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, stamps_, copy_stamp);
    }
    const VkImage source = reinterpret_cast<VkImage>(image);
    const VkImageLayout kept = VkImageLayout(layout);
    Barrier(p.copy, source, kept, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_MEMORY_WRITE_BIT,
            VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    Barrier(p.copy, p.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    // A blit, not a copy: the renderer's targets are RGBA and these BGRA (a
    // copy takes the bytes as they are - red and blue swapped).
    VkImageBlit region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = region.srcSubresource;
    // SetPictureCrop's edge left out (pixel for pixel: the picture that much smaller).
    const std::uint32_t cx = std::min(crop_x_.load(), width / 8), cy = std::min(crop_y_.load(), height / 8);
    region.srcOffsets[0] = {std::int32_t(cx), std::int32_t(cy), 0};
    region.srcOffsets[1] = {std::int32_t(width), std::int32_t(height), 1};
    region.dstOffsets[1] = {std::int32_t(width - cx), std::int32_t(height - cy), 1};
    vkCmdBlitImage(p.copy, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, p.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &region, VK_FILTER_NEAREST);
    width -= cx;
    height -= cy;
    Barrier(p.copy, p.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT);
    Barrier(p.copy, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, kept, VK_ACCESS_TRANSFER_READ_BIT,
            VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    if (stamps_) {
        vkCmdWriteTimestamp(p.copy, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, stamps_, copy_stamp + 1);
        copy_stamped_[slot] = true;
    }
    vkEndCommandBuffer(p.copy);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &p.copy;
    {
        std::lock_guard<std::mutex> hold(g_queue_lock);
        if (!Check(vkQueueSubmit(queue_, 1, &submit, p.copied), "vkQueueSubmit (paced)")) return false;
    }
    p.width = width;
    p.height = height;
    p.due_ns = due_ns;
    {
        std::lock_guard<std::mutex> hold(paced_lock_);
        ready_.push_back(slot);
    }
    paced_wake_.notify_all();
    return !closed_;
}

namespace {
// Asleep until `when` to a tenth of a millisecond (platform.h).
using ps2platform::SleepUntil;

/* Whether a deadline may be waited for by spinning its last stretch (a
   sleep's wake-up can be late by a little): only with cores to spare - more
   than three for the process. With fewer, a time-critical thread spinning
   takes a core the game's threads need, the game falls behind, and the
   timer, retrying the blank it has not taken, spins the more: on two cores
   the race lost a third of its blanks and stopped for seconds at a time.
   There the high-resolution timer's sleep is the whole wait.
   PS2HOST_SPIN=0 or 1 decides instead. Looked at again now and then (the
   process's cores can change). */
bool SpareCores() {
    static const char* forced = std::getenv("PS2HOST_SPIN");
    if (forced) return forced[0] == '1';
    static std::atomic<int> looks{0};
    static std::atomic<bool> spare{true};
    if (looks.fetch_add(1, std::memory_order_relaxed) % 64 == 0)
        spare.store(ps2platform::ProcessCores() >= 4, std::memory_order_relaxed);
    return spare.load(std::memory_order_relaxed);
}
}  // namespace

void VulkanDisplay::PresentLoop() {
    using Clock = std::chrono::steady_clock;
    // Each picture shown at its time with every core busy: the highest priority
    // (asleep almost always).
    ps2platform::SetThreadPriority(ps2platform::Priority::kTimeCritical);
    for (;;) {
        int slot;
        {
            std::unique_lock<std::mutex> hold(paced_lock_);
            paced_wake_.wait(hold, [&] { return stop_ || !ready_.empty(); });
            if (stop_) return;
            slot = ready_.front();
        }
        // The swapchain's next image, and the last present's command buffer
        // done, ahead of the picture's time: what may wait (the compositor
        // holding the images, a 144 Hz desktop's) waits now, not when it is
        // due - then only the copy and the present are left.
        std::unique_lock<std::mutex> presenting(present_lock_);
        std::uint32_t index = 0;
        bool ok = true;
        const bool began = BeginPresent(&index, &ok);
        // In order, each when it is due.
        const Clock::time_point due = std::min(Clock::time_point{std::chrono::nanoseconds(paced_[slot].due_ns)},
                                               Clock::now() + std::chrono::milliseconds(50));
        if (!SpareCores()) {
            SleepUntil(due);
        } else {
            if (Clock::now() + std::chrono::microseconds(1500) < due) SleepUntil(due - std::chrono::microseconds(700));
            while (Clock::now() < due) std::this_thread::yield();
        }
        {
            std::unique_lock<std::mutex> hold(paced_lock_);
            if (stop_) {
                if (began) vkEndCommandBuffer(cmd_);
                return;
            }
            // The oldest waiting now (a newer one may have taken its turn
            // meanwhile: CopyPaced takes the oldest's slot, then queues its
            // own; a resize empties the queue) - waited for a little: the
            // image is taken and must be shown. Left so (its command buffer
            // ended, nothing submitted) its slot's fence stayed reset and
            // never signalled: the third picture on waited for it the whole
            // hang limit and the device counted lost (ps2-port-engine 28bd074,
            // the same path here).
            if (ready_.empty() && began)
                paced_wake_.wait_for(hold, std::chrono::milliseconds(250), [&] { return stop_ || !ready_.empty(); });
            if (stop_) {
                if (began) vkEndCommandBuffer(cmd_);
                return;
            }
            if (ready_.empty()) {
                hold.unlock();
                if (began) {
                    // (none came: the image shown black - submitted, its fence signalled, the image given back)
                    static int told = 0;
                    if (told < 5) {
                        ++told;
                        std::fprintf(stderr, "[display] a picture's turn with none to show: the screen black once\n");
                    }
                    const VkImage target = images_[index];
                    Barrier(cmd_, target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
                    const VkClearColorValue black{};
                    const VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                    vkCmdClearColorImage(cmd_, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &all);
                    Barrier(cmd_, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                            VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
                    Submit(index);
                }
                continue;
            }
            slot = ready_.front();
            ready_.pop_front();
            taken_ = slot;
        }
        {
            if (began) {
                FinishPresent(index, paced_[slot].image, paced_[slot].width, paced_[slot].height);
                Submit(index);
                PresentObserver observer;
                {
                    std::lock_guard<std::mutex> hold_paced(paced_lock_);
                    observer = observer_;
                }
                if (observer)
                    observer(paced_[slot].due_ns, std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                                   Clock::now().time_since_epoch()).count()), false);
            }
        }
        presenting.unlock();
        {
            std::lock_guard<std::mutex> hold(paced_lock_);
            taken_ = -1;
        }
        paced_wake_.notify_all();
    }
}

bool VulkanDisplay::Submit(std::uint32_t index) {
    if (stamps_) {
        vkCmdWriteTimestamp(cmd_, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, stamps_, 2 * std::uint32_t(slot_) + 1);
        present_stamped_[slot_] = true;
    }
    vkEndCommandBuffer(cmd_);
    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &acquired_;
    submit.pWaitDstStageMask = &wait_stage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd_;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &rendered_[index];
    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &rendered_[index];
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain_;
    present.pImageIndices = &index;
    VkResult r;
    {
        std::lock_guard<std::mutex> hold(g_queue_lock);
        if (!Check(vkQueueSubmit(queue_, 1, &submit, fence_), "vkQueueSubmit")) return false;
        r = vkQueuePresentKHR(queue_, &present);
    }
    if (shot_buffer_) ShotWrite();
    {
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> hold(pacing_lock_);
        if (last_present_.time_since_epoch().count()) {
            const double ms = std::chrono::duration<double, std::milli>(now - last_present_).count();
            ++presents_;
            interval_sum_ += ms;
            interval_squares_ += ms * ms;
            interval_longest_ = std::max(interval_longest_, ms);
            static const bool hitch = std::getenv("PS2HOST_HITCH") != nullptr;
            if (hitch && ms > 25.0)
                std::fprintf(stderr, "[hitch] present: %.1f ms since the last\n", ms);
        }
        last_present_ = now;
        // The FPS counter's: presents counted over half a second at a time.
        if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) ++fps_presents_;
        const double counted = std::chrono::duration<double>(now - fps_since_).count();
        if (counted >= 0.5) {
            fps_.store(fps_since_.time_since_epoch().count() ? fps_presents_ / counted : 0.0);
            fps_presents_ = 0;
            fps_since_ = now;
        }
    }
    // SUBOPTIMAL from a swapchain sized by the window (a turned panel, IDENTITY rather than the surface's own
    // transform: CreateSwapchain) is what it is made as - it said so at every present, and a new swapchain each frame
    // (a vkDeviceWaitIdle under the queue's lock each time) held the renderer's thread ~70 % of the time on an Odin 2
    // (agent L, 2026-11-21). A new window size is seen by BeginPresent's check and the resize event.
    if (r == VK_ERROR_OUT_OF_DATE_KHR || (r == VK_SUBOPTIMAL_KHR && !extent_from_window_)) stale_ = true;
    else if (r != VK_SUBOPTIMAL_KHR && !Check(r, "vkQueuePresentKHR")) return false;
    return true;
}

Display::Pacing VulkanDisplay::TakePacing() {
    std::lock_guard<std::mutex> hold(pacing_lock_);
    Pacing p;
    p.presents = presents_;
    if (presents_) {
        p.mean_ms = interval_sum_ / double(presents_);
        p.jitter_ms = std::sqrt(std::max(0.0, interval_squares_ / double(presents_) - p.mean_ms * p.mean_ms));
        p.longest_ms = interval_longest_;
    }
    presents_ = 0;
    interval_sum_ = interval_squares_ = interval_longest_ = 0;
    return p;
}

void VulkanDisplay::SetVsync(bool on) {
    if (vsync_ == on) return;
    vsync_ = on;
    stale_ = true;                              // the swapchain again, with the other mode
}

/* The display mode fullscreen takes: the desktop's resolution at a refresh
   that the game's ~60 frames a second divide evenly (120 Hz, else 60 Hz),
   not above the desktop's - null for the desktop's own (borderless), when it
   is such a rate already or the display offers none. On a 144 Hz display a
   frame took two refreshes, then three (14 and 21 ms): a judder no pacing
   undoes. */
const SDL_DisplayMode* EvenMode(SDL_Window* window) {
    const SDL_DisplayID id = SDL_GetDisplayForWindow(window);
    const SDL_DisplayMode* desktop = id ? SDL_GetDesktopDisplayMode(id) : nullptr;
    if (!desktop) return nullptr;
    auto even = [](float hz) {
        const int n = int(hz / 60.0f + 0.5f);
        return n >= 1 && std::fabs(hz - 60.0f * float(n)) < 0.6f * float(n);
    };
    if (even(desktop->refresh_rate)) return nullptr;
    int count = 0;
    SDL_DisplayMode** modes = SDL_GetFullscreenDisplayModes(id, &count);
    static SDL_DisplayMode chosen;
    const SDL_DisplayMode* best = nullptr;
    for (int i = 0; modes && i < count; ++i) {
        const SDL_DisplayMode* m = modes[i];
        if (m->w != desktop->w || m->h != desktop->h || !even(m->refresh_rate)) continue;
        if (m->refresh_rate > desktop->refresh_rate + 0.5f) continue;
        if (!best || m->refresh_rate > best->refresh_rate) best = m;
    }
    if (best) chosen = *best;
    SDL_free(modes);
    return best ? &chosen : nullptr;
}

void VulkanDisplay::SetBorderless(bool on) {
    borderless_ = on;
    if (Fullscreen()) SetFullscreen(true);
}

void VulkanDisplay::SetFullscreen(bool on) {
    if (!window_) return;
    if (!OnWindowThread()) {
        std::lock_guard<std::mutex> hold(requests_lock_);
        fullscreen_request_ = on ? 1 : 0;
        return;
    }
    if (on) {
        const SDL_DisplayMode* mode = borderless_ ? nullptr : EvenMode(window_);
        SDL_SetWindowFullscreenMode(window_, mode);
        if (mode)
            std::fprintf(stderr, "[display] fullscreen at %dx%d, %.2f Hz\n", mode->w, mode->h,
                         double(mode->refresh_rate));
    }
    SDL_SetWindowFullscreen(window_, on);
    fullscreen_cursor_stale_ = true;        // the system's pointer: hidden in fullscreen (UpdateCursor)
    cursor_showing_ = -1;
    UpdateCursor();
    stale_ = true;
}

bool VulkanDisplay::Fullscreen() const {
    return window_ && (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN) != 0;
}

bool VulkanDisplay::Pump() {
    if (threaded_) return !closed_;     // the window thread takes the events (WindowLoop)
    return PumpEvents();
}

/* What other threads asked of the window (SetWindowRect, SetFullscreen) - on the window's thread. */
void VulkanDisplay::TakeRequests() {
    RectRequest rect;
    int fullscreen = -1;
    {
        std::lock_guard<std::mutex> hold(requests_lock_);
        rect = rect_request_;
        rect_request_.wanted = false;
        fullscreen = fullscreen_request_;
        fullscreen_request_ = -1;
    }
    if (rect.wanted) SetWindowRect(rect.x, rect.y, rect.width, rect.height);
    if (fullscreen >= 0) SetFullscreen(fullscreen == 1);
}

bool VulkanDisplay::StartOnOwnThread(const char* title, int width, int height) {
    threaded_ = true;
    std::mutex ready_lock;
    std::condition_variable ready_wake;
    int ready = 0;                      // 1 made, -1 failed
    window_thread_ = std::thread([&, name = std::string(title), width, height] {
        window_thread_id_ = std::this_thread::get_id();
        const bool made = Init(name.c_str(), width, height);
        {
            std::lock_guard<std::mutex> hold(ready_lock);
            ready = made ? 1 : -1;
        }
        ready_wake.notify_one();
        if (made) WindowLoop(name, width, height);
        else SDL_Quit();
    });
    std::unique_lock<std::mutex> hold(ready_lock);
    ready_wake.wait(hold, [&] { return ready != 0; });
    if (ready < 0) {
        hold.unlock();
        window_thread_.join();
        return false;
    }
    return true;
}

/* The window thread after Init: its events and requests about every millisecond, until the display ends;
   then the window and SDL go, here, where they were made. */
void VulkanDisplay::WindowLoop(std::string, int, int) {
    while (!window_stop_.load()) {
        TakeRequests();
        PumpEvents();
        // A millisecond asleep, not SDL_WaitEventTimeout(nullptr, 1): with no event taken it returned at once
        // (an event SDL keeps queued, never polled away) and the loop spun a core whole (P-Gate 3 trial
        // 2026-11-19: 98 % of a core in every scene, since the window got its thread, 63b0917).
        SDL_Delay(1);
    }
    {
        std::lock_guard<std::mutex> hold(pads_lock_);
        for (Gamepad& g : pads_) SDL_CloseGamepad(g.pad);
        pads_.clear();
    }
    if (window_) SDL_DestroyWindow(window_);
    window_ = nullptr;
    SDL_Quit();
    window_gone_ = true;
}

bool VulkanDisplay::PumpEvents() {
    {
        std::lock_guard<std::mutex> hold(title_lock_);
        if (title_changed_ && window_) SDL_SetWindowTitle(window_, title_.c_str());
        title_changed_ = false;
    }
    // Each controller's: the triggers' effects as last set, when they change
    // (off on one SetTriggerTargets leaves out), and the motors as the game
    // last set them - renewed before SDL's duration runs out, stopped when
    // set to zero.
    {
        TriggerEffect wanted[2];
        std::vector<std::uint32_t> targets;
        {
            std::lock_guard<std::mutex> hold(triggers_lock_);
            wanted[0] = triggers_[0];
            wanted[1] = triggers_[1];
            targets = trigger_targets_;
        }
        const std::uint64_t now = SDL_GetTicks();
        std::lock_guard<std::mutex> hold(pads_lock_);
        for (Gamepad& g : pads_) {
            const bool target = targets.empty() || std::find(targets.begin(), targets.end(), g.id) != targets.end();
            const TriggerEffect left = target ? wanted[0] : TriggerEffect{}, right = target ? wanted[1] : TriggerEffect{};
            if (g.triggers_unsent || !(left == g.triggers_sent[0]) || !(right == g.triggers_sent[1])) {
                SendTriggers(g.pad, left, right);
                g.triggers_sent[0] = left;
                g.triggers_sent[1] = right;
                g.triggers_unsent = false;
            }
            const std::uint32_t r = g.rumble;
            if (r != g.rumble_sent || (r && now - g.rumble_renewed > 200)) {
                SDL_RumbleGamepad(g.pad, std::uint16_t(r >> 16), std::uint16_t(r), r ? 400 : 0);
                g.rumble_sent = r;
                g.rumble_renewed = now;
            }
        }
    }
    // The window keeps the picture's shape (the ports' rule: 4:3 960x720, 16:9 1280x720 - no bars in a
    // window; dragging keeps it): its width follows its height, and SDL holds the ratio while it is dragged.
    // "auto" (0): the window's own shape, free. ZOE's first played session (2026-11-07): 1280x896, side bars.
    // The size first, the ratio after: SDL_SetWindowAspectRatio fits the window inside its current size, so set
    // first it took height (to 16:9) or width (to 4:3) off at every change. The height is the player's (the last
    // they dragged it to), the width from it - narrowed to the screen's usable width, the height then with it,
    // the player's kept for the next change.
    if (const float aspect = aspect_.load(); aspect != window_aspect_ && window_) {
        window_aspect_ = aspect;
        int w = 0, h = 0;
        if (aspect > 0.0f && !Fullscreen() && SDL_GetWindowSize(window_, &w, &h) && h > 0) {
            if (player_height_ <= 0) player_height_ = h;
            int nh = player_height_, nw = int(float(nh) * aspect + 0.5f);
            SDL_Rect usable{};
            const SDL_DisplayID screen = SDL_GetDisplayForWindow(window_);
            if (screen && SDL_GetDisplayUsableBounds(screen, &usable) && usable.w > 0 && nw > usable.w) {
                nw = usable.w;
                nh = int(float(nw) / aspect + 0.5f);
            }
            SDL_SetWindowAspectRatio(window_, 0.0f, 0.0f);
            our_width_ = nw;
            our_height_ = nh;
            SDL_SetWindowSize(window_, nw, nh);
        }
        SDL_SetWindowAspectRatio(window_, aspect, aspect);
    }
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) closed_ = true;
        if (e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) stale_ = true;
        // The player's height: a resize that is not the size ASPECT RATIO last set (Pump, above).
        if (e.type == SDL_EVENT_WINDOW_RESIZED && !Fullscreen() &&
            !(e.window.data1 == our_width_ && e.window.data2 == our_height_))
            player_height_ = e.window.data2;
        if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.scancode == SDL_SCANCODE_RETURN &&
            (e.key.mod & SDL_KMOD_ALT))
            SetFullscreen(!Fullscreen());
        if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.scancode == SDL_SCANCODE_F12) capture_ = true;
        if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.scancode == SDL_SCANCODE_TAB) texture_pack_ = true;
        if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.scancode == SDL_SCANCODE_F1) fps_toggle_ = true;
        // No controller combination for the capture: games use L3 + R3 themselves (God of War II, the maintainer
        // 2026-11-17) - F12 only. The PC settings panel is F9 only (host pc_menu.cpp).
        if (e.type == SDL_EVENT_MOUSE_MOTION) MouseAt(e.motion.x, e.motion.y);
        if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP) {
            MouseAt(e.button.x, e.button.y);
            const int bit = e.button.button == SDL_BUTTON_LEFT ? 0 : e.button.button == SDL_BUTTON_MIDDLE ? 1
                          : e.button.button == SDL_BUTTON_RIGHT ? 2 : -1;
            std::lock_guard<std::mutex> hold(mouse_lock_);
            if (bit >= 0 && e.button.down) {
                mouse_.held |= 1u << bit;
                mouse_.pressed |= 1u << bit;
            } else if (bit >= 0) {
                mouse_.held &= ~(1u << bit);
            }
        }
        if (e.type == SDL_EVENT_MOUSE_WHEEL) {
            std::lock_guard<std::mutex> hold(mouse_lock_);
            mouse_.wheel += e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e.wheel.y : e.wheel.y;
        }
        if (e.type == SDL_EVENT_WINDOW_MOUSE_LEAVE) {
            std::lock_guard<std::mutex> hold(mouse_lock_);
            mouse_.over = false;
            mouse_.held = 0;
        }
        if (e.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
            std::lock_guard<std::mutex> hold(mouse_lock_);
            mouse_.held = 0;
        }
        if (e.type == SDL_EVENT_GAMEPAD_ADDED) {
            std::lock_guard<std::mutex> hold(pads_lock_);
            if (!FindPad(e.gdevice.which)) {
                if (SDL_Gamepad* pad = SDL_OpenGamepad(e.gdevice.which)) {
                    Gamepad g;
                    g.pad = pad;
                    g.id = e.gdevice.which;
                    pads_.push_back(g);
                    std::fprintf(stderr, "[window] controller %u: %s\n", unsigned(g.id), SDL_GetGamepadName(pad));
                }
            }
        }
        if (e.type == SDL_EVENT_GAMEPAD_REMOVED) {
            std::lock_guard<std::mutex> hold(pads_lock_);
            for (auto g = pads_.begin(); g != pads_.end(); ++g)
                if (g->id == e.gdevice.which) {
                    std::fprintf(stderr, "[window] controller %u unplugged\n", unsigned(g->id));
                    SDL_CloseGamepad(g->pad);
                    pads_.erase(g);
                    break;
                }
        }
    }
    UpdateCursor();
    return !closed_;
}

/* The pointer at (x, y), in the window's own units (points): in pixels, then
   across the picture as shown. */
void VulkanDisplay::MouseAt(float x, float y) {
    const float density = window_ ? SDL_GetWindowPixelDensity(window_) : 1.0f;
    const float px = x * (density > 0.0f ? density : 1.0f), py = y * (density > 0.0f ? density : 1.0f);
    std::lock_guard<std::mutex> hold(mouse_lock_);
    if (px != mouse_px_ || py != mouse_py_ || !mouse_.over) mouse_.moved = true;
    mouse_px_ = px;
    mouse_py_ = py;
    mouse_.over = true;
    mouse_.x = (px - float(shown_x0_.load())) / float(shown_w_.load());
    mouse_.y = (py - float(shown_y0_.load())) / float(shown_h_.load());
}

/* The window's pointer: the game's, at a size that goes with the picture's
   (its art's height a twentieth of the picture's), while the game wants it;
   otherwise the system's - hidden in fullscreen. */
void VulkanDisplay::UpdateCursor() {
    if (!window_) return;
    const bool want = cursor_wanted_.load();
    std::uint32_t size = std::uint32_t(float(shown_h_.load()) / 20.0f + 0.5f);
    size = std::clamp<std::uint32_t>(size, 20, 128);
    if (want) {
        std::lock_guard<std::mutex> hold(mouse_lock_);
        if (!cursor_art_.empty() && (!cursor_ || cursor_size_ != size)) {
            // The art resampled to `size` (bilinear, by premultiplied colour).
            const std::uint32_t w = size, h = std::uint32_t(float(size) * float(cursor_height_) /
                                                            float(cursor_width_) + 0.5f);
            SDL_Surface* surface = SDL_CreateSurface(int(w), int(h), SDL_PIXELFORMAT_RGBA32);
            if (surface) {
                for (std::uint32_t y = 0; y < h; ++y) {
                    auto* row = reinterpret_cast<std::uint8_t*>(surface->pixels) + std::size_t(y) * surface->pitch;
                    for (std::uint32_t x = 0; x < w; ++x) {
                        const float sx = (float(x) + 0.5f) * float(cursor_width_) / float(w) - 0.5f;
                        const float sy = (float(y) + 0.5f) * float(cursor_height_) / float(h) - 0.5f;
                        const int x0 = int(std::floor(sx)), y0 = int(std::floor(sy));
                        const float fx = sx - float(x0), fy = sy - float(y0);
                        float sum[4] = {};
                        for (int k = 0; k < 4; ++k) {
                            const int ix = std::clamp(x0 + (k & 1), 0, int(cursor_width_) - 1);
                            const int iy = std::clamp(y0 + (k >> 1), 0, int(cursor_height_) - 1);
                            const float weight = ((k & 1) ? fx : 1.0f - fx) * ((k >> 1) ? fy : 1.0f - fy);
                            const std::uint32_t p = cursor_art_[std::size_t(iy) * cursor_width_ + ix];
                            const float a = float(p >> 24) / 255.0f;
                            sum[0] += weight * float(p & 0xFF) * a;
                            sum[1] += weight * float((p >> 8) & 0xFF) * a;
                            sum[2] += weight * float((p >> 16) & 0xFF) * a;
                            sum[3] += weight * a;
                        }
                        const float a = sum[3];
                        row[4 * x + 0] = std::uint8_t(a > 0.0f ? std::min(255.0f, sum[0] / a + 0.5f) : 0.0f);
                        row[4 * x + 1] = std::uint8_t(a > 0.0f ? std::min(255.0f, sum[1] / a + 0.5f) : 0.0f);
                        row[4 * x + 2] = std::uint8_t(a > 0.0f ? std::min(255.0f, sum[2] / a + 0.5f) : 0.0f);
                        row[4 * x + 3] = std::uint8_t(std::min(255.0f, a * 255.0f + 0.5f));
                    }
                }
                SDL_Cursor* made = SDL_CreateColorCursor(surface, int(float(cursor_hot_x_) * float(w) / float(cursor_width_)),
                                                         int(float(cursor_hot_y_) * float(h) / float(cursor_height_)));
                SDL_DestroySurface(surface);
                if (made) {
                    if (cursor_showing_ == 1) SDL_SetCursor(made);
                    if (cursor_) SDL_DestroyCursor(cursor_);
                    cursor_ = made;
                    cursor_size_ = size;
                    std::fprintf(stderr, "[display] the game's pointer at %ux%u\n", w, h);
                } else {
                    std::fprintf(stderr, "[display] the game's pointer: %s\n", SDL_GetError());
                }
            }
        }
    }
    const int showing = want && cursor_ ? 1 : 0;
    if (showing == cursor_showing_ && !(showing == 0 && fullscreen_cursor_stale_)) return;
    cursor_showing_ = showing;
    fullscreen_cursor_stale_ = false;
    if (showing) {
        SDL_SetCursor(cursor_);
        SDL_ShowCursor();
    } else {
        SDL_SetCursor(SDL_GetDefaultCursor());
        if (Fullscreen()) SDL_HideCursor();
        else SDL_ShowCursor();
    }
}

namespace {
std::uint8_t FamilyOf(SDL_Gamepad* pad) {
    switch (SDL_GetGamepadType(pad)) {
        case SDL_GAMEPAD_TYPE_PS3:
        case SDL_GAMEPAD_TYPE_PS4:
        case SDL_GAMEPAD_TYPE_PS5:
            return Display::kPlayStationController;
        case SDL_GAMEPAD_TYPE_XBOX360:
        case SDL_GAMEPAD_TYPE_XBOXONE:
            return Display::kXboxController;
        default:
            return Display::kOtherController;
    }
}

/* One controller as a DualShock 2 reads. */
Display::PadState ReadPad(SDL_Gamepad* pad) {
    // SDL's buttons by position (south is the DualShock's cross) to
    // DualShock 2 inputs.
    static const std::pair<SDL_GamepadButton, int> kButtons[] = {
        {SDL_GAMEPAD_BUTTON_BACK, 0}, {SDL_GAMEPAD_BUTTON_LEFT_STICK, 1}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK, 2},
        {SDL_GAMEPAD_BUTTON_START, 3}, {SDL_GAMEPAD_BUTTON_DPAD_UP, 4}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 5},
        {SDL_GAMEPAD_BUTTON_DPAD_DOWN, 6}, {SDL_GAMEPAD_BUTTON_DPAD_LEFT, 7},
        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 10}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 11},
        {SDL_GAMEPAD_BUTTON_NORTH, 12}, {SDL_GAMEPAD_BUTTON_EAST, 13}, {SDL_GAMEPAD_BUTTON_SOUTH, 14},
        {SDL_GAMEPAD_BUTTON_WEST, 15}};
    Display::PadState s;
    for (const auto& [button, input] : kButtons)
        if (SDL_GetGamepadButton(pad, button)) s.buttons |= std::uint16_t(1u << input);
    // Triggers (0-32767) are L2 and R2: pressed past a quarter of their travel.
    const int l2 = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
    const int r2 = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    s.l2 = std::uint8_t(std::clamp(l2, 0, 32767) >> 7);
    s.r2 = std::uint8_t(std::clamp(r2, 0, 32767) >> 7);
    if (l2 > 8192) s.buttons |= 1u << 8;
    if (r2 > 8192) s.buttons |= 1u << 9;
    // Sticks: -32768-32767 to 0-255, down and right the larger, as the
    // DualShock's; a small dead zone reads as centred.
    static const SDL_GamepadAxis kAxes[4] = {SDL_GAMEPAD_AXIS_RIGHTX, SDL_GAMEPAD_AXIS_RIGHTY,
                                             SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY};
    for (int i = 0; i < 4; ++i) {
        const int v = SDL_GetGamepadAxis(pad, kAxes[i]);
        s.sticks[i] = (v > -4096 && v < 4096) ? 0x80 : std::uint8_t((v + 32768) >> 8);
    }
    s.family = FamilyOf(pad);
    return s;
}
}  // namespace

bool VulkanDisplay::Pad(PadState* state) const {
    std::lock_guard<std::mutex> hold(pads_lock_);
    if (pads_.empty()) return false;
    *state = ReadPad(pads_.front().pad);
    return true;
}

bool VulkanDisplay::PadOf(std::uint32_t id, PadState* state) const {
    std::lock_guard<std::mutex> hold(pads_lock_);
    const Gamepad* g = FindPad(id);
    if (!g) return false;
    *state = ReadPad(g->pad);
    return true;
}

std::vector<Display::ControllerInfo> VulkanDisplay::Controllers() const {
    std::lock_guard<std::mutex> hold(pads_lock_);
    std::vector<ControllerInfo> list;
    for (const Gamepad& g : pads_) {
        const char* name = SDL_GetGamepadName(g.pad);
        list.push_back({g.id, name ? name : "Controller", FamilyOf(g.pad)});
    }
    return list;
}

/* A DualSense's effects packet (SDL's HIDAPI PS5 driver takes it as the
   controller's output report's effects part: SDL_SendGamepadEffect), the
   triggers' alone: their effects at 10 (right) and 21 (left), 11 bytes each,
   enabled by bits 2 and 3 of the first byte. An effect: its mode, then its
   parameters - 0x05 off; 0x01 resistance (start, force); 0x06 vibration
   (frequency, amplitude, start). */
void VulkanDisplay::SendTriggers(SDL_Gamepad* pad, const TriggerEffect& left, const TriggerEffect& right) {
    if (!pad || SDL_GetGamepadType(pad) != SDL_GAMEPAD_TYPE_PS5) return;
    std::uint8_t packet[47] = {};
    packet[0] = 0x04 | 0x08;
    auto put = [](std::uint8_t* at, const TriggerEffect& t) {
        switch (t.kind) {
            case TriggerEffect::kResistance:
                at[0] = 0x01, at[1] = t.start, at[2] = t.force;
                break;
            case TriggerEffect::kVibration:
                at[0] = 0x06, at[1] = t.frequency, at[2] = t.force, at[3] = t.start;
                break;
            default:
                at[0] = 0x05;
        }
    };
    put(packet + 10, right);
    put(packet + 21, left);
    if (!SDL_SendGamepadEffect(pad, packet, sizeof packet))
        std::fprintf(stderr, "[window] trigger effects: %s\n", SDL_GetError());
}

void VulkanDisplay::ReleaseController() {
    std::lock_guard<std::mutex> hold(pads_lock_);
    for (Gamepad& g : pads_) {
        SendTriggers(g.pad, TriggerEffect{}, TriggerEffect{});
        SDL_RumbleGamepad(g.pad, 0, 0, 0);
    }
}

bool VulkanDisplay::Focused() const {
    return window_ && (SDL_GetWindowFlags(window_) & SDL_WINDOW_INPUT_FOCUS) != 0;
}

bool VulkanDisplay::KeyDown(int scancode) const {
    int n = 0;
    const bool* keys = SDL_GetKeyboardState(&n);
    return keys && scancode >= 0 && scancode < n && keys[scancode];
}

VulkanDisplay::~VulkanDisplay() {
#ifdef __ANDROID__
    SDL_RemoveEventWatch(LifeCycle, this);
#endif
    {
        std::lock_guard<std::mutex> hold(paced_lock_);
        stop_ = true;
    }
    paced_wake_.notify_all();
    if (present_thread_.joinable()) present_thread_.join();
    shared_.reset();
    if (device_) vkDeviceWaitIdle(device_);
    DestroyPaced();
    if (copy_pool_) vkDestroyCommandPool(device_, copy_pool_, nullptr);
    DestroyPicture();
    DestroyPostImages();
    DestroySmaa();
    DestroyFsr();
    if (fxaa_pipeline_) vkDestroyPipeline(device_, fxaa_pipeline_, nullptr);
    if (downsample_pipeline_) vkDestroyPipeline(device_, downsample_pipeline_, nullptr);
    if (post_layout_) vkDestroyPipelineLayout(device_, post_layout_, nullptr);
    if (post_descriptors_) vkDestroyDescriptorPool(device_, post_descriptors_, nullptr);
    if (post_set_layout_) vkDestroyDescriptorSetLayout(device_, post_set_layout_, nullptr);
    if (post_sampler_) vkDestroySampler(device_, post_sampler_, nullptr);
    if (overlay_buffer_) vkDestroyBuffer(device_, overlay_buffer_, nullptr);
    if (overlay_memory_) vkFreeMemory(device_, overlay_memory_, nullptr);
    if (stamps_) vkDestroyQueryPool(device_, stamps_, nullptr);
    DestroyLabel();
    DestroyBloomImages();
    if (bloom_pipeline_) vkDestroyPipeline(device_, bloom_pipeline_, nullptr);
    if (bloom_layout_) vkDestroyPipelineLayout(device_, bloom_layout_, nullptr);
    if (bloom_descriptors_) vkDestroyDescriptorPool(device_, bloom_descriptors_, nullptr);
    if (bloom_set_layout_) vkDestroyDescriptorSetLayout(device_, bloom_set_layout_, nullptr);
    if (bloom_sampler_) vkDestroySampler(device_, bloom_sampler_, nullptr);
    DestroySwapchain();
    if (device_) {
        for (int i = 0; i < kInFlight; ++i) {
            if (fences_[i]) vkDestroyFence(device_, fences_[i], nullptr);
            if (acquire_fences_[i]) vkDestroyFence(device_, acquire_fences_[i], nullptr);
            if (acquireds_[i]) vkDestroySemaphore(device_, acquireds_[i], nullptr);
        }
        if (pool_) vkDestroyCommandPool(device_, pool_, nullptr);
        vkDestroyDevice(device_, nullptr);
    }
    if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
    if (instance_) vkDestroyInstance(instance_, nullptr);
    if (threaded_) {
        // The window and SDL go on the thread that made them (WindowLoop's end).
        window_stop_ = true;
        if (window_thread_.joinable()) window_thread_.join();
        return;
    }
    for (Gamepad& g : pads_) SDL_CloseGamepad(g.pad);
    if (window_) SDL_DestroyWindow(window_);
    SDL_Quit();
}

}  // namespace

std::unique_ptr<Display> CreateVulkanDisplay(const char* title, int width, int height, std::string* error) {
    auto display = std::make_unique<VulkanDisplay>();
    const char* own = std::getenv("PS2HOST_WINDOW_THREAD");
    const bool threaded = !(own && own[0] == '0');
    if (!(threaded ? display->StartOnOwnThread(title, width, height) : display->Init(title, width, height))) {
        if (error) *error = display->error();
        return nullptr;
    }
    return display;
}

}  // namespace ps2gpu
