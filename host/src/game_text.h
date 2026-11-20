// Lines of text drawn into the game's picture (a port's own menu page in the game's look): Liberation Sans Bold
// (SIL Open Font License 1.1, engine/data/fonts, LICENSES/LiberationFonts-OFL.txt) built into the executable and
// rasterised by stb_truetype (MIT or public domain) - the same letters on every system.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ps2host/run.h"

namespace ps2host {

// The texts drawn into a `width` x `height` RGBA picture (0xAARRGGBB, transparent where nothing is), each at its
// place in fractions of the picture: its capitals `height` tall, their top at y; its left edge at x (or its right
// edge, `right`), with a dark edge a twelfth of the capitals' height down and right - as the game's own menu
// lettering. The picture's pixels are taken as square. False when the font cannot be read.
bool DrawGameTexts(const std::vector<GameText>& texts, std::uint32_t width, std::uint32_t height,
                   std::vector<std::uint32_t>* out);

// A text's width in fractions of the picture's width (the same picture size and capitals' height).
float GameTextWidth(const std::string& text, float height, std::uint32_t width, std::uint32_t picture_height,
                    float stretch = 1.0f);

}  // namespace ps2host
