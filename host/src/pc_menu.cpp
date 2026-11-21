/*
 * The PC settings panel: F9 over any game - for debugging (the player's settings are the port's RECOMP OPTIONS
 * page in the game's own menu; no controller combination opens this panel, the maintainer 2026-11-20).
 *
 * A port can put its PC settings in the game's own menus (Burnout 3's main
 * menu has PC SETTINGS); this panel is the engine's, for every port: the
 * settings the host itself applies, changed while the game runs and saved
 * with the PC settings (settings.h). While it is open the game gets no input.
 *
 *   Up / Down          choose
 *   Left / Right       change (Enter or Cross too)
 *   F9, Esc, Circle    close
 */

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "ps2host/input_bind.h"
#include "ps2host/run.h"
#include "ps2host/settings.h"

#include "ps2platform/keys.h"

namespace ps2host {

namespace key = ps2platform::key;

namespace {

bool g_open = false;
bool g_port_controls = false;       // BUTTON MAPPING open alone in a port's look (OpenPcMenuControls + SetControlsLook)
// A port's own look for BUTTON MAPPING (SetControlsLook): it draws the page's lines.
void (*g_controls_look)(bool open, const std::vector<ControlsLine>& lines, int row) = nullptr;
bool g_releasing = false;                // closed, the game's input held back until the keys are let go
int g_row = 0;

struct Choice {
    const char* label;
    std::vector<std::string> values;        // as the setting keeps them
    std::vector<std::string> shown;         // as the panel says them
    std::string (*get)();
    void (*set)(const std::string&);
    const char* game_label;                 // short and in capitals, for a game's own menu (PcSettings)
    std::vector<std::string> game_values;
    const char* help;
    int page = 0;                           // kMain, or the page it is on (kPost, kRate)
};

// The panel's pages (porting guide 4b, Orta's RECOMP OPTIONS): the list, POST PROCESSING's and FRAME RATE's.
enum Page { kMain, kPost, kRate };
int g_page = kMain;

std::string OnOff(bool on) { return on ? "on" : "off"; }

const std::vector<Choice>& Choices() {
    // In the ports' RECOMP OPTIONS order (porting guide 4b): POST PROCESSING (its page: antialiasing, upscaling),
    // display mode, resolution, aspect, FRAME RATE (its page: V-sync, frame pacing, FPS counter), textures,
    // language (no boot animation line for the PS2 ports: boot_video.h, hidden). Rows() puts the pages' rows in.
    static const std::vector<Choice> choices = {
        {"Display mode", {"windowed", "fullscreen", "borderless"}, {"Windowed", "Fullscreen", "Borderless"},
         [] { return WindowDisplayMode(); },
         [](const std::string& v) { SetWindowDisplayMode(v); },
         "DISPLAY MODE", {"WINDOWED", "FULLSCREEN", "BORDERLESS"},
         "A window, the whole screen, or borderless (the desktop as it is)."},
        // RENDER RESOLUTION (the ports' rule, as Orta's): the resolution alone ("896X784"), its values the
        // bridge's (1.00x-5.00x in quarter steps at 4:3, the standard line counts wider; StepRenderScale) - no
        // list here, the ends stop and a step the device cannot hold is refused.
        {"Render resolution", {}, {},
         [] { return RenderResolutionText(); },
         [](const std::string&) {},
         "RENDER RESOLUTION", {}, "The resolution the game is drawn at."},
        // ASPECT RATIO (porting guide 4): the game's own wide picture (its patch, SetWidescreen) - never stretched;
        // 21:9 an ultrawide screen's exact shape. Only shown for a game that has one.
        {"Aspect ratio", {"4:3", "16:9", "16:10", "21:9"}, {"4:3", "16:9", "16:10", "21:9"},
         [] { return Setting("aspect", "4:3"); },
         [](const std::string& v) { SetWindowAspect(v); },
         "ASPECT RATIO", {"4:3", "16:9", "16:10", "21:9"}, "The picture's shape: more of the world at the sides."},
        // STRETCH CUTSCENES (the maintainer 2026-11-20): a wide shape's movies - 4:3 between bars (OFF, the default)
        // or stretched to the shape (ON: the shape never changes) - the "stretch_cutscenes" setting, a port's
        // SetWidescreen callback reads it (ZOE's ShowsWide).
        {"Stretch cutscenes", {"off", "on"}, {"Off", "On"},
         [] { return OnOff(SettingOn("stretch_cutscenes", false)); },
         [](const std::string& v) { SetSettingOn("stretch_cutscenes", v == "on"); },
         "STRETCH CUTSCENES", {"OFF", "ON"}, "Movies fill a wide picture too (stretched) instead of 4:3 between bars."},
        // FIELD OF VIEW (4b): the original, or wider by 5-30 degrees (Orta's); the same hook as the aspect's.
        {"Field of view", {"0", "5", "10", "15", "20", "25", "30"},
         {"Original", "+5", "+10", "+15", "+20", "+25", "+30"},
         [] { return Setting("field_of_view", "0"); },
         [](const std::string& v) { SetSetting("field_of_view", v); },
         "FIELD OF VIEW", {"ORIGINAL", "+5", "+10", "+15", "+20", "+25", "+30"}, "How much of the world the camera takes in."},
        {"Antialiasing", {"off", "fxaa", "smaa", "ssaa2", "ssaa4"}, {"Off", "FXAA", "SMAA", "SSAA 2x", "SSAA 4x"},
         [] { return Setting("antialiasing", "off"); },
         [](const std::string& v) { SetWindowAntiAliasing(v); },
         "ANTIALIASING", {"OFF", "FXAA", "SMAA", "SSAA 2X", "SSAA 4X"}, "Smoother edges.", kPost},
        // ANISOTROPIC FILTER (4b; a PC enhancement, off by default): textures on floors and walls seen at a
        // flat angle stay sharp (gs.frag's AnisoSample).
        {"Anisotropic", {"off", "2", "4", "8", "16"}, {"Off", "2x", "4x", "8x", "16x"},
         [] { return Setting("anisotropic", "off"); },
         [](const std::string& v) {
             SetSetting("anisotropic", v);
             ApplyAnisotropySetting();
         },
         "ANISOTROPIC FILTER", {"OFF", "2X", "4X", "8X", "16X"},
         "Textures seen at a flat angle stay sharp.", kPost},
        // BLOOM + BLOOM STRENGTH (4b; a PC enhancement, off by default, its own pass after the frame): the
        // game's own glow stays as the original draws it (no line for that).
        {"Bloom", {"off", "on"}, {"Off", "On"},
         [] { return OnOff(SettingOn("bloom", false)); },
         [](const std::string& v) {
             SetSettingOn("bloom", v == "on");
             ApplyBloomSetting();
         },
         "BLOOM", {"OFF", "ON"}, "Bright parts of the picture glow softly.", kPost},
        {"Bloom strength", {"1", "2", "3", "4", "5", "6", "7", "8", "9", "10"},
         {"1", "2", "3", "4", "5", "6", "7", "8", "9", "10"},
         [] { return Setting("bloom_strength", "5"); },
         [](const std::string& v) {
             SetSetting("bloom_strength", v);
             ApplyBloomSetting();
         },
         "BLOOM STRENGTH", {"1", "2", "3", "4", "5", "6", "7", "8", "9", "10"}, "How strongly bright parts glow.", kPost},
        // FSR (AMD's FSR 1; "FSR" alone, the maintainer 2026-11-20); with it, the line under it: a preset drawing
        // at a part of the window (QUALITY 1/1.5, BALANCED 1/1.7, PERFORMANCE 1/2) or CUSTOM (RENDER RESOLUTION's).
        {"Upscaling", {"linear", "fsr1"}, {"Bilinear", "FSR"},
         [] { return Setting("upscaling", "linear").rfind("fsr", 0) == 0 ? std::string("fsr1") : std::string("linear"); },
         [](const std::string& v) { SetWindowUpscaling(v); },
         "UPSCALING", {"BILINEAR", "FSR"},
         "A picture drawn smaller than the window: filtered up, or FSR (sharper edges).", kPost},
        // (Three only, all shown on the line - a game's own page puts them side by side: the maintainer 2026-11-21.)
        {"FSR preset", {"quality", "balanced", "performance"}, {"Quality", "Balanced", "Performance"},
         [] {
             const std::string v = Setting("fsr_preset", "quality");
             return v == "balanced" || v == "performance" ? v : std::string("quality");
         },
         [](const std::string& v) { SetSetting("fsr_preset", v); },
         "FSR PRESET", {"QUALITY", "BALANCED", "PERFORMANCE"},
         "FSR draws at a part of the window's size: QUALITY the most, PERFORMANCE the least.", kPost},
        {"FSR sharpness", {"low", "medium", "high", "max"}, {"Low", "Medium", "High", "Max"},
         [] { return Setting("fsr_sharpness", "high"); },
         [](const std::string& v) { SetWindowFsrSharpness(v); },
         "FSR SHARPNESS", {"LOW", "MEDIUM", "HIGH", "MAX"}, "How strongly FSR sharpens the picture.", kPost},
        {"V-sync", {"off", "on"}, {"Off", "On"},
         [] { return OnOff(WindowVsync()); },
         [](const std::string& v) { SetWindowVsync(v == "on"); },
         "V-SYNC", {"OFF", "ON"}, "Wait for the screen's refresh: no tearing.", kRate},
        // Frame pacing (Orta's two): TIMER = pictures at a steady 60 Hz timer (the "frame_pacing" setting on),
        // DISPLAY = each shown as it is done, in step with the monitor (off).
        {"Frame pacing", {"on", "off"}, {"Timer", "Display"},
         [] { return OnOff(SettingOn("frame_pacing", FramePacingDefault())); },
         [](const std::string& v) { SetSettingOn("frame_pacing", v == "on"); },
         "PACING", {"TIMER", "DISPLAY"},
         "TIMER: pictures at a steady 60 Hz. DISPLAY: in step with the monitor.", kRate},
        {"FPS counter", {"off", "on", "frametime", "both"}, {"Off", "FPS", "Frame time", "Both"},
         [] { return Setting("fps_counter", "off"); },
         [](const std::string& v) { SetSetting("fps_counter", v); },
         "FPS COUNTER", {"OFF", "FPS", "FRAME TIME", "BOTH"},
         "The game's pictures a second, the time a picture, or both, in a corner (F1).", kRate},
        // SHADER COMPILE (4b, option-c's four): while a kind of draw's pipeline is made - OFF the frame waits for
        // it, SKIP the draw is left out, WAIT the frame waits up to 2.5 ms then leaves it out, HYBRID the whole
        // shader draws it meanwhile (the same picture). At once; SHADER NOTICE says so on the screen.
        {"Shader compile", {"off", "skip", "wait", "hybrid"}, {"Off", "Skip", "Wait", "Hybrid"},
         [] { return Setting("shader_compile", "hybrid"); },
         [](const std::string& v) {
             SetSetting("shader_compile", v);
             ApplyShaderCompileSetting();
         },
         "SHADER COMPILE", {"OFF", "SKIP", "WAIT", "HYBRID"},
         "New shaders: OFF waits, SKIP leaves the draw out, WAIT a little then out, HYBRID draws it at once."},
        {"Shader notice", {"on", "off"}, {"On", "Off"},
         [] { return OnOff(SettingOn("shader_notice", true)); },
         [](const std::string& v) { SetSettingOn("shader_notice", v == "on"); },
         "SHADER NOTICE", {"ON", "OFF"}, "BUILDING SHADERS in a corner while new shaders are made."},
        // The player's packs (a folder each in "textures" beside the game): Off or one of them, stepped through by
        // Step (the list is the folder's, read at the start).
        {"Texture pack", {"off"}, {"Off"},
         [] { return TexturePack(); },
         [](const std::string& v) { SelectTexturePack(v); },
         "TEXTURES", {"OFF"}, "A texture pack from the textures folder, or the game's own textures."},
        // The ports' rule: a LANGUAGE line for a disc with more than one (shown only when the port says so:
        // SetLanguages); the game takes it at its next start, as the console's language.
        {"Language", {"en", "fr", "de", "it", "ja", "es"}, {"English", "French", "German", "Italian", "Japanese", "Spanish"},
         [] { return Setting("language", "en"); },
         [](const std::string& v) { SetSetting("language", v); },
         "LANGUAGE", {"ENGLISH", "FRENCH", "GERMAN", "ITALIAN", "JAPANESE", "SPANISH"},
         "The game's language (subtitles and texts), at the next start."},
        // DUB (M9, the maintainer 2026-11-20): the voices, under LANGUAGE - only when the port has a second one to
        // play (SetDubs: ZOE's Japanese from the player's Japanese disc); at the next start.
        {"Dub", {"en", "ja", "fr", "de", "it", "es"}, {"English", "Japanese", "French", "German", "Italian", "Spanish"},
         [] { return Setting("dub", "en"); },
         [](const std::string& v) { SetSetting("dub", v); },
         "DUB", {"ENGLISH", "JAPANESE", "FRENCH", "GERMAN", "ITALIAN", "SPANISH"},
         "The voices, at the next start."},
        // (No boot animation line for the PS2 ports, the maintainer 2026-11-17: boot.mpg beside the game plays,
        // the file is the switch - boot_video.h.)
    };
    return choices;
}

// SetLanguages: the languages the running disc has (setting values), none until a port says.
std::vector<std::string>& Languages() {
    static std::vector<std::string> languages;
    return languages;
}

// SetDubs: the voices the port can play, and whether the line is there at all now.
std::vector<std::string>& Dubs() {
    static std::vector<std::string> dubs;
    return dubs;
}
bool (*g_dubs_available)() = nullptr;

// A choice's default (as its getter's when the setting is unset; ResetPcSetting).
std::string DefaultValue(const std::string& label) {
    if (label == "Display mode") return "windowed";
    if (label == "Aspect ratio") return "4:3";
    if (label == "Field of view") return "0";
    if (label == "Antialiasing" || label == "Anisotropic" || label == "Bloom" || label == "V-sync" ||
        label == "FPS counter" || label == "Texture pack")
        return "off";
    if (label == "Bloom strength") return "5";
    if (label == "Upscaling") return "linear";
    if (label == "Frame pacing") return OnOff(FramePacingDefault());
    if (label == "Shader compile") return "hybrid";
    if (label == "Shader notice") return "on";
    if (label == "Language" || label == "Dub") return "en";
    return "";
}

// A value the choice may take: the language line's only the disc's languages, the dub line's the port's voices.
bool Allowed(const Choice& c, const std::string& value) {
    const std::string label = c.label;
    const std::vector<std::string>* l = label == "Language" ? &Languages() : label == "Dub" ? &Dubs() : nullptr;
    return !l || std::find(l->begin(), l->end(), value) != l->end();
}

// The choice stepped on to its next allowed value either way.
void Step(const Choice& c, int step) {
    if (std::string(c.label) == "Render resolution") {
        StepRenderScale(step);
        return;
    }
    if (std::string(c.label) == "Texture pack") {           // Off, then the textures folder's packs
        std::vector<std::string> list = {"off"};
        for (const std::string& p : TexturePacks()) list.push_back(p);
        const std::string now = TexturePack();
        int at = 0;
        for (int i = 0; i < int(list.size()); ++i)
            if (list[std::size_t(i)] == now) at = i;
        const int n = int(list.size());
        SelectTexturePack(list[std::size_t(((at + step) % n + n) % n)]);
        return;
    }
    const std::string now = c.get();
    int at = 0;
    for (int i = 0; i < int(c.values.size()); ++i)
        if (c.values[std::size_t(i)] == now) at = i;
    const int count = int(c.values.size());
    for (int tries = 0; tries < count; ++tries) {
        at = ((at + step) % count + count) % count;
        if (Allowed(c, c.values[std::size_t(at)])) {
            c.set(c.values[std::size_t(at)]);
            return;
        }
    }
}

// A choice shown at all: the texture pack's only when the textures folder has one, the language's only for a
// disc with more than one.
bool Shown(const Choice& c) {
    const std::string label = c.label;
    if (label == "Texture pack" && TexturePacks().empty()) return false;
    if (label == "Language" && Languages().size() < 2) return false;
    if (label == "Dub" && (Dubs().size() < 2 || !g_dubs_available || !g_dubs_available())) return false;
    if ((label == "Aspect ratio" || label == "Field of view" || label == "Stretch cutscenes") && !WidescreenSupported())
        return false;
    // FSR's own lines only under UPSCALING FSR (the maintainer 2026-11-21).
    if ((label == "FSR preset" || label == "FSR sharpness") && Setting("upscaling", "linear").rfind("fsr", 0) != 0)
        return false;
    return true;
}

// The rows of a page: the list's - POST PROCESSING >, its own lines (FRAME RATE > after the aspect ratio's),
// CONTROLS >, Close; a page's - its lines, then Back.
constexpr int kCloseRow = -1, kControlsRow = -2, kPostRow = -3, kRateRow = -4, kBackRow = -5;
// A port's own debug lines (AddDebugLine: ZOE's subtitle timing of the scene playing), rows kPortBase - n.
constexpr int kPortBase = -100;
struct DebugLine {
    std::string label;
    std::string (*get)();
    void (*step)(int);
};
std::vector<DebugLine>& DebugLines() {
    static std::vector<DebugLine> lines;
    return lines;
}
std::vector<int> Rows(int page = g_page) {
    std::vector<int> rows;
    if (page == kMain) rows.push_back(kPostRow);
    const auto& c = Choices();
    std::size_t rate_at = rows.size();      // FRAME RATE > after render resolution, aspect ratio, field of view
    for (int i = 0; i < int(c.size()); ++i) {
        if (c[i].page != page || !Shown(c[i])) continue;
        rows.push_back(i);
        const std::string label = c[i].label;
        if (label == "Render resolution" || label == "Aspect ratio" || label == "Field of view") rate_at = rows.size();
    }
    if (page == kMain) rows.insert(rows.begin() + std::ptrdiff_t(rate_at), kRateRow);
    if (page == kMain) {
        for (int n = 0; n < int(DebugLines().size()); ++n) rows.push_back(kPortBase - n);
        rows.push_back(kControlsRow);
        rows.push_back(kCloseRow);
    } else {
        rows.push_back(kBackRow);
    }
    return rows;
}

// Every choice shown, the pages' in their place (a game's own menu lists them flat: PcSettings).
std::vector<int> AllChoices() {
    std::vector<int> all;
    for (int r : Rows(kMain)) {
        if (r == kPostRow || r == kRateRow)
            for (int s : Rows(r == kPostRow ? kPost : kRate))
                if (s >= 0) all.push_back(s);
        if (r >= 0) all.push_back(r);
    }
    return all;
}

void Draw(bool force = false) {
    std::vector<std::string> lines = {g_page == kPost   ? "RECOMP OPTIONS > POST PROCESSING   (Esc back)"
                                      : g_page == kRate ? "RECOMP OPTIONS > FRAME RATE   (Esc back)"
                                                        : "RECOMP OPTIONS   (F9 closes)"};
    const std::vector<int> rows = Rows();
    for (int r = 0; r < int(rows.size()); ++r) {
        std::string text;
        if (rows[r] == kControlsRow) {
            text = "Controls >";
        } else if (rows[r] == kPostRow) {
            text = "Post processing >";
        } else if (rows[r] == kRateRow) {
            text = "Frame rate >";
        } else if (rows[r] == kBackRow) {
            text = "Back";
        } else if (rows[r] <= kPortBase) {
            const DebugLine& d = DebugLines()[std::size_t(kPortBase - rows[r])];
            text = d.label + std::string(16 - std::min<std::size_t>(15, d.label.size()), ' ') + "< " + d.get() + " >";
        } else if (rows[r] < 0) {
            text = "Close";
        } else {
            const Choice& c = Choices()[rows[r]];
            const std::string now = c.get();
            std::string shown = now;
            for (std::size_t i = 0; i < c.values.size(); ++i)
                if (c.values[i] == now) shown = c.shown[i];
            text = std::string(c.label) + std::string(16 - std::min<std::size_t>(15, std::string(c.label).size()), ' ') +
                   "< " + shown + " >";
        }
        lines.push_back((r == g_row ? "> " : "  ") + text);
    }
    lines.push_back("");
    lines.push_back("Up/Down choose, Left/Right change");
    static std::vector<std::string> shown;
    if (force || lines != shown) SetWindowOverlay(lines);     // drawn again only when it changes
    shown = lines;
}

// RESTART NEEDED (the ports' rule): the lines that take effect only at a start, as they were when the panel
// opened; leaving it with one changed asks RESTART NOW / LATER once.
bool g_prompt = false;
int g_prompt_row = 0;
std::vector<std::pair<std::string, std::string>> g_snapshot;     // label, value

bool NeedsRestart(const Choice& c) {
    const std::string label = c.label;
    return label == "Language" || label == "Dub";
}

void TakeSnapshot() {
    g_snapshot.clear();
    for (const Choice& c : Choices())
        if (NeedsRestart(c)) g_snapshot.push_back({c.label, c.get()});
}

std::vector<std::string> ChangedForRestart() {
    std::vector<std::string> changed;
    for (const Choice& c : Choices())
        for (const auto& [label, value] : g_snapshot)
            if (label == c.label && value != c.get()) {
                std::string upper = label;
                for (char& ch : upper) ch = char(std::toupper(static_cast<unsigned char>(ch)));
                changed.push_back(upper + " CHANGED");
            }
    return changed;
}

void DrawPrompt() {
    std::vector<std::string> lines = {"RESTART NEEDED"};
    for (const std::string& what : ChangedForRestart()) lines.push_back("  " + what);
    lines.push_back("");
    lines.push_back(std::string(g_prompt_row == 0 ? "> " : "  ") + "Restart now");
    lines.push_back(std::string(g_prompt_row == 1 ? "> " : "  ") + "Later");
    static std::vector<std::string> shown;
    if (lines != shown) SetWindowOverlay(lines);
    shown = lines;
}

// Leaving the panel: RESTART NEEDED first when a start-only line changed.
void Close() {
    if (!ChangedForRestart().empty()) {
        g_prompt = true;
        g_prompt_row = 0;
        DrawPrompt();
        return;
    }
    g_open = false;
    SetWindowOverlay({});
}

void OpenControls();

// A page of the list opened, or the list again (on the page's row).
void OpenPage(int page) {
    const int from = g_page;
    g_page = page;
    g_row = 0;
    if (page == kMain) {
        const std::vector<int> rows = Rows();
        for (int r = 0; r < int(rows.size()); ++r)
            if (rows[r] == (from == kPost ? kPostRow : kRateRow)) g_row = r;
    }
    Draw(true);
}

void Change(int step) {
    const std::vector<int> rows = Rows();
    const int which = rows[std::size_t(g_row)];
    if (which == kControlsRow) {
        if (step > 0) OpenControls();
        return;
    }
    if (which == kPostRow || which == kRateRow) {
        if (step > 0) OpenPage(which == kPostRow ? kPost : kRate);
        return;
    }
    if (which == kBackRow) {
        OpenPage(kMain);
        return;
    }
    if (which <= kPortBase) {
        DebugLines()[std::size_t(kPortBase - which)].step(step);
        Draw(true);
        return;
    }
    if (which < 0) {
        Close();
        return;
    }
    Step(Choices()[which], step);
}

// Keys and buttons, each acting when it goes down.
enum Input { kToggle, kUp, kDown, kLeft, kRight, kAccept, kBack, kInputs };

std::array<bool, kInputs> Held() {
    std::array<bool, kInputs> held{};
    const bool focused = WindowFocused();
    auto down = [&](int k) { return focused && HostKeyDown(k); };
#if PS2HOST_NO_DEBUG_PANEL
    held[kToggle] = false;          // a player's build: no F9 panel (CMake PS2HOST_DEBUG_PANEL)
#else
    held[kToggle] = down(key::kF9);
#endif
    held[kUp] = down(key::kUp);
    held[kDown] = down(key::kDown);
    held[kLeft] = down(key::kLeft);
    held[kRight] = down(key::kRight);
    held[kAccept] = down(key::kReturn);
    held[kBack] = down(key::kEscape);
    HostPadState pad;
    if (focused && HostPad(&pad)) {
        // DualShock 2 order: 0 select, 4 up, 5 right, 6 down, 7 left, 11 R1, 13 circle, 14 cross.
        auto button = [&](int bit) { return (pad.buttons >> bit) & 1; };
        // No controller combination opens it (the maintainer 2026-11-20: the player's settings are the game's own
        // RECOMP OPTIONS page, this panel is for debugging - F9 only; Select + R1 removed). Open, a pad steers it.
        held[kUp] = held[kUp] || button(4);
        held[kDown] = held[kDown] || button(6);
        held[kLeft] = held[kLeft] || button(7);
        held[kRight] = held[kRight] || button(5);
        // (BUTTON MAPPING opened from a port's own page in its game's look: the game's buttons - Circle chooses,
        // Cross goes back - as on its pages; the panel's own the other way round.)
        held[kAccept] = held[kAccept] || button(g_port_controls ? 13 : 14);
        held[kBack] = held[kBack] || button(g_port_controls ? 14 : 13);
    }
    return held;
}

// ---- CONTROLS > BUTTON MAPPING (the ports' rule, Orta's model): DEVICE, RESET TO DEFAULT, then the device's
// bindings, each "INPUT: SOURCE"; Enter / Cross on one waits for a key or button (all let go first, ~5 s, Esc
// cancels). The panel's own keys are not bound: the default layout always gets back here.
bool g_controls = false;
bool g_controls_only = false;       // opened alone (OpenPcMenuControls: a port's CONTROLS line) - closing ends it all

void Draw(bool force);
// The BUTTON MAPPING page closed: back on the panel's list, or - opened alone from a port's own page - the panel
// closed (a player's build has no panel to land on).
void CloseControls() {
    g_controls = false;
    if (g_controls_only) {
        g_controls_only = false;
        g_open = false;
        g_releasing = true;
        if (g_port_controls) g_controls_look(false, {}, 0);
        else SetWindowOverlay({});
        g_port_controls = false;
        return;
    }
    Draw(true);
}
int g_controls_row = 0;
std::size_t g_device = 0;           // index into ControlsDevices()
int g_capture = -1;                 // the control waiting for a press, -1 none
int g_capture_frames = 0;
bool g_capture_released = false;

std::vector<HostDevice> ControlsDevices() {
    std::vector<HostDevice> list = {{kKeyboardDevice, "Keyboard", ControllerFamily::kOther}};
    for (const HostDevice& d : WindowControllers()) list.push_back(d);
    return list;
}

bind::Kind KindOf(const HostDevice& d) { return d.id == kKeyboardDevice ? bind::Kind::kKeyboard : bind::Kind::kPad; }

const char* const kInputNames[bind::kControls] = {
    "Select", "L3", "R3", "Start", "Up", "Right", "Down", "Left", "L2", "R2", "L1", "R1",
    "Triangle", "Circle", "Cross", "Square", "Stick left", "Stick right", "Stick up", "Stick down"};

// A source as the player knows it: the key's name, the button's (the controller's own: an Xbox pad's A for the
// place of Cross).
// A key's name in English by its place (set-1 scan code): a port's own look writes the game's language, not the
// system's ("EINGABE", "R?CK" on a German Windows; his shot 2026-11-21). "" when not one of these.
std::string EnglishKeyName(u16 src) {
    static const struct { u16 code; const char* name; } kNames[] = {
        {0x01, "Esc"}, {0x0E, "Backspace"}, {0x0F, "Tab"}, {0x1C, "Enter"}, {0x1D, "Ctrl"}, {0x2A, "Shift"},
        {0x36, "Right Shift"}, {0x38, "Alt"}, {0x39, "Space"}, {0x3A, "Caps Lock"}, {0xE048, "Up"},
        {0xE050, "Down"}, {0xE04B, "Left"}, {0xE04D, "Right"}, {0xE01C, "Num Enter"}, {0xE01D, "Right Ctrl"},
        {0xE038, "Right Alt"}, {0xE047, "Home"}, {0xE04F, "End"}, {0xE049, "Page Up"}, {0xE051, "Page Down"},
        {0xE052, "Insert"}, {0xE053, "Delete"}, {0x0C, "-"}, {0x0D, "="}, {0x1A, "["}, {0x1B, "]"}, {0x27, ";"},
        {0x28, "'"}, {0x29, "`"}, {0x2B, "\\"}, {0x33, ","}, {0x34, "."}, {0x35, "/"}};
    for (const auto& k : kNames)
        if (k.code == src) return k.name;
    static const char* const kRow1 = "QWERTYUIOP", *const kRow2 = "ASDFGHJKL", *const kRow3 = "ZXCVBNM";
    if (src >= 0x02 && src <= 0x0B) return std::string(1, src == 0x0B ? '0' : char('1' + (src - 0x02)));
    if (src >= 0x10 && src <= 0x19) return std::string(1, kRow1[src - 0x10]);
    if (src >= 0x1E && src <= 0x26) return std::string(1, kRow2[src - 0x1E]);
    if (src >= 0x2C && src <= 0x32) return std::string(1, kRow3[src - 0x2C]);
    if (src >= 0x3B && src <= 0x44) return "F" + std::to_string(src - 0x3B + 1);
    return "";
}

std::string Shown(const HostDevice& d, u16 src) {
    if (!src) return "-";
    if (KindOf(d) == bind::Kind::kKeyboard) {
        if (g_port_controls) {
            const std::string english = EnglishKeyName(src);
            if (!english.empty()) return english;
        }
        std::string name;
        if (ps2platform::ScanCodeName(src, &name)) return name;
        return bind::SourceName(bind::Kind::kKeyboard, src);
    }
    static const char* const xbox[bind::kPadControls] = {"Back", "LS", "RS", "Start", "D-pad up", "D-pad right",
        "D-pad down", "D-pad left", "LT", "RT", "LB", "RB", "Y", "B", "A", "X"};
    const int input = src - 1;
    if (input < 0 || input >= bind::kPadControls) return "-";
    return d.family == ControllerFamily::kXbox ? xbox[input] : kInputNames[input];
}

bind::Map CurrentMap(const HostDevice& d) {
    return KindOf(d) == bind::Kind::kKeyboard ? bind::KeyboardMap() : bind::PadMap(d.name);
}

void StoreMap(const HostDevice& d, const bind::Map& m) {
    if (KindOf(d) == bind::Kind::kKeyboard) bind::StoreKeyboard(m);
    else bind::StorePad(d.name, m);
}

// The first source of the device held now (0: none).
u16 HeldSource(const HostDevice& d) {
    if (KindOf(d) == bind::Kind::kPad) {
        HostPadState pad;
        if (!WindowControllerPad(d.id, &pad)) return 0;
        for (int i = 0; i < bind::kPadControls; ++i)
            if (pad.buttons & (1u << i)) return bind::PadSource(i);
        return 0;
    }
    if (!WindowFocused()) return 0;
    static const u16 kExtended[] = {0xE01C, 0xE01D, 0xE035, 0xE038, 0xE047, 0xE048, 0xE049, 0xE04B,
                                    0xE04D, 0xE04F, 0xE050, 0xE051, 0xE052, 0xE053};
    for (u16 sc = 0x02; sc <= 0x58; ++sc) {
        if (sc == 0x43) continue;                               // F9: the panel's own
        if (HostKeyDown(ps2platform::KeyOfScanCode(sc))) return sc;
    }
    for (u16 sc : kExtended)
        if (HostKeyDown(ps2platform::KeyOfScanCode(sc))) return sc;
    return 0;
}

// In a port's own look (g_port_controls) no Back row: Cross / Esc go back.
int ControlsRows(const HostDevice& d) { return 2 + bind::Controls(KindOf(d)) + (g_port_controls ? 0 : 1); }

void DrawControls() {
    const std::vector<HostDevice> devices = ControlsDevices();
    if (g_device >= devices.size()) g_device = 0;
    const HostDevice& d = devices[g_device];
    const bind::Map m = CurrentMap(d);
    const u32 dup = bind::Duplicates(KindOf(d), m);
    if (g_port_controls) {                  // the port draws it (its game's look)
        std::vector<ControlsLine> rows;
        auto upper = [](std::string s) {
            for (char& ch : s) ch = char(std::toupper(static_cast<unsigned char>(ch)));
            return s;
        };
        rows.push_back({"DEVICE", upper(d.name), false, false});
        rows.push_back({"RESET TO DEFAULT", "", false, false});
        for (int c = 0; c < bind::Controls(KindOf(d)); ++c) {
            std::string src = Shown(d, m.src[c][0]);
            if (m.src[c][1]) src += " / " + Shown(d, m.src[c][1]);
            const bool capture = g_capture == c;
            rows.push_back({upper(kInputNames[c]),
                            capture ? (KindOf(d) == bind::Kind::kKeyboard ? "PRESS A KEY" : "PRESS A BUTTON") : upper(src),
                            capture, (dup & (1u << c)) != 0});
        }
        g_controls_look(true, rows, g_controls_row);
        return;
    }
    std::vector<std::string> lines = {"CONTROLS > BUTTON MAPPING   (Esc back)"};
    const int rows = ControlsRows(d);
    for (int r = 0; r < rows; ++r) {
        std::string text;
        if (r == 0) {
            text = "Device          < " + d.name + " >";
        } else if (r == 1) {
            text = "Reset to default";
        } else if (r == rows - 1) {
            text = "Back";
        } else {
            const int c = r - 2;
            std::string label = kInputNames[c];
            label += std::string(16 - std::min<std::size_t>(15, label.size()), ' ');
            if (g_capture == c) {
                text = label + (KindOf(d) == bind::Kind::kKeyboard ? "PRESS A KEY" : "PRESS A BUTTON");
            } else {
                std::string src = Shown(d, m.src[c][0]);
                if (m.src[c][1]) src += " / " + Shown(d, m.src[c][1]);
                text = label + src + ((dup & (1u << c)) ? "   DOUBLE" : "");
            }
        }
        lines.push_back((r == g_controls_row ? "> " : "  ") + text);
    }
    lines.push_back("");
    lines.push_back("Enter / Cross: bind, Left/Right: device");
    static std::vector<std::string> shown;
    if (lines != shown) SetWindowOverlay(lines);
    shown = lines;
}

void OpenControls() {
    g_controls = true;
    g_controls_row = 0;
    g_capture = -1;
    DrawControls();
}

// One panel update on the controls page; `went_down` the panel's inputs (unbound) that went down now.
template <typename WentDown>
void UpdateControls(WentDown went_down) {
    const std::vector<HostDevice> devices = ControlsDevices();
    if (g_device >= devices.size()) g_device = 0;
    const HostDevice& d = devices[g_device];
    const int rows = ControlsRows(d);
    if (g_capture >= 0) {
        // Waiting for the press: everything let go first, then the first source down; ~5 s (300 reads) or Esc.
        const u16 src = HeldSource(d);
        if (!g_capture_released) {
            g_capture_released = src == 0;
        } else if (src) {
            bind::Map m = CurrentMap(d);
            bind::Set(&m, g_capture, src);
            StoreMap(d, m);
            g_capture = -1;
        }
        // (A controller's Circle is a button to bind, so only the keyboard's Esc cancels.)
        if (g_capture >= 0 && (++g_capture_frames > 300 ||
                               (KindOf(d) == bind::Kind::kKeyboard && WindowFocused() && HostKeyDown(key::kEscape))))
            g_capture = -1;
        DrawControls();
        return;
    }
    if (went_down(kBack) || went_down(kToggle)) {
        CloseControls();
        return;
    }
    if (went_down(kUp)) g_controls_row = (g_controls_row + rows - 1) % rows;
    if (went_down(kDown)) g_controls_row = (g_controls_row + 1) % rows;
    if (g_controls_row >= rows) g_controls_row = rows - 1;
    if (g_controls_row == 0 && (went_down(kLeft) || went_down(kRight))) {
        const int n = int(devices.size());
        g_device = std::size_t(((int(g_device) + (went_down(kLeft) ? -1 : 1)) % n + n) % n);
    } else if (went_down(kAccept)) {
        if (g_controls_row == 0) {
            g_device = (g_device + 1) % devices.size();
        } else if (g_controls_row == 1) {
            bind::Map def;
            if (KindOf(d) == bind::Kind::kKeyboard) bind::KeyboardDefault(&def);
            else bind::PadDefault(&def);
            StoreMap(d, def);
        } else if (!g_port_controls && g_controls_row == rows - 1) {
            CloseControls();
            return;
        } else {
            g_capture = g_controls_row - 2;
            g_capture_frames = 0;
            g_capture_released = false;
        }
    }
    DrawControls();
}

}  // namespace

std::vector<PcSetting> PcSettings() {
    std::vector<PcSetting> settings;
    for (int i : AllChoices()) {
        const Choice& c = Choices()[std::size_t(i)];
        const std::string now = c.get();
        std::string value = now;
        for (std::size_t v = 0; v < c.values.size(); ++v)
            if (c.values[v] == now) value = c.game_values[v];
        settings.push_back({c.game_label, value, c.help,
                            c.page == kPost ? "POST PROCESSING" : c.page == kRate ? "FRAME RATE" : "",
                            NeedsRestart(c), {}});
        for (std::size_t v = 0; v < c.values.size() && v < c.game_values.size(); ++v)
            if (Allowed(c, c.values[v])) settings.back().values.push_back(c.game_values[v]);
    }
    return settings;
}

void StepPcSetting(int index, int step) {
    int n = 0;
    for (int i : AllChoices()) {
        if (n++ != index) continue;
        Step(Choices()[std::size_t(i)], step);
        return;
    }
}

void ResetPcSetting(int index) {
    int n = 0;
    for (int i : AllChoices()) {
        if (n++ != index) continue;
        const Choice& c = Choices()[std::size_t(i)];
        const std::string label = c.label;
        if (label == "Render resolution") {                     // NATIVE: the first size
            for (int k = 0; k < 64 && StepRenderScale(-1); ++k) {}
            return;
        }
        const std::string value = DefaultValue(label);
        if (Allowed(c, value)) c.set(value);
        return;
    }
}

void SetLanguages(const std::vector<std::string>& languages) { Languages() = languages; }

void SetDubs(const std::vector<std::string>& dubs, bool (*available)()) {
    Dubs() = dubs;
    g_dubs_available = available;
}

// ---- SELECT's menu (the maintainer 2026-11-21, ZOE issue 3): a port whose game leaves Select unused
// (EnableSelectMenu) opens EXIT GAME / EXIT TO MENU / BACK with it - the game held still meanwhile (devices.cpp's
// frame waits, the last picture shown again). The "select" setting (internal: in pc_settings.ini only, no menu
// line) = "stock" gives Select back to the game.
bool g_select_menu = false;         // the port's EnableSelectMenu
bool g_quit_open = false;
int g_quit_row = 0;
int g_quit_ask = 0;                 // EXIT GAME (2) / EXIT TO MENU (3) asked: YES / NO
bool g_quit_yes = false;

bool SelectMenuActive() { return g_select_menu && Setting("select", "ours") != "stock"; }

void (*g_select_look)(bool open, int row) = nullptr;   // SetSelectMenuLook: the port draws it in the game's look

void SetSelectMenuLook(void (*draw)(bool open, int row)) { g_select_look = draw; }

void CloseQuitLook() {
    if (g_select_look) g_select_look(false, 0);
    else SetWindowOverlay({});
}

void DrawQuit() {
    // (Asked: row 4 + 2 * (0 EXIT GAME, 1 EXIT TO MENU) + (0 YES, 1 NO).)
    const int row = g_quit_ask ? 4 + 2 * (g_quit_ask - 2) + (g_quit_yes ? 0 : 1) : g_quit_row;
    if (g_select_look) {
        g_select_look(true, row);
        return;
    }
    static const char* const kRows[4] = {"RESUME", "SETTINGS", "EXIT GAME", "EXIT TO MENU"};
    std::vector<std::string> lines;
    if (g_quit_ask) {
        lines.push_back(std::string(kRows[g_quit_ask]) + "?  Unsaved progress will be lost.");
        lines.push_back(std::string(g_quit_yes ? "> YES   NO" : "  YES > NO"));
        SetWindowOverlay(lines);
        return;
    }
    for (int r = 0; r < 4; ++r) lines.push_back(std::string(g_quit_row == r ? "> " : "  ") + kRows[r]);
    SetWindowOverlay(lines);
}

// SETTINGS (the port's own pages, SetSelectSettings) and EXIT TO MENU (the port's own way to its title,
// SetExitToMenu; without one the game starts again).
bool (*g_select_settings)(unsigned held, unsigned pressed) = nullptr;
bool g_settings_open = false;
void (*g_exit_to_menu)() = nullptr;
void SetSelectSettings(bool (*step)(unsigned held, unsigned pressed)) { g_select_settings = step; }
bool (*g_select_allowed)() = nullptr;      // SetSelectAllowed: where Select opens it (the port's gameplay)
void SetSelectAllowed(bool (*allowed)()) { g_select_allowed = allowed; }
bool SelectMenuHere() { return !g_select_allowed || g_select_allowed(); }
void SetExitToMenu(void (*go)()) { g_exit_to_menu = go; }

void EnableSelectMenu(bool on) { g_select_menu = on; }
bool SelectMenuOpen() { return g_quit_open; }
bool SelectToGame() { return !SelectMenuActive(); }

// Each window pump: Select (the pad's or Backspace) opens it; up / down, then Circle or Enter (the game's own
// confirm: ZOE's); Cross, Select or Esc back to the game. In SETTINGS the port's pages have the input (DS2 bits:
// 4 up, 5 right, 6 down, 7 left held; 12 triangle, 13 circle, 14 cross pressed) until they close.
void UpdateSelectMenu() {
    if (!SelectMenuActive()) {
        g_quit_open = g_settings_open = false;
        return;
    }
    if (g_open) return;                 // the panel's (BUTTON MAPPING from SETTINGS' CONTROLS) has the input
    // The game's inputs through the player's bindings (BUTTON MAPPING, a port's SwapDefaults): the menu follows
    // what is bound to Select, the directions, Circle, Cross, Triangle; Esc goes back too.
    const u16 bound = BoundButtonsNow();
    auto button = [&](int bit) { return ((bound >> bit) & 1) != 0; };
    const bool select = button(0), up = button(4), down = button(6), left = button(7), right = button(5),
               accept = button(13), back = button(14) || (WindowFocused() && HostKeyDown(key::kEscape)),
               deflt = button(12);
    static bool before[7] = {};
    const bool now[7] = {select, up, down, accept, back, left, right};
    // PS2HOST_SELECT_MENU_TEST=<pump>[,<row>[,open]] (a scripted run's look at it): opened at that window pump on
    // that row (",open": SETTINGS' pages too, their first page drawn), closed 90 pumps later.
    static const long long test_at = std::getenv("PS2HOST_SELECT_MENU_TEST")
                                         ? std::atoll(std::getenv("PS2HOST_SELECT_MENU_TEST")) : -1;
    static long long pumps = 0;
    if (test_at >= 0) {
        ++pumps;
        if (pumps == test_at) {
            const char* comma = std::strchr(std::getenv("PS2HOST_SELECT_MENU_TEST"), ',');
            g_quit_open = true;
            g_quit_row = comma ? std::atoi(comma + 1) % 4 : 0;
            g_quit_ask = std::strstr(std::getenv("PS2HOST_SELECT_MENU_TEST"), ",ask") && g_quit_row >= 2 ? g_quit_row : 0;
            g_quit_yes = false;
            DrawQuit();
            if (std::strstr(std::getenv("PS2HOST_SELECT_MENU_TEST"), ",open") && g_select_settings) {
                g_settings_open = true;
                g_select_settings(0, 0);
            }
            if (std::strstr(std::getenv("PS2HOST_SELECT_MENU_TEST"), ",map")) OpenPcMenuControls();   // BUTTON MAPPING
            if (std::strstr(std::getenv("PS2HOST_SELECT_MENU_TEST"), ",exit") && g_exit_to_menu) {
                g_exit_to_menu();                                   // EXIT TO MENU at once, the menu closed
                g_quit_open = false;
                CloseQuitLook();
            }
        } else if (pumps == test_at + 90 && g_quit_open) {
            if (g_settings_open) g_select_settings(0, 1u << 14);    // its pages closed (Cross)
            g_quit_open = g_settings_open = false;
            CloseQuitLook();
        }
        if (pumps >= test_at && pumps <= test_at + 90) return;
    }
    if (g_settings_open) {
        for (int i = 0; i < 7; ++i) before[i] = now[i];         // (back on the menu: no press carried over)
        static bool seen[4] = {};
        const bool press[4] = {accept, back || select, deflt, false};
        unsigned pressed = 0;
        if (press[0] && !seen[0]) pressed |= 1u << 13;
        if (press[1] && !seen[1]) pressed |= 1u << 14;
        if (press[2] && !seen[2]) pressed |= 1u << 12;
        for (int i = 0; i < 3; ++i) seen[i] = press[i];
        const unsigned held = (up ? 1u << 4 : 0) | (right ? 1u << 5 : 0) | (down ? 1u << 6 : 0) | (left ? 1u << 7 : 0);
        if (!g_select_settings(held, pressed)) {
            g_settings_open = false;
            DrawQuit();
        }
        return;
    }
    auto went_down = [&](int i) { return now[i] && !before[i]; };
    if (g_quit_open && g_quit_ask) {
        // EXIT GAME / EXIT TO MENU asked (the maintainer 2026-11-21: in the game an exit asks first): left / right
        // (up / down too) between YES and NO, Circle takes it, Cross / Select / Esc back to the menu.
        if (went_down(1) || went_down(2) || went_down(5) || went_down(6)) {
            g_quit_yes = !g_quit_yes;
            DrawQuit();
        } else if (went_down(0) || went_down(4) || (went_down(3) && !g_quit_yes)) {
            g_quit_ask = 0;
            DrawQuit();
        } else if (went_down(3)) {
            const int ask = g_quit_ask;
            g_quit_ask = 0;
            g_quit_open = false;
            g_releasing = true;
            CloseQuitLook();
            if (ask == 2) {
                QuitGame();
            } else {
                if (g_exit_to_menu) g_exit_to_menu();           // the port's way to its title
                else RequestRestart();                          // none: the game started again (it quits too)
            }
        }
        for (int i = 0; i < 7; ++i) before[i] = now[i];
        return;
    }
    if (!g_quit_open) {
        if (went_down(0) && !g_open && (!g_select_allowed || g_select_allowed())) {
            g_quit_open = true;
            g_quit_row = 0;
            g_quit_ask = 0;
            DrawQuit();
        }
    } else if (went_down(0) || went_down(4)) {
        g_quit_open = false;
        g_releasing = true;             // the game gets no input until all is let go
        CloseQuitLook();
    } else if (went_down(1) || went_down(2)) {
        g_quit_row = (g_quit_row + (went_down(2) ? 1 : 3)) % 4;
        DrawQuit();
    } else if (went_down(3)) {
        if (g_quit_row == 1 && g_select_settings) {            // SETTINGS: the port's pages from here on
            g_settings_open = true;
            g_select_settings(0, 0);
            for (int i = 0; i < 7; ++i) before[i] = now[i];
            return;
        }
        if (g_quit_row == 2 || g_quit_row == 3) {               // asked first (YES / NO, NO chosen)
            g_quit_ask = g_quit_row;
            g_quit_yes = false;
            DrawQuit();
            for (int i = 0; i < 7; ++i) before[i] = now[i];
            return;
        }
        g_quit_open = false;
        g_releasing = true;
        CloseQuitLook();
    }
    for (int i = 0; i < 7; ++i) before[i] = now[i];
}

bool PcMenuOpen() { return g_open || g_releasing || g_quit_open; }

bool PcPanelOpen() { return g_open; }

void AddDebugLine(const std::string& label, std::string (*get)(), void (*step)(int)) {
    DebugLines().push_back({label, get, step});
}

void OpenPcMenu() {
    if (g_open) return;
    g_open = true;
    g_row = 0;
    g_page = kMain;
    g_controls = false;
    TakeSnapshot();
    Draw(true);
}

void UpdatePcMenu() {
    static std::array<bool, kInputs> before{};
    const std::array<bool, kInputs> now = Held();
    const bool was_open = g_open;
    auto went_down = [&](Input i) { return now[i] && !before[i]; };
    if (g_prompt) {
        // RESTART NEEDED: Restart now (the game starts again as it was started) or Later.
        if (went_down(kUp) || went_down(kDown)) g_prompt_row = 1 - g_prompt_row;
        const bool later = went_down(kBack) || went_down(kToggle) || (went_down(kAccept) && g_prompt_row == 1);
        if (went_down(kAccept) && g_prompt_row == 0) {
            RequestRestart();
            QuitGame();
        }
        if (later || (went_down(kAccept) && g_prompt_row == 0)) {
            g_prompt = g_open = false;
            SetWindowOverlay({});
        } else {
            DrawPrompt();
        }
    } else if (g_open && g_controls) {
        UpdateControls(went_down);
    } else if (g_open && g_page != kMain && went_down(kBack) && !went_down(kToggle)) {
        OpenPage(kMain);                                // a page: Esc / Circle back to the list
    } else if (went_down(kToggle) || (g_open && went_down(kBack))) {
        g_row = 0;
        g_page = kMain;
        if (g_open) {
            Close();
        } else {
            g_open = true;
            TakeSnapshot();
            Draw(true);
        }
    } else if (g_open) {
        const int rows = int(Rows().size());
        if (went_down(kUp)) g_row = (g_row + rows - 1) % rows;
        if (went_down(kDown)) g_row = (g_row + 1) % rows;
        if (went_down(kLeft)) Change(-1);
        if (went_down(kRight) || went_down(kAccept)) Change(+1);
    }
    before = now;
    if (!g_open && was_open) g_releasing = true;
    if (g_releasing) {
        bool any = false;
        for (bool held : now) any = any || held;
        g_releasing = any;
    }
    if (g_open && !g_prompt && !g_controls) Draw();
}

void OpenPcMenuControls() {
    g_port_controls = g_controls_look != nullptr;      // (before the page is first drawn)
    OpenPcMenu();
    if (g_port_controls) SetWindowOverlay({});         // (the panel's main page OpenPcMenu drew: not shown here)
    OpenControls();
    g_controls_only = true;
}

void SetControlsLook(void (*draw)(bool open, const std::vector<ControlsLine>& lines, int row)) {
    g_controls_look = draw;
}

}  // namespace ps2host
