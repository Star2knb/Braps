// Near-lossless mode (codec plan §5.5; acceptance A2).
#include <algorithm>
#include <cstdlib>
#include <vector>

#include "nearlossless.h"
#include "test_util.h"

using namespace tu;

namespace {

int max_abs_error(const Yuv& a, const Yuv& b) {
    int m = 0;
    for (int i = 0; i < 3; ++i)
        for (int y = 0; y < a.ph(i); ++y)
            for (int x = 0; x < a.pw(i); ++x) m = std::max(m, std::abs(int(a.at(i, x, y)) - int(b.at(i, x, y))));
    return m;
}

struct NearResult {
    std::vector<std::vector<uint8_t>> packets;
    std::vector<int> types, errors;  // per frame: type, max |decoded - source|
    bool ok = true;
};

// Encodes frames[i] with NEAR near[i] through one encoder; decodes every packet and measures the
// largest error against that frame's source.
NearResult run_near(const std::vector<Yuv>& frames, const std::vector<int>& near, rcv_encoder_config cfg,
                    int dec_threads = 2) {
    NearResult r;
    rcv_encoder* enc = nullptr;
    if (rcv_encoder_create(&cfg, &enc) != RCV_OK) {
        r.ok = false;
        return r;
    }
    uint8_t seq[32];
    rcv_write_sequence_header(&cfg, seq);
    rcv_decoder* dec = nullptr;
    rcv_decoder_create(seq, uint8_t(dec_threads), &dec);
    std::vector<uint8_t> pkt(rcv_max_packet_size(&cfg));
    for (size_t i = 0; i < frames.size(); ++i) {
        rcv_encode_params params{};
        params.near_level = uint8_t(near[i % near.size()]);
        const rcv_frame_in in = frame_in(frames[i]);
        rcv_frame_info info{};
        if (rcv_encode_frame(enc, &in, &params, pkt.data(), pkt.size(), &info) != RCV_OK) {
            r.ok = false;
            break;
        }
        r.packets.emplace_back(pkt.begin(), pkt.begin() + info.packet_size);
        r.types.push_back(info.frame_type);
        Yuv out = blank_like(frames[i], 2);
        if (decode_into(dec, r.packets.back(), out) != RCV_OK) {
            r.ok = false;
            r.errors.push_back(999);
            continue;
        }
        r.errors.push_back(max_abs_error(frames[i], out));
    }
    rcv_decoder_destroy(dec);
    rcv_encoder_destroy(enc);
    return r;
}

// Adds random offsets in [-amp, amp] to every sample (clamped).
Yuv jitter(const Yuv& f, int amp, uint64_t seed) {
    tf::Rng rng(seed);
    Yuv o = f;
    for (int i = 0; i < 3; ++i)
        for (int y = 0; y < o.ph(i); ++y)
            for (int x = 0; x < o.pw(i); ++x) {
                const int v = int(o.at(i, x, y)) + int(rng.below(uint32_t(2 * amp + 1))) - amp;
                o.at(i, x, y) = clamp8(v);
            }
    return o;
}

}  // namespace

TEST_CASE("near: quantiser keeps |rec - x| <= n for every NEAR, prediction and sample; tables match") {
    long bad = 0;
    for (int n = 1; n <= rcv::kMaxNear; ++n) {
        rcv::NearTables t;
        rcv::init_near_tables(n, &t);
        CHECK(t.step == 2 * n + 1);
        for (int pred = 0; pred < 256; ++pred)
            for (int x = 0; x < 256; ++x) {
                const int q = rcv::near_quantise(x - pred, n);
                const int rec = rcv::near_reconstruct(pred, q, n);
                if (std::abs(rec - x) > n || q < -128 || q > 127) ++bad;
                if (t.q[x - pred + 255] != q) ++bad;                                 // encoder table
                if (rcv::clamp255(pred + t.dq[uint8_t(q)]) != rec) ++bad;            // decoder table
            }
        for (int s = 0; s < 256; ++s)  // any byte, even from a corrupt stream, dequantises in range
            if (t.dq[s] != int8_t(uint8_t(s)) * t.step) ++bad;
    }
    CHECK(bad == 0);
}

TEST_CASE("near: error bounded by NEAR on I-frames (A2), sizes x contents x predictors x slices") {
    const int sizes[][2] = {{2, 2}, {18, 18}, {66, 50}, {322, 180}};
    uint64_t seed = 60;
    for (const auto& sz : sizes)
        for (Content c : kAllContent)
            for (int pred = 0; pred < 2; ++pred)
                for (int n = 1; n <= 3; ++n) {
                    const int by = (sz[1] + 15) / 16;
                    for (int slices : {0, by}) {
                        rcv_encoder_config cfg = config_for(sz[0], sz[1], pred, slices);
                        const NearResult r = run_near({make_yuv(sz[0], sz[1], c, seed++, 3)}, {n}, cfg);
                        CHECK(r.ok);
                        REQUIRE(!r.errors.empty());
                        CHECK(r.errors[0] <= n);
                        CHECK(r.packets[0][9] == n);           // NEAR in the header
                        CHECK((r.packets[0][6] & 2) != 0);     // near-lossless flag
                    }
                }
}

TEST_CASE("near: smaller than lossless on natural content, larger NEAR smaller still") {
    const Yuv f = make_yuv(320, 176, Content::Natural, 5);
    size_t prev = 0;
    for (int n = 0; n <= 3; ++n) {
        const NearResult r = run_near({f}, {n}, config_for(320, 176));
        REQUIRE(r.ok);
        CHECK(r.errors[0] <= n);
        if (n) CHECK(r.packets[0].size() < prev);
        prev = r.packets[0].size();
    }
}

TEST_CASE("near: no drift over P-frame chains; changes within NEAR of the reference become DUP") {
    // Phase A compares with the *reference* - the previous reconstruction, itself up to NEAR away
    // from its source - so "within NEAR" is measured from the decoded frame, which is identical to
    // the encoder's reference.
    const int w = 130, h = 98;
    for (int n = 1; n <= 3; ++n) {
        rcv_encoder_config cfg = config_for(w, h);
        rcv_encoder* enc = nullptr;
        REQUIRE(rcv_encoder_create(&cfg, &enc) == RCV_OK);
        uint8_t seqh[32];
        rcv_write_sequence_header(&cfg, seqh);
        rcv_decoder* dec = nullptr;
        REQUIRE(rcv_decoder_create(seqh, 2, &dec) == RCV_OK);
        std::vector<uint8_t> pkt(rcv_max_packet_size(&cfg));
        rcv_encode_params params{};
        params.near_level = uint8_t(n);
        auto step = [&](const Yuv& src, int* type, int* err) -> Yuv {
            const rcv_frame_in in = frame_in(src);
            rcv_frame_info info{};
            CHECK(rcv_encode_frame(enc, &in, &params, pkt.data(), pkt.size(), &info) == RCV_OK);
            *type = info.frame_type;
            Yuv out = blank_like(src);
            CHECK(decode_into(dec, std::vector<uint8_t>(pkt.begin(), pkt.begin() + info.packet_size), out) == RCV_OK);
            *err = max_abs_error(src, out);
            return out;
        };
        int type = 0, err = 0;
        const Yuv rec = step(make_yuv(w, h, Content::Natural, 9), &type, &err);
        CHECK(type == 1);
        CHECK(err <= n);
        for (int i = 0; i < 6; ++i) {  // every sample within +-n of the reference -> DUP
            step(jitter(rec, n, 100 + uint64_t(i)), &type, &err);
            CHECK(type == 0);
            CHECK(err <= n);
        }
        int p_frames = 0;
        for (int s = 0; s < 12; ++s) {  // a square moving over the static, jittered scene -> P
            Yuv g = jitter(rec, n, 200 + uint64_t(s));
            for (int y = 30; y < 60; ++y)
                for (int x = 5 * s; x < 5 * s + 25 && x < w; ++x) g.at(0, x, y) = uint8_t(20 * s);
            step(g, &type, &err);
            CHECK(err <= n);  // no drift along the P chain
            p_frames += type == 2;
        }
        CHECK(p_frames == 12);
        rcv_decoder_destroy(dec);
        rcv_encoder_destroy(enc);
    }
}

TEST_CASE("near: NEAR switched on every frame (rate controller) stays within each frame's bound") {
    const int w = 96, h = 64;
    std::vector<Yuv> seq;
    Yuv f = make_yuv(w, h, Content::Natural, 3);
    for (int i = 0; i < 24; ++i) {
        for (int y = 10; y < 30; ++y)
            for (int x = (3 * i) % 60; x < (3 * i) % 60 + 20; ++x) f.at(0, x, y) = uint8_t(i * 9 + x);
        seq.push_back(i % 5 == 4 ? seq.back() : f);  // some repeats
    }
    const std::vector<int> near = {0, 1, 3, 2, 0, 2, 1, 3, 3, 0, 1, 2};
    const NearResult r = run_near(seq, near, config_for(w, h));
    CHECK(r.ok);
    for (size_t i = 0; i < r.errors.size(); ++i) CHECK(r.errors[i] <= near[i % near.size()]);
    // A lossless frame after near-lossless ones must be exact again.
    for (size_t i = 0; i < r.errors.size(); ++i)
        if (near[i % near.size()] == 0 && r.types[i] != 0) CHECK(r.errors[i] == 0);
}

TEST_CASE("near: packets identical for every ISA and 1-4 threads; NV12 = I420") {
    const int w = 162, h = 98;
    std::vector<Yuv> seq;
    Yuv f = make_yuv(w, h, Content::Natural, 12);
    for (int i = 0; i < 6; ++i) {
        for (int y = 20; y < 50; ++y)
            for (int x = 10 * i; x < 10 * i + 30; ++x) f.at(0, x, y) = uint8_t(x * i);
        seq.push_back(f);
    }
    const std::vector<int> near = {2, 1, 3, 0, 2, 2};
    rcv_encoder_config cfg = config_for(w, h);
    cfg.isa = RCV_ISA_SCALAR;
    cfg.num_threads = 1;
    const NearResult ref = run_near(seq, near, cfg);
    REQUIRE(ref.ok);
    for (int isa = RCV_ISA_SCALAR; isa <= int(rcv_cpu_isa()); ++isa)
        for (int threads = 1; threads <= 4; ++threads) {
            cfg.isa = rcv_isa(isa);
            cfg.num_threads = uint8_t(threads);
            CHECK(run_near(seq, near, cfg, threads).packets == ref.packets);
        }

    // NV12 input: same packets.
    cfg = config_for(w, h);
    cfg.input_layout = RCV_IN_NV12;
    rcv_encoder* enc = nullptr;
    REQUIRE(rcv_encoder_create(&cfg, &enc) == RCV_OK);
    std::vector<uint8_t> pkt(rcv_max_packet_size(&cfg));
    for (size_t i = 0; i < seq.size(); ++i) {
        const Yuv& s = seq[i];
        std::vector<uint8_t> uv(size_t(w) * (h / 2));
        for (int r = 0; r < h / 2; ++r)
            for (int x = 0; x < w / 2; ++x) {
                uv[size_t(r) * w + 2 * x] = s.at(1, x, r);
                uv[size_t(r) * w + 2 * x + 1] = s.at(2, x, r);
            }
        rcv_frame_in in{};
        in.plane[0] = s.p[0].data();
        in.stride[0] = s.stride[0];
        in.plane[1] = uv.data();
        in.stride[1] = w;
        rcv_encode_params params{};
        params.near_level = uint8_t(near[i]);
        rcv_frame_info info{};
        REQUIRE(rcv_encode_frame(enc, &in, &params, pkt.data(), pkt.size(), &info) == RCV_OK);
        CHECK(std::vector<uint8_t>(pkt.begin(), pkt.begin() + info.packet_size) == ref.packets[i]);
    }
    rcv_encoder_destroy(enc);
}

TEST_CASE("near: random corruption of near-lossless packets never crashes") {
    const Yuv f = make_yuv(64, 48, Content::Natural, 4);
    const NearResult r = run_near({f, jitter(f, 3, 1), make_yuv(64, 48, Content::Noise, 5)}, {2, 3, 1},
                                  config_for(64, 48, 1, 3));
    REQUIRE(r.ok);
    rcv_encoder_config cfg = config_for(64, 48, 1, 3);
    uint8_t seq[32];
    rcv_write_sequence_header(&cfg, seq);
    rcv_decoder* dec = nullptr;
    REQUIRE(rcv_decoder_create(seq, 2, &dec) == RCV_OK);
    tf::Rng rng(8);
    int rejected = 0;
    for (int iter = 0; iter < 3000; ++iter) {
        if (iter % 3 == 0)
            rcv_decode_frame(dec, r.packets[0].data(), r.packets[0].size(), RCV_OUT_I420, nullptr, nullptr, nullptr);
        std::vector<uint8_t> m = r.packets[rng.below(uint32_t(r.packets.size()))];
        m[rng.below(uint32_t(m.size()))] ^= uint8_t(1u << rng.below(8));
        if (rng.below(4) == 0) m.resize(rng.below(uint32_t(m.size())) + 1);
        if (rcv_decode_frame(dec, m.data(), m.size(), RCV_OUT_I420, nullptr, nullptr, nullptr) != RCV_OK) ++rejected;
    }
    CHECK(rejected > 0);
    rcv_decoder_destroy(dec);
}
