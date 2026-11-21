// Lines of text drawn into the game's picture (game_text.h).

#include "game_text.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

namespace ps2host {
namespace {

// engine/data/fonts/LiberationSans-Bold.ttf, built in (host/CMakeLists.txt: liberation_sans_bold.inc).
const unsigned char kFont[] = {
#include "liberation_sans_bold.inc"
};

struct Font {
    stbtt_fontinfo info{};
    bool ok = false;
    float cap = 0;              // the capitals' height in font units
    Font() {
        if (!stbtt_InitFont(&info, kFont, stbtt_GetFontOffsetForIndex(kFont, 0))) return;
        int x0, y0, x1, y1;
        // The capitals' height: the H's box.
        if (stbtt_GetCodepointBox(&info, 'H', &x0, &y0, &x1, &y1)) cap = float(y1);
        ok = cap > 0;
    }
};

const Font& TheFont() {
    static const Font font;
    return font;
}

// A port's second font for the letters Liberation Sans has not (SetGameTextFallbackFont: ZOE's Japanese subtitles,
// Noto Sans JP), scaled to the same em.
struct Fallback {
    stbtt_fontinfo info{};
    bool ok = false;
    float em_ratio = 1.0f;      // its units to Liberation's (the same em size)
};
Fallback& TheFallback() {
    static Fallback f;
    return f;
}

// The font a code point is drawn with, and the scale factor to Liberation's units.
const stbtt_fontinfo& FontOf(const Font& f, int cp, float* ratio) {
    const Fallback& fb = TheFallback();
    *ratio = 1.0f;
    if (fb.ok && cp >= 0x80 && !stbtt_FindGlyphIndex(&f.info, cp) && stbtt_FindGlyphIndex(&fb.info, cp)) {
        *ratio = fb.em_ratio;
        return fb.info;
    }
    return f.info;
}

// UTF-8 to code points (a malformed byte stands for itself).
std::vector<int> CodePoints(const std::string& s) {
    std::vector<int> out;
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        const int n = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
        if (n < 0 || i + std::size_t(n) >= s.size() + (n ? 0 : 1)) {
            out.push_back(c);
            ++i;
            continue;
        }
        int cp = n == 0 ? c : n == 1 ? (c & 0x1F) : n == 2 ? (c & 0x0F) : (c & 0x07);
        bool good = true;
        for (int k = 1; k <= n; ++k) {
            const unsigned char d = static_cast<unsigned char>(s[i + k]);
            if ((d & 0xC0) != 0x80) good = false;
            cp = (cp << 6) | (d & 0x3F);
        }
        if (!good) {
            out.push_back(c);
            ++i;
            continue;
        }
        out.push_back(cp);
        i += std::size_t(n) + 1;
    }
    return out;
}

// The text's advance in pixels at `scale` across.
float Advance(const Font& f, const std::vector<int>& cps, float scale) {
    float x = 0;
    for (std::size_t i = 0; i < cps.size(); ++i) {
        int advance, bearing;
        float ratio;
        const stbtt_fontinfo& info = FontOf(f, cps[i], &ratio);
        stbtt_GetCodepointHMetrics(&info, cps[i], &advance, &bearing);
        x += advance * scale * ratio;
        if (i + 1 < cps.size() && &info == &f.info) x += stbtt_GetCodepointKernAdvance(&f.info, cps[i], cps[i + 1]) * scale;
    }
    return x;
}

// One text's coverage added into `cover` (w x h, 0..255), its pen starting at (x, baseline); `sx` / `sy` the
// scales across and down.
void Coverage(const Font& f, const std::vector<int>& cps, float sx, float sy, float x, float baseline,
              std::vector<unsigned char>& cover, int w, int h) {
    std::vector<unsigned char> glyph;
    const float scale = sx;
    for (std::size_t i = 0; i < cps.size(); ++i) {
        int advance, bearing;
        float ratio;
        const stbtt_fontinfo& info = FontOf(f, cps[i], &ratio);
        const float gsx = sx * ratio, gsy = sy * ratio;
        stbtt_GetCodepointHMetrics(&info, cps[i], &advance, &bearing);
        const float shift_x = x - std::floor(x), shift_y = baseline - std::floor(baseline);
        int x0, y0, x1, y1;
        stbtt_GetCodepointBitmapBoxSubpixel(&info, cps[i], gsx, gsy, shift_x, shift_y, &x0, &y0, &x1, &y1);
        const int gw = x1 - x0, gh = y1 - y0;
        if (gw > 0 && gh > 0) {
            glyph.assign(std::size_t(gw) * gh, 0);
            stbtt_MakeCodepointBitmapSubpixel(&info, glyph.data(), gw, gh, gw, gsx, gsy, shift_x, shift_y, cps[i]);
            const int ox = int(std::floor(x)) + x0, oy = int(std::floor(baseline)) + y0;
            for (int gy = 0; gy < gh; ++gy) {
                const int py = oy + gy;
                if (py < 0 || py >= h) continue;
                for (int gx = 0; gx < gw; ++gx) {
                    const int px = ox + gx;
                    if (px < 0 || px >= w) continue;
                    unsigned char& c = cover[std::size_t(py) * w + px];
                    c = static_cast<unsigned char>(std::min(255, c + glyph[std::size_t(gy) * gw + gx]));
                }
            }
        }
        x += advance * scale * ratio;
        if (i + 1 < cps.size() && &info == &f.info) x += stbtt_GetCodepointKernAdvance(&f.info, cps[i], cps[i + 1]) * scale;
    }
}

// A port's mask (GameText::mask) into its rectangle, sampled bilinear (texel centres as the GS's), in its colour
// over what is there.
void DrawMask(const GameText& t, int w, int h, std::vector<std::uint32_t>* out) {
    const int mw = int(t.mask_w), mh = int(t.mask_h);
    if (mw <= 0 || mh <= 0 || t.mask.size() < std::size_t(mw) * mh || t.width <= 0 || t.height <= 0) return;
    const float x0 = t.x * w, y0 = t.y * h, rw = t.width * w, rh = t.height * h;
    auto at = [&](int x, int y) -> float {
        if (x < 0 || y < 0 || x >= mw || y >= mh) return 0.0f;
        return t.mask[std::size_t(y) * mw + x] / 255.0f;
    };
    const std::uint32_t rgb = t.colour & 0xFFFFFFu;
    for (int py = std::max(0, int(y0)); py < std::min(h, int(y0 + rh) + 2); ++py) {
        for (int px = std::max(0, int(x0)); px < std::min(w, int(x0 + rw) + 2); ++px) {
            const float u = (px + 0.5f - x0) / rw * mw - 0.5f, v = (py + 0.5f - y0) / rh * mh - 0.5f;
            const int ix = int(std::floor(u)), iy = int(std::floor(v));
            const float fx = u - ix, fy = v - iy;
            const float a = (at(ix, iy) * (1 - fx) + at(ix + 1, iy) * fx) * (1 - fy) +
                            (at(ix, iy + 1) * (1 - fx) + at(ix + 1, iy + 1) * fx) * fy;
            if (a <= 0) continue;
            std::uint32_t& p = (*out)[std::size_t(py) * w + px];
            const float under = (p >> 24) / 255.0f, total = a + under * (1.0f - a);
            auto mix = [&](std::uint32_t top, std::uint32_t below) {
                return std::uint32_t((top * a + below * under * (1.0f - a)) / std::max(total, 1e-6f) + 0.5f);
            };
            p = (std::uint32_t(total * 255.0f + 0.5f) << 24) | (mix((rgb >> 16) & 0xFF, (p >> 16) & 0xFF) << 16) |
                (mix((rgb >> 8) & 0xFF, (p >> 8) & 0xFF) << 8) | mix(rgb & 0xFF, p & 0xFF);
        }
    }
}

}  // namespace

void SetGameTextFallbackFont(const unsigned char* data, std::size_t size) {
    Fallback& fb = TheFallback();
    const Font& f = TheFont();
    fb.ok = false;
    if (!data || size < 12 || !f.ok) return;
    if (!stbtt_InitFont(&fb.info, data, stbtt_GetFontOffsetForIndex(data, 0))) return;
    // (the same em: pixels per unit of each at one pixel's em)
    fb.em_ratio = stbtt_ScaleForMappingEmToPixels(&fb.info, 1.0f) / stbtt_ScaleForMappingEmToPixels(&f.info, 1.0f);
    fb.ok = true;
}

float GameTextWidth(const std::string& text, float height, std::uint32_t width, std::uint32_t picture_height,
                    float stretch) {
    const Font& f = TheFont();
    if (!f.ok || !width) return 0;
    const float scale = height * float(picture_height) / f.cap * (stretch > 0 ? stretch : 1.0f);
    return Advance(f, CodePoints(text), scale) / float(width);
}

bool DrawGameTexts(const std::vector<GameText>& texts, std::uint32_t width, std::uint32_t height,
                   std::vector<std::uint32_t>* out) {
    const Font& f = TheFont();
    if (!f.ok || !width || !height) return false;
    const int w = int(width), h = int(height);
    out->assign(std::size_t(w) * h, 0);
    std::vector<unsigned char> cover(std::size_t(w) * h), edge(std::size_t(w) * h);
    for (const GameText& t : texts) {
        if (!t.mask.empty()) {
            DrawMask(t, w, h, out);
            continue;
        }
        if (t.text.empty() || t.height <= 0) continue;
        const std::vector<int> cps = CodePoints(t.text);
        const float sy = t.height * float(h) / f.cap, sx = sy * (t.stretch > 0 ? t.stretch : 1.0f);
        const float advance = Advance(f, cps, sx);
        const float left = t.right ? t.x * w - advance : t.centre ? t.x * w - advance * 0.5f : t.x * w;
        const float baseline = (t.y + t.height) * h;
        const float off = std::max(1.0f, t.height * h / 12.0f);
        std::fill(cover.begin(), cover.end(), 0);
        std::fill(edge.begin(), edge.end(), 0);
        Coverage(f, cps, sx, sy, left, baseline, cover, w, h);
        Coverage(f, cps, sx, sy, left + off, baseline + off, edge, w, h);
        const std::uint32_t rgb = t.colour & 0xFFFFFFu;
        for (std::size_t i = 0; i < cover.size(); ++i) {
            if (!cover[i] && !edge[i]) continue;
            // The letters over their dark edge: alpha = coverage + edge's (under the letters), colour mixed.
            const float a = cover[i] / 255.0f, e = edge[i] / 255.0f * 0.85f;
            const float alpha = a + e * (1.0f - a);
            if (alpha <= 0) continue;
            const float k = a / alpha;     // the share of the letters' colour (the edge is black)
            const std::uint32_t r = std::uint32_t(((rgb >> 16) & 0xFF) * k + 0.5f);
            const std::uint32_t g = std::uint32_t(((rgb >> 8) & 0xFF) * k + 0.5f);
            const std::uint32_t b = std::uint32_t((rgb & 0xFF) * k + 0.5f);
            std::uint32_t& p = (*out)[i];
            const float under = (p >> 24) / 255.0f;     // a text drawn before, under this one
            const float total = alpha + under * (1.0f - alpha);
            auto mix = [&](std::uint32_t top, std::uint32_t below) {
                return std::uint32_t((top * alpha + below * under * (1.0f - alpha)) / std::max(total, 1e-6f) + 0.5f);
            };
            p = (std::uint32_t(total * 255.0f + 0.5f) << 24) | (mix(r, (p >> 16) & 0xFF) << 16) |
                (mix(g, (p >> 8) & 0xFF) << 8) | mix(b, p & 0xFF);
        }
    }
    return true;
}

}  // namespace ps2host
