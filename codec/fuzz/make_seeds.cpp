// Writes the seed corpus for rcv_fuzz: valid streams in its input format (see fuzz_decoder.cpp)
// covering both formats, several sizes, predictors, slice counts, NEAR, CRC, every chunk mode and
// I/P/DUP frames.   Usage: rcv_fuzz_seeds <output dir>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "rcv/rcv.h"

namespace {

using Bytes = std::vector<uint8_t>;

struct Rng {
    uint64_t s;
    uint8_t byte() {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return uint8_t(s >> 56);
    }
};

enum Content { kNatural, kNoise, kFlat };

// One frame as the encoder's input (I420 planes or BGRA pixels).
struct Frame {
    Bytes p[3];
    int stride[3] = {};
};

Frame make_frame(rcv_format fmt, int w, int h, Content c, uint64_t seed) {
    Rng rng{seed};
    Frame f;
    auto sample = [&](int x, int y, int ch) -> uint8_t {
        switch (c) {
        case kNoise: return rng.byte();
        case kFlat: return 128;
        default: {
            const double v = 128 + 60 * std::sin(x * 0.11 + ch) + 50 * std::cos(y * 0.07) + (rng.byte() & 3);
            return uint8_t(v < 0 ? 0 : v > 255 ? 255 : v);
        }
        }
    };
    if (fmt == RCV_FMT_GBR) {
        f.stride[0] = 4 * w;
        f.p[0].resize(size_t(4) * w * h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                for (int ch = 0; ch < 4; ++ch) f.p[0][size_t(y) * f.stride[0] + 4 * size_t(x) + ch] = sample(x, y, ch);
        return f;
    }
    for (int p = 0; p < 3; ++p) {
        const int pw = p ? w / 2 : w, ph = p ? h / 2 : h;
        f.stride[p] = pw;
        f.p[p].resize(size_t(pw) * ph);
        for (int y = 0; y < ph; ++y)
            for (int x = 0; x < pw; ++x) f.p[p][size_t(y) * pw + x] = sample(x, y, p);
    }
    return f;
}

// Changes the top-left 16x16 block of the first plane (so the next frame is a P-frame).
void change_block(Frame& f, rcv_format fmt, int w, int h) {
    const int bpp = fmt == RCV_FMT_GBR ? 4 : 1;
    for (int y = 0; y < 16 && y < h; ++y)
        for (int x = 0; x < 16 * bpp && x < w * bpp; ++x) f.p[0][size_t(y) * f.stride[0] + x] ^= 0x21;
}

struct Variant {
    rcv_format fmt;
    int w, h, predictor, slices, crc, near_level, opt;
    Content last;
};

bool write_seed(const std::string& dir, int index, const Variant& v) {
    rcv_encoder_config cfg;
    rcv_encoder_config_init(&cfg);
    cfg.format = v.fmt;
    cfg.input_layout = v.fmt == RCV_FMT_GBR ? RCV_IN_BGRA : RCV_IN_I420;
    cfg.coded_width = uint16_t(v.w);
    cfg.coded_height = uint16_t(v.h);
    cfg.predictor = uint8_t(v.predictor);
    cfg.num_slices = uint8_t(v.slices);
    cfg.num_threads = 1;
    cfg.enable_crc = uint8_t(v.crc);
    rcv_encoder* enc = nullptr;
    if (rcv_encoder_create(&cfg, &enc) != RCV_OK) return false;

    Bytes stream(32);
    rcv_write_sequence_header(&cfg, stream.data());
    stream.push_back(uint8_t(v.opt));

    // I, P, DUP, then a frame of `last` content (noise: RAW chunks, flat: SINGLE chunks).
    std::vector<Frame> frames;
    frames.push_back(make_frame(v.fmt, v.w, v.h, kNatural, uint64_t(index) + 1));
    frames.push_back(frames[0]);
    change_block(frames[1], v.fmt, v.w, v.h);
    frames.push_back(frames[1]);
    frames.push_back(make_frame(v.fmt, v.w, v.h, v.last, uint64_t(index) + 99));
    Bytes pkt(rcv_max_packet_size(&cfg));
    bool ok = true;
    for (size_t i = 0; i < frames.size(); ++i) {
        rcv_frame_in in{};
        for (int p = 0; p < 3; ++p) {
            in.plane[p] = frames[i].p[p].empty() ? nullptr : frames[i].p[p].data();
            in.stride[p] = frames[i].stride[p];
        }
        rcv_encode_params params{};
        params.near_level = uint8_t(i >= 1 ? v.near_level : 0);
        rcv_frame_info fi{};
        if (rcv_encode_frame(enc, &in, &params, pkt.data(), pkt.size(), &fi) != RCV_OK) {
            ok = false;
            break;
        }
        const uint32_t n = fi.packet_size;
        const uint8_t len[4] = {uint8_t(n), uint8_t(n >> 8), uint8_t(n >> 16), uint8_t(n >> 24)};
        stream.insert(stream.end(), len, len + 4);
        stream.insert(stream.end(), pkt.begin(), pkt.begin() + n);
    }
    rcv_encoder_destroy(enc);
    if (!ok) return false;

    char name[64];
    std::snprintf(name, sizeof(name), "/seed_%03d.bin", index);
    FILE* f = nullptr;
    if (fopen_s(&f, (dir + name).c_str(), "wb") != 0 || !f) return false;
    const bool written = std::fwrite(stream.data(), 1, stream.size(), f) == stream.size();
    std::fclose(f);
    return written;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: rcv_fuzz_seeds <output dir>\n");
        return 2;
    }
    struct Size {
        rcv_format fmt;
        int w, h;
    };
    const Size sizes[] = {{RCV_FMT_YUV420, 16, 16}, {RCV_FMT_YUV420, 64, 48}, {RCV_FMT_YUV420, 34, 18},
                          {RCV_FMT_YUV420, 130, 34}, {RCV_FMT_GBR, 16, 16},   {RCV_FMT_GBR, 33, 21},
                          {RCV_FMT_GBR, 64, 48}};
    int index = 0, failed = 0;
    for (const Size& s : sizes) {
        const int blocks_y = (s.h + 15) / 16;
        for (int k = 0; k < 8; ++k) {
            Variant v;
            v.fmt = s.fmt;
            v.w = s.w;
            v.h = s.h;
            v.predictor = k & 1;
            v.slices = (k >> 1) % 3 == 0 ? 1 : (k >> 1) % 3 == 1 ? 0 : blocks_y;
            v.crc = k == 5;
            v.near_level = s.fmt == RCV_FMT_YUV420 ? k % 4 : 0;
            v.opt = k;  // decoder threads and output layout
            v.last = k % 3 == 0 ? kNoise : k % 3 == 1 ? kFlat : kNatural;
            if (!write_seed(argv[1], index++, v)) ++failed;
        }
    }
    std::printf("%d seeds written to %s, %d failed\n", index - failed, argv[1], failed);
    return failed ? 1 : 0;
}
