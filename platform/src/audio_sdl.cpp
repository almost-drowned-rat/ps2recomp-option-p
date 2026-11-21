/*
 * Sound out through SDL3 (platform.h's RunAudioOutput) on the systems without
 * a file of their own (Linux: PipeWire, PulseAudio or ALSA, whichever SDL
 * finds). The same pull as src/audio_win32.cpp's WASAPI loop: the calling
 * thread renders what the device has room for, a 20 ms buffer as WASAPI's;
 * the ring, the rate control and the mixing stay in engine/host's audio.cpp.
 */

#include <SDL3/SDL.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include "ps2platform/platform.h"

namespace ps2platform {

void RunAudioOutput(const AudioOutput& a) {
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        std::fprintf(stderr, "[audio] SDL audio: %s\n", SDL_GetError());
        a.opened->store(-1);
        return;
    }
    const unsigned buffer_frames = unsigned(a.rate / 50);   // 20 ms, WASAPI's buffer
    const unsigned chunk = unsigned(a.rate / 400);          // rendered 2.5 ms at a time
    std::vector<float> scratch(std::size_t(chunk) * 2);
    bool ever = false;
    while (!a.stop->load()) {
        SDL_AudioSpec spec{SDL_AUDIO_F32, 2, a.rate};
        SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (!stream) {
            if (!ever) {
                std::fprintf(stderr, "[audio] no sound device: %s\n", SDL_GetError());
                a.opened->store(-1);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }
        if (!ever) {
            const char* driver = SDL_GetCurrentAudioDriver();
            std::fprintf(stderr, "[audio] playing (SDL %s, %.0f ms buffer, %u ms target)\n", driver ? driver : "?",
                         1000.0 * buffer_frames / a.rate, a.target_ms);
        }
        ever = true;
        a.opened->store(1);
        SDL_ResumeAudioStreamDevice(stream);
        const int frame_bytes = int(sizeof(float) * 2);
        bool lost = false;
        while (!a.stop->load()) {
            const int queued = SDL_GetAudioStreamQueued(stream);
            if (queued < 0) {               // the device went away
                lost = true;
                break;
            }
            if (unsigned(queued / frame_bytes) + chunk <= buffer_frames) {
                a.render(a.user, scratch.data(), chunk);
                if (!SDL_PutAudioStreamData(stream, scratch.data(), int(chunk) * frame_bytes)) {
                    lost = true;
                    break;
                }
                continue;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(1000));
        }
        SDL_DestroyAudioStream(stream);
        if (!lost) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));       // a new default device
    }
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

}  // namespace ps2platform
