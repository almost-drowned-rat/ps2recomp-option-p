/*
 * The native MPEG-2 video decoder (design 7f, tier N): an elementary stream
 * in, pictures out in display order as Y, Cb, Cr planes (4:2:0). What
 * libmpeg 2000 asks of the IPU and does on the EE besides - headers, motion
 * compensation, reordering - done here; the coefficient VLCs, dequantisation
 * and the IDCT are mpeg_common.h's, the IPU model's own (an intra block bit
 * for bit as tier E has it).
 *
 * Covered: MPEG-2 frame pictures with frame prediction and frame DCT
 * (frame_pred_frame_dct = 1), 4:2:0, I/P/B - what every ZOE US movie is
 * (all 603 pictures of the boot stream, 2026-11-15). Anything else (field
 * pictures, field/dual-prime motion, MPEG-1) is refused with an error: the
 * caller keeps tier E (libmpeg + the IPU model) for that stream.
 */

#ifndef PS2HOST_MPEG2_VIDEO_H
#define PS2HOST_MPEG2_VIDEO_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ps2host {
namespace mpeg {

struct Picture {
    int width = 0, height = 0;          // the sequence's, in pixels (whole macroblocks: the planes' size)
    int shown_width = 0, shown_height = 0;  // the sequence header's own (the planes' top-left part)
    int type = 0;                       // 1 I, 2 P, 3 B
    int temporal_reference = 0;
    std::int64_t pts = -1, dts = -1;    // as the caller's OnPicture set them (libmpeg's time stamp callback)
    std::vector<std::uint8_t> y, cb, cr;    // width x height, and half that each way
};

/* A picture as sceMpegGetPicture hands it over: RGBA32 by macroblocks (16 x 16 pixels each, 1024 bytes a
   macroblock, a column of macroblocks after another), through the IPU's colour conversion (its integer arithmetic,
   ipu.cpp's CSC: alpha 0 below TH0, 0x40 below TH1, else 0x80). */
void ToRgb32Macroblocks(const Picture& picture, std::uint32_t th0, std::uint32_t th1, std::vector<std::uint32_t>* out);

class VideoDecoder {
public:
    /* The stream from `bytes` (a whole stream, or one piece after another): appended. */
    void Feed(const std::uint8_t* bytes, std::size_t count);
    /* The next picture in display order, when the stream holds one decoded whole; false otherwise (more
       stream needed, or an error - error() then says what). `end`: no more stream comes (the last anchor
       picture goes out). */
    bool Next(Picture* out, bool end = false);
    void Reset();
    /* The last picture that could not be decoded (skipped): why; and how many were. */
    const std::string& error() const { return error_; }
    int errors() const { return errors_; }
    /* A sequence end code was reached and every picture before it is out (sceMpegIsEnd's end). */
    bool ended() const { return ended_; }
    /* Told once for each picture decoded, in decoding order, as libmpeg asks its time stamp callback after a
       picture header: the picture's start code's offset in the whole stream fed so far (bytes since the first
       Feed or Reset); it may set the picture's pts / dts. */
    using OnPicture = void (*)(std::uint64_t offset, Picture& picture);
    void SetOnPicture(OnPicture on) { on_picture_ = on; }

private:
    struct State;
    std::vector<std::uint8_t> stream_;
    std::size_t at_ = 0;                // the next byte not yet parsed
    std::string error_;
    int errors_ = 0;
    bool ended_ = false;
    int decoded_count_ = 0;             // pictures decoded (PS2HOST_MPEG_TRACE's numbering, decode order)
    std::uint64_t base_ = 0;            // stream_[0]'s offset in the whole stream (what Feed dropped before)
    OnPicture on_picture_ = nullptr;
    // Sequence and picture parameters.
    int width_ = 0, height_ = 0, mb_width_ = 0, mb_height_ = 0;
    std::uint8_t intra_matrix_[64] = {}, non_intra_matrix_[64] = {};     // scan order, as transmitted
    bool have_sequence_ = false;
    // Reference pictures (anchors) and the one waiting to be shown.
    Picture forward_, backward_;
    bool have_forward_ = false, have_backward_ = false, anchor_waiting_ = false;
    /* `whole`: the data may stop inside the picture - true only if every macroblock decoded within it. */
    bool DecodePicture(std::size_t picture_start, std::size_t end, Picture* decoded, bool whole = false);
};

}  // namespace mpeg
}  // namespace ps2host

#endif
