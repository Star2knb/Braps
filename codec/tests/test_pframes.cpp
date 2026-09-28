// Temporal skip, P-frames, DUP and keyframe logic (codec plan §5.2-§5.4, §6.3; acceptance A1, A3, A10).
#include <algorithm>
#include <chrono>
#include <vector>

#include "format.h"
#include "skipmap.h"
#include "test_util.h"

using namespace tu;

namespace {

// Encodes frames through one encoder and decodes every packet through one decoder, checking each
// decoded frame against its source. Returns the frame type of each packet.
struct SeqResult {
    std::vector<int> types;
    std::vector<uint32_t> skipped;
    std::vector<std::vector<uint8_t>> packets;
    bool all_exact = true;
};

SeqResult run_sequence(const std::vector<Yuv>& frames, rcv_encoder_config cfg, const std::vector<int>& force = {}) {
    SeqResult r;
    rcv_encoder* enc = nullptr;
    if (rcv_encoder_create(&cfg, &enc) != RCV_OK) {
        r.all_exact = false;
        return r;
    }
    uint8_t seq[32];
    rcv_write_sequence_header(&cfg, seq);
    rcv_decoder* dec = nullptr;
    rcv_decoder_create(seq, 2, &dec);
    std::vector<uint8_t> pkt(rcv_max_packet_size(&cfg));
    for (size_t i = 0; i < frames.size(); ++i) {
        const rcv_frame_in in = frame_in(frames[i]);
        rcv_encode_params params{};
        params.force_keyframe = std::find(force.begin(), force.end(), int(i)) != force.end();
        rcv_frame_info info{};
        if (rcv_encode_frame(enc, &in, &params, pkt.data(), pkt.size(), &info) != RCV_OK) {
            r.all_exact = false;
            break;
        }
        r.types.push_back(info.frame_type);
        r.skipped.push_back(info.blocks_skipped);
        r.packets.emplace_back(pkt.begin(), pkt.begin() + info.packet_size);
        Yuv out = blank_like(frames[i], 1);
        if (decode_into(dec, r.packets.back(), out) != RCV_OK || !same_image(frames[i], out)) r.all_exact = false;
    }
    rcv_decoder_destroy(dec);
    rcv_encoder_destroy(enc);
    return r;
}

// A sequence exercising every kind of change: identical frames, one pixel, a moving square,
// chroma-only, the right/bottom edge blocks, a noisy region, and a full scene change.
std::vector<Yuv> change_sequence(int w, int h, uint64_t seed) {
    std::vector<Yuv> v;
    Yuv f = make_yuv(w, h, Content::Natural, seed, 3);
    v.push_back(f);
    v.push_back(f);  // identical -> DUP
    f.at(0, w / 3, h / 2) ^= 0x40;  // one luma pixel -> P
    v.push_back(f);
    for (int step = 0; step < 4; ++step) {  // moving 20x12 square
        const int x0 = (step * 13) % std::max(1, w - 20), y0 = (step * 7) % std::max(1, h - 12);
        for (int y = y0; y < std::min(h, y0 + 12); ++y)
            for (int x = x0; x < std::min(w, x0 + 20); ++x) f.at(0, x, y) = uint8_t(40 * step + x);
        v.push_back(f);
    }
    f.at(1, (w / 2) - 1, (h / 2) - 1) ^= 1;  // chroma only, bottom-right chroma sample
    v.push_back(f);
    f.at(2, 0, 0) ^= 0x80;  // Cr only, top-left
    v.push_back(f);
    f.at(0, w - 1, h - 1) ^= 0x10;  // last (possibly partial) block
    v.push_back(f);
    const Yuv noise = make_yuv(w, h, Content::Noise, seed + 1, 3);
    for (int y = 0; y < h / 2; ++y)  // noisy top-left quadrant
        for (int x = 0; x < w / 2; ++x) f.at(0, x, y) = noise.at(0, x, y);
    v.push_back(f);
    v.push_back(f);  // DUP again
    v.push_back(make_yuv(w, h, Content::Gradient, seed + 2, 3));  // scene change -> I
    return v;
}

}  // namespace

TEST_CASE("pframes: skip map pack/unpack round trip and padding checks") {
    rcv::Geometry g;
    REQUIRE(rcv::init_geometry(&g, RCV_FMT_YUV420, 1366, 770, 0));  // 86 x 49 blocks
    const size_t blocks = size_t(g.blocks_x) * g.blocks_y;
    tf::Rng rng(3);
    std::vector<uint8_t> flags(blocks), back(blocks), map(rcv::skip_map_size(g));
    for (auto& f : flags) f = rng.below(2) ? 1 : 0;
    rcv::pack_skip_map(g, flags.data(), map.data());
    CHECK(rcv::unpack_skip_map(g, map.data(), back.data()));
    CHECK(flags == back);
    std::vector<uint8_t> bad = map;
    bad[blocks >> 3] |= uint8_t(0x80);  // a bit past the last block (4214 blocks: 6 bits used in the last byte)
    CHECK(!rcv::unpack_skip_map(g, bad.data(), back.data()));
    bad = map;
    bad.back() = 1;  // padding byte
    CHECK(!rcv::unpack_skip_map(g, bad.data(), back.data()));
}

TEST_CASE("pframes: frame types follow §5.2 (DUP, P, I on change, keyframe interval, force, disabled)") {
    const std::vector<Yuv> seq = change_sequence(96, 64, 11);
    rcv_encoder_config cfg = config_for(96, 64);
    const SeqResult r = run_sequence(seq, cfg);
    CHECK(r.all_exact);
    REQUIRE(r.types.size() == seq.size());
    CHECK(r.types[0] == rcv::kFrameI);    // no reference
    CHECK(r.types[1] == rcv::kFrameDup);  // identical
    CHECK(r.types[2] == rcv::kFrameP);    // one pixel
    CHECK(r.skipped[2] == 6 * 4 - 1);     // 6 x 4 blocks, one changed
    CHECK(r.packets[1].size() == 8);
    for (size_t i = 3; i <= 10; ++i) CHECK(r.types[i] == rcv::kFrameP);  // square, chroma, edge, noise
    CHECK(r.types[11] == rcv::kFrameDup);
    CHECK(r.types[12] == rcv::kFrameI);   // every block changed

    // Keyframe interval 4: counts every packet, DUPs included.
    cfg.keyframe_interval = 4;
    const SeqResult k = run_sequence(seq, cfg);
    CHECK(k.all_exact);
    for (size_t i = 0; i < k.types.size(); i += 4) CHECK(k.types[i] == rcv::kFrameI);

    // force_keyframe wins even over an identical frame.
    cfg.keyframe_interval = 120;
    const SeqResult f = run_sequence(seq, cfg, {1, 5});
    CHECK(f.all_exact);
    CHECK(f.types[1] == rcv::kFrameI);
    CHECK(f.types[5] == rcv::kFrameI);

    // Skip disabled: every frame is an I-frame.
    cfg.enable_skip = 0;
    const SeqResult d = run_sequence(seq, cfg);
    CHECK(d.all_exact);
    for (int t : d.types) CHECK(t == rcv::kFrameI);
}

TEST_CASE("pframes: round trips over change patterns, sizes, slices, predictors (A1)") {
    const int sizes[][2] = {{18, 18}, {34, 20}, {96, 64}, {322, 180}, {1366, 770}};
    uint64_t seed = 100;
    for (const auto& sz : sizes)
        for (int pred = 0; pred < 2; ++pred) {
            const std::vector<Yuv> seq = change_sequence(sz[0], sz[1], seed++);
            const int by = (sz[1] + 15) / 16;
            for (int slices : {0, 1, by}) {
                const SeqResult r = run_sequence(seq, config_for(sz[0], sz[1], pred, slices));
                CHECK(r.all_exact);
                CHECK(std::count(r.types.begin(), r.types.end(), int(rcv::kFrameP)) > 0);
            }
        }
}

TEST_CASE("pframes: NV12 input gives the same packets as I420 (incl. P and DUP)") {
    const std::vector<Yuv> seq = change_sequence(98, 66, 5);
    rcv_encoder_config cfg = config_for(98, 66);
    const SeqResult ref = run_sequence(seq, cfg);
    REQUIRE(ref.all_exact);

    cfg.input_layout = RCV_IN_NV12;
    rcv_encoder* enc = nullptr;
    REQUIRE(rcv_encoder_create(&cfg, &enc) == RCV_OK);
    std::vector<uint8_t> pkt(rcv_max_packet_size(&cfg));
    for (size_t i = 0; i < seq.size(); ++i) {
        const Yuv& f = seq[i];
        const int uvs = f.w + 6;
        std::vector<uint8_t> uv(size_t(uvs) * (f.h / 2), 0);
        for (int r = 0; r < f.h / 2; ++r)
            for (int x = 0; x < f.w / 2; ++x) {
                uv[size_t(r) * uvs + 2 * x] = f.at(1, x, r);
                uv[size_t(r) * uvs + 2 * x + 1] = f.at(2, x, r);
            }
        rcv_frame_in in{};
        in.plane[0] = f.p[0].data();
        in.stride[0] = f.stride[0];
        in.plane[1] = uv.data();
        in.stride[1] = uvs;
        rcv_frame_info info{};
        REQUIRE(rcv_encode_frame(enc, &in, nullptr, pkt.data(), pkt.size(), &info) == RCV_OK);
        CHECK(std::vector<uint8_t>(pkt.begin(), pkt.begin() + info.packet_size) == ref.packets[i]);
    }
    rcv_encoder_destroy(enc);
}

TEST_CASE("pframes: packets identical for every ISA and 1-4 threads (A3)") {
    const std::vector<Yuv> seq = change_sequence(322, 180, 77);
    rcv_encoder_config cfg = config_for(322, 180);
    cfg.isa = RCV_ISA_SCALAR;
    cfg.num_threads = 1;
    const SeqResult ref = run_sequence(seq, cfg);
    REQUIRE(ref.all_exact);
    for (int isa = RCV_ISA_SCALAR; isa <= int(rcv_cpu_isa()); ++isa)
        for (int threads = 1; threads <= 4; ++threads) {
            cfg.isa = rcv_isa(isa);
            cfg.num_threads = uint8_t(threads);
            CHECK(run_sequence(seq, cfg).packets == ref.packets);
        }
}

TEST_CASE("pframes: rcv_encode_duplicate is 8 bytes, needs no input, is fast (A10), counts toward keyint") {
    const Yuv f = make_yuv(64, 48, Content::Natural, 1);
    rcv_encoder_config cfg = config_for(64, 48);
    cfg.keyframe_interval = 3;
    rcv_encoder* enc = nullptr;
    REQUIRE(rcv_encoder_create(&cfg, &enc) == RCV_OK);
    std::vector<uint8_t> pkt(rcv_max_packet_size(&cfg));
    const rcv_frame_in in = frame_in(f);
    rcv_frame_info info{};
    REQUIRE(rcv_encode_frame(enc, &in, nullptr, pkt.data(), pkt.size(), &info) == RCV_OK);
    CHECK(info.frame_type == rcv::kFrameI);
    CHECK(rcv_encode_duplicate(enc, pkt.data(), pkt.size(), &info) == RCV_OK);
    CHECK(info.packet_size == 8);
    CHECK(rcv_encode_duplicate(enc, pkt.data(), pkt.size(), &info) == RCV_OK);
    // Three packets since the I-frame: the next coded frame must be a keyframe.
    REQUIRE(rcv_encode_frame(enc, &in, nullptr, pkt.data(), pkt.size(), &info) == RCV_OK);
    CHECK(info.frame_type == rcv::kFrameI);

    const int n = 20000;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < n; ++i) rcv_encode_duplicate(enc, pkt.data(), 8, nullptr);
    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / n;
    CHECK(us < 1.0);
    rcv_encoder_destroy(enc);
}

TEST_CASE("pframes: decoder rejects bad P-frames and recovers") {
    const std::vector<Yuv> seq = change_sequence(96, 64, 21);
    rcv_encoder_config cfg = config_for(96, 64);
    const SeqResult r = run_sequence(seq, cfg);
    REQUIRE(r.all_exact);
    REQUIRE(r.types[2] == rcv::kFrameP);
    uint8_t seqh[32];
    rcv_write_sequence_header(&cfg, seqh);
    rcv_decoder* dec = nullptr;
    REQUIRE(rcv_decoder_create(seqh, 1, &dec) == RCV_OK);
    auto decode = [&](const std::vector<uint8_t>& p) {
        return rcv_decode_frame(dec, p.data(), p.size(), RCV_OUT_I420, nullptr, nullptr, nullptr);
    };
    CHECK(decode(r.packets[2]) == RCV_ERR_NO_REFERENCE);  // P before any I
    CHECK(decode(r.packets[0]) == RCV_OK);
    std::vector<uint8_t> bad = r.packets[2];
    bad[32 + 3] = 0xFF;  // skip-map byte 3: 24 blocks use bytes 0-2, byte 3 is padding
    CHECK(decode(bad) == RCV_ERR_BITSTREAM);
    CHECK(decode(r.packets[2]) == RCV_OK);  // rejected before decoding: the reference is intact
    bad = r.packets[3];
    bad[32] ^= 0x01;  // un-skip block 0: the chunk sizes no longer match the coded sample counts
    CHECK(decode(bad) == RCV_ERR_BITSTREAM);
    // That one failed midway, after overwriting part of the reference, so the decoder must refuse
    // P-frames until the next I-frame.
    CHECK(decode(r.packets[3]) == RCV_ERR_NO_REFERENCE);
    CHECK(decode(r.packets[0]) == RCV_OK);
    CHECK(decode(r.packets[2]) == RCV_OK);
    rcv_decoder_reset(dec);
    CHECK(decode(r.packets[3]) == RCV_ERR_NO_REFERENCE);  // P after a seek
    rcv_decoder_destroy(dec);
}

TEST_CASE("pframes: random corruption of P and DUP packets never crashes") {
    const std::vector<Yuv> seq = change_sequence(80, 48, 31);
    rcv_encoder_config cfg = config_for(80, 48, 1, 3);
    const SeqResult r = run_sequence(seq, cfg);
    REQUIRE(r.all_exact);
    uint8_t seqh[32];
    rcv_write_sequence_header(&cfg, seqh);
    rcv_decoder* dec = nullptr;
    REQUIRE(rcv_decoder_create(seqh, 2, &dec) == RCV_OK);
    tf::Rng rng(55);
    int rejected = 0;
    for (int iter = 0; iter < 4000; ++iter) {
        // Keep a valid reference most of the time so P-frames get past the reference check.
        if (iter % 3 == 0) rcv_decode_frame(dec, r.packets[0].data(), r.packets[0].size(), RCV_OUT_I420, nullptr,
                                            nullptr, nullptr);
        std::vector<uint8_t> m = r.packets[1 + rng.below(uint32_t(r.packets.size() - 1))];
        for (int e = 0; e < 1 + int(rng.below(3)); ++e) {
            if (m.empty()) break;
            switch (rng.below(3)) {
            case 0: m[rng.below(uint32_t(m.size()))] ^= uint8_t(1u << rng.below(8)); break;
            case 1: m[rng.below(uint32_t(m.size()))] = rng.byte(); break;
            case 2: m.resize(rng.below(uint32_t(m.size()) + 1)); break;
            }
        }
        if (m.empty()) m.push_back(0);
        if (rcv_decode_frame(dec, m.data(), m.size(), RCV_OUT_I420, nullptr, nullptr, nullptr) != RCV_OK) ++rejected;
    }
    CHECK(rejected > 0);
    rcv_decoder_destroy(dec);
}
