/* The optional, hidden boot animation (boot_video.h). After the Xbox toolkit's src/bootvideo/boot_video.c
   (option-c); for the PS2 ports boot.mpg sits in the game's own folder and nothing in the menus switches it
   (maintainer 2026-11-17). The pictures go to the port's window (Display::Present). */

#include "ps2host/boot_video.h"

#include <cstdio>                           // before pl_mpeg.h: it names FILE
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <vector>

#include "ps2platform/platform.h"
#include "ps2host/run.h"
#include "ps2host/settings.h"

#ifdef PS2HOST_VULKAN
#include <SDL3/SDL.h>
#include "ps2gpu/display.h"
#define PL_MPEG_IMPLEMENTATION
#include "pl_mpeg/pl_mpeg.h"
#endif

namespace ps2host {

namespace {

// The game's own folder (the executable's): boot.mpg is looked for there (PS2 ports, 2026-11-17: no media folder).
std::filesystem::path RootDir() { return ps2platform::ExeDir(); }

#ifdef PS2HOST_VULKAN
constexpr std::size_t kChunk = 64 * 1024;

struct Playing {
    SDL_IOStream* io = nullptr;
    bool eof = false;
    std::vector<std::uint8_t> chunk = std::vector<std::uint8_t>(kChunk);
    std::vector<std::uint8_t> pixels;       // the frame as B8G8R8A8 for the window
    int width = 0, height = 0;
    std::vector<std::uint32_t> canvas;      // the frame letterboxed in the game's shape (empty: as it is)
    int canvas_height = 0;
    ps2gpu::Display* display = nullptr;
    SDL_AudioStream* audio = nullptr;
    bool stop = false;
};

// pl_mpeg asks for more: the next chunk of the file (SDL_IOStream reads UTF-8 paths everywhere).
void Load(plm_buffer_t* buffer, void* user) {
    Playing& p = *static_cast<Playing*>(user);
    if (p.eof) return;
    const std::size_t read = SDL_ReadIO(p.io, p.chunk.data(), p.chunk.size());
    if (read > 0) plm_buffer_write(buffer, p.chunk.data(), read);
    if (read < p.chunk.size()) {
        p.eof = true;
        plm_buffer_signal_end(buffer);
    }
}

// The probe at the start seeks back (plm_demux_probe): the buffer reads again from there.
void Seek(plm_buffer_t*, std::size_t offset, void* user) {
    Playing& p = *static_cast<Playing*>(user);
    SDL_SeekIO(p.io, Sint64(offset), SDL_IO_SEEK_SET);
    p.eof = false;
}

std::size_t Tell(plm_buffer_t*, void* user) {
    const Sint64 at = SDL_TellIO(static_cast<Playing*>(user)->io);
    return at > 0 ? std::size_t(at) : 0;
}

void Video(plm_t*, plm_frame_t* frame, void* user) {
    Playing& p = *static_cast<Playing*>(user);
    if (p.stop || int(frame->width) != p.width || int(frame->height) != p.height) return;
    plm_frame_to_bgra(frame, p.pixels.data(), p.width * 4);
    const std::uint32_t* shown = reinterpret_cast<const std::uint32_t*>(p.pixels.data());
    int shown_h = p.height;
    if (!p.canvas.empty()) {
        // into the middle of the canvas, its bars staying black
        const std::size_t top = std::size_t((p.canvas_height - p.height) / 2) * std::size_t(p.width);
        std::memcpy(p.canvas.data() + top, p.pixels.data(), p.pixels.size());
        for (std::size_t i = top; i < top + std::size_t(p.width) * std::size_t(p.height); ++i)
            p.canvas[i] |= 0xFF000000u;
        shown = p.canvas.data();
        shown_h = p.canvas_height;
    }
    if (!p.display->Present(shown, std::uint32_t(p.width), std::uint32_t(shown_h)) || !p.display->Pump())
        p.stop = true;                      // the window was closed
}

void Audio(plm_t*, plm_samples_t* samples, void* user) {
    Playing& p = *static_cast<Playing*>(user);
    if (p.audio && !p.stop)
        SDL_PutAudioStreamData(p.audio, samples->interleaved, int(samples->count * 2 * sizeof(float)));
}
#endif

}  // namespace

BootVideoState FindBootVideo(std::string* path) {
    std::error_code error;
    const std::filesystem::path root = RootDir();
    if (std::filesystem::is_regular_file(root / "boot.mpg", error)) {
        if (path) *path = (root / "boot.mpg").string();
        return BootVideoState::kFound;
    }
    if (std::filesystem::is_regular_file(root / "boot.mp4", error) ||
        std::filesystem::is_regular_file(root / "boot.mkv", error))
        return BootVideoState::kConvert;
    return BootVideoState::kNone;
}

bool PlayBootVideo() {
#ifdef PS2HOST_VULKAN
    std::string path;
    // No setting: the file there is the switch (the maintainer 2026-11-17 - no BOOT ANIMATION line for PS2 ports).
    const BootVideoState state = FindBootVideo(&path);
    if (state == BootVideoState::kConvert)
        std::fprintf(stderr, "[boot] boot.mp4 or boot.mkv but no boot.mpg beside the game - convert it (README)\n");
    if (state != BootVideoState::kFound) return false;
    ps2gpu::Display* display = WindowDisplay();
    if (!display) return false;
    Playing p;
    p.display = display;
    if (!(p.io = SDL_IOFromFile(path.c_str(), "rb"))) {
        std::fprintf(stderr, "[boot] cannot open %s (%s)\n", path.c_str(), SDL_GetError());
        return false;
    }
    // A file-mode buffer on the SDL stream (load / seek / tell), set up before plm_create_with_buffer: it reads
    // the headers at once.
    const Sint64 size = SDL_GetIOSize(p.io);
    plm_buffer_t* buffer = plm_buffer_create_with_callbacks(Load, Seek, Tell, size > 0 ? std::size_t(size) : 0, &p);
    plm_t* plm = buffer ? plm_create_with_buffer(buffer, 1) : nullptr;
    if (!plm) {
        if (buffer) plm_buffer_destroy(buffer);
        SDL_CloseIO(p.io);
        return false;
    }
    if (!plm_probe(plm, 5000 * 1024) || plm_get_num_video_streams(plm) < 1 || plm_get_width(plm) <= 0 ||
        plm_get_height(plm) <= 0) {
        std::fprintf(stderr, "[boot] %s has no MPEG-1 video stream - not played (convert it as the README says)\n",
                     path.c_str());
        plm_destroy(plm);
        SDL_CloseIO(p.io);
        return false;
    }
    p.width = plm_get_width(plm);
    p.height = plm_get_height(plm);
    p.pixels.resize(std::size_t(p.width) * std::size_t(p.height) * 4);
    plm_set_video_decode_callback(plm, Video, &p);
    bool audio_init = false;
    if (plm_get_num_audio_streams(plm) > 0 && plm_get_samplerate(plm) > 0) {
        SDL_AudioSpec spec{SDL_AUDIO_F32, 2, plm_get_samplerate(plm)};
        if ((audio_init = SDL_InitSubSystem(SDL_INIT_AUDIO)) &&
            (p.audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr))) {
            plm_set_audio_enabled(plm, 1);
            plm_set_audio_stream(plm, 0);
            plm_set_audio_decode_callback(plm, Audio, &p);
            plm_set_audio_lead_time(plm, 0.05);     // decoded this far ahead of the picture: the device's buffer
            SDL_ResumeAudioStreamDevice(p.audio);
        } else {
            std::fprintf(stderr, "[boot] no audio output (%s) - picture only\n", SDL_GetError());
            plm_set_audio_enabled(plm, 0);
        }
    } else {
        plm_set_audio_enabled(plm, 0);
    }
    std::fprintf(stderr, "[boot] %s: %dx%d, %.2f fps, audio %s, %.1f s\n", path.c_str(), p.width, p.height,
                 plm_get_framerate(plm), p.audio ? "on" : "none", plm_get_duration(plm));
    // In its own shape: a 16:9 video letterboxed in the game's 4:3 picture (black above and below), not squeezed.
    // On a canvas in the game's shape (the "aspect" setting), so the window keeps its own (the display's
    // SetAspect would reshape the window).
    {
        float aw = 4.0f, ah = 3.0f;
        const std::string aspect = Setting("aspect", "4:3");
        if (aspect == "auto" || std::sscanf(aspect.c_str(), "%f:%f", &aw, &ah) != 2 || aw <= 0.0f || ah <= 0.0f)
            aw = float(p.width), ah = float(p.height);
        const int canvas_h = int(float(p.width) * ah / aw + 0.5f);
        if (canvas_h > p.height) {
            p.canvas_height = canvas_h;
            p.canvas.assign(std::size_t(p.width) * std::size_t(canvas_h), 0xFF000000u);
        }
    }
    // Paced by the clock: pl_mpeg decodes what the elapsed time asks for (picture and sound in step).
    const Uint64 started = SDL_GetTicksNS();
    Uint64 last = started;
    while (!p.stop && !plm_has_ended(plm)) {
        const Uint64 now = SDL_GetTicksNS();
        double dt = double(now - last) / 1e9;
        last = now;
        if (dt > 0.25) dt = 0.25;           // a stall: catch up, never race through
        plm_decode(plm, dt);
        if (!p.display->Pump()) p.stop = true;
        SDL_DelayNS(2 * 1000 * 1000);
    }
    if (p.audio) {
        for (int i = 0; !p.stop && i < 100 && SDL_GetAudioStreamQueued(p.audio) > 0; ++i) SDL_Delay(5);
        SDL_DestroyAudioStream(p.audio);
    }
    if (audio_init) SDL_QuitSubSystem(SDL_INIT_AUDIO);
    std::fprintf(stderr, "[boot] %s after %.1f s\n", p.stop ? "stopped (window closed)" : "played",
                 double(SDL_GetTicksNS() - started) / 1e9);
    plm_destroy(plm);
    SDL_CloseIO(p.io);
    return !p.stop;
#else
    return false;
#endif
}

}  // namespace ps2host
