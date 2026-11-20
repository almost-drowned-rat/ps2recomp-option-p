/*
 * The host's key codes (the keyboard the controller, the PC settings panel and
 * --key read): Windows' virtual-key numbers, by name, so that no file outside
 * the platform layer needs <windows.h> for them. Another system keeps the
 * numbers (they are only names here; the SDL window maps them to its scan
 * codes, window.cpp) and implements the three calls below.
 *
 * A binding (input_bind.h) stores a key as a set-1 scan code (0xE0xx the
 * extended ones), the same on every keyboard layout; KeyOfScanCode turns it
 * into the key code for the layout in use.
 */

#ifndef PS2PLATFORM_KEYS_H
#define PS2PLATFORM_KEYS_H

#include <cstdint>
#include <string>

namespace ps2platform {
namespace key {
inline constexpr int kBack = 0x08, kTab = 0x09, kReturn = 0x0D, kEscape = 0x1B, kSpace = 0x20;
inline constexpr int kPrior = 0x21, kNext = 0x22, kEnd = 0x23, kHome = 0x24;
inline constexpr int kLeft = 0x25, kUp = 0x26, kRight = 0x27, kDown = 0x28;
inline constexpr int kInsert = 0x2D, kDelete = 0x2E, kCapital = 0x14;
inline constexpr int kNumpad0 = 0x60, kNumpad1 = 0x61, kNumpad9 = 0x69;
inline constexpr int kMultiply = 0x6A, kAdd = 0x6B, kSubtract = 0x6D, kDecimal = 0x6E, kDivide = 0x6F;
inline constexpr int kF1 = 0x70, kF9 = 0x78, kF12 = 0x7B;
inline constexpr int kLShift = 0xA0, kRShift = 0xA1, kLControl = 0xA2, kRControl = 0xA3, kLMenu = 0xA4, kRMenu = 0xA5;
inline constexpr int kOem1 = 0xBA, kOemPlus = 0xBB, kOemComma = 0xBC, kOemMinus = 0xBD, kOemPeriod = 0xBE;
inline constexpr int kOem2 = 0xBF, kOem3 = 0xC0, kOem4 = 0xDB, kOem5 = 0xDC, kOem6 = 0xDD, kOem7 = 0xDE;
// Letters and digits are their ASCII capitals: 'A'..'Z', '0'..'9'.
}  // namespace key

// A set-1 scan code (0xE0xx: extended) as the key code in this keyboard layout; 0 if none.
int KeyOfScanCode(std::uint16_t scan_code);
// A scan code's key as this layout names it ("Enter", "Ä"); false where the system cannot say.
bool ScanCodeName(std::uint16_t scan_code, std::string* name);
// Whether a key is held now, read from the system (the GDI window's way, without SDL's events).
bool SystemKeyDown(int key);

}  // namespace ps2platform

#endif
