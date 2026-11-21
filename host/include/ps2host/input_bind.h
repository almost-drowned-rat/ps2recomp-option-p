/*
 * Button bindings per input device (the ports' CONTROLS > BUTTON MAPPING rule, Orta's model:
 * xboxrecomp_option_c src/input/xinput_bind.h): what each DualShock 2 input of the game is driven by.
 *
 * A device is the keyboard or a controller model (by its name: identical controllers share one binding). A
 * binding maps each control - the 16 DualShock 2 inputs in libpad's order (select l3 r3 start up right down left
 * l2 r2 l1 r1 triangle circle cross square) and, for the keyboard, the left stick's four directions (lleft lright
 * lup ldown) - to up to two sources: a key by its place (a set-1 scan code, 0xE0xx for the extended keys: the
 * arrows) or a controller's button by its place (the DualShock 2 input at that place: south is cross whatever the
 * controller calls it). Sticks of a controller are not bound.
 *
 * The device's own binding always acts; the default layout stays where the PC settings panel reads it (the
 * panel's own navigation is not bound), so a bad binding can be undone, and PS2HOST_BIND_RESET=1 starts with the
 * defaults (the emergency exit).
 *
 * Text form (pc_settings.ini "bind.keyboard", "bind.pad.<controller name>"): "control:source[+source],..." on
 * top of the default, "control:none" unbinds; keyboard sources "sc<hex scan code>", controller sources the
 * DualShock 2 names (cross circle ... as above). Only controls that differ from the default are written.
 */

#ifndef PS2HOST_INPUT_BIND_H
#define PS2HOST_INPUT_BIND_H

#include <string>

#include "ps2rt/types.h"

namespace ps2host {
namespace bind {

enum class Kind { kKeyboard, kPad };
constexpr int kSources = 2;
constexpr int kPadControls = 16;            // the DualShock 2 inputs
constexpr int kControls = 20;               // and the keyboard's left stick: lleft lright lup ldown
constexpr int kStickLeft = 16, kStickRight = 17, kStickUp = 18, kStickDown = 19;
/* Controller sources are the DualShock 2 input + 1 (0 is "none"). */
constexpr u16 PadSource(int input) { return u16(input + 1); }

struct Map {
    u16 src[kControls][kSources] = {};      // 0: an unused slot
};

int Controls(Kind kind);
/* A port's own default layout: controls a and b (DualShock 2 inputs, 0 select ... 3 start) trade their default
   sources on the keyboard and every controller (ZOE: Start and Select - SELECT's menu on the Start button, the
   game's pause on Select; the maintainer 2026-11-21). Before the bindings are first read. */
void SwapDefaults(int a, int b);
void KeyboardDefault(Map* m);
void PadDefault(Map* m);
const char* ControlName(int control);
/* "sc1C" for a key, "cross" for a controller's button, "none". */
std::string SourceName(Kind kind, u16 src);
/* Text on top of `def`: false (out untouched) on an unknown control or source. */
bool Parse(Kind kind, const std::string& text, const Map& def, Map* out);
std::string Format(Kind kind, const Map& m, const Map& def);
/* Bit c: control c shares a source with another control. */
u32 Duplicates(Kind kind, const Map& m);
/* Control c to exactly this source (0: unbound). */
void Set(Map* m, int control, u16 src);
bool Uses(Kind kind, const Map& m, u16 src);

/* The stored bindings (pc_settings.ini; PS2HOST_BIND_RESET=1: the defaults, nothing read). */
const Map& KeyboardMap();
const Map& PadMap(const std::string& controller);
void StoreKeyboard(const Map& m);
void StorePad(const std::string& controller, const Map& m);
/* A controller's buttons (bit n: input n at its place) and their pressures through its binding. */
void ApplyPad(const Map& m, u16* buttons, u8 pressure[16]);

}  // namespace bind
}  // namespace ps2host

#endif  /* PS2HOST_INPUT_BIND_H */
