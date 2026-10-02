// The encoding pipeline (recorder plan §6.3, §8.3): timeline with DUP filling, RCV1 encoder, packet
// queue, AVI writer and telemetry, checked by decoding the file it wrote.
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include "rec/avi.h"
#include "rec/pipeline.h"
#include "test_util.h"
#include "testfw.h"

using namespace rec;

namespace {

constexpr uint32_t kW = 128, kH = 96;
constexpr int64_t kFreq = 10000000;  // the fake clock ticks at 10 MHz
constexpr uint32_t kFps = 60;

struct Nv12 {
    std::vector<uint8_t> y, uv;
};

// A frame whose content depends on `content` only (so equal numbers give identical frames).
Nv12 make_frame(uint32_t content) {
    Nv12 f;
    f.y.resize(size_t(kW) * kH);
    f.uv.resize(size_t(kW) * kH / 2);
    uint32_t x = content * 2654435761u + 12345;
    for (uint32_t row = 0; row < kH; ++row)
        for (uint32_t col = 0; col < kW; ++col) {
            x = x * 1664525u + 1013904223u;
            f.y[size_t(row) * kW + col] = uint8_t((col * 3 + row * 5 + content * 11) + ((x >> 28) & 7));
        }
    for (size_t i = 0; i < f.uv.size(); ++i) f.uv[i] = uint8_t(128 + (int(i % 64) - 32) / 4 + int(content % 5));
    return f;
}

struct Fixture {
    rt::TempDir dir;
    int64_t now = 0;
    PipelineConfig cfg;
    explicit Fixture(const char* tag) : dir(tag) {
        cfg.avi_path = dir.path() / "t.avi";
        cfg.csv_path = dir.path() / "t.frames.csv";
        cfg.width = kW;
        cfg.height = kH;
        cfg.fps = kFps;
        cfg.source_w = 160;
        cfg.source_h = 90;
        cfg.game = "test.exe";
        cfg.t0_qpc = 1000000;
        cfg.qpc_frequency = kFreq;
        cfg.queue_mb = 1;
        cfg.disk_buffer_mb = 1;
        cfg.clock = [this] { return now; };
        now = cfg.t0_qpc;
    }
    int64_t tick_time(uint64_t tick) const { return cfg.t0_qpc + int64_t(tick) * kFreq / kFps; }
};

// Decodes the whole AVI to NV12 frames.
bool decode_all(const std::filesystem::path& path, std::vector<Nv12>* frames, std::vector<int>* types, std::vector<bool>* keys) {
    const AviScan scan = scan_avi(path);
    if (!scan.ok || !scan.problems.empty()) {
        for (const std::string& p : scan.problems) std::fprintf(stderr, "avi problem: %s\n", p.c_str());
        return false;
    }
    rcv_decoder* dec = nullptr;
    if (rcv_decoder_create(scan.info.sequence_header, 1, &dec) != RCV_OK) return false;
    AviFile file;
    if (!file.open(path)) return false;
    for (const AviChunk& c : scan.video) {
        std::vector<uint8_t> packet;
        if (!file.read(c.data_offset, c.size, &packet)) return false;
        Nv12 f;
        f.y.resize(size_t(kW) * kH);
        f.uv.resize(size_t(kW) * kH / 2);
        uint8_t* planes[3] = {f.y.data(), f.uv.data(), nullptr};
        const int32_t strides[3] = {int32_t(kW), int32_t(kW), 0};
        rcv_frame_info info{};
        if (rcv_decode_frame(dec, packet.data(), packet.size(), RCV_OUT_NV12, planes, strides, &info) != RCV_OK) return false;
        frames->push_back(std::move(f));
        types->push_back(info.frame_type);
        keys->push_back(c.key);
    }
    rcv_decoder_destroy(dec);
    return true;
}

}  // namespace

TEST_CASE("pipeline: gaps become DUPs, the tail is padded, and every frame decodes back exactly") {
    Fixture fx("pipe1");
    EncodePipeline p;
    std::string error;
    REQUIRE(p.start(fx.cfg, &error));

    // Ticks and content. 8 and 9 are missing; 12 repeats 11's picture (the codec turns it into a DUP);
    // 13..19 are missing.
    struct In {
        uint64_t tick;
        uint32_t content;
    };
    const In inputs[] = {{5, 1}, {6, 2}, {7, 3}, {10, 4}, {11, 5}, {12, 5}, {20, 6}};
    std::vector<Nv12> sent;
    for (const In& in : inputs) {
        sent.push_back(make_frame(in.content));
        FrameMeta m;
        m.tick = in.tick;
        m.present_qpc = fx.tick_time(in.tick);
        m.game_frame_time_us = 16667;
        m.hook_cost_us = 300;
        fx.now = fx.tick_time(in.tick) + kFreq / 100;
        p.on_frame(m, sent.back().y.data(), kW, sent.back().uv.data(), kW);
    }
    fx.now = fx.tick_time(24);
    REQUIRE(p.finish(fx.tick_time(24), &error));

    const PipelineStats& s = p.stats();
    CHECK(s.output_frames == 19);  // ticks 5..23
    CHECK(s.frames_i + s.frames_p + s.dup_encoded == 7);
    CHECK(s.dup_encoded == 1);     // tick 12
    CHECK(s.dup_filled == 2 + 7 + 3);
    CHECK(s.dup_stall == 0);
    CHECK(s.dropped_late == 0 && s.dropped_queue == 0 && s.dropped_error == 0);
    CHECK(s.first_tick == 5 && s.last_tick == 23);
    CHECK(s.ratio_real > 1.0);
    CHECK(s.file_bytes > 4096);

    std::vector<Nv12> frames;
    std::vector<int> types;
    std::vector<bool> keys;
    REQUIRE(decode_all(fx.cfg.avi_path, &frames, &types, &keys));
    REQUIRE(frames.size() == 19);
    CHECK(types[0] == 1 && keys[0]);  // the first frame is an I-frame and a keyframe
    for (uint64_t tick = 5; tick < 24; ++tick) {
        // The picture on a tick is the newest frame at or before it.
        size_t src = 0;
        for (size_t i = 0; i < std::size(inputs); ++i)
            if (inputs[i].tick <= tick) src = i;
        const Nv12& expect = sent[src];
        const Nv12& got = frames[tick - 5];
        CHECK(got.y == expect.y);
        CHECK(got.uv == expect.uv);
    }

    // Telemetry: a header and one row per output frame; DUP rows are marked.
    const std::string csv = rt::read_file(fx.cfg.csv_path);
    std::istringstream in(csv);
    std::string line;
    std::vector<std::string> lines;
    while (std::getline(in, line)) lines.push_back(line);
    REQUIRE(lines.size() == 20);
    CHECK(lines[0].rfind("out_index,type,tick,present_qpc_us", 0) == 0);
    CHECK(lines[1].rfind("0,I,5,", 0) == 0);
    CHECK(lines[4].find(",DUP,8,") != std::string::npos);   // tick 8 filled
    CHECK(lines[13].find(",DUP,17,") != std::string::npos);
    int dups = 0;
    for (size_t i = 1; i < lines.size(); ++i)
        if (lines[i].find(",DUP,") != std::string::npos) ++dups;
    CHECK(dups == 13);  // 12 filled + 1 found by the codec
}

TEST_CASE("pipeline: a frame for a tick that was filled already is dropped, not written twice") {
    Fixture fx("pipe2");
    EncodePipeline p;
    std::string error;
    REQUIRE(p.start(fx.cfg, &error));
    const Nv12 a = make_frame(1), b = make_frame(2), c = make_frame(3);
    FrameMeta m;
    m.tick = 3;
    m.present_qpc = fx.tick_time(3);
    p.on_frame(m, a.y.data(), kW, a.uv.data(), kW);
    m.tick = 6;  // 4 and 5 filled with DUPs
    p.on_frame(m, b.y.data(), kW, b.uv.data(), kW);
    m.tick = 4;  // arrives late
    p.on_frame(m, c.y.data(), kW, c.uv.data(), kW);
    fx.now = fx.tick_time(8);
    REQUIRE(p.finish(fx.tick_time(8), &error));
    CHECK(p.stats().dropped_late == 1);
    CHECK(p.stats().output_frames == 5);  // ticks 3..7
    const std::string csv = rt::read_file(fx.cfg.csv_path);
    CHECK(csv.find(",DROP,4,") != std::string::npos);
}

TEST_CASE("pipeline: a stall is filled with DUPs on its own, and the video resumes in step") {
    Fixture fx("pipe3");
    EncodePipeline p;
    std::string error;
    REQUIRE(p.start(fx.cfg, &error));
    const Nv12 a = make_frame(1), b = make_frame(2);
    FrameMeta m;
    m.tick = 10;
    m.present_qpc = fx.tick_time(10);
    fx.now = fx.tick_time(10);
    p.on_frame(m, a.y.data(), kW, a.uv.data(), kW);

    // 100 ms of quiet is not a stall yet.
    p.on_idle(fx.tick_time(10) + kFreq / 10);
    CHECK(p.stats().dup_filled == 0);
    // Two seconds of nothing: ticks older than now - 150 ms are filled.
    const int64_t later = fx.tick_time(10) + 2 * kFreq;
    p.on_idle(later);
    // Filled: ticks 11 .. floor((later - 150ms - t0) * 60 / freq) - 1.
    const uint64_t limit = uint64_t(later - kFreq * 150 / 1000 - fx.cfg.t0_qpc) * kFps / kFreq;
    fx.now = later;

    // The game wakes up with a frame for a tick after the filled ones.
    m.tick = limit + 2;
    m.present_qpc = fx.tick_time(m.tick);
    p.on_frame(m, b.y.data(), kW, b.uv.data(), kW);
    fx.now = fx.tick_time(m.tick + 3);
    REQUIRE(p.finish(fx.tick_time(m.tick + 3), &error));
    const PipelineStats& s = p.stats();
    CHECK(s.dup_stall == limit - 11);
    CHECK(s.dropped_late == 0);
    CHECK(s.output_frames == (m.tick + 3) - 10);  // one frame per tick from the first to the stop

    std::vector<Nv12> frames;
    std::vector<int> types;
    std::vector<bool> keys;
    REQUIRE(decode_all(fx.cfg.avi_path, &frames, &types, &keys));
    REQUIRE(frames.size() == s.output_frames);
    CHECK(frames[1].y == a.y);                     // DUPs repeat the last picture during the freeze
    CHECK(frames[limit - 10].y == a.y);
    CHECK(frames[m.tick - 10].y == b.y);           // and the new picture appears on its tick
}

TEST_CASE("pipeline: the packet queue wraps correctly over many frames") {
    Fixture fx("pipe4");
    EncodePipeline p;
    std::string error;
    REQUIRE(p.start(fx.cfg, &error));
    constexpr int kFrames = 900;  // tens of MB through a 1 MB queue
    std::vector<Nv12> sent;
    for (int i = 0; i < kFrames; ++i) {
        sent.push_back(make_frame(uint32_t(i / 2 + 1)));  // every second frame repeats: P / DUP mix
        FrameMeta m;
        m.tick = uint64_t(i);
        m.present_qpc = fx.tick_time(m.tick);
        fx.now = m.present_qpc;
        p.on_frame(m, sent.back().y.data(), kW, sent.back().uv.data(), kW);
    }
    fx.now = fx.tick_time(kFrames);
    REQUIRE(p.finish(fx.now, &error));
    CHECK(p.stats().output_frames == kFrames);
    CHECK(p.stats().dropped_queue == 0);
    std::vector<Nv12> frames;
    std::vector<int> types;
    std::vector<bool> keys;
    REQUIRE(decode_all(fx.cfg.avi_path, &frames, &types, &keys));
    REQUIRE(frames.size() == size_t(kFrames));
    int wrong = 0;
    for (int i = 0; i < kFrames; ++i)
        if (frames[i].y != sent[i].y || frames[i].uv != sent[i].uv) ++wrong;
    CHECK(wrong == 0);
    // Keyframes appear (at least the first, and the codec's interval of 120).
    int keyframes = 0;
    for (bool k : keys) keyframes += k ? 1 : 0;
    CHECK(keyframes >= 2);
}

TEST_CASE("pipeline: a slow disk makes the rate controller compress harder, then drop frames, and the timeline stays whole") {
    Fixture fx("pipe_rate_disk");
    fx.cfg.debug_disk_mb_s = 1.0;  // a disk of 1 MB/s
    fx.cfg.queue_mb = 1;
    fx.cfg.disk_buffer_mb = 1;
    EncodePipeline p;
    std::string error;
    REQUIRE(p.start(fx.cfg, &error));
    constexpr int kFrames = 400;
    std::vector<Nv12> sent;
    for (int i = 0; i < kFrames; ++i) {
        sent.push_back(make_frame(uint32_t(i + 1)));
        FrameMeta m;
        m.tick = uint64_t(i);
        m.present_qpc = fx.tick_time(m.tick);
        fx.now = m.present_qpc;
        p.on_frame(m, sent.back().y.data(), kW, sent.back().uv.data(), kW);
    }
    fx.now = fx.tick_time(kFrames);
    REQUIRE(p.finish(fx.now, &error));
    const PipelineStats& s = p.stats();
    CHECK(s.output_frames == kFrames);            // one frame (real or DUP) on every tick
    CHECK(s.dropped_queue == 0);                  // the queue never overflowed: the controller acted first
    CHECK(s.dropped_rate > 0);                    // frames were turned into DUPs on purpose
    CHECK(s.dup_filled >= s.dropped_rate);
    const double above_lossless = s.seconds_at_level[1] + s.seconds_at_level[2] + s.seconds_at_level[3] + s.seconds_at_level[4];
    CHECK(above_lossless > 0);
    CHECK(s.seconds_at_level[4] > 0);             // and it reached the top level
    std::vector<Nv12> frames;
    std::vector<int> types;
    std::vector<bool> keys;
    REQUIRE(decode_all(fx.cfg.avi_path, &frames, &types, &keys));
    CHECK(frames.size() == size_t(kFrames));
    CHECK(frames[0].y == sent[0].y);              // the first frames, before any pressure, are lossless
}

TEST_CASE("pipeline: a frame ring that fills up drops frames until it drains, and nothing else changes") {
    Fixture fx("pipe_rate_cpu");
    fx.cfg.queue_mb = 64;  // the disk is not what this test is about: the queue must stay empty even when frames arrive in a burst
    EncodePipeline p;
    std::string error;
    REQUIRE(p.start(fx.cfg, &error));
    constexpr int kFrames = 200;
    std::vector<Nv12> sent;
    for (int i = 0; i < kFrames; ++i) {
        sent.push_back(make_frame(uint32_t(i + 1)));
        FrameMeta m;
        m.tick = uint64_t(i);
        m.present_qpc = fx.tick_time(m.tick);
        m.ring_fill_pct = (i >= 100 && i < 150) ? 60 : 10;  // the host is behind for 50 frames
        fx.now = m.present_qpc;
        p.on_frame(m, sent.back().y.data(), kW, sent.back().uv.data(), kW);
    }
    fx.now = fx.tick_time(kFrames);
    REQUIRE(p.finish(fx.now, &error));
    const PipelineStats& s = p.stats();
    CHECK(s.dropped_rate == 50);
    CHECK(s.dup_filled == 50);
    CHECK(s.output_frames == kFrames);
    CHECK(s.seconds_at_level[1] + s.seconds_at_level[2] + s.seconds_at_level[3] + s.seconds_at_level[4] == 0);  // the disk was fine
    std::vector<Nv12> frames;
    std::vector<int> types;
    std::vector<bool> keys;
    REQUIRE(decode_all(fx.cfg.avi_path, &frames, &types, &keys));
    REQUIRE(frames.size() == size_t(kFrames));
    CHECK(frames[99].y == sent[99].y);
    CHECK(frames[120].y == sent[99].y);   // frozen on the last frame that got through
    CHECK(frames[150].y == sent[150].y);  // and back to normal, lossless
    CHECK(frames[199].uv == sent[199].uv);
}

TEST_CASE("pipeline: rcv-strict never goes near-lossless: it is lossless or it drops") {
    Fixture fx("pipe_rate_strict");
    fx.cfg.debug_disk_mb_s = 1.0;
    fx.cfg.queue_mb = 1;
    fx.cfg.disk_buffer_mb = 1;
    fx.cfg.rate.strict = true;
    EncodePipeline p;
    std::string error;
    REQUIRE(p.start(fx.cfg, &error));
    constexpr int kFrames = 400;
    std::vector<Nv12> sent;
    for (int i = 0; i < kFrames; ++i) {
        sent.push_back(make_frame(uint32_t(i + 1)));
        FrameMeta m;
        m.tick = uint64_t(i);
        m.present_qpc = fx.tick_time(m.tick);
        fx.now = m.present_qpc;
        p.on_frame(m, sent.back().y.data(), kW, sent.back().uv.data(), kW);
    }
    fx.now = fx.tick_time(kFrames);
    REQUIRE(p.finish(fx.now, &error));
    const PipelineStats& s = p.stats();
    CHECK(s.dropped_rate > 0);
    CHECK(s.seconds_at_level[1] + s.seconds_at_level[2] + s.seconds_at_level[3] == 0);
    std::vector<Nv12> frames;
    std::vector<int> types;
    std::vector<bool> keys;
    REQUIRE(decode_all(fx.cfg.avi_path, &frames, &types, &keys));
    REQUIRE(frames.size() == size_t(kFrames));
    // Every frame that got through is exactly what was sent (a DUP shows the picture before it).
    int wrong = 0;
    for (int i = 0; i < kFrames; ++i) {
        const bool exact = frames[i].y == sent[i].y && frames[i].uv == sent[i].uv;
        const bool repeat = i > 0 && frames[i].y == frames[i - 1].y && frames[i].uv == frames[i - 1].uv;  // a DUP shows the picture before it
        if (!exact && !repeat) ++wrong;
    }
    CHECK(wrong == 0);
}
