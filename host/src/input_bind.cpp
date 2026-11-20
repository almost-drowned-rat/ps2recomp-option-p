// Button bindings per input device: input_bind.h.

#include "ps2host/input_bind.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

#include "ps2host/settings.h"

namespace ps2host {
namespace bind {

namespace {

const char* const kControlNames[kControls] = {
    "select", "l3", "r3", "start", "up", "right", "down", "left", "l2", "r2", "l1", "r1",
    "triangle", "circle", "cross", "square", "lleft", "lright", "lup", "ldown"};

// The keyboard's default (dbc.cpp's keys until 2026-11-16): Backspace Select, Enter Start, the arrows the D-pad,
// Z(QWERTZ Y) P U O L2 R2 L1 R1, I L K J Triangle Circle Cross Square, W A S D the left stick.
const u16 kKeyboardDefault[kControls] = {
    0x0E, 0, 0, 0x1C, 0xE048, 0xE04D, 0xE050, 0xE04B, 0x2C, 0x19, 0x16, 0x18,
    0x17, 0x26, 0x25, 0x24, 0x1E, 0x20, 0x11, 0x1F};

std::string Trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}

int ControlOf(const std::string& name) {
    for (int c = 0; c < kControls; ++c)
        if (name == kControlNames[c]) return c;
    return -1;
}

// A source's text: 0 for "none", -1 if unknown.
int SourceOf(Kind kind, const std::string& name) {
    if (name == "none") return 0;
    if (kind == Kind::kKeyboard) {
        if (name.size() < 3 || name.compare(0, 2, "sc") != 0) return -1;
        char* end = nullptr;
        const unsigned long v = std::strtoul(name.c_str() + 2, &end, 16);
        if (*end || v == 0 || v > 0xFFFF) return -1;
        return int(v);
    }
    for (int c = 0; c < kPadControls; ++c)
        if (name == kControlNames[c]) return PadSource(c);
    return -1;
}

std::string Key(const std::string& controller) {
    std::string key = "bind.pad.";
    for (char ch : controller) key += (std::isalnum(static_cast<unsigned char>(ch)) ? ch : '_');
    return key;
}

bool Reset() {
    static const bool reset = std::getenv("PS2HOST_BIND_RESET") != nullptr;
    return reset;
}

}  // namespace

int Controls(Kind kind) { return kind == Kind::kKeyboard ? kControls : kPadControls; }

void KeyboardDefault(Map* m) {
    *m = Map{};
    for (int c = 0; c < kControls; ++c) m->src[c][0] = kKeyboardDefault[c];
}

void PadDefault(Map* m) {
    *m = Map{};
    for (int c = 0; c < kPadControls; ++c) m->src[c][0] = PadSource(c);
}

const char* ControlName(int control) { return control >= 0 && control < kControls ? kControlNames[control] : "?"; }

std::string SourceName(Kind kind, u16 src) {
    if (!src) return "none";
    if (kind == Kind::kKeyboard) {
        char buf[16];
        std::snprintf(buf, sizeof buf, "sc%X", unsigned(src));
        return buf;
    }
    return src <= kPadControls ? kControlNames[src - 1] : "none";
}

bool Parse(Kind kind, const std::string& text, const Map& def, Map* out) {
    Map m = def;
    std::size_t at = 0;
    while (at <= text.size()) {
        std::size_t comma = text.find(',', at);
        if (comma == std::string::npos) comma = text.size();
        const std::string item = Trim(text.substr(at, comma - at));
        at = comma + 1;
        if (item.empty()) {
            if (comma == text.size()) break;
            continue;
        }
        const std::size_t colon = item.find(':');
        if (colon == std::string::npos) return false;
        const int control = ControlOf(Trim(item.substr(0, colon)));
        if (control < 0 || control >= Controls(kind)) return false;
        u16 sources[kSources] = {};
        int n = 0;
        std::string rest = Trim(item.substr(colon + 1));
        std::size_t p = 0;
        while (p <= rest.size()) {
            std::size_t plus = rest.find('+', p);
            if (plus == std::string::npos) plus = rest.size();
            const int s = SourceOf(kind, Trim(rest.substr(p, plus - p)));
            if (s < 0) return false;
            if (s && n < kSources) sources[n++] = u16(s);
            p = plus + 1;
            if (plus == rest.size()) break;
        }
        for (int i = 0; i < kSources; ++i) m.src[control][i] = sources[i];
        if (comma == text.size()) break;
    }
    *out = m;
    return true;
}

std::string Format(Kind kind, const Map& m, const Map& def) {
    std::string text;
    for (int c = 0; c < Controls(kind); ++c) {
        if (std::memcmp(m.src[c], def.src[c], sizeof m.src[c]) == 0) continue;
        if (!text.empty()) text += ',';
        text += std::string(kControlNames[c]) + ':';
        bool any = false;
        for (int i = 0; i < kSources; ++i)
            if (m.src[c][i]) {
                if (any) text += '+';
                text += SourceName(kind, m.src[c][i]);
                any = true;
            }
        if (!any) text += "none";
    }
    return text;
}

u32 Duplicates(Kind kind, const Map& m) {
    u32 dup = 0;
    for (int a = 0; a < Controls(kind); ++a)
        for (int b = a + 1; b < Controls(kind); ++b)
            for (int i = 0; i < kSources; ++i)
                for (int j = 0; j < kSources; ++j)
                    if (m.src[a][i] && m.src[a][i] == m.src[b][j]) dup |= (1u << a) | (1u << b);
    return dup;
}

void Set(Map* m, int control, u16 src) {
    if (control < 0 || control >= kControls) return;
    m->src[control][0] = src;
    for (int i = 1; i < kSources; ++i) m->src[control][i] = 0;
}

bool Uses(Kind kind, const Map& m, u16 src) {
    for (int c = 0; c < Controls(kind); ++c)
        for (int i = 0; i < kSources; ++i)
            if (m.src[c][i] == src) return true;
    return false;
}

const Map& KeyboardMap() {
    static Map map;
    static std::string text = "\x01";       // what `map` was made from
    const std::string now = Reset() ? "" : Setting("bind.keyboard");
    if (now != text) {
        Map def;
        KeyboardDefault(&def);
        if (!Parse(Kind::kKeyboard, now, def, &map)) {
            std::fprintf(stderr, "[bind] bind.keyboard \"%s\": not understood - the default\n", now.c_str());
            map = def;
        }
        text = now;
    }
    return map;
}

const Map& PadMap(const std::string& controller) {
    struct Entry { std::string text = "\x01"; Map map; };
    static std::map<std::string, Entry> maps;
    Entry& e = maps[controller];
    const std::string now = Reset() ? "" : Setting(Key(controller));
    if (now != e.text) {
        Map def;
        PadDefault(&def);
        if (!Parse(Kind::kPad, now, def, &e.map)) {
            std::fprintf(stderr, "[bind] %s \"%s\": not understood - the default\n", Key(controller).c_str(), now.c_str());
            e.map = def;
        }
        e.text = now;
    }
    return e.map;
}

void StoreKeyboard(const Map& m) {
    Map def;
    KeyboardDefault(&def);
    SetSetting("bind.keyboard", Format(Kind::kKeyboard, m, def));
}

void StorePad(const std::string& controller, const Map& m) {
    Map def;
    PadDefault(&def);
    SetSetting(Key(controller), Format(Kind::kPad, m, def));
}

void ApplyPad(const Map& m, u16* buttons, u8 pressure[16]) {
    const u16 in = *buttons;
    u8 in_pressure[16];
    std::memcpy(in_pressure, pressure, sizeof in_pressure);
    u16 out = 0;
    for (int c = 0; c < kPadControls; ++c) {
        u8 most = 0;
        for (int i = 0; i < kSources; ++i) {
            const u16 s = m.src[c][i];
            if (!s || s > kPadControls) continue;
            const int b = s - 1;
            if (in & (1u << b)) {
                out |= u16(1u << c);
                most = in_pressure[b] > most ? in_pressure[b] : most;
            }
        }
        pressure[c] = most;
    }
    *buttons = out;
}

}  // namespace bind
}  // namespace ps2host
