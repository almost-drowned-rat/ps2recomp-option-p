/*
 * Sound out through SDL3 (platform.h's RunAudioOutput) on the systems without
 * a file of their own (Linux: PipeWire, PulseAudio or ALSA, whichever SDL
 * finds). As src/audio_win32.cpp's WASAPI loop renders what the device asks
 * for when its event fires, SDL's stream asks here (its get callback, on SDL's
 * audio thread): exactly the frames the device is about to play, so nothing
 * runs dry between the device's (larger) periods and nothing piles up. The
 * calling thread waits until stop; the ring, the rate control and the mixing
 * stay in engine/host's audio.cpp.
 */

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include "ps2platform/platform.h"

namespace ps2platform {

namespace {

struct Pull {
    const AudioOutput* out;
    std::vector<float> scratch;
};

void SDLCALL Wanted(void* user, SDL_AudioStream* stream, int additional, int) {
    auto* p = static_cast<Pull*>(user);
    const int frame_bytes = int(sizeof(float) * 2);
    int frames = additional / frame_bytes;
    while (frames > 0) {
        const unsigned n = unsigned(std::min<std::size_t>(std::size_t(frames), p->scratch.size() / 2));
        p->out->render(p->out->user, p->scratch.data(), n);
        SDL_PutAudioStreamData(stream, p->scratch.data(), int(n) * frame_bytes);
        frames -= int(n);
    }
}

}  // namespace

void RunAudioOutput(const AudioOutput& a) {
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        std::fprintf(stderr, "[audio] SDL audio: %s\n", SDL_GetError());
        a.opened->store(-1);
        return;
    }
    Pull pull{&a, std::vector<float>(std::size_t(a.rate / 50) * 2)};      // 20 ms at a time at most
    bool ever = false;
    while (!a.stop->load()) {
        SDL_AudioSpec spec{SDL_AUDIO_F32, 2, a.rate};
        SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, Wanted, &pull);
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
            SDL_AudioSpec device{};
            int period = 0;
            SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(stream), &device, &period);
            const char* driver = SDL_GetCurrentAudioDriver();
            std::fprintf(stderr, "[audio] playing (SDL %s, device %d Hz, %d-frame periods = %.0f ms, %u ms target)\n",
                         driver ? driver : "?", device.freq, period,
                         device.freq ? 1000.0 * period / device.freq : 0.0, a.target_ms);
        }
        ever = true;
        a.opened->store(1);
        SDL_ResumeAudioStreamDevice(stream);
        // (SDL moves the stream to a new default device by itself - headphones plugged in or pulled)
        while (!a.stop->load()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        SDL_DestroyAudioStream(stream);
    }
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

}  // namespace ps2platform
