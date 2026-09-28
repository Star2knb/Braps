// Argument/bitstream validation (§6.5) and a quick mutation "fuzz" (the real libFuzzer run is M8).
#include "format.h"
#include "test_util.h"

using namespace tu;

namespace {

struct DecoderFixture {
    rcv_encoder_config cfg;
    rcv_decoder* dec = nullptr;
    explicit DecoderFixture(int w, int h, int slices = 0) : cfg(config_for(w, h, 1, slices)) {
        uint8_t seq[32];
        rcv_write_sequence_header(&cfg, seq);
        rcv_decoder_create(seq, 1, &dec);
    }
    ~DecoderFixture() { rcv_decoder_destroy(dec); }
    rcv_status decode(const std::vector<uint8_t>& p) {
        return rcv_decode_frame(dec, p.data(), p.size(), RCV_OUT_I420, nullptr, nullptr, nullptr);
    }
};

}  // namespace

TEST_CASE("errors: encoder config validation") {
    rcv_encoder* enc = nullptr;
    rcv_encoder_config c = config_for(33, 20);
    CHECK(rcv_encoder_create(&c, &enc) == RCV_ERR_INVALID_ARG);  // odd width for YUV420
    c = config_for(0, 20);
    CHECK(rcv_encoder_create(&c, &enc) == RCV_ERR_INVALID_ARG);
    c = config_for(64, 32, 1, 3);                                  // 2 block rows < 3 slices
    CHECK(rcv_encoder_create(&c, &enc) == RCV_ERR_INVALID_ARG);
    c = config_for(64, 32);
    c.predictor = 2;
    CHECK(rcv_encoder_create(&c, &enc) == RCV_ERR_INVALID_ARG);
    c = config_for(64, 32);
    c.format = RCV_FMT_GBR;  // GBR needs a BGRA/BGRX input layout
    CHECK(rcv_encoder_create(&c, &enc) == RCV_ERR_INVALID_ARG);
    CHECK(rcv_max_packet_size(&c) == 0);
    c = config_for(64, 32);
    c.input_layout = RCV_IN_BGRA;  // ... and YUV420 an I420/NV12 one
    CHECK(rcv_encoder_create(&c, &enc) == RCV_ERR_INVALID_ARG);
    c = config_for(33, 21);
    c.format = RCV_FMT_GBR;
    c.input_layout = RCV_IN_BGRX;  // odd sizes are fine for GBR
    CHECK(rcv_encoder_create(&c, &enc) == RCV_OK);
    rcv_encoder_destroy(enc);
    enc = nullptr;
    c.format = rcv_format(7);
    CHECK(rcv_encoder_create(&c, &enc) == RCV_ERR_INVALID_ARG);
    CHECK(enc == nullptr);
}

TEST_CASE("errors: encoder call validation") {
    const Yuv f = make_yuv(32, 32, Content::Natural, 1);
    rcv_encoder_config c = config_for(32, 32);
    rcv_encoder* enc = nullptr;
    REQUIRE(rcv_encoder_create(&c, &enc) == RCV_OK);
    std::vector<uint8_t> out(rcv_max_packet_size(&c));
    const rcv_frame_in in = frame_in(f);

    CHECK(rcv_encode_duplicate(enc, out.data(), out.size(), nullptr) == RCV_ERR_NO_REFERENCE);
    CHECK(rcv_encode_frame(enc, &in, nullptr, out.data(), out.size() - 1, nullptr) == RCV_ERR_BUFFER_TOO_SMALL);
    rcv_encode_params params{};
    params.near_level = 4;  // NEAR is 0..3
    CHECK(rcv_encode_frame(enc, &in, &params, out.data(), out.size(), nullptr) == RCV_ERR_INVALID_ARG);
    rcv_frame_in bad = in;
    bad.stride[1] = 3;
    CHECK(rcv_encode_frame(enc, &bad, nullptr, out.data(), out.size(), nullptr) == RCV_ERR_INVALID_ARG);
    CHECK(rcv_encode_frame(enc, &in, nullptr, out.data(), out.size(), nullptr) == RCV_OK);
    CHECK(rcv_encode_duplicate(enc, out.data(), 7, nullptr) == RCV_ERR_BUFFER_TOO_SMALL);
    CHECK(rcv_encode_duplicate(enc, out.data(), out.size(), nullptr) == RCV_OK);
    rcv_encoder_destroy(enc);
}

TEST_CASE("errors: sequence header validation") {
    rcv_encoder_config c = config_for(64, 32);
    uint8_t seq[32];
    rcv_write_sequence_header(&c, seq);
    rcv_sequence_info info{};
    CHECK(rcv_parse_sequence_header(seq, &info) == RCV_OK);
    CHECK(info.coded_width == 64);
    CHECK(info.display_height == 32);
    CHECK(info.full_range == 1);

    rcv_decoder* dec = nullptr;
    uint8_t bad[32];
    std::memcpy(bad, seq, 32);
    bad[0] = 'X';
    CHECK(rcv_decoder_create(bad, 1, &dec) == RCV_ERR_BITSTREAM);
    std::memcpy(bad, seq, 32);
    bad[6] = 63;  // odd coded width
    CHECK(rcv_decoder_create(bad, 1, &dec) == RCV_ERR_BITSTREAM);
    std::memcpy(bad, seq, 32);
    bad[10] = 65;  // display wider than coded
    CHECK(rcv_decoder_create(bad, 1, &dec) == RCV_ERR_BITSTREAM);
    std::memcpy(bad, seq, 32);
    bad[5] = 2;  // unknown format
    CHECK(rcv_decoder_create(bad, 1, &dec) == RCV_ERR_BITSTREAM);
    CHECK(dec == nullptr);
    std::memcpy(bad, seq, 32);
    bad[5] = 1;   // GBR
    bad[6] = 63;  // odd width: fine for GBR
    bad[10] = 63;
    CHECK(rcv_decoder_create(bad, 1, &dec) == RCV_OK);
    rcv_decoder_destroy(dec);
}

TEST_CASE("errors: packets rejected by the decoder") {
    DecoderFixture fx(64, 48);
    REQUIRE(fx.dec != nullptr);
    const std::vector<uint8_t> good = encode_one(fx.cfg, make_yuv(64, 48, Content::Natural, 2));
    REQUIRE(!good.empty());

    std::vector<uint8_t> dup(8);
    rcv::write_dup_packet(dup.data());
    CHECK(fx.decode(dup) == RCV_ERR_NO_REFERENCE);
    std::vector<uint8_t> p = good;
    p[5] = rcv::kFrameP;
    CHECK(fx.decode(p) == RCV_ERR_NO_REFERENCE);

    auto mutated = [&](size_t off, uint8_t v) {
        std::vector<uint8_t> m = good;
        m[off] = v;
        return fx.decode(m);
    };
    CHECK(mutated(0, 'X') == RCV_ERR_BITSTREAM);         // magic
    CHECK(mutated(4, 2) == RCV_ERR_BITSTREAM);           // version
    CHECK(mutated(5, 9) == RCV_ERR_BITSTREAM);           // frame type
    CHECK(mutated(6, 0x80) == RCV_ERR_BITSTREAM);        // unknown flag
    CHECK(mutated(9, 1) == RCV_ERR_BITSTREAM);           // NEAR without the flag
    CHECK(mutated(10, 2) == RCV_ERR_BITSTREAM);          // predictor
    CHECK(mutated(11, 0) == RCV_ERR_BITSTREAM);          // 0 slices
    CHECK(mutated(11, 4) == RCV_ERR_BITSTREAM);          // slices > blocks_y (3)
    CHECK(mutated(12, 32) == RCV_ERR_BITSTREAM);         // width differs from sequence
    CHECK(mutated(16, 3) == RCV_ERR_BITSTREAM);          // block size
    CHECK(mutated(24, good[24] ^ 4) == RCV_ERR_BITSTREAM);  // payload size
    CHECK(mutated(28, 1) == RCV_ERR_BITSTREAM);          // CRC value without the flag

    std::vector<uint8_t> t(good.begin(), good.end() - 4);
    CHECK(fx.decode(t) == RCV_ERR_BITSTREAM);
    t.assign(good.begin(), good.begin() + 7);
    CHECK(fx.decode(t) == RCV_ERR_BITSTREAM);

    CHECK(fx.decode(good) == RCV_OK);
    CHECK(fx.decode(dup) == RCV_OK);
    rcv_decoder_reset(fx.dec);
    CHECK(fx.decode(dup) == RCV_ERR_NO_REFERENCE);

    uint8_t* planes[3] = {nullptr, nullptr, nullptr};
    std::vector<uint8_t> buf(64 * 48 * 4);
    planes[0] = buf.data();
    const int32_t strides[3] = {256, 0, 0};
    CHECK(rcv_decode_frame(fx.dec, good.data(), good.size(), RCV_OUT_BGRA, planes, strides, nullptr) ==
          RCV_ERR_UNSUPPORTED);
}

TEST_CASE("errors: CRC mismatch is detected") {
    DecoderFixture fx(64, 48);
    rcv_encoder_config cfg = fx.cfg;
    cfg.enable_crc = 1;
    std::vector<uint8_t> pkt = encode_one(cfg, make_yuv(64, 48, Content::Natural, 3));
    REQUIRE(!pkt.empty());
    CHECK(fx.decode(pkt) == RCV_OK);
    pkt[pkt.size() - 5] ^= 0x10;
    CHECK(fx.decode(pkt) == RCV_ERR_BITSTREAM);
}

TEST_CASE("errors: random corruption never crashes, decoder recovers") {
    const int w = 64, h = 48;
    DecoderFixture fx(w, h, 3);
    REQUIRE(fx.dec != nullptr);
    std::vector<std::vector<uint8_t>> seeds;
    for (Content c : kAllContent) seeds.push_back(encode_one(fx.cfg, make_yuv(w, h, c, 40)));
    tf::Rng rng(77);
    int ok = 0, rejected = 0;
    for (int iter = 0; iter < 4000; ++iter) {
        std::vector<uint8_t> m = seeds[rng.below(uint32_t(seeds.size()))];
        const int edits = 1 + int(rng.below(4));
        for (int e = 0; e < edits; ++e) {
            switch (rng.below(4)) {
            case 0: m[rng.below(uint32_t(m.size()))] ^= uint8_t(1u << rng.below(8)); break;
            case 1: m[rng.below(uint32_t(m.size()))] = rng.byte(); break;
            case 2: m.resize(rng.below(uint32_t(m.size()) + 1)); break;
            case 3: m.push_back(rng.byte()); break;
            }
            if (m.empty()) break;
        }
        if (m.empty()) m.push_back(0);
        (fx.decode(m) == RCV_OK ? ok : rejected)++;
    }
    CHECK(rejected > 0);
    // A valid I-frame must decode correctly afterwards.
    const Yuv f = make_yuv(w, h, Content::Natural, 41);
    Yuv out = blank_like(f);
    CHECK(decode_into(fx.dec, encode_one(fx.cfg, f), out) == RCV_OK);
    CHECK(same_image(f, out));
    (void)ok;
}
