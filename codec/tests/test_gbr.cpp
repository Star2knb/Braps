// Lossless RGB: GBR format, BGRA/BGRX input, BGRA output (codec plan §4.1, §4.2, §4.4; acceptance A1).
#include <algorithm>
#include <cmath>
#include <vector>

#include "colour.h"
#include "format.h"
#include "test_util.h"

using namespace tu;

namespace {

// Packed BGRA frame with arbitrary stride (bytes).
struct Bgra {
    int w = 0, h = 0, stride = 0;
    std::vector<uint8_t> px;
    uint8_t* at(int x, int y) { return &px[size_t(y) * stride + 4 * size_t(x)]; }
    const uint8_t* at(int x, int y) const { return &px[size_t(y) * stride + 4 * size_t(x)]; }
};

Bgra make_bgra(int w, int h, Content c, uint64_t seed, int extra = 0, bool random_alpha = false) {
    tf::Rng rng(seed);
    Bgra f;
    f.w = w;
    f.h = h;
    f.stride = 4 * w + extra;
    f.px.assign(size_t(f.stride) * h, 0xEE);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t b = 0, g = 0, r = 0;
            switch (c) {
            case Content::Zero: break;
            case Content::Full: b = g = r = 255; break;
            case Content::Noise: b = rng.byte(); g = rng.byte(); r = rng.byte(); break;
            case Content::Gradient: b = uint8_t(x * 3); g = uint8_t(y * 2 + x); r = uint8_t(x + y * 5); break;
            case Content::Checker: b = g = r = ((x >> 2) + (y >> 2)) & 1 ? 235 : 16; break;
            case Content::Natural: {  // correlated channels, like real images
                const double base = 128 + 60 * std::sin(x * 0.05) + 50 * std::cos(y * 0.07);
                g = clamp8(base + double(rng.below(5)));
                r = clamp8(base + 30 * std::sin(y * 0.03) + double(rng.below(5)));
                b = clamp8(base - 25 + double(rng.below(5)));
                break;
            }
            }
            uint8_t* p = f.at(x, y);
            p[0] = b;
            p[1] = g;
            p[2] = r;
            p[3] = random_alpha ? rng.byte() : 255;
        }
    return f;
}

bool same_rgb(const Bgra& a, const Bgra& out) {  // colour equal, output alpha 255
    for (int y = 0; y < a.h; ++y)
        for (int x = 0; x < a.w; ++x) {
            const uint8_t* p = a.at(x, y);
            const uint8_t* q = out.at(x, y);
            if (p[0] != q[0] || p[1] != q[1] || p[2] != q[2] || q[3] != 255) return false;
        }
    return true;
}

rcv_encoder_config gbr_config(int w, int h, int predictor = 1, int slices = 0) {
    rcv_encoder_config c = config_for(w, h, predictor, slices);
    c.format = RCV_FMT_GBR;
    c.input_layout = RCV_IN_BGRA;
    return c;
}

struct GbrResult {
    std::vector<std::vector<uint8_t>> packets;
    std::vector<int> types;
    bool all_exact = true;
};

// Encodes a BGRA sequence through one encoder; decodes each packet to BGRA and compares.
GbrResult run_gbr(const std::vector<Bgra>& frames, const rcv_encoder_config& cfg, int dec_threads = 2) {
    GbrResult r;
    rcv_encoder* enc = nullptr;
    if (rcv_encoder_create(&cfg, &enc) != RCV_OK) {
        r.all_exact = false;
        return r;
    }
    uint8_t seq[32];
    rcv_write_sequence_header(&cfg, seq);
    rcv_decoder* dec = nullptr;
    rcv_decoder_create(seq, uint8_t(dec_threads), &dec);
    std::vector<uint8_t> pkt(rcv_max_packet_size(&cfg));
    for (const Bgra& f : frames) {
        rcv_frame_in in{};
        in.plane[0] = f.px.data();
        in.stride[0] = f.stride;
        rcv_frame_info info{};
        if (rcv_encode_frame(enc, &in, nullptr, pkt.data(), pkt.size(), &info) != RCV_OK) {
            r.all_exact = false;
            break;
        }
        r.packets.emplace_back(pkt.begin(), pkt.begin() + info.packet_size);
        r.types.push_back(info.frame_type);
        Bgra out;
        out.w = f.w;
        out.h = f.h;
        out.stride = 4 * f.w + 12;
        out.px.assign(size_t(out.stride) * f.h, 0x33);
        uint8_t* planes[3] = {out.px.data(), nullptr, nullptr};
        const int32_t strides[3] = {out.stride, 0, 0};
        if (rcv_decode_frame(dec, r.packets.back().data(), r.packets.back().size(), RCV_OUT_BGRA, planes, strides,
                             nullptr) != RCV_OK ||
            !same_rgb(f, out))
            r.all_exact = false;
    }
    rcv_decoder_destroy(dec);
    rcv_encoder_destroy(enc);
    return r;
}

}  // namespace

TEST_CASE("gbr: colour kernels equal the scalar reference (widths 1-130)") {
    tf::Rng rng(8);
    for (int n = 1; n <= 130; ++n) {
        std::vector<uint8_t> bgra(size_t(n) * 4);
        for (auto& v : bgra) v = rng.byte();
        std::vector<uint8_t> g1(n + 16, 1), b1(n + 16, 1), r1(n + 16, 1), g2(n + 16, 1), b2(n + 16, 1), r2(n + 16, 1);
        rcv::bgra_to_gbr_row(bgra.data(), n, g1.data(), b1.data(), r1.data());
        rcv::bgra_to_gbr_row_scalar(bgra.data(), n, g2.data(), b2.data(), r2.data());
        CHECK(g1 == g2);
        CHECK(b1 == b2);
        CHECK(r1 == r2);
        std::vector<uint8_t> o1(size_t(n) * 4 + 16, 7), o2(size_t(n) * 4 + 16, 7);
        rcv::gbr_to_bgra_row(g1.data(), b1.data(), r1.data(), n, o1.data());
        rcv::gbr_to_bgra_row_scalar(g1.data(), b1.data(), r1.data(), n, o2.data());
        CHECK(o1 == o2);
        for (int x = 0; x < n; ++x)  // exact inverse (alpha becomes 255)
            CHECK(o1[4 * x] == bgra[4 * x] && o1[4 * x + 1] == bgra[4 * x + 1] && o1[4 * x + 2] == bgra[4 * x + 2] &&
                  o1[4 * x + 3] == 255);
    }
}

TEST_CASE("gbr: lossless round trips, odd sizes, predictors, slice counts (A1 on RGB)") {
    const int sizes[][2] = {{2, 2}, {3, 5}, {17, 9}, {33, 21}, {130, 66}, {321, 179}};
    uint64_t seed = 400;
    for (const auto& sz : sizes)
        for (Content c : kAllContent)
            for (int pred = 0; pred < 2; ++pred) {
                const int by = (sz[1] + 15) / 16;
                for (int slices : {0, 1, by}) {
                    const Bgra f = make_bgra(sz[0], sz[1], c, seed++, (sz[0] % 5) * 4 + 1);
                    const rcv_encoder_config cfg = gbr_config(sz[0], sz[1], pred, slices);
                    const GbrResult r = run_gbr({f}, cfg);
                    CHECK(r.all_exact);
                    REQUIRE(!r.packets.empty());
                    CHECK(r.packets[0].size() <= rcv_max_packet_size(&cfg));
                }
            }
}

TEST_CASE("gbr: alpha is ignored (BGRX with garbage alpha gives identical packets)") {
    const Bgra a = make_bgra(97, 41, Content::Natural, 9, 0, false);
    Bgra x = a;
    tf::Rng rng(1);
    for (int y = 0; y < x.h; ++y)
        for (int i = 0; i < x.w; ++i) x.at(i, y)[3] = rng.byte();
    rcv_encoder_config cfg = gbr_config(97, 41);
    const GbrResult ra = run_gbr({a}, cfg);
    cfg.input_layout = RCV_IN_BGRX;
    const GbrResult rx = run_gbr({x}, cfg);
    CHECK(ra.all_exact);
    CHECK(rx.all_exact);
    CHECK(ra.packets == rx.packets);
}

TEST_CASE("gbr: P-frames and DUP on RGB, identical for every ISA and 1-4 threads") {
    const int w = 99, h = 70;  // odd width, partial edge blocks
    std::vector<Bgra> seq;
    Bgra f = make_bgra(w, h, Content::Natural, 3);
    seq.push_back(f);
    seq.push_back(f);  // DUP
    f.at(5, 5)[2] ^= 0x20;  // one red value -> P
    seq.push_back(f);
    f.at(w - 1, h - 1)[0] ^= 1;  // blue in the last (partial) block
    seq.push_back(f);
    for (int y = 20; y < 40; ++y)
        for (int x = 30; x < 60; ++x) f.at(x, y)[1] = uint8_t(x * y);
    seq.push_back(f);
    seq.push_back(make_bgra(w, h, Content::Gradient, 4));  // scene change -> I

    rcv_encoder_config cfg = gbr_config(w, h);
    cfg.isa = RCV_ISA_SCALAR;
    cfg.num_threads = 1;
    const GbrResult ref = run_gbr(seq, cfg);
    REQUIRE(ref.all_exact);
    REQUIRE(ref.types.size() == seq.size());
    CHECK(ref.types[0] == rcv::kFrameI);
    CHECK(ref.types[1] == rcv::kFrameDup);
    CHECK(ref.types[2] == rcv::kFrameP);
    CHECK(ref.types[3] == rcv::kFrameP);
    CHECK(ref.types[4] == rcv::kFrameP);
    CHECK(ref.types[5] == rcv::kFrameI);
    for (int isa = RCV_ISA_SCALAR; isa <= int(rcv_cpu_isa()); ++isa)
        for (int threads = 1; threads <= 4; ++threads) {
            cfg.isa = rcv_isa(isa);
            cfg.num_threads = uint8_t(threads);
            const GbrResult r = run_gbr(seq, cfg, threads);
            CHECK(r.all_exact);
            CHECK(r.packets == ref.packets);
        }
}

TEST_CASE("gbr: argument checks") {
    const Bgra f = make_bgra(40, 30, Content::Natural, 2);
    rcv_encoder_config cfg = gbr_config(40, 30);
    rcv_encoder* enc = nullptr;
    REQUIRE(rcv_encoder_create(&cfg, &enc) == RCV_OK);
    std::vector<uint8_t> pkt(rcv_max_packet_size(&cfg));
    rcv_frame_in in{};
    in.plane[0] = f.px.data();
    in.stride[0] = 4 * 40 - 1;  // too small
    CHECK(rcv_encode_frame(enc, &in, nullptr, pkt.data(), pkt.size(), nullptr) == RCV_ERR_INVALID_ARG);
    in.stride[0] = f.stride;
    rcv_encode_params params{};
    params.near_level = 1;  // near-lossless is never allowed for GBR (§4.1)
    CHECK(rcv_encode_frame(enc, &in, &params, pkt.data(), pkt.size(), nullptr) == RCV_ERR_INVALID_ARG);
    rcv_frame_info info{};
    REQUIRE(rcv_encode_frame(enc, &in, nullptr, pkt.data(), pkt.size(), &info) == RCV_OK);
    rcv_encoder_destroy(enc);

    uint8_t seq[32];
    rcv_write_sequence_header(&cfg, seq);
    rcv_sequence_info si{};
    CHECK(rcv_parse_sequence_header(seq, &si) == RCV_OK);
    CHECK(si.format == RCV_FMT_GBR);
    rcv_decoder* dec = nullptr;
    REQUIRE(rcv_decoder_create(seq, 1, &dec) == RCV_OK);
    std::vector<uint8_t> buf(40 * 30 * 4);
    uint8_t* planes[3] = {buf.data(), buf.data(), buf.data()};
    const int32_t yuv_strides[3] = {40, 40, 40};
    CHECK(rcv_decode_frame(dec, pkt.data(), info.packet_size, RCV_OUT_I420, planes, yuv_strides, nullptr) ==
          RCV_ERR_UNSUPPORTED);  // the codec never converts RGB <-> YUV
    const int32_t small[3] = {40 * 4 - 4, 0, 0};
    CHECK(rcv_decode_frame(dec, pkt.data(), info.packet_size, RCV_OUT_BGRA, planes, small, nullptr) ==
          RCV_ERR_INVALID_ARG);
    const int32_t ok[3] = {40 * 4, 0, 0};
    CHECK(rcv_decode_frame(dec, pkt.data(), info.packet_size, RCV_OUT_BGRA, planes, ok, nullptr) == RCV_OK);
    rcv_decoder_destroy(dec);
}

TEST_CASE("gbr: max packet size covers three full-size planes") {
    const rcv_encoder_config cfg = gbr_config(1360, 744, 1, 8);
    // 32 header + 500 skip map + 96 directory + 3 planes x (8 chunk headers + 1360 x 744).
    CHECK(rcv_max_packet_size(&cfg) == size_t(32 + 500 + 96 + 3 * (8 * 4 + 1360 * 744)));
}
