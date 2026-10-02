// Host side of capture (recorder plan §5, §6, §7): output planning, frame tools, and the recording
// session driven by a simulated hook that fills the real shared-memory ring.
#include <windows.h>

#include <chrono>
#include <cstring>
#include <fstream>
#include <memory>
#include <thread>
#include <vector>

#include "rec/avi.h"
#include "rec/frame_tools.h"
#include "rec/hooklink.h"
#include "rec/session.h"
#include "test_util.h"
#include "testfw.h"

using namespace rec;

namespace {

constexpr uint32_t kFakePid = 0x7F000002;

// A synthetic NV12 frame (out_w x out_h, no letterbox: the game is the same size) with the test
// app's barcode in the top sixth.
void draw_barcode_frame(uint8_t* y, uint8_t* uv, uint32_t w, uint32_t h, uint32_t counter) {
    std::memset(y, 100, size_t(w) * h);
    std::memset(uv, 128, size_t(w) * (h / 2));
    for (uint32_t row = 0; row < h / 6; ++row)
        for (int bit = 0; bit < 32; ++bit) {
            const bool one = (counter >> (31 - bit)) & 1u;
            std::memset(y + size_t(row) * w + w * bit / 32, one ? 255 : 0, w * (bit + 1) / 32 - w * bit / 32);
        }
}

}  // namespace

TEST_CASE("plan_output: size and frame rate follow the settings and the game") {
    Config::Record r;
    OutputPlan p;
    std::string error;

    REQUIRE(plan_output(r, 1366, 745, 60, &p, &error));  // defaults: 1280x720 @ 60
    CHECK(p.width == 1280 && p.height == 720 && p.fps == 60);
    CHECK(!p.size_capped && !p.fps_capped);

    r.size = "native";
    REQUIRE(plan_output(r, 1366, 745, 60, &p, &error));
    CHECK(p.width == 1366 && p.height == 744);  // even

    r.size = "1920x1080";  // larger than the game draws: capped, aspect kept, even
    REQUIRE(plan_output(r, 1366, 745, 60, &p, &error));
    CHECK(p.size_capped);
    CHECK(p.width <= 1366 && p.height <= 745);
    CHECK(p.width % 2 == 0 && p.height % 2 == 0);
    CHECK(std::abs(double(p.width) / p.height - 1920.0 / 1080.0) < 0.02);

    r.size = "1280x720";
    r.fps = 144;  // faster than the display
    REQUIRE(plan_output(r, 1366, 745, 60, &p, &error));
    CHECK(p.fps == 60 && p.fps_capped);
    REQUIRE(plan_output(r, 1366, 745, 0, &p, &error));  // refresh unknown: not capped
    CHECK(p.fps == 144 && !p.fps_capped);
    r.fps = 30;
    REQUIRE(plan_output(r, 1366, 745, 60, &p, &error));
    CHECK(p.fps == 30 && !p.fps_capped);

    CHECK(!plan_output(r, 0, 0, 60, &p, &error));  // no frame seen yet
    r.size = "bogus";
    CHECK(!plan_output(r, 1366, 745, 60, &p, &error));
}

TEST_CASE("frame tools: hash, percentile, barcode, colours, PNG") {
    // Hash.
    std::vector<uint8_t> a(1000, 7), b = a;
    CHECK(hash_bytes(a.data(), a.size()) == hash_bytes(b.data(), b.size()));
    b[999] ^= 1;
    CHECK(hash_bytes(a.data(), a.size()) != hash_bytes(b.data(), b.size()));
    b = a;
    b[3] ^= 0x80;
    CHECK(hash_bytes(a.data(), a.size()) != hash_bytes(b.data(), b.size()));

    // Percentile.
    CHECK(percentile({}, 50) == 0);
    CHECK(percentile({5}, 99) == 5);
    CHECK(std::abs(percentile({1, 2, 3, 4, 5}, 50) - 3.0) < 1e-9);
    CHECK(percentile({1, 2, 3, 4, 5}, 100) == 5);

    // Barcode round trip, with and without letterboxing.
    const uint32_t w = 1280, h = 720;
    std::vector<uint8_t> y(size_t(w) * h), uv(size_t(w) * h / 2);
    for (uint32_t counter : {0u, 1u, 0x80000001u, 123456u, 0xFFFFFFFFu}) {
        draw_barcode_frame(y.data(), uv.data(), w, h, counter);
        uint32_t value = 0;
        REQUIRE(decode_barcode(y.data(), w, w, h, w, h, &value));
        CHECK(value == counter);
    }
    std::memset(y.data(), 128, y.size());  // mid grey: not a barcode
    uint32_t value = 0;
    CHECK(!decode_barcode(y.data(), w, w, h, w, h, &value));

    // Colour check: bars filled with the expected values (rounded) have at most rounding error; wrong ones are caught.
    {
        const uint32_t frame = 77;
        draw_barcode_frame(y.data(), uv.data(), w, h, frame);
        auto fill = [&](double dy, double dc) {
            for (int bar = 0; bar < 8; ++bar) {
                double ey, ecb, ecr;
                testapp_bar_yuv(frame, bar, &ey, &ecb, &ecr);
                for (uint32_t row = h / 6; row < h; ++row)
                    for (uint32_t x = w * bar / 8; x < w * (bar + 1) / 8; ++x) y[size_t(row) * w + x] = uint8_t(ey + dy + 0.5);
                for (uint32_t row = h / 12; row < h / 2; ++row)
                    for (uint32_t x = w * bar / 16; x < w * (bar + 1) / 16; ++x) {
                        uv[size_t(row) * w + x * 2] = uint8_t(ecb + dc + 0.5);
                        uv[size_t(row) * w + x * 2 + 1] = uint8_t(ecr + dc + 0.5);
                    }
            }
        };
        fill(0, 0);
        ColorError ok;
        REQUIRE(check_testapp_colors(y.data(), w, uv.data(), w, w, h, w, h, frame, &ok));
        CHECK(ok.y <= 1 && ok.cb <= 1 && ok.cr <= 1);
        fill(10, 7);
        ColorError bad;
        REQUIRE(check_testapp_colors(y.data(), w, uv.data(), w, w, h, w, h, frame, &bad));
        CHECK(bad.y >= 9 && bad.cb >= 6 && bad.cr >= 6);
    }

    // PNG: signature, IHDR size, and the stored data round-trips by size.
    const rt::TempDir dir("png");
    std::vector<uint8_t> rgb(size_t(64) * 48 * 3, 200);
    const auto path = dir.path() / "t.png";
    REQUIRE(write_png_rgb(path, 64, 48, rgb.data()));
    std::ifstream in(path, std::ios::binary);
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    REQUIRE(file.size() > rgb.size());
    const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    CHECK(std::memcmp(file.data(), sig, 8) == 0);
    CHECK(std::memcmp(file.data() + 12, "IHDR", 4) == 0);
    CHECK(file[19] == 64 && file[23] == 48);
    CHECK(std::memcmp(file.data() + file.size() - 8, "IEND", 4) == 0);

    // NV12 -> RGB: neutral grey stays grey, pure luma ramps.
    std::vector<uint8_t> yy(16 * 16, 90), cc(16 * 8, 128), out;
    nv12_to_rgb(yy.data(), 16, cc.data(), 16, 16, 16, &out);
    CHECK(out[0] == 90 && out[1] == 90 && out[2] == 90);
}

TEST_CASE("recording session: frames through the real ring from a simulated hook, into an AVI file") {
    const rt::TempDir out("session");
    std::string error;
    auto link = HookLink::open_or_create(kFakePid, &error);
    REQUIRE(link != nullptr);
    proto::ControlBlock* ctl = link->control();
    ctl->hook_state.store(uint32_t(proto::HookState::Hooked));
    ctl->backend.store(proto::kApiD3D11);
    ctl->backbuffer_width.store(1280);
    ctl->backbuffer_height.store(720);
    ctl->display_refresh_hz.store(60);

    Config cfg;
    cfg.record.size = "1280x720";
    cfg.record.out_dir = out.path().string();
    RecordingSession session(*link, cfg, "rec_testapp.exe");
    CHECK(session.state() == RecordingSession::State::Idle);
    REQUIRE(session.start(&error));
    CHECK(session.state() == RecordingSession::State::Recording);
    CHECK(session.plan().width == 1280 && session.plan().height == 720 && session.plan().fps == 60);
    CHECK(!session.start(&error));  // already recording

    // The hook's side: open the ring the host made and fill slots.
    CHECK(ctl->host_state.load() == uint32_t(proto::SessionState::Recording));
    CHECK(ctl->rec_out_w == 1280 && ctl->rec_out_h == 720 && ctl->rec_fps == 60);
    CHECK(ctl->rec_lock == 1);  // lock mode is the default
    const uint32_t gen = ctl->rec_generation.load();
    wchar_t name[64];
    proto::frame_ring_name(name, 64, kFakePid, gen, false);
    HANDLE map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
    REQUIRE(map != nullptr);
    auto* ring = static_cast<proto::FrameRingHeader*>(MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, 0));
    REQUIRE(ring != nullptr);
    CHECK(ring->magic == proto::kFrameRingMagic);
    CHECK(ring->slot_bytes == proto::nv12_slot_bytes(1280, 720));
    proto::frame_ring_name(name, 64, kFakePid, gen, true);
    HANDLE sem = OpenSemaphoreW(SEMAPHORE_MODIFY_STATE | SYNCHRONIZE, FALSE, name);
    REQUIRE(sem != nullptr);
    ctl->capture_state.store(uint32_t(proto::CaptureState::Capturing));

    const uint32_t ticks[] = {0, 1, 2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20};  // tick 3 missing
    const uint32_t frames = uint32_t(std::size(ticks));
    uint32_t next = 0;
    for (uint32_t i = 0; i < frames; ++i) {
        proto::SlotHeader* slot = proto::ring_slot(ring, next);
        // Wait for the host to free the slot (the ring has 8 slots, we write 20).
        for (int spin = 0; spin < 2000 && slot->state.load() != uint32_t(proto::SlotState::Free); ++spin) Sleep(1);
        REQUIRE(slot->state.load() == uint32_t(proto::SlotState::Free));
        slot->state.store(uint32_t(proto::SlotState::Writing));
        uint8_t* base = reinterpret_cast<uint8_t*>(slot);
        draw_barcode_frame(base + proto::nv12_y_offset(), base + proto::nv12_uv_offset(1280, 720), 1280, 720, 1000 + i * 2);  // game renders 2 per capture
        slot->seq = i + 1;
        slot->tick = ticks[i];
        LARGE_INTEGER q;
        QueryPerformanceCounter(&q);
        slot->present_qpc = uint64_t(q.QuadPart);
        slot->capture_done_qpc = uint64_t(q.QuadPart);
        slot->width = 1280;
        slot->height = 720;
        slot->layout = uint8_t(proto::Layout::Nv12);
        slot->stride0 = slot->stride1 = 1280;
        slot->game_frame_time_us = 16667;
        slot->hook_cost_us = i == 0 ? 5000 : 300;
        slot->readback_frames = 4;
        slot->state.store(uint32_t(proto::SlotState::Ready));
        ReleaseSemaphore(sem, 1, nullptr);
        ctl->frames_captured.fetch_add(1);
        next = (next + 1) % ring->slot_count;
    }
    ctl->gpu_backlog_skips.fetch_add(3);

    for (int i = 0; i < 500 && session.live().frames < frames; ++i) Sleep(2);
    CHECK(session.live().frames == frames);

    session.request_stop();
    CHECK(session.state() == RecordingSession::State::Stopping);
    CHECK(ctl->host_state.load() == uint32_t(proto::SessionState::Stopping));
    CHECK(!session.poll());  // the hook has not finished yet
    ctl->capture_state.store(uint32_t(proto::CaptureState::Off));
    bool finished = false;
    for (int i = 0; i < 100 && !finished; ++i) finished = session.poll();
    REQUIRE(finished);
    CHECK(session.state() == RecordingSession::State::Idle);
    CHECK(ctl->host_state.load() == uint32_t(proto::SessionState::Idle));

    const RecordingSummary& s = session.summary();
    REQUIRE(s.valid);
    CHECK(s.frames == frames);
    CHECK(s.tick_gaps == 1);
    CHECK(s.tick_repeats == 0);
    CHECK(s.seq_gaps == 0);
    CHECK(s.gpu_backlog == 3);
    CHECK(s.ring_drops == 0);
    CHECK(std::abs(s.first_frame_cost_ms - 5.0) < 1e-6);
    CHECK(std::abs(s.cost_p50_ms - 0.3) < 1e-6);  // the first frame is reported apart
    CHECK(s.barcode);
    CHECK(s.barcode_read == frames);
    CHECK(s.barcode_out_of_order == 0);
    CHECK(s.barcode_unreadable == 0);
    CHECK(s.barcode_source_frames_skipped == frames - 1);  // one game frame between consecutive captures
    CHECK(s.barcode_first == 1000 && s.barcode_last == 1000 + (frames - 1) * 2);
    CHECK(s.checksum != 0);
    CHECK(s.text().find("Recording finished: 21 frames") != std::string::npos);

    // The file: 21 output frames (ticks 0..20, tick 3 a DUP), a finished AVI whose indexes agree, and the companions.
    CHECK(s.file_ok);
    CHECK(s.pipeline.output_frames == 21);
    CHECK(s.pipeline.dup_filled == 1);
    CHECK(std::filesystem::exists(s.avi));
    CHECK(std::filesystem::exists(s.csv));
    CHECK(std::filesystem::exists(s.json_file));
    const AviScan scan = scan_avi(s.avi);
    CHECK(scan.ok);
    for (const std::string& p : scan.problems) std::fprintf(stderr, "avi problem: %s\n", p.c_str());
    CHECK(scan.problems.empty());
    CHECK(scan.video.size() == 21);
    CHECK(scan.info.width == 1280 && scan.info.height == 720 && scan.info.fps_num == 60);
    CHECK(scan.info.comment.find("rec_testapp.exe") != std::string::npos);
    const std::string json = rt::read_file(s.json_file);
    CHECK(json.find("\"status\": \"OK\"") != std::string::npos);
    CHECK(json.find("\"output\": 21") != std::string::npos);

    // A second recording gets a new ring (a new generation).
    REQUIRE(session.start(&error));
    CHECK(ctl->rec_generation.load() == gen + 1);
    // A capture failure reported by the hook begins the stop.
    ctl->capture_state.store(uint32_t(proto::CaptureState::Error));
    std::string message;
    CHECK(session.take_error(&message));
    CHECK(message.find("capture failed") != std::string::npos);
    CHECK(session.state() == RecordingSession::State::Stopping);
    CHECK(!session.take_error(&message));  // reported once
    ctl->capture_state.store(uint32_t(proto::CaptureState::Off));
    for (int i = 0; i < 100 && !session.poll(); ++i) Sleep(1);
    CHECK(session.state() == RecordingSession::State::Idle);

    UnmapViewOfFile(ring);
    CloseHandle(map);
    CloseHandle(sem);
}

TEST_CASE("recording session: refuses what the game can't give") {
    std::string error;
    auto link = HookLink::open_or_create(kFakePid + 1, &error);
    REQUIRE(link != nullptr);
    proto::ControlBlock* ctl = link->control();
    Config cfg;
    RecordingSession session(*link, cfg, "game.exe");
    CHECK(!session.start(&error));  // hook not active yet
    ctl->hook_state.store(uint32_t(proto::HookState::Hooked));
    CHECK(!session.start(&error));  // nothing presented
    ctl->backend.store(proto::kApiOpenGL);
    CHECK(!session.start(&error));
    CHECK(error.find("Direct3D 11") != std::string::npos);
    CHECK(session.state() == RecordingSession::State::Idle);
}
