/*
 * Sound out on Windows (platform.h's RunAudioOutput): the system's audio
 * engine (WASAPI, shared mode), or its wave device (waveOut) where that cannot
 * open. Moved from engine/host/src/audio.cpp unchanged (2026-11-17); the ring,
 * the rate control and the mixing stay there, the same on every system.
 */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#include <mmreg.h>
#include <audioclient.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include "ps2platform/platform.h"

#if defined(_MSC_VER)
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "ole32.lib")
#endif

namespace ps2platform {

namespace {

// ---------------------------------------------------------------- WASAPI

// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, without ksuser.lib.
const GUID kFloatFormat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

template <typename T>
void Release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

// Plays until stopped; false if the device could not be opened at all (the
// caller falls back to waveOut). A device that goes away (headphones pulled)
// is opened again - the default one, whatever it now is.
bool RunWasapi(const AudioOutput& a) {
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return false;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    bool ever = false;
    while (!a.stop->load()) {
        IMMDeviceEnumerator* enumerator = nullptr;
        IMMDevice* device = nullptr;
        IAudioClient* client = nullptr;
        IAudioRenderClient* render = nullptr;
        ISimpleAudioVolume* volume = nullptr;
        bool ok = SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                             __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator))) &&
                  SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)) &&
                  SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                             reinterpret_cast<void**>(&client)));
        UINT32 buffer_frames = 0;
        if (ok) {
            // The rate in float stereo, the system converting to its mix format
            // (another rate, more channels) where that differs.
            WAVEFORMATEXTENSIBLE f = {};
            f.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
            f.Format.nChannels = 2;
            f.Format.nSamplesPerSec = DWORD(a.rate);
            f.Format.wBitsPerSample = 32;
            f.Format.nBlockAlign = 8;
            f.Format.nAvgBytesPerSec = DWORD(a.rate) * 8;
            f.Format.cbSize = sizeof f - sizeof f.Format;
            f.Samples.wValidBitsPerSample = 32;
            f.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
            f.SubFormat = kFloatFormat;
            const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
            ok = SUCCEEDED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 200000 /* 20 ms */, 0,
                                              &f.Format, nullptr)) &&
                 SUCCEEDED(client->SetEventHandle(event)) && SUCCEEDED(client->GetBufferSize(&buffer_frames)) &&
                 SUCCEEDED(client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&render)));
        }
        if (ok) {
            if (SUCCEEDED(client->GetService(__uuidof(ISimpleAudioVolume), reinterpret_cast<void**>(&volume)))) {
                // PS2HOST_AUDIO_RESET_VOLUME=1: this program's volume in the
                // mixer back to full (an earlier build's mute set it to
                // nothing, and the mixer keeps it).
                if (std::getenv("PS2HOST_AUDIO_RESET_VOLUME")) volume->SetMasterVolume(1.0f, nullptr);
                float level = 1;
                volume->GetMasterVolume(&level);
                if (!ever)
                    std::fprintf(stderr, "[audio] playing (WASAPI, %.0f ms buffer, %u ms target); this program's "
                                         "volume in the system's mixer %.0f%%\n",
                                 1000.0 * buffer_frames / a.rate, a.target_ms, 100.0 * level);
            }
            ever = true;
            a.opened->store(1);
            BYTE* out = nullptr;
            if (SUCCEEDED(render->GetBuffer(buffer_frames, &out)))
                render->ReleaseBuffer(buffer_frames, AUDCLNT_BUFFERFLAGS_SILENT);
            ok = SUCCEEDED(client->Start());
        }
        while (ok && !a.stop->load()) {
            WaitForSingleObject(event, 100);
            UINT32 padding = 0;
            if (FAILED(client->GetCurrentPadding(&padding))) break;      // the device went away
            const UINT32 room = buffer_frames - padding;
            if (!room) continue;
            BYTE* out = nullptr;
            if (FAILED(render->GetBuffer(room, &out))) break;
            a.render(a.user, reinterpret_cast<float*>(out), room);
            render->ReleaseBuffer(room, 0);
        }
        if (client) client->Stop();
        Release(volume);
        Release(render);
        Release(client);
        Release(device);
        Release(enumerator);
        if (!ever) break;                   // never opened: waveOut instead
        if (!a.stop->load()) std::this_thread::sleep_for(std::chrono::milliseconds(500));   // a new default device
    }
    CloseHandle(event);
    CoUninitialize();
    return ever;
}

// ---------------------------------------------------------------- waveOut

void RunWaveOut(const AudioOutput& a) {
    constexpr int kBuffers = 4;
    const unsigned frames = unsigned(a.rate / 100);         // 10 ms each
    WAVEFORMATEX format = {};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 2;
    format.nSamplesPerSec = DWORD(a.rate);
    format.wBitsPerSample = 16;
    format.nBlockAlign = 4;
    format.nAvgBytesPerSec = DWORD(a.rate) * 4;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HWAVEOUT out = nullptr;
    if (waveOutOpen(&out, WAVE_MAPPER, &format, DWORD_PTR(event), 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) {
        CloseHandle(event);
        a.opened->store(-1);
        return;
    }
    std::fprintf(stderr, "[audio] playing (waveOut, %u ms target)\n", a.target_ms);
    a.opened->store(1);
    WAVEHDR headers[kBuffers] = {};
    std::vector<short> data[kBuffers];
    std::vector<float> scratch(frames * 2);
    for (int i = 0; i < kBuffers; ++i) {
        data[i].assign(frames * 2, 0);
        headers[i].lpData = reinterpret_cast<LPSTR>(data[i].data());
        headers[i].dwBufferLength = frames * 4;
        headers[i].dwFlags = WHDR_DONE;
    }
    while (!a.stop->load()) {
        for (int i = 0; i < kBuffers; ++i) {
            WAVEHDR& h = headers[i];
            if (!(h.dwFlags & WHDR_DONE)) continue;
            if (h.dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(out, &h, sizeof h);
            a.render(a.user, scratch.data(), frames);
            for (unsigned k = 0; k < frames * 2; ++k)
                data[i][k] = short(std::lrint(std::clamp(scratch[k], -1.0f, 1.0f) * 32767.0f));
            h.dwFlags = 0;
            waveOutPrepareHeader(out, &h, sizeof h);
            waveOutWrite(out, &h, sizeof h);
        }
        WaitForSingleObject(event, 20);
    }
    waveOutReset(out);
    for (WAVEHDR& h : headers)
        if (h.dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(out, &h, sizeof h);
    waveOutClose(out);
    CloseHandle(event);
}

}  // namespace

void RunAudioOutput(const AudioOutput& output) {
    if (output.wave_only || !RunWasapi(output)) RunWaveOut(output);
}

}  // namespace ps2platform
