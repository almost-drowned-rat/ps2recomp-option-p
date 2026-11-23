/* The Vulkan objects behind ps2gpu::Gpu, for the library's own sources. */

#ifndef PS2GPU_GPU_INTERNAL_H
#define PS2GPU_GPU_INTERNAL_H

#include <volk.h>

#include <cstdlib>
#ifndef _WIN32
#include <dlfcn.h>
#endif
/* volkInitialize, or - SDL_VULKAN_LIBRARY set (Android: the app's driver shim, which picks the system driver or the
   player's Turnip) - that library's vkGetInstanceProcAddr, so the game and SDL's surface use the same Vulkan. */
inline VkResult LoadVulkan() {
#ifndef _WIN32
    if (const char* lib = std::getenv("SDL_VULKAN_LIBRARY"); lib && *lib) {
        if (void* h = dlopen(lib, RTLD_NOW | RTLD_LOCAL)) {
            if (auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(h, "vkGetInstanceProcAddr"))) {
                volkInitializeCustom(gipa);
                return VK_SUCCESS;
            }
        }
    }
#endif
    return volkInitialize();
}

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <string>
#include <utility>
#include <vector>

#include "ps2gpu/gs_renderer.h"

namespace ps2gpu {

/* SubmitStats: submissions, the time spent waiting for the GPU, and what
   waited (a read-back of which kind, a resize, a full ring). */
inline std::uint64_t g_submits = 0;
/* The one queue the renderer and the display share: Vulkan wants its
   submissions, presents and the device's wait-idle from one thread at a
   time, and the display presents from a thread of its own. */
inline std::mutex g_queue_lock;
/* The renderer's waits for g_queue_lock (its submits; the present thread may hold it), summed - the GPU thread log's. */
inline std::atomic<std::uint64_t> g_queue_wait_ns{0};
/* ...and its waits for the GPU (a recording slot's fence: the ring full, or a sync), summed. */
inline std::atomic<std::uint64_t> g_gpu_wait_ns{0};
/* VK_EXT_extended_dynamic_state3's colour blending - enable, equation, write mask - as dynamic state: the GS
   renderer's pipelines then differ by shader only, not by each draw's blending (agent L, 2026-11-22: ZOE's boss
   fight bound a pipeline every third draw, the alpha-blended and opaque passes taking turns; a bind on Turnip
   copies its program state, ~20 % of the renderer's thread). Taken where the device offers all three, unless
   PS2HOST_GPU_DYNAMIC_BLEND=0; set by whoever makes the device (the window's, or VulkanGpu's own). */
inline std::atomic<bool> g_dynamic_blend{false};
/* The device's offer: true with `f3` filled in (all three on) and the extension's name added to `extensions` - the
   caller chains `f3` into its features and enables them, then sets g_dynamic_blend. */
inline bool OfferDynamicBlend(VkPhysicalDevice gpu, VkPhysicalDeviceExtendedDynamicState3FeaturesEXT* f3,
                              std::vector<const char*>* extensions) {
    if (const char* e = std::getenv("PS2HOST_GPU_DYNAMIC_BLEND"); e && e[0] == '0') return false;
    std::uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> offered(count);
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &count, offered.data());
    bool has = false;
    for (const auto& e : offered) has = has || std::string(e.extensionName) == VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME;
    if (!has) return false;
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &query;
    vkGetPhysicalDeviceFeatures2(gpu, &f2);
    if (!query.extendedDynamicState3ColorBlendEnable || !query.extendedDynamicState3ColorBlendEquation ||
        !query.extendedDynamicState3ColorWriteMask)
        return false;
    *f3 = VkPhysicalDeviceExtendedDynamicState3FeaturesEXT{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
    f3->extendedDynamicState3ColorBlendEnable = VK_TRUE;
    f3->extendedDynamicState3ColorBlendEquation = VK_TRUE;
    f3->extendedDynamicState3ColorWriteMask = VK_TRUE;
    extensions->push_back(VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME);
    return true;
}
inline double g_wait_seconds = 0;
inline GpuWaitObserver g_wait_observer = nullptr;      // SetGpuWaitObserver
/* The GPU's time on the command buffers handed over, in all (timestamps at
   each one's start and end; GpuBusyNanoseconds). */
inline std::atomic<std::uint64_t> g_gpu_ns{0};
/* PS2HOST_GPU_PROFILE: the display's own GPU time, apart from the renderer's (its
   presents' command buffers: clear, AA / FSR, blit, overlay; and the paced copies). */
inline std::atomic<std::uint64_t> g_display_present_ns{0}, g_display_copy_ns{0}, g_display_presents{0};
/* PS2HOST_GPU_PROFILE: the GPU's time by what it did (GpuProfileReport). */
enum ProfileKind { kProfOther, kProfDraw, kProfSwizzle, kProfLinear, kProfMemory, kProfFeedback, kProfDecode,
                   kProfUpload, kProfClear, kProfDate, kProfReadback, kProfKinds };
inline std::atomic<std::uint64_t> g_profile_ns[kProfKinds] = {};
/* PS2HOST_GPU_PROFILE: the draws' time by their tag too (MarkDraw: the
   renderer's - its target and what kind of draw), for the report's list of
   the dearest. Only where the slots are retired, under the lock. */
inline std::mutex g_draw_ns_lock;
inline std::unordered_map<std::uint32_t, std::uint64_t> g_draw_ns;
struct WaitReason { const char* why; std::uint64_t count; double seconds; };
inline WaitReason g_waits[16] = {};
inline void CountWait(const char* why, double seconds) {
    for (WaitReason& w : g_waits) {
        if (!w.why) w.why = why;
        if (w.why == why || std::string(w.why) == why) {
            ++w.count;
            w.seconds += seconds;
            return;
        }
    }
}

class VulkanGpu final : public Gpu {
public:
    ~VulkanGpu() override;
    bool Init(std::string* error);
    /* On a device someone else made and keeps (a Display's): only the command
       pool, command buffers and fences are this one's own. */
    bool InitShared(VkInstance instance, VkPhysicalDevice gpu, VkDevice device, std::uint32_t family, VkQueue queue,
                    bool dual_source, const std::string& describe, std::string* error);
    std::string Describe() const override { return describe_; }
    /* samplerAnisotropy (enabled where supported) and the device's limit into sampler_anisotropy, max_anisotropy. */
    void NoteAnisotropy(VkPhysicalDevice physical, const VkPhysicalDeviceFeatures& supported);

    std::uint32_t FindMemory(std::uint32_t bits, VkMemoryPropertyFlags wanted) const;
    /* Records into the open command buffer (opening one if none is), to run
       at the next Submit: a frame's work goes to the GPU in one submission.
       A rendering pass left open (RecordInPass) is ended first. */
    template <typename F>
    void Record(F&& record, int line = __builtin_LINE());
    /* The same, leaving a rendering pass a previous call left open: for draws
       that continue it (end_pass says how to end one). */
    template <typename F>
    void RecordInPass(F&& record);
    std::function<void(VkCommandBuffer)> end_pass;
    /* Runs what has been recorded and waits for it, and for everything
       Flush handed over before; then what was waiting for that (AfterSubmit:
       freeing staging buffers, say) runs. */
    bool Submit(const char* why = "other");
    /* Hands what has been recorded to the GPU without waiting for it, so that
       the GPU works while the CPU goes on: the next recording goes into the
       next of kSlots command buffers (waiting only if that one is still
       running). What waited on the work handed over runs once it has run. */
    bool Flush();
    void AfterSubmit(std::function<void()> done) { after_submit_.push_back(std::move(done)); }
    bool recording() const { return open_; }
    /* PS2HOST_GPU_PROFILE: the GPU's time since the last mark (or the command
       buffer's start) counted as `kind` - a timestamp as the work before it
       ends. */
    void Mark(VkCommandBuffer c, int kind) {
        if (!profile_ || marks_[current_].size() >= kMarks) return;
        vkCmdWriteTimestamp(c, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, profile_,
                            std::uint32_t(current_) * kMarks + std::uint32_t(marks_[current_].size()));
        marks_[current_].push_back(kind);
        tags_[current_].push_back(0);
    }
    /* A draw's mark with its tag. */
    void MarkDraw(VkCommandBuffer c, std::uint32_t tag) {
        if (!profile_ || marks_[current_].size() >= kMarks) return;
        Mark(c, kProfDraw);
        tags_[current_].back() = tag;
    }
    /* Counts command buffers begun: what was set in one is not in the next. */
    std::uint64_t recording_id() const { return recordings_; }
    /* The recording the next recorded work goes into: the open one, or the one the next Record begins - for what
       is placed for it ahead (the GS renderer's CLUT ring, filled as primitives arrive): recording_id() is the
       next one's only once it has begun (agent L, 2026-11-22: what was placed before a frame's first recording
       began was counted for the recording before, and the new one's own entries wrote over it). */
    std::uint64_t filling_id() const { return open_ ? recordings_ : recordings_ + 1; }
    static constexpr int kSlots = 3;
    /* The command buffer recording goes into (or will). */
    int slot() const { return current_; }
    /* Records with `record`, submits, and waits for it to finish. */
    template <typename F>
    bool Run(F&& record, const char* why = "other") {
        Record(std::forward<F>(record));
        return Submit(why);
    }

    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    std::uint32_t family = 0;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;           /* the current slot's */
    VkCommandBuffer cmds[kSlots] = {};
    VkFence fences[kSlots] = {};
    bool dual_source = false;       /* dualSrcBlend: the GS's blending */
    bool logic_op = false;          /* logicOp: FBMSK's partly masked channels */
    bool bc = false;                /* textureCompressionBC: texture packs' DDS uploaded as they are */
    /* samplerAnisotropy and its limit: TEXTURE FILTERING FAST's anisotropic filter in the GPU's sampler */
    bool sampler_anisotropy = false;
    float max_anisotropy = 1.0f;
    bool owns_device = true;

private:
    std::string describe_;
    bool open_ = false;
    std::uint64_t recordings_ = 0;
    // Two timestamps a slot: as its command buffer starts and as it ends.
    VkQueryPool timestamps_ = VK_NULL_HANDLE;
    double timestamp_ns_ = 0;               // a tick, in nanoseconds
    bool stamped_[kSlots] = {};
    void MakeTimestamps();
    // (8,192 cut an explosion picture's profile short: one command buffer holds ~10,000 draws' marks)
    static constexpr std::uint32_t kMarks = 65536;
    VkQueryPool profile_ = VK_NULL_HANDLE;
    std::vector<int> marks_[kSlots];
    std::vector<std::uint32_t> tags_[kSlots];
    std::vector<std::function<void()>> after_submit_;
    int current_ = 0;
    bool in_flight_[kSlots] = {};
    std::vector<std::function<void()>> slot_done_[kSlots];
    /* Waits for a slot's work, if handed over, and runs what waited on it;
       the time it waited goes to `why`. */
    bool Retire(int slot, const char* why);
};

template <typename F>
void VulkanGpu::RecordInPass(F&& record) {
    if (!open_) {
        vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &begin);
        open_ = true;
        ++recordings_;
        if (timestamps_) {
            vkCmdResetQueryPool(cmd, timestamps_, 2 * std::uint32_t(current_), 2);
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timestamps_, 2 * std::uint32_t(current_));
        }
        if (profile_) {
            vkCmdResetQueryPool(cmd, profile_, std::uint32_t(current_) * kMarks, kMarks);
            marks_[current_].clear();
            tags_[current_].clear();
            Mark(cmd, kProfOther);
        }
    }
    record(cmd);
}

// The source line of the Record call that last ended a render pass (counters: which work ends passes).
inline int g_record_line = 0;

template <typename F>
void VulkanGpu::Record(F&& record, int line) {
    g_record_line = line;
    RecordInPass([&](VkCommandBuffer c) {
        if (end_pass) end_pass(c);
        record(c);
    });
}

inline bool VulkanGpu::Retire(int slot, const char* why) {
    if (!in_flight_[slot]) return true;
    const auto start = std::chrono::steady_clock::now();
    const auto fence_waiting = std::chrono::steady_clock::now();
    const bool ok = vkWaitForFences(device, 1, &fences[slot], VK_TRUE, UINT64_MAX) == VK_SUCCESS;
    g_gpu_wait_ns.fetch_add(std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - fence_waiting).count()), std::memory_order_relaxed);
    const auto end = std::chrono::steady_clock::now();
    const double waited = std::chrono::duration<double>(end - start).count();
    g_wait_seconds += waited;
    if (g_wait_observer && waited > 0.0001)
        g_wait_observer(why, std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(start.time_since_epoch()).count()),
                        std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(end.time_since_epoch()).count()));
    CountWait(why, waited);
    in_flight_[slot] = false;
    if (stamped_[slot]) {
        std::uint64_t ticks[2] = {};
        if (vkGetQueryPoolResults(device, timestamps_, 2 * std::uint32_t(slot), 2, sizeof ticks, ticks,
                                  sizeof ticks[0], VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && ticks[1] > ticks[0])
            g_gpu_ns += std::uint64_t(double(ticks[1] - ticks[0]) * timestamp_ns_);
        stamped_[slot] = false;
    }
    if (profile_ && marks_[slot].size() > 1) {
        std::vector<std::uint64_t> ticks(marks_[slot].size());
        if (vkGetQueryPoolResults(device, profile_, std::uint32_t(slot) * kMarks, std::uint32_t(ticks.size()),
                                  ticks.size() * sizeof ticks[0], ticks.data(), sizeof ticks[0],
                                  VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
            for (std::size_t i = 1; i < ticks.size(); ++i)
                if (ticks[i] > ticks[i - 1]) {
                    const std::uint64_t ns = std::uint64_t(double(ticks[i] - ticks[i - 1]) * timestamp_ns_);
                    g_profile_ns[marks_[slot][i]] += ns;
                    if (marks_[slot][i] == kProfDraw) {
                        std::lock_guard<std::mutex> hold(g_draw_ns_lock);
                        g_draw_ns[tags_[slot][i]] += ns;
                    }
                }
        marks_[slot].clear();
        tags_[slot].clear();
    }
    std::vector<std::function<void()>> done;
    done.swap(slot_done_[slot]);
    for (auto& f : done) f();
    return ok;
}

inline bool VulkanGpu::Flush() {
    if (!open_) return true;
    if (end_pass) end_pass(cmd);
    Mark(cmd, kProfOther);
    open_ = false;
    if (timestamps_) {
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestamps_, 2 * std::uint32_t(current_) + 1);
        stamped_[current_] = true;
    }
    vkEndCommandBuffer(cmd);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vkResetFences(device, 1, &fences[current_]);
    bool ok;
    {
        const auto waiting = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> hold(g_queue_lock);
        g_queue_wait_ns.fetch_add(std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                      std::chrono::steady_clock::now() - waiting).count()), std::memory_order_relaxed);
        ok = vkQueueSubmit(queue, 1, &submit, fences[current_]) == VK_SUCCESS;
    }
    ++g_submits;
    in_flight_[current_] = ok;
    slot_done_[current_].swap(after_submit_);
    if (!ok) {                                      // nothing will run: nothing to wait for
        for (auto& f : slot_done_[current_]) f();
        slot_done_[current_].clear();
    }
    current_ = (current_ + 1) % kSlots;
    cmd = cmds[current_];
    return Retire(current_, "a full ring") && ok;
}

inline bool VulkanGpu::Submit(const char* why) {
    bool ok = Flush();
    // Oldest first: current_ is free, the one after it was handed over first.
    for (int i = 1; i <= kSlots; ++i) ok = Retire((current_ + i) % kSlots, why) && ok;
    std::vector<std::function<void()>> done;        // waited on nothing recorded
    done.swap(after_submit_);
    for (auto& f : done) f();
    return ok;
}

}  // namespace ps2gpu

#endif  /* PS2GPU_GPU_INTERNAL_H */
