// Synthetic test frames and encode/decode helpers (codec plan §11.2 item 4).
#pragma once

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "rcv/rcv.h"
#include "testfw.h"

namespace tu {

enum class Content { Zero, Full, Noise, Gradient, Checker, Natural };

inline const Content kAllContent[] = {Content::Zero,     Content::Full,    Content::Noise,
                                      Content::Gradient, Content::Checker, Content::Natural};

// Planar I420 frame with arbitrary strides.
struct Yuv {
    int w = 0, h = 0;
    int stride[3] = {};
    std::vector<uint8_t> p[3];
    int pw(int i) const { return i ? w / 2 : w; }
    int ph(int i) const { return i ? h / 2 : h; }
    uint8_t& at(int i, int x, int y) { return p[i][size_t(y) * stride[i] + x]; }
    uint8_t at(int i, int x, int y) const { return p[i][size_t(y) * stride[i] + x]; }
};

inline uint8_t clamp8(double v) { return uint8_t(v < 0 ? 0 : v > 255 ? 255 : v); }

inline Yuv make_yuv(int w, int h, Content c, uint64_t seed, int extra_stride = 0) {
    tf::Rng rng(seed);
    Yuv f;
    f.w = w;
    f.h = h;
    for (int i = 0; i < 3; ++i) {
        f.stride[i] = f.pw(i) + extra_stride;
        f.p[i].assign(size_t(f.stride[i]) * f.ph(i), 0xEE);  // padding bytes must never matter
        for (int y = 0; y < f.ph(i); ++y)
            for (int x = 0; x < f.pw(i); ++x) {
                uint8_t v = 0;
                switch (c) {
                case Content::Zero: v = 0; break;
                case Content::Full: v = 255; break;
                case Content::Noise: v = rng.byte(); break;
                case Content::Gradient: v = uint8_t(x * 3 + y * 2 + i * 40); break;
                case Content::Checker: v = ((x >> 2) + (y >> 2)) & 1 ? 235 : 16; break;
                case Content::Natural:
                    v = clamp8(128 + 60 * std::sin(x * 0.05 + i) + 50 * std::cos(y * 0.07) + double(rng.below(5)));
                    break;
                }
                f.at(i, x, y) = v;
            }
    }
    return f;
}

inline bool same_image(const Yuv& a, const Yuv& b) {
    if (a.w != b.w || a.h != b.h) return false;
    for (int i = 0; i < 3; ++i)
        for (int y = 0; y < a.ph(i); ++y)
            if (std::memcmp(&a.p[i][size_t(y) * a.stride[i]], &b.p[i][size_t(y) * b.stride[i]], size_t(a.pw(i))))
                return false;
    return true;
}

inline rcv_frame_in frame_in(const Yuv& f) {
    rcv_frame_in in{};
    for (int i = 0; i < 3; ++i) {
        in.plane[i] = f.p[i].data();
        in.stride[i] = f.stride[i];
    }
    return in;
}

// RCV_FORCE_ISA=scalar|sse41|avx2 re-runs the whole suite on one kernel level (plan §3.3).
inline rcv_isa forced_isa() {
    char* v = nullptr;
    size_t len = 0;
    rcv_isa isa = RCV_ISA_AUTO;
    if (_dupenv_s(&v, &len, "RCV_FORCE_ISA") == 0 && v) {
        if (!std::strcmp(v, "scalar")) isa = RCV_ISA_SCALAR;
        else if (!std::strcmp(v, "sse41")) isa = RCV_ISA_SSE41;
        else if (!std::strcmp(v, "avx2")) isa = RCV_ISA_AVX2;
        std::free(v);
    }
    return isa;
}

// RCV_FORCE_THREADS=N re-runs the whole suite with N encoder threads (default: auto).
inline uint8_t forced_threads() {
    char* v = nullptr;
    size_t len = 0;
    int n = 0;
    if (_dupenv_s(&v, &len, "RCV_FORCE_THREADS") == 0 && v) {
        n = std::atoi(v);
        std::free(v);
    }
    return uint8_t(n < 0 ? 0 : n > 32 ? 32 : n);
}

inline rcv_encoder_config config_for(int w, int h, int predictor = 1, int slices = 0) {
    rcv_encoder_config c;
    rcv_encoder_config_init(&c);
    c.isa = forced_isa();
    c.num_threads = forced_threads();
    c.coded_width = uint16_t(w);
    c.coded_height = uint16_t(h);
    c.predictor = uint8_t(predictor);
    c.num_slices = uint8_t(slices);
    return c;
}

// Encodes one frame with a fresh encoder. Returns the packet (empty on failure).
inline std::vector<uint8_t> encode_one(const rcv_encoder_config& cfg, const Yuv& f, rcv_status* status = nullptr) {
    std::vector<uint8_t> pkt;
    rcv_encoder* enc = nullptr;
    rcv_status st = rcv_encoder_create(&cfg, &enc);
    if (st == RCV_OK) {
        pkt.resize(rcv_max_packet_size(&cfg));
        const rcv_frame_in in = frame_in(f);
        rcv_frame_info info{};
        st = rcv_encode_frame(enc, &in, nullptr, pkt.data(), pkt.size(), &info);
        if (st == RCV_OK)
            pkt.resize(info.packet_size);
        else
            pkt.clear();
        rcv_encoder_destroy(enc);
    }
    if (status) *status = st;
    return pkt;
}

inline Yuv blank_like(const Yuv& f, int extra_stride = 0) {
    Yuv o;
    o.w = f.w;
    o.h = f.h;
    for (int i = 0; i < 3; ++i) {
        o.stride[i] = o.pw(i) + extra_stride;
        o.p[i].assign(size_t(o.stride[i]) * o.ph(i), 0x77);
    }
    return o;
}

inline rcv_status decode_into(rcv_decoder* dec, const std::vector<uint8_t>& pkt, Yuv& out) {
    uint8_t* planes[3] = {out.p[0].data(), out.p[1].data(), out.p[2].data()};
    return rcv_decode_frame(dec, pkt.data(), pkt.size(), RCV_OUT_I420, planes, out.stride, nullptr);
}

}  // namespace tu
