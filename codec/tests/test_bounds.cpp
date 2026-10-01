// A13: every packet <= rcv_max_packet_size (codec plan §6.6, §11.4), and the encoder never writes
// past that size. Noise is the worst case: every chunk falls back to RAW, which the bound assumes.
#include <algorithm>
#include <vector>

#include "format.h"
#include "test_util.h"

using namespace tu;

namespace {

using Bytes = std::vector<uint8_t>;
constexpr size_t kGuard = 64;
constexpr uint8_t kGuardByte = 0xA5;

struct Frame {  // I420 or BGRA source
    Yuv yuv;
    Bytes bgra;
    rcv_frame_in in{};
};

Frame noise_frame(rcv_format fmt, int w, int h, uint64_t seed) {
    Frame f;
    if (fmt == RCV_FMT_YUV420) {
        f.yuv = make_yuv(w, h, Content::Noise, seed);
    } else {
        tf::Rng rng(seed);
        f.bgra.resize(size_t(4) * w * h);
        for (auto& b : f.bgra) b = rng.byte();
    }
    return f;
}

Frame flat_frame(rcv_format fmt, int w, int h) {
    Frame f;
    if (fmt == RCV_FMT_YUV420)
        f.yuv = make_yuv(w, h, Content::Zero, 0);
    else
        f.bgra.assign(size_t(4) * w * h, 0);
    return f;
}

void bind(Frame& f, rcv_format fmt, int w) {
    if (fmt == RCV_FMT_YUV420) {
        f.in = frame_in(f.yuv);
    } else {
        f.in = rcv_frame_in{};
        f.in.plane[0] = f.bgra.data();
        f.in.stride[0] = 4 * w;
    }
}

// Copies the top half of `from` into `to` (so a P-frame skips about half the blocks).
void copy_top_half(const Frame& from, Frame& to, rcv_format fmt, int h) {
    if (fmt == RCV_FMT_YUV420) {
        for (int p = 0; p < 3; ++p)
            std::copy(from.yuv.p[p].begin(), from.yuv.p[p].begin() + ptrdiff_t(from.yuv.stride[p]) * (from.yuv.ph(p) / 2),
                      to.yuv.p[p].begin());
    } else {
        std::copy(from.bgra.begin(), from.bgra.begin() + ptrdiff_t(from.bgra.size() / size_t(h) * size_t(h / 2)),
                  to.bgra.begin());
    }
}

struct Stats {
    int packets = 0, over = 0, guard_broken = 0, decode_failed = 0, tight = 0, tight_expected = 0;
};

// Encodes noise I, half-changed noise (P), new noise (I), flat (SINGLE chunks), repeat (DUP).
void run(rcv_format fmt, int w, int h, int slices, int predictor, int crc, int near_level, Stats* st) {
    rcv_encoder_config cfg = config_for(w, h, predictor, slices);
    if (fmt == RCV_FMT_GBR) {
        cfg.format = RCV_FMT_GBR;
        cfg.input_layout = RCV_IN_BGRA;
    }
    cfg.enable_crc = uint8_t(crc);
    rcv_encoder* enc = nullptr;
    if (rcv_encoder_create(&cfg, &enc) != RCV_OK) {
        ++st->decode_failed;
        return;
    }
    uint8_t seq[32];
    rcv_write_sequence_header(&cfg, seq);
    rcv_decoder* dec = nullptr;
    rcv_decoder_create(seq, 1, &dec);
    const size_t max = rcv_max_packet_size(&cfg);
    rcv::Geometry g;
    rcv::init_geometry(&g, fmt, w, h, slices);
    const size_t map = rcv::skip_map_size(g);

    std::vector<Frame> frames;
    frames.push_back(noise_frame(fmt, w, h, 100 + uint64_t(w) * h));
    frames.push_back(noise_frame(fmt, w, h, 200 + uint64_t(w) * h));
    copy_top_half(frames[0], frames[1], fmt, h);
    frames.push_back(noise_frame(fmt, w, h, 300 + uint64_t(w) * h));
    frames.push_back(flat_frame(fmt, w, h));
    frames.push_back(flat_frame(fmt, w, h));
    for (Frame& f : frames) bind(f, fmt, w);

    Bytes out(max + kGuard);
    for (size_t i = 0; i < frames.size(); ++i) {
        std::fill(out.begin(), out.end(), kGuardByte);
        rcv_encode_params params{};
        params.near_level = uint8_t(near_level);
        rcv_frame_info info{};
        if (rcv_encode_frame(enc, &frames[i].in, &params, out.data(), max, &info) != RCV_OK) {
            ++st->decode_failed;
            continue;
        }
        ++st->packets;
        if (info.packet_size > max) ++st->over;
        if (std::any_of(out.begin() + ptrdiff_t(max), out.end(), [](uint8_t b) { return b != kGuardByte; }))
            ++st->guard_broken;
        // Lossless noise I-frames big enough that no chunk is SINGLE meet the bound exactly: every
        // chunk is RAW and only the skip map is missing.
        if (near_level == 0 && info.frame_type == 1 && i != 3 && w * h >= 64 * 48) {
            ++st->tight_expected;
            if (info.packet_size == max - map) ++st->tight;
        }
        if (rcv_decode_frame(dec, out.data(), info.packet_size, fmt == RCV_FMT_GBR ? RCV_OUT_BGRA : RCV_OUT_I420,
                             nullptr, nullptr, nullptr) != RCV_OK)
            ++st->decode_failed;
    }
    rcv_decoder_destroy(dec);
    rcv_encoder_destroy(enc);
}

}  // namespace

TEST_CASE("bounds (A13): noise, flat and P-frames stay within rcv_max_packet_size") {
    struct Size {
        rcv_format fmt;
        int w, h;
    };
    const Size sizes[] = {{RCV_FMT_YUV420, 2, 2},   {RCV_FMT_YUV420, 16, 16},  {RCV_FMT_YUV420, 18, 34},
                          {RCV_FMT_YUV420, 64, 48}, {RCV_FMT_YUV420, 130, 66}, {RCV_FMT_GBR, 2, 2},
                          {RCV_FMT_GBR, 3, 5},      {RCV_FMT_GBR, 33, 21},     {RCV_FMT_GBR, 64, 48},
                          {RCV_FMT_GBR, 129, 65}};
    Stats st;
    for (const Size& s : sizes) {
        const int blocks_y = (s.h + 15) / 16;
        for (int slices : {1, std::min(blocks_y, 64)})
            for (int predictor : {0, 1})
                for (int crc : {0, 1})
                    for (int n = 0; n <= (s.fmt == RCV_FMT_YUV420 ? 3 : 0); ++n)
                        run(s.fmt, s.w, s.h, slices, predictor, crc, n, &st);
    }
    CHECK(st.packets > 500);
    CHECK(st.over == 0);
    CHECK(st.guard_broken == 0);
    CHECK(st.decode_failed == 0);
    CHECK(st.tight_expected > 0);
    CHECK(st.tight == st.tight_expected);
}

TEST_CASE("bounds (A13): 1360x744 noise I-frame fills the bound exactly") {
    for (rcv_format fmt : {RCV_FMT_YUV420, RCV_FMT_GBR}) {
        Stats st;
        run(fmt, 1360, 744, 0, 1, 1, 0, &st);
        CHECK(st.packets == 5);
        CHECK(st.over == 0);
        CHECK(st.guard_broken == 0);
        CHECK(st.decode_failed == 0);
        CHECK(st.tight_expected >= 2);
        CHECK(st.tight == st.tight_expected);
    }
}
