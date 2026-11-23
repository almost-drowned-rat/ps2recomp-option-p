/*
 * Where the host's time goes, for the report: a few sections timed with the
 * steady clock, exclusively (a section entered inside another pauses the
 * outer one, so VU1's time does not include the GS drawing its XGKICKs cause).
 * Cheap enough to stay on: a clock read on entry and exit.
 */

#ifndef PS2HOST_PROFILE_H
#define PS2HOST_PROFILE_H

#include <string>
#include <vector>

namespace ps2host {

enum class Section { GsDraw, GsUpload, Vu1, Vu0, GpuBridge, GpuRender, GpuSync, Dma, Iop, Spu2, GsWait, Count };

/* Whether sections are timed: always on a PC; on Android only for a measured run (PS2HOST_PROFILE or
   PS2HOST_TIMED set) - its thread-local bookkeeping and two clock reads a section were ~4.5 % of the GS thread on an
   Odin 2, the thread that held the frame back there (agent L, 2026-11-21). */
extern const bool g_timed;

class Timed {
public:
    explicit Timed(Section section) : section_(section) {
        if (g_timed) Enter();
    }
    ~Timed() {
        if (g_timed) Leave();
    }
    Timed(const Timed&) = delete;
    Timed& operator=(const Timed&) = delete;

private:
    void Enter();
    void Leave();
    Section section_;
};

/* "GS drawing 61.2%, ..." of the run's wall time so far. */
std::string ProfileSummary();

/* --sample: a sampling profiler on the calling thread (the EE's, which runs
   the translated code, the host's devices and the GPU bridge): a thousand
   times a second, the function it is in and seven callers. At the
   end, the functions most often found running ("self"), and those most
   often on the stack at all ("total"), by name from the executable's
   symbols. Windows only (elsewhere it reports nothing). */
void StartSampler();
/* --sample-gs: the sampler on the GS thread instead (gs_thread.cpp starts it
   there), where the GS model and the GPU bridge run. */
void SetSampleGsThread(bool on);
bool SampleGsThread();
/* --sample-vif1: on VIF1's worker (real time), where VU1 runs. */
void SetSampleVif1Thread(bool on);
bool SampleVif1Thread();
/* --sample-render: on the threaded GPU renderer's thread (--gpu), where Vulkan records the draws the GS thread
   hands over - what the GS thread waits for when its queue is full. */
void SetSampleRenderThread(bool on);
bool SampleRenderThread();
/* A host crash (an access violation and the like, on any thread): its code,
   address and the stack by name, to stderr, before the process ends. */
void InstallCrashReport();
/* A program to start after the crash's lines are written (Windows: a command line - crash_report.cpp's helper);
   "" none. */
void SetCrashCommand(const wchar_t* command);
/* The same elsewhere (Linux): the helper's arguments, argv[0] the program; empty none. */
void SetCrashHelper(const std::vector<std::string>& argv);
/* --sample-from N: no samples until frame N (devices.cpp calls this each frame). */
void SetSampleFrom(unsigned long long frame);
void SampleFrame(unsigned long long frame);
/* The report counts only samples taken within the spans given (steady clock
   ns): with PS2HOST_HITCH, the times a vertical blank waited for the EE to
   take it. None given, it counts them all. On the EE's thread the report
   gives the guest's call stacks too. */
void SampleOnly(unsigned long long from_ns, unsigned long long to_ns);
std::string SampleReport(int top = 30);

}  // namespace ps2host

#endif  /* PS2HOST_PROFILE_H */
