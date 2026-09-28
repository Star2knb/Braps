// Lossless round trips (acceptance A1), determinism (A3, single-thread part) and bounds (A13).
#include <algorithm>
#include <set>

#include "format.h"
#include "test_util.h"

using namespace tu;

namespace {

// Encodes with a fresh encoder, checks size bound and determinism, decodes and compares.
void roundtrip(const Yuv& f, int predictor, int slices, int out_extra_stride = 0) {
    const rcv_encoder_config cfg = config_for(f.w, f.h, predictor, slices);
    rcv_status st;
    const std::vector<uint8_t> pkt = encode_one(cfg, f, &st);
    REQUIRE(st == RCV_OK);
    CHECK(pkt.size() <= rcv_max_packet_size(&cfg));
    CHECK(pkt.size() % 4 == 0);
    CHECK(encode_one(cfg, f) == pkt);  // deterministic

    uint8_t seq[32];
    rcv_write_sequence_header(&cfg, seq);
    rcv_decoder* dec = nullptr;
    REQUIRE(rcv_decoder_create(seq, 1, &dec) == RCV_OK);
    Yuv out = blank_like(f, out_extra_stride);
    CHECK(decode_into(dec, pkt, out) == RCV_OK);
    CHECK(same_image(f, out));
    rcv_decoder_destroy(dec);
}

std::vector<int> slice_options(int h) {
    const int by = (h + 15) / 16;
    std::set<int> s = {0, 1, 4, 8, by < 64 ? by : 64};
    std::vector<int> r;
    for (int v : s)
        if (v <= by) r.push_back(v);
    return r;
}

// Chunk modes of an I-frame packet, in directory order.
std::vector<int> chunk_modes(const std::vector<uint8_t>& pkt) {
    std::vector<int> modes;
    const int S = pkt[11];
    const uint8_t* dir = pkt.data() + 32;
    const uint8_t* c = dir + 12 * S;
    for (int k = 0; k < 3 * S; ++k) {
        modes.push_back(c[0]);
        c += rcv::get_u32(dir + 4 * k);
    }
    return modes;
}

}  // namespace

TEST_CASE("roundtrip: synthetic vectors x predictors x slice counts") {
    const int sizes[][2] = {{2, 2}, {16, 16}, {18, 18}, {34, 20}, {130, 66}, {320, 180}};
    uint64_t seed = 100;
    for (const auto& sz : sizes)
        for (Content c : kAllContent)
            for (int pred = 0; pred < 2; ++pred)
                for (int s : slice_options(sz[1])) roundtrip(make_yuv(sz[0], sz[1], c, seed++), pred, s);
}

TEST_CASE("roundtrip: every even width 2..130, odd strides") {
    uint64_t seed = 1000;
    for (int w = 2; w <= 130; w += 2)
        for (Content c : {Content::Noise, Content::Natural})
            for (int pred = 0; pred < 2; ++pred) {
                const int extra = (w % 7) + 1;
                roundtrip(make_yuv(w, 34, c, seed++, extra), pred, 0, extra + 2);
            }
}

TEST_CASE("roundtrip: 1360x744 natural content") {
    const Yuv f = make_yuv(1360, 744, Content::Natural, 7);
    roundtrip(f, 1, 8);
    roundtrip(f, 0, 1);
}

TEST_CASE("roundtrip: NV12 input gives the same packet as I420; NV12 output") {
    const Yuv f = make_yuv(66, 50, Content::Natural, 21);
    rcv_encoder_config cfg = config_for(f.w, f.h);
    const std::vector<uint8_t> ref = encode_one(cfg, f);
    REQUIRE(!ref.empty());

    // Build NV12 with padded strides.
    const int ys = f.w + 5, uvs = f.w + 3;
    std::vector<uint8_t> y(size_t(ys) * f.h, 0), uv(size_t(uvs) * (f.h / 2), 0);
    for (int r = 0; r < f.h; ++r)
        for (int x = 0; x < f.w; ++x) y[size_t(r) * ys + x] = f.at(0, x, r);
    for (int r = 0; r < f.h / 2; ++r)
        for (int x = 0; x < f.w / 2; ++x) {
            uv[size_t(r) * uvs + 2 * x] = f.at(1, x, r);
            uv[size_t(r) * uvs + 2 * x + 1] = f.at(2, x, r);
        }
    cfg.input_layout = RCV_IN_NV12;
    rcv_encoder* enc = nullptr;
    REQUIRE(rcv_encoder_create(&cfg, &enc) == RCV_OK);
    std::vector<uint8_t> pkt(rcv_max_packet_size(&cfg));
    rcv_frame_in in{};
    in.plane[0] = y.data();
    in.plane[1] = uv.data();
    in.stride[0] = ys;
    in.stride[1] = uvs;
    rcv_frame_info info{};
    CHECK(rcv_encode_frame(enc, &in, nullptr, pkt.data(), pkt.size(), &info) == RCV_OK);
    pkt.resize(info.packet_size);
    CHECK(pkt == ref);
    rcv_encoder_destroy(enc);

    uint8_t seq[32];
    rcv_write_sequence_header(&cfg, seq);
    rcv_decoder* dec = nullptr;
    REQUIRE(rcv_decoder_create(seq, 1, &dec) == RCV_OK);
    std::vector<uint8_t> oy(y.size(), 0x11), ouv(uv.size(), 0x11);
    uint8_t* planes[3] = {oy.data(), ouv.data(), nullptr};
    const int32_t strides[3] = {ys, uvs, 0};
    CHECK(rcv_decode_frame(dec, pkt.data(), pkt.size(), RCV_OUT_NV12, planes, strides, nullptr) == RCV_OK);
    for (int r = 0; r < f.h; ++r)
        CHECK(std::memcmp(&oy[size_t(r) * ys], &y[size_t(r) * ys], size_t(f.w)) == 0);
    for (int r = 0; r < f.h / 2; ++r)
        CHECK(std::memcmp(&ouv[size_t(r) * uvs], &uv[size_t(r) * uvs], size_t(f.w)) == 0);
    rcv_decoder_destroy(dec);
}

TEST_CASE("roundtrip: chunk modes (SINGLE, RAW, HUFFMAN)") {
    auto modes_of = [](const Yuv& f) { return chunk_modes(encode_one(config_for(f.w, f.h), f)); };
    // Constant 128: every residual is 0 (the slice's first sample predicts 128).
    Yuv flat = make_yuv(256, 256, Content::Zero, 9);
    for (auto& plane : flat.p) std::fill(plane.begin(), plane.end(), uint8_t(128));
    for (int m : modes_of(flat)) CHECK(m == rcv::kChunkSingle);
    for (int m : modes_of(make_yuv(256, 256, Content::Noise, 9))) CHECK(m == rcv::kChunkRaw);
    for (int m : modes_of(make_yuv(256, 256, Content::Natural, 9))) CHECK(m == rcv::kChunkHuffman);
}

TEST_CASE("roundtrip: frame sequence with DUP through one encoder/decoder") {
    const int w = 96, h = 64;
    rcv_encoder_config cfg = config_for(w, h);
    cfg.enable_crc = 1;
    rcv_encoder* enc = nullptr;
    REQUIRE(rcv_encoder_create(&cfg, &enc) == RCV_OK);
    uint8_t seq[32];
    rcv_write_sequence_header(&cfg, seq);
    rcv_decoder* dec = nullptr;
    REQUIRE(rcv_decoder_create(seq, 1, &dec) == RCV_OK);

    std::vector<uint8_t> pkt(rcv_max_packet_size(&cfg));
    Yuv last;
    for (int i = 0; i < 6; ++i) {
        rcv_frame_info info{};
        Yuv out;
        if (i == 3) {
            CHECK(rcv_encode_duplicate(enc, pkt.data(), pkt.size(), &info) == RCV_OK);
            CHECK(info.packet_size == 8);
            CHECK(info.frame_type == 0);
            out = blank_like(last);
        } else {
            last = make_yuv(w, h, kAllContent[i % 6], 300 + uint64_t(i));
            const rcv_frame_in in = frame_in(last);
            CHECK(rcv_encode_frame(enc, &in, nullptr, pkt.data(), pkt.size(), &info) == RCV_OK);
            CHECK(info.frame_type == 1);
            CHECK(info.is_keyframe == 1);
            out = blank_like(last);
        }
        std::vector<uint8_t> p(pkt.begin(), pkt.begin() + info.packet_size);
        CHECK(decode_into(dec, p, out) == RCV_OK);
        CHECK(same_image(last, out));
    }
    rcv_decoder_destroy(dec);
    rcv_encoder_destroy(enc);
}

TEST_CASE("format: max packet size formula (§6.6)") {
    const rcv_encoder_config cfg = config_for(1360, 744, 1, 8);
    // 32 header + 500 skip map + 96 directory + chunks (1360 and 680 are multiples of 4).
    const size_t expect = 32 + 500 + 96 + (8 * 4 + 1360 * 744) + 2 * (8 * 4 + 680 * 372);
    CHECK(rcv_max_packet_size(&cfg) == expect);

    rcv::Geometry g;
    REQUIRE(rcv::init_geometry(&g, RCV_FMT_YUV420, 1360, 744, 0));
    CHECK(g.blocks_x == 85);
    CHECK(g.blocks_y == 47);
    CHECK(g.num_slices == 8);
    CHECK(g.slice_row(0, 8) == 744);
    CHECK(g.slice_row(1, 8) == 372);
}
