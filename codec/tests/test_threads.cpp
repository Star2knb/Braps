// Thread pool and multi-threaded determinism (codec plan §8, acceptance A3).
#include <atomic>
#include <mutex>
#include <set>
#include <vector>

#include "test_util.h"
#include "threadpool.h"

using namespace rcv;

namespace {

struct CountCtx {
    std::atomic<int>* hits;
    std::atomic<int>* bad_worker;
    int threads;
};

void count_job(void* ctx, int job, int worker) {
    CountCtx* c = static_cast<CountCtx*>(ctx);
    c->hits[job].fetch_add(1);
    if (worker < 0 || worker >= c->threads) c->bad_worker->fetch_add(1);
}

// Same, but sometimes gives up the time slice so workers are late for the next dispatch -
// the interleaving that exposed the claim race fixed in threadpool.cpp.
void count_job_yield(void* ctx, int job, int worker) {
    count_job(ctx, job, worker);
    if ((job * 7 + worker) % 5 == 0) std::this_thread::yield();
}

std::mutex g_start_mutex;
std::set<int> g_started;

void record_start(void* user, int worker) {
    std::lock_guard<std::mutex> lock(g_start_mutex);
    g_started.insert(worker);
    static_cast<std::atomic<int>*>(user)->fetch_add(1);
}

// Encodes `frames` through one encoder with the given thread count / ISA; returns all packets.
std::vector<std::vector<uint8_t>> encode_sequence(const std::vector<tu::Yuv>& frames, int threads, rcv_isa isa,
                                                  int slices) {
    rcv_encoder_config cfg = tu::config_for(frames[0].w, frames[0].h, 1, slices);
    cfg.num_threads = uint8_t(threads);
    cfg.isa = isa;
    rcv_encoder* enc = nullptr;
    std::vector<std::vector<uint8_t>> out;
    if (rcv_encoder_create(&cfg, &enc) != RCV_OK) return out;
    std::vector<uint8_t> pkt(rcv_max_packet_size(&cfg));
    for (const tu::Yuv& f : frames) {
        const rcv_frame_in in = tu::frame_in(f);
        rcv_frame_info info{};
        if (rcv_encode_frame(enc, &in, nullptr, pkt.data(), pkt.size(), &info) != RCV_OK) break;
        out.emplace_back(pkt.begin(), pkt.begin() + info.packet_size);
    }
    rcv_encoder_destroy(enc);
    return out;
}

}  // namespace

TEST_CASE("threads: pool runs every job exactly once across changing dispatch sizes") {
    for (int threads = 1; threads <= 4; ++threads) {
        ThreadPool pool;
        REQUIRE(pool.start(threads, nullptr, nullptr));
        CHECK(pool.threads() == threads);
        std::atomic<int> hits[64];
        std::atomic<int> bad_worker{0};
        CountCtx ctx{hits, &bad_worker, threads};
        tf::Rng rng{uint64_t(threads)};
        long wrong = 0;
        for (int iter = 0; iter < 20000; ++iter) {
            // Alternating tiny and large dispatches: a worker late for one must not run jobs of the next.
            const int jobs = iter % 2 == 0 ? int(rng.below(3)) : 32 + int(rng.below(33));
            for (auto& h : hits) h.store(0);
            pool.run(iter % 4 == 0 ? count_job_yield : count_job, &ctx, jobs);
            for (int j = 0; j < 64; ++j)
                if (hits[j].load() != (j < jobs ? 1 : 0)) ++wrong;
        }
        CHECK(wrong == 0);
        CHECK(bad_worker.load() == 0);
    }
}

TEST_CASE("threads: on_worker_start is called once per worker with indices 1..n-1") {
    std::atomic<int> calls{0};
    {
        std::lock_guard<std::mutex> lock(g_start_mutex);
        g_started.clear();
    }
    rcv_encoder_config cfg = tu::config_for(64, 64);
    cfg.num_threads = 4;
    cfg.on_worker_start = record_start;
    cfg.user = &calls;
    rcv_encoder* enc = nullptr;
    REQUIRE(rcv_encoder_create(&cfg, &enc) == RCV_OK);
    // Workers call it as they start; give them a moment by encoding a frame (which needs them).
    const tu::Yuv f = tu::make_yuv(64, 64, tu::Content::Natural, 1);
    std::vector<uint8_t> pkt(rcv_max_packet_size(&cfg));
    const rcv_frame_in in = tu::frame_in(f);
    CHECK(rcv_encode_frame(enc, &in, nullptr, pkt.data(), pkt.size(), nullptr) == RCV_OK);
    rcv_encoder_destroy(enc);  // joins the workers, so every on_start has run
    CHECK(calls.load() == 3);
    std::lock_guard<std::mutex> lock(g_start_mutex);
    CHECK((g_started == std::set<int>{1, 2, 3}));
}

TEST_CASE("threads: packets identical for 1-4 threads at every ISA level (A3)") {
    const rcv_isa best = rcv_cpu_isa();
    const int sizes[][2] = {{66, 50}, {322, 180}, {1280, 720}};
    uint64_t seed = 5000;
    for (const auto& sz : sizes) {
        std::vector<tu::Yuv> frames;
        for (tu::Content c : {tu::Content::Natural, tu::Content::Noise, tu::Content::Checker, tu::Content::Natural})
            frames.push_back(tu::make_yuv(sz[0], sz[1], c, seed++, 5));
        const auto ref = encode_sequence(frames, 1, RCV_ISA_SCALAR, 0);
        REQUIRE(ref.size() == frames.size());
        for (int isa = RCV_ISA_SCALAR; isa <= int(best); ++isa)
            for (int threads = 1; threads <= 4; ++threads)
                CHECK(encode_sequence(frames, threads, rcv_isa(isa), 0) == ref);
    }
}

TEST_CASE("threads: multi-threaded decode is bit-exact for 1, 2, 3, 4 threads") {
    const int w = 322, h = 180;
    std::vector<tu::Yuv> frames;
    for (int i = 0; i < 4; ++i) frames.push_back(tu::make_yuv(w, h, tu::kAllContent[i % 6], 7000 + uint64_t(i)));
    for (int slices : {1, 5, 12}) {  // odd slice counts leave an unpaired chunk per plane
        const auto packets = encode_sequence(frames, 2, RCV_ISA_AUTO, slices);
        REQUIRE(packets.size() == frames.size());
        rcv_encoder_config cfg = tu::config_for(w, h, 1, slices);
        uint8_t seq[32];
        rcv_write_sequence_header(&cfg, seq);
        for (int threads = 1; threads <= 4; ++threads) {
            rcv_decoder* dec = nullptr;
            REQUIRE(rcv_decoder_create(seq, uint8_t(threads), &dec) == RCV_OK);
            for (size_t i = 0; i < frames.size(); ++i) {
                tu::Yuv out = tu::blank_like(frames[i], 3);
                CHECK(tu::decode_into(dec, packets[i], out) == RCV_OK);
                CHECK(tu::same_image(frames[i], out));
            }
            rcv_decoder_destroy(dec);
        }
    }
}

TEST_CASE("threads: stress - many small frames through 4-thread encoder and decoder") {
    const int w = 64, h = 48;  // 3 slices x 3 planes = 9 jobs per frame
    rcv_encoder_config cfg = tu::config_for(w, h);
    cfg.num_threads = 4;
    rcv_encoder* enc = nullptr;
    REQUIRE(rcv_encoder_create(&cfg, &enc) == RCV_OK);
    rcv_encoder_config ref_cfg = cfg;
    ref_cfg.num_threads = 1;
    rcv_encoder* ref_enc = nullptr;
    REQUIRE(rcv_encoder_create(&ref_cfg, &ref_enc) == RCV_OK);
    uint8_t seq[32];
    rcv_write_sequence_header(&cfg, seq);
    rcv_decoder* dec = nullptr;
    REQUIRE(rcv_decoder_create(seq, 4, &dec) == RCV_OK);

    std::vector<uint8_t> pkt(rcv_max_packet_size(&cfg)), ref(pkt.size());
    long mismatched = 0, wrong_decode = 0;
    for (int i = 0; i < 600; ++i) {
        const tu::Yuv f = tu::make_yuv(w, h, tu::kAllContent[i % 6], 9000 + uint64_t(i));
        const rcv_frame_in in = tu::frame_in(f);
        rcv_frame_info a{}, b{};
        REQUIRE(rcv_encode_frame(enc, &in, nullptr, pkt.data(), pkt.size(), &a) == RCV_OK);
        REQUIRE(rcv_encode_frame(ref_enc, &in, nullptr, ref.data(), ref.size(), &b) == RCV_OK);
        if (a.packet_size != b.packet_size || std::memcmp(pkt.data(), ref.data(), a.packet_size) != 0) ++mismatched;
        tu::Yuv out = tu::blank_like(f);
        std::vector<uint8_t> p(pkt.begin(), pkt.begin() + a.packet_size);
        if (tu::decode_into(dec, p, out) != RCV_OK || !tu::same_image(f, out)) ++wrong_decode;
    }
    CHECK(mismatched == 0);
    CHECK(wrong_decode == 0);
    rcv_decoder_destroy(dec);
    rcv_encoder_destroy(ref_enc);
    rcv_encoder_destroy(enc);
}
