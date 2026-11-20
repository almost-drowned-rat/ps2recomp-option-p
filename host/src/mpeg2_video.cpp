/*
 * The native MPEG-2 video decoder (mpeg2_video.h; design 7f). ISO/IEC
 * 13818-2: 6.2 (syntax), 7.2-7.6 (VLD, inverse scan, dequantisation, motion
 * compensation). The block layer as the IPU model has it (ipu.cpp's Block:
 * the same tables, the same dequantisation, saturation and mismatch control,
 * the same IDCT), so intra blocks agree with tier E bit for bit; what libmpeg
 * does on the EE - prediction from the reference pictures with the
 * standard's half-sample rounding, the sum saturated to 0-255 - after the
 * standard.
 */

#include "mpeg2_video.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "mpeg_common.h"

namespace ps2host {
namespace mpeg {

namespace {

/* Bits from a byte buffer, the first the highest; past the end, zeros. */
struct Bits {
    const std::uint8_t* data;
    std::size_t size;           // bytes
    std::size_t bit = 0;
    u32 Peek(u32 n) const {     // n <= 32
        u64 v = 0;
        const std::size_t byte = bit >> 3;
        for (int i = 0; i < 5; ++i) v = (v << 8) | (byte + i < size ? data[byte + i] : 0u);
        return u32((v >> (40 - (bit & 7) - n)) & ((u64(1) << n) - 1));
    }
    void Skip(u32 n) { bit += n; }
    u32 Get(u32 n) {
        const u32 v = Peek(n);
        bit += n;
        return v;
    }
    bool Overrun() const { return (bit >> 3) > size; }
    // A VLC from a 16-bit lookup: its value, and false for no such code.
    bool Vlc(const Lookup& t, s32* value) {
        const u32 next = Peek(16);
        const u32 length = t.length[next];
        if (!length) return false;
        *value = t.value[next];
        bit += length;
        return true;
    }
};

// The default intra quantiser matrix (7.3.2), row by row.
constexpr std::uint8_t kDefaultIntra[64] = {
    8,  16, 19, 22, 26, 27, 29, 34, 16, 16, 22, 24, 27, 29, 34, 37, 19, 22, 26, 27, 29, 34, 34, 38,
    22, 22, 26, 27, 29, 34, 37, 40, 22, 26, 27, 29, 32, 35, 40, 48, 26, 27, 29, 32, 35, 40, 48, 58,
    26, 27, 29, 34, 38, 46, 56, 69, 27, 29, 35, 38, 46, 56, 69, 83};

/* The next start code at or after `from` (its first byte, 00 00 01 xx), or npos. */
std::size_t StartCode(const std::vector<std::uint8_t>& s, std::size_t from) {
    for (std::size_t i = from; i + 3 < s.size(); ++i)
        if (s[i] == 0 && s[i + 1] == 0 && s[i + 2] == 1) return i;
    return std::string::npos;
}

}  // namespace

void ToRgb32Macroblocks(const Picture& p, std::uint32_t th0, std::uint32_t th1, std::vector<std::uint32_t>* out) {
    const int mbw = p.width / 16, mbh = p.height / 16;
    out->assign(std::size_t(mbw) * mbh * 256, 0);
    for (int my = 0; my < mbh; ++my)
        for (int mx = 0; mx < mbw; ++mx) {
            // libmpeg's order: by columns (a column's macroblocks top to bottom, then the next column) - read
            // off ZOE's pictures, 2026-11-15.
            std::uint32_t* to = out->data() + (std::size_t(mx) * mbh + my) * 256;
            for (int py = 0; py < 16; ++py)
                for (int px = 0; px < 16; ++px) {
                    const int x = mx * 16 + px, y = my * 16 + py;
                    const int luma = (0x95 * std::max(0, int(p.y[std::size_t(y) * p.width + x]) - 16)) >> 6;
                    const std::size_t ci = std::size_t(y / 2) * (p.width / 2) + x / 2;
                    const int cb = int(p.cb[ci]) - 128, cr = int(p.cr[ci]) - 128;
                    const int red = std::clamp((luma + ((0xCC * cr) >> 6) + 1) >> 1, 0, 255);
                    const int green = std::clamp((luma + ((-0x68 * cr) >> 6) + ((-0x32 * cb) >> 6) + 1) >> 1, 0, 255);
                    const int blue = std::clamp((luma + ((0x102 * cb) >> 6) + 1) >> 1, 0, 255);
                    const auto below = [&](std::uint32_t t) {
                        return red < int(t) && green < int(t) && blue < int(t);
                    };
                    const std::uint32_t alpha = below(th0) ? 0u : below(th1) ? 0x40u : 0x80u;
                    to[py * 16 + px] = std::uint32_t(red) | std::uint32_t(green) << 8 | std::uint32_t(blue) << 16 |
                                       alpha << 24;
                }
        }
}

/* What a picture's decoding keeps between its macroblocks. */
struct VideoDecoder::State {
    int type = 0;
    int f_code[2][2] = {};
    int dc_precision = 0;
    bool q_scale_type = false, intra_vlc_format = false, alternate_scan = false;
    std::uint8_t intra[64] = {}, non_intra[64] = {};     // by position (row * 8 + column)
    s32 dc_pred[3] = {};
    int pmv[2][2] = {};                 // [forward/backward][x/y], half samples
    // The previous macroblock's motion (a B picture's skipped macroblocks repeat it).
    bool last_forward = false, last_backward = false;
    int last_mv[2][2] = {};
    u32 scale_code = 1;
};

void VideoDecoder::Feed(const std::uint8_t* bytes, std::size_t count) {
    // What is parsed already goes, past a margin (a start code's bytes).
    if (at_ > (1u << 20)) {
        stream_.erase(stream_.begin(), stream_.begin() + std::ptrdiff_t(at_));
        base_ += at_;
        at_ = 0;
    }
    stream_.insert(stream_.end(), bytes, bytes + count);
}

void VideoDecoder::Reset() {
    stream_.clear();
    at_ = 0;
    base_ = 0;
    error_.clear();
    have_sequence_ = have_forward_ = have_backward_ = anchor_waiting_ = ended_ = false;
}

bool VideoDecoder::Next(Picture* out, bool end) {
    for (;;) {
        // Headers up to the next picture: sequence (size, matrices), its extension, GOP.
        std::size_t s = StartCode(stream_, at_);
        while (s != std::string::npos && stream_[s + 3] != 0x00) {
            const std::uint8_t code = stream_[s + 3];
            if (code == 0xB3) {
                if (s + 12 > stream_.size()) return false;
                Bits b{stream_.data() + s + 4, stream_.size() - s - 4};
                const int w = int(b.Get(12)), h = int(b.Get(12));
                b.Skip(4 + 4 + 18 + 1 + 10 + 1);        // aspect, frame rate, bit rate, marker, VBV, constrained
                if (b.Get(1)) {
                    for (int i = 0; i < 64; ++i) intra_matrix_[i] = std::uint8_t(b.Get(8));
                } else {
                    for (int i = 0; i < 64; ++i) intra_matrix_[i] = kDefaultIntra[kZigZag[i]];
                }
                if (b.Get(1)) {
                    for (int i = 0; i < 64; ++i) non_intra_matrix_[i] = std::uint8_t(b.Get(8));
                } else {
                    std::fill(non_intra_matrix_, non_intra_matrix_ + 64, std::uint8_t(16));
                }
                if (b.Overrun()) return false;
                if (w != width_ || h != height_) {
                    have_forward_ = have_backward_ = anchor_waiting_ = false;    // a new size: no references
                }
                width_ = w;
                height_ = h;
                ended_ = false;                         // a sequence after an end: on again
                mb_width_ = (w + 15) / 16;
                mb_height_ = (h + 15) / 16;
                have_sequence_ = true;
            } else if (code == 0xB7) {                                          // sequence end: the last anchor
                at_ = s + 4;
                ended_ = true;                          // (with the anchor out, too: nothing follows it)
                if (anchor_waiting_) {
                    anchor_waiting_ = false;
                    *out = backward_;
                    return true;
                }
                return false;
            }
            s = StartCode(stream_, s + 4);
        }
        if (s == std::string::npos) {
            if (end && anchor_waiting_) {
                anchor_waiting_ = false;
                *out = backward_;
                return true;
            }
            return false;
        }
        // The picture runs to the next picture, GOP, sequence header or end (its extensions and slices between).
        std::size_t e = StartCode(stream_, s + 4);
        while (e != std::string::npos) {
            const std::uint8_t code = stream_[e + 3];
            if (code == 0x00 || code == 0xB3 || code == 0xB7 || code == 0xB8) break;
            e = StartCode(stream_, e + 4);
        }
        bool tentative = false;
        if (e == std::string::npos) {
            // No start code after it yet: a picture is whole when its last macroblock is (as libmpeg has it - the
            // game demuxes the stream's next part only once the picture is out). Tried on what is here; taken only
            // if every macroblock decoded inside the data.
            e = stream_.size();
            tentative = !end;
        }
        if (tentative) {
            if (!have_sequence_) return false;
            Picture trial;
            const std::string before = error_;
            if (!DecodePicture(s, e, &trial, true)) {
                error_ = before;
                return false;                           // more stream needed
            }
            // On from just before the end: a start code that has begun to arrive (00 00) is found whole later.
            at_ = std::max(s + 4, e >= 3 ? e - 3 : e);
            ++decoded_count_;
            if (on_picture_) on_picture_(base_ + s, trial);
            if (trial.y.empty()) continue;
            Picture decoded = std::move(trial);
            if (decoded.type == 3) {
                if (!have_forward_ || !have_backward_) continue;
                *out = std::move(decoded);
                return true;
            }
            const bool show = anchor_waiting_;
            if (have_backward_) {
                forward_ = std::move(backward_);
                have_forward_ = true;
            }
            backward_ = std::move(decoded);
            have_backward_ = true;
            anchor_waiting_ = true;
            if (show) {
                *out = forward_;
                return true;
            }
            continue;
        }
        at_ = e;
        if (!have_sequence_) continue;                  // pictures before the first sequence header: skipped
        Picture decoded;
        if (!DecodePicture(s, e, &decoded)) {
            // A picture the stream breaks off in (a restart: libmpeg cleared the IPU and fed it again) or one
            // not covered: counted and skipped, the references kept.
            ++errors_;
            if (std::getenv("PS2HOST_MPEG_DEBUG"))
                std::fprintf(stderr, "mpeg: picture at %zu skipped: %s\n", s, error_.c_str());
            continue;
        }
        ++decoded_count_;
        if (on_picture_) on_picture_(base_ + s, decoded);
        if (decoded.y.empty()) continue;                // nothing to predict from yet: skipped
        if (decoded.type == 3) {
            if (!have_forward_ || !have_backward_) continue;    // a B picture without its references (an open GOP's start)
            *out = std::move(decoded);
            return true;
        }
        // An anchor: the one before it is shown now.
        const bool show = anchor_waiting_;
        if (have_backward_) {
            forward_ = std::move(backward_);
            have_forward_ = true;
        }
        backward_ = std::move(decoded);
        have_backward_ = true;
        anchor_waiting_ = true;
        if (show) {
            *out = forward_;
            return true;
        }
    }
}

bool VideoDecoder::DecodePicture(std::size_t start, std::size_t end, Picture* p, bool whole) {
    int last_address = -1;                              // the last macroblock decoded (whole's check)
    bool overrun = false;
    State st;
    Bits b{stream_.data() + start + 4, end - start - 4};
    p->temporal_reference = int(b.Get(10));
    st.type = int(b.Get(3));
    b.Skip(16);                                         // vbv_delay
    if (st.type == 2 || st.type == 3) b.Skip(4);        // full_pel_forward_vector, forward_f_code
    if (st.type == 3) b.Skip(4);
    while (b.Get(1)) b.Skip(8);                         // extra_information_picture
    if (st.type < 1 || st.type > 3) {
        error_ = "picture coding type " + std::to_string(st.type) + " (D pictures not covered)";
        return false;
    }
    // Its picture coding extension (MPEG-2): the parameters the slices need.
    bool extension = false;
    for (std::size_t s = StartCode(stream_, start + 4); s != std::string::npos && s < end;
         s = StartCode(stream_, s + 4)) {
        if (stream_[s + 3] != 0xB5) continue;
        Bits x{stream_.data() + s + 4, end - s - 4};
        const u32 id = x.Get(4);
        if (id == 8) {
            st.f_code[0][0] = int(x.Get(4));
            st.f_code[0][1] = int(x.Get(4));
            st.f_code[1][0] = int(x.Get(4));
            st.f_code[1][1] = int(x.Get(4));
            st.dc_precision = int(x.Get(2));
            const u32 structure = x.Get(2);
            x.Skip(1);                                  // top_field_first
            const u32 frame_pred_frame_dct = x.Get(1), concealment = x.Get(1);
            st.q_scale_type = x.Get(1) != 0;
            st.intra_vlc_format = x.Get(1) != 0;
            st.alternate_scan = x.Get(1) != 0;
            if (structure != 3 || !frame_pred_frame_dct || concealment) {
                error_ = "picture structure " + std::to_string(structure) + ", frame_pred_frame_dct " +
                         std::to_string(frame_pred_frame_dct) + ", concealment " + std::to_string(concealment) +
                         " (only frame pictures with frame prediction are covered)";
                return false;
            }
            extension = true;
        } else if (id == 3) {                           // quant matrix extension: intra, non-intra
            if (x.Get(1))
                for (int i = 0; i < 64; ++i) intra_matrix_[i] = std::uint8_t(x.Get(8));
            if (x.Get(1))
                for (int i = 0; i < 64; ++i) non_intra_matrix_[i] = std::uint8_t(x.Get(8));
        }
    }
    if (!extension) {
        error_ = "no picture coding extension (MPEG-1 is not covered)";
        return false;
    }
    if (st.type != 1 && !have_backward_) {
        if (whole) return false;                        // (decided once its end is here)
        error_.clear();
        p->type = st.type;                              // a P or B picture before any I: nothing to predict from
        return true;
    }
    // The matrices by position (transmitted in zig-zag order whatever the scan).
    for (int i = 0; i < 64; ++i) {
        st.intra[kZigZag[i]] = intra_matrix_[i];
        st.non_intra[kZigZag[i]] = non_intra_matrix_[i];
    }
    const int w = mb_width_ * 16, h = mb_height_ * 16;
    p->width = w;
    p->height = h;
    p->shown_width = width_;
    p->shown_height = height_;
    p->type = st.type;
    p->y.assign(std::size_t(w) * h, 0);
    p->cb.assign(std::size_t(w / 2) * (h / 2), 128);
    p->cr.assign(std::size_t(w / 2) * (h / 2), 128);
    // The references: a P picture's forward is the newest anchor; a B picture's forward the older, backward the newer.
    const Picture* fwd = st.type == 2 ? &backward_ : &forward_;
    const Picture* bwd = &backward_;
    const u8* scan = st.alternate_scan ? kAlternate : kZigZag;

    // One block: decoded, dequantised, mismatch-controlled, through the IDCT (ipu.cpp's Block, MPEG-2 only).
    auto block = [&](Bits& r, bool intra, int component, u32 scale, s32 pixels[64]) -> bool {
        s32 coefficients[64] = {};
        int index = 0;
        if (intra) {
            s32 size = 0;
            if (!r.Vlc(component == 0 ? DcLuminance() : DcChrominance(), &size)) return false;
            s32 differential = 0;
            if (size) {
                const u32 bits = r.Get(u32(size));
                differential = (bits >> (size - 1)) ? s32(bits) : s32(bits) - (1 << size) + 1;
            }
            st.dc_pred[component] += differential;
            coefficients[0] = st.dc_pred[component] << (3 - st.dc_precision);
            index = 1;
        }
        const Lookup& table = (intra && st.intra_vlc_format) ? CoefficientsB15() : CoefficientsB14();
        const std::uint8_t* matrix = intra ? st.intra : st.non_intra;
        bool first = !intra;
        for (;;) {
            s32 run, level;
            if (first && r.Peek(1)) {
                r.Skip(1);
                run = 0;
                level = r.Get(1) ? -1 : 1;
            } else {
                s32 value = 0;
                if (!r.Vlc(table, &value)) return false;
                if (value == kEndOfBlock) break;
                if (value == kEscape) {
                    run = s32(r.Get(6));
                    level = s32(r.Get(12) << 20) >> 20;
                } else {
                    run = value >> 8;
                    level = value & 0xFF;
                    if (r.Get(1)) level = -level;
                }
            }
            first = false;
            index += run;
            if (index > 63) return false;
            const int position = scan[index];
            const s32 magnitude = level < 0 ? -level : level;
            s32 v = intra ? (magnitude * s32(scale) * matrix[position]) >> 4
                          : ((2 * magnitude + 1) * s32(scale) * matrix[position]) >> 5;
            if (level < 0) v = -v;
            coefficients[position] = std::clamp(v, -2048, 2047);
            ++index;
        }
        s32 sum = 0;
        for (int i = 0; i < 64; ++i) sum += coefficients[i];
        if ((sum & 1) == 0) coefficients[63] ^= 1;
        Idct(coefficients, pixels);
        return true;
    };

    // A reference's sample, clamped into the picture (the standard keeps vectors inside; a broken one stays safe).
    auto sample = [](const std::vector<std::uint8_t>& plane, int pw, int ph, int x, int y) -> int {
        x = std::clamp(x, 0, pw - 1);
        y = std::clamp(y, 0, ph - 1);
        return plane[std::size_t(y) * pw + x];
    };
    // A size x size prediction from `plane` at (x, y) moved by (mx, my) half samples (7.6.4: half-sample
    // averages rounded up).
    auto predict = [&](const std::vector<std::uint8_t>& plane, int pw, int ph, int x, int y, int mx, int my,
                       int size, int out[256]) {
        const int ix = x + (mx >> 1), iy = y + (my >> 1), hx = mx & 1, hy = my & 1;
        static const bool debug = std::getenv("PS2HOST_MPEG_DEBUG") != nullptr;
        if (debug && (ix < 0 || iy < 0 || ix + size + hx > pw || iy + size + hy > ph))
            std::fprintf(stderr, "mpeg: a prediction past the picture: block %d at (%d,%d), vector (%d,%d) half samples, "
                                 "plane %dx%d\n", size, x, y, mx, my, pw, ph);
        for (int r = 0; r < size; ++r)
            for (int c = 0; c < size; ++c) {
                const int a = sample(plane, pw, ph, ix + c, iy + r);
                int v;
                if (!hx && !hy) {
                    v = a;
                } else if (hx && !hy) {
                    v = (a + sample(plane, pw, ph, ix + c + 1, iy + r) + 1) >> 1;
                } else if (!hx && hy) {
                    v = (a + sample(plane, pw, ph, ix + c, iy + r + 1) + 1) >> 1;
                } else {
                    v = (a + sample(plane, pw, ph, ix + c + 1, iy + r) + sample(plane, pw, ph, ix + c, iy + r + 1) +
                         sample(plane, pw, ph, ix + c + 1, iy + r + 1) + 2) >> 2;
                }
                out[r * size + c] = v;
            }
    };
    // The macroblock at (mx, my)'s prediction into its planes: forward, backward or both (averaged, rounded up).
    auto motion = [&](int mbx, int mby, bool forward, bool backward, const int mv[2][2]) {
        int luma[2][256], chroma_b[2][256], chroma_r[2][256];
        int n = 0;
        for (int d = 0; d < 2; ++d) {
            if (!(d == 0 ? forward : backward)) continue;
            const Picture& ref = d == 0 ? *fwd : *bwd;
            const int vx = mv[d][0], vy = mv[d][1];
            predict(ref.y, w, h, mbx * 16, mby * 16, vx, vy, 16, luma[n]);
            // 4:2:0 chroma: the vector halved, towards zero (7.6.3.7).
            const int cx = vx / 2, cy = vy / 2;
            predict(ref.cb, w / 2, h / 2, mbx * 8, mby * 8, cx, cy, 8, chroma_b[n]);
            predict(ref.cr, w / 2, h / 2, mbx * 8, mby * 8, cx, cy, 8, chroma_r[n]);
            ++n;
        }
        for (int r = 0; r < 16; ++r)
            for (int c = 0; c < 16; ++c) {
                const int v = n == 2 ? (luma[0][r * 16 + c] + luma[1][r * 16 + c] + 1) >> 1 : luma[0][r * 16 + c];
                p->y[std::size_t(mby * 16 + r) * w + mbx * 16 + c] = std::uint8_t(v);
            }
        for (int r = 0; r < 8; ++r)
            for (int c = 0; c < 8; ++c) {
                const int i = r * 8 + c;
                const int vb = n == 2 ? (chroma_b[0][i] + chroma_b[1][i] + 1) >> 1 : chroma_b[0][i];
                const int vr = n == 2 ? (chroma_r[0][i] + chroma_r[1][i] + 1) >> 1 : chroma_r[0][i];
                p->cb[std::size_t(mby * 8 + r) * (w / 2) + mbx * 8 + c] = std::uint8_t(vb);
                p->cr[std::size_t(mby * 8 + r) * (w / 2) + mbx * 8 + c] = std::uint8_t(vr);
            }
    };
    // A motion vector component (7.6.3.1): the predictor moved by the coded difference, wrapped into range.
    auto vector = [&](Bits& r, int s, int t) -> bool {
        const int r_size = st.f_code[s][t] - 1, f = 1 << r_size;
        s32 code = 0;
        if (!r.Vlc(MotionCode(), &code)) return false;
        int delta = 0;
        if (code) {
            const bool negative = r.Get(1) != 0;
            const int residual = r_size > 0 ? int(r.Get(u32(r_size))) : 0;
            delta = ((code - 1) << r_size) + residual + 1;
            if (negative) delta = -delta;
        }
        int v = st.pmv[s][t] + delta;
        const int low = -16 * f, high = 16 * f - 1, range = 32 * f;
        if (v < low) v += range;
        else if (v > high) v -= range;
        st.pmv[s][t] = v;
        return true;
    };
    auto reset_dc = [&] { st.dc_pred[0] = st.dc_pred[1] = st.dc_pred[2] = 128 << st.dc_precision; };

    for (std::size_t s = StartCode(stream_, start + 4); s != std::string::npos && s < end;
         s = StartCode(stream_, s + 4)) {
        const std::uint8_t code = stream_[s + 3];
        if (code < 0x01 || code > 0xAF) continue;
        Bits r{stream_.data() + s + 4, end - s - 4};
        const int row = code - 1;
        st.scale_code = r.Get(5);
        while (r.Get(1)) r.Skip(8);                     // extra_information_slice
        reset_dc();
        st.pmv[0][0] = st.pmv[0][1] = st.pmv[1][0] = st.pmv[1][1] = 0;
        int address = row * mb_width_ - 1;
        bool first = true;
        while (r.Peek(23) != 0 && (r.bit >> 3) < r.size) {
            int increment = 0;
            for (;;) {
                s32 v = 0;
                if (!r.Vlc(AddressIncrement(), &v)) {
                    error_ = "macroblock address increment";
                    return false;
                }
                if (v == 0x22) continue;                // stuffing
                if (v == 0x23) {
                    increment += 33;
                    continue;
                }
                increment += v;
                break;
            }
            if (!first) {
                // Skipped macroblocks: P copies the forward reference (vector 0, predictors reset); B repeats the
                // macroblock before's prediction. No coefficients either way; DC predictors reset.
                for (int k = 1; k < increment; ++k) {
                    const int a = address + k;
                    if (a >= mb_width_ * mb_height_) break;
                    if (st.type == 2) {
                        const int zero[2][2] = {};
                        st.pmv[0][0] = st.pmv[0][1] = 0;
                        motion(a % mb_width_, a / mb_width_, true, false, zero);
                    } else if (st.type == 3) {
                        motion(a % mb_width_, a / mb_width_, st.last_forward, st.last_backward, st.last_mv);
                    }
                    reset_dc();
                }
                address += increment;
            } else {
                address += increment;
                first = false;
            }
            if (address >= mb_width_ * mb_height_) {
                error_ = "macroblock address past the picture";
                return false;
            }
            const int mbx = address % mb_width_, mby = address / mb_width_;
            s32 type = 0;
            if (!r.Vlc(MacroblockType(u32(st.type)), &type)) {
                error_ = "macroblock type";
                return false;
            }
            const bool quant = type & 0x10, forward = type & 8, backward = type & 4, pattern = type & 2,
                       intra = type & 1;
            if (quant) st.scale_code = r.Get(5);
            int mv[2][2] = {};
            if (forward) {
                if (!vector(r, 0, 0) || !vector(r, 0, 1)) { error_ = "motion vector"; return false; }
                mv[0][0] = st.pmv[0][0];
                mv[0][1] = st.pmv[0][1];
            }
            if (backward) {
                if (!vector(r, 1, 0) || !vector(r, 1, 1)) { error_ = "motion vector"; return false; }
                mv[1][0] = st.pmv[1][0];
                mv[1][1] = st.pmv[1][1];
            }
            if (intra) {
                st.pmv[0][0] = st.pmv[0][1] = st.pmv[1][0] = st.pmv[1][1] = 0;
            } else {
                reset_dc();
                if (st.type == 2 && !forward) st.pmv[0][0] = st.pmv[0][1] = 0;   // P "no MC": vector 0
            }
            u32 cbp = intra ? 63u : 0u;
            if (pattern) {
                s32 v = 0;
                if (!r.Vlc(BlockPattern(), &v)) { error_ = "coded block pattern"; return false; }
                cbp = u32(v);
            }
            // PS2HOST_MPEG_TRACE=<row>: each macroblock of that row from column 24 on (a difference's search).
            static const int trace_row = std::getenv("PS2HOST_MPEG_TRACE") ? std::atoi(std::getenv("PS2HOST_MPEG_TRACE")) : -1;
            if (mby == trace_row && mbx >= 24)
                std::fprintf(stderr, "mpeg-mb: picture %d (type %d, tref %d) mb %d,%d type 0x%02X cbp %02X q %u mv f(%d,%d) b(%d,%d) "
                                     "after %d skipped\n", decoded_count_, st.type, p->temporal_reference, mbx, mby,
                             unsigned(type), unsigned(cbp), unsigned(st.scale_code), mv[0][0], mv[0][1], mv[1][0],
                             mv[1][1], increment - 1);
            const u32 scale = st.q_scale_type ? kNonLinearScale[st.scale_code] : st.scale_code << 1;
            if (!intra) {
                const bool f = forward || st.type == 2;         // P without a vector: forward, vector 0
                motion(mbx, mby, f, backward, mv);
                st.last_forward = f;
                st.last_backward = backward;
                std::memcpy(st.last_mv, mv, sizeof mv);
            }
            for (int bl = 0; bl < 6; ++bl) {
                if (!(cbp & (32u >> bl))) continue;
                s32 pixels[64];
                if (!block(r, intra, bl < 4 ? 0 : bl - 3, scale, pixels)) {
                    error_ = "block " + std::to_string(bl) + " of macroblock " + std::to_string(address) + " (row " +
                             std::to_string(row) + "), picture type " + std::to_string(st.type) +
                             (intra ? ", intra" : "") + ", " + std::to_string(r.bit >> 3) + " of the slice's " +
                             std::to_string(r.size) + " bytes";
                    return false;
                }
                std::vector<std::uint8_t>& plane = bl < 4 ? p->y : bl == 4 ? p->cb : p->cr;
                const int pw = bl < 4 ? w : w / 2;
                const int x0 = bl < 4 ? mbx * 16 + (bl & 1) * 8 : mbx * 8;
                const int y0 = bl < 4 ? mby * 16 + (bl >> 1) * 8 : mby * 8;
                for (int rr = 0; rr < 8; ++rr)
                    for (int c = 0; c < 8; ++c) {
                        std::uint8_t& at = plane[std::size_t(y0 + rr) * pw + x0 + c];
                        const int v = intra ? pixels[rr * 8 + c] : at + pixels[rr * 8 + c];
                        at = std::uint8_t(std::clamp(v, 0, 255));
                    }
            }
            last_address = address;
            if (r.bit > r.size * 8) overrun = true;     // read past what is here (zeros): not whole yet
        }
    }
    if (whole && (overrun || last_address != mb_width_ * mb_height_ - 1)) {
        error_ = "picture not whole yet";
        return false;
    }
    return true;
}

}  // namespace mpeg
}  // namespace ps2host
