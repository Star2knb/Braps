#include "rec/session.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <thread>

#include "rec/convert.h"
#include "rec/frame_tools.h"
#include "rec/log.h"
#include "rec/options.h"
#include "rec/paths.h"
#include "rec/scheduling.h"
#include "rec/version.h"

namespace rec {
namespace {

int64_t qpc_now() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

int64_t qpc_freq() {
    static const int64_t f = [] {
        LARGE_INTEGER x;
        QueryPerformanceFrequency(&x);
        return x.QuadPart;
    }();
    return f;
}

bool ends_with_ci(const std::string& s, const std::string& suffix) {
    if (s.size() < suffix.size()) return false;
    for (size_t i = 0; i < suffix.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(s[s.size() - suffix.size() + i])) != std::tolower(static_cast<unsigned char>(suffix[i]))) return false;
    return true;
}

std::string format(const char* fmt, ...) {
    char buf[768];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    return buf;
}

std::string json_escape(const std::string& s) {
    std::string r;
    for (char c : s) {
        if (c == '"' || c == '\\') {
            r += '\\';
            r += c;
        } else if (static_cast<unsigned char>(c) < 0x20) {
            r += ' ';
        } else {
            r += c;
        }
    }
    return r;
}

// A file name part without characters Windows refuses.
std::string file_safe(std::string s) {
    for (char& c : s)
        if (std::strchr("<>:\"/\\|?*", c) || static_cast<unsigned char>(c) < 0x20) c = '_';
    return s;
}

// Reserves "<base>.avi", "<base> (2).avi", ... by creating the file exclusively, so two recordings that start in
// the same hundredth of a second (two games, two rec instances) can't pick the same name.
std::string unique_base(const std::filesystem::path& dir, const std::string& base) {
    for (int n = 1; n < 1000; ++n) {
        const std::string candidate = n == 1 ? base : base + " (" + std::to_string(n) + ")";
        HANDLE h = CreateFileW((dir / (candidate + ".avi")).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
            return candidate;
        }
        if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS) break;  // not a name clash: let the writer report it
    }
    return base;
}

}  // namespace

bool plan_output(const Config::Record& settings, uint32_t bb_w, uint32_t bb_h, uint32_t refresh_hz, OutputPlan* plan, std::string* error) {
    if (!bb_w || !bb_h) {
        *error = "the game has not presented a frame yet";
        return false;
    }
    OutputSize size;
    if (!parse_size(settings.size, &size)) {
        *error = "record.size is not valid: " + settings.size;
        return false;
    }
    OutputPlan p;
    uint32_t w = size.native ? bb_w : uint32_t(size.width);
    uint32_t h = size.native ? bb_h : uint32_t(size.height);
    if (w > bb_w || h > bb_h) {  // never larger than what the game draws (R4)
        const double s = (std::min)(double(bb_w) / w, double(bb_h) / h);
        w = uint32_t(w * s);
        h = uint32_t(h * s);
        p.size_capped = true;
    }
    p.width = (std::max)(16u, w & ~1u);
    p.height = (std::max)(16u, h & ~1u);
    p.fps = uint32_t((std::max)(1, settings.fps));
    if (refresh_hz >= 2 && p.fps > refresh_hz) {  // never faster than the display (R5)
        p.fps = refresh_hz;
        p.fps_capped = true;
    }
    *plan = p;
    return true;
}

std::string recording_basename(const std::string& exe_name, const SYSTEMTIME& t) {
    std::string stem = exe_name;
    if (ends_with_ci(stem, ".exe")) stem.resize(stem.size() - 4);
    if (stem.empty()) stem = "recording";
    return format("%s %04d-%02d-%02d %02d-%02d-%02d-%02d", file_safe(stem).c_str(), t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
                  t.wMilliseconds / 10);
}

std::string RecordingSummary::text() const {
    const PipelineStats& p = pipeline;
    std::string t;
    const std::string avi_text = to_utf8(avi.wstring());
    t += format("Recording finished: %llu frames, %.1f s (%u fps), %ux%u from a %ux%u game%s\n", (unsigned long long)p.output_frames, seconds, plan.fps,
                plan.width, plan.height, source_w, source_h, lock ? ", locked" : "");
    t += format("  file: %s (%.1f MB)%s\n", avi_text.c_str(), double(p.file_bytes) / 1048576.0, file_ok ? "" : "  ** NOT FINISHED **");
    if (!error.empty()) t += "  ERROR: " + error + "\n";
    t += format("  video: I %llu, P %llu, DUP %llu (%llu filled for ticks without a frame, %llu identical to the one before); dropped %llu (late %llu, "
                "queue %llu, encoder %llu)\n",
                (unsigned long long)p.frames_i, (unsigned long long)p.frames_p, (unsigned long long)(p.dup_filled + p.dup_encoded),
                (unsigned long long)p.dup_filled, (unsigned long long)p.dup_encoded,
                (unsigned long long)(p.dropped_late + p.dropped_queue + p.dropped_error), (unsigned long long)p.dropped_late,
                (unsigned long long)p.dropped_queue, (unsigned long long)p.dropped_error);
    t += format("  compression: %.2f:1 on real frames, %.2f:1 overall; encode avg %.2f ms, p99 %.2f ms, max %.2f ms\n", p.ratio_real, p.ratio_overall,
                p.encode_ms_avg, p.encode_ms_p99, p.encode_ms_max);
    t += format("  disk: %.1f MB/s average, %.1f MB/s best, %u slow writes (> limit), %u very slow, worst %.0f ms\n", p.disk.avg_mb_s(), p.disk.peak_mb_s,
                p.disk.slow_writes, p.disk.very_slow_writes, p.disk.max_latency_ms);
    t += format("  hook time per captured frame: p50 %.3f ms, p99 %.3f ms, max %.3f ms (first frame, which creates the GPU objects: %.2f ms)\n",
                cost_p50_ms, cost_p99_ms, cost_max_ms, first_frame_cost_ms);
    if (converted)
        t += format("  host conversion BGRA to NV12 (OpenGL frames): p50 %.2f ms, p99 %.2f ms, max %.2f ms\n", convert_p50_ms, convert_p99_ms, convert_max_ms);
    t += format("  Present to frame in the host: p50 %.1f ms, p99 %.1f ms; GPU read-back lag p50 %.0f Presents; game frame p50 %.2f ms, p99 %.2f ms\n",
                latency_p50_ms, latency_p99_ms, readback_p50_frames, game_frame_p50_ms, game_frame_p99_ms);
    t += format("  captured frames received: %llu; ticks without one: %llu, ticks with two: %llu, lost between hook and host: %llu\n", (unsigned long long)frames,
                (unsigned long long)tick_gaps, (unsigned long long)tick_repeats, (unsigned long long)seq_gaps);
    if (lock)
        t += format("  pacing: capture began %.2f ms after its tick at the median, %.2f ms at p99 (worst %.2f ms); the game was held for %.0f ms in total\n",
                    pacing_error_p50_ms, pacing_error_p99_ms, pacing_error_max_ms, pacing_wait_total_ms);
    t += format("  GPU backlog skips (W1201): %llu, ring-full drops (W1202): %llu, capture errors: %llu\n", (unsigned long long)gpu_backlog,
                (unsigned long long)ring_drops, (unsigned long long)capture_errors);
    if (barcode)
        t += format("  test pattern: %llu frames read, counters %u..%u, %llu out of order, %llu game frames not captured, %llu unreadable\n",
                    (unsigned long long)barcode_read, barcode_first, barcode_last, (unsigned long long)barcode_out_of_order,
                    (unsigned long long)barcode_source_frames_skipped, (unsigned long long)barcode_unreadable);
    if (barcode && color_checked)
        t += format("  colour check against BT.601 on %llu frames: largest difference Y %d, Cb %d, Cr %d (of 255)\n", (unsigned long long)color_checked,
                    color_error.y, color_error.cb, color_error.cr);
    if (barcode) t += format("  checksum of all captured frames: %016llx\n", (unsigned long long)checksum);
    t += "  telemetry: " + to_utf8(csv.filename().wstring()) + ", " + to_utf8(json_file.filename().wstring()) + ", " + to_utf8(log.filename().wstring());
    return t;
}

std::string RecordingSummary::json() const {
    const PipelineStats& p = pipeline;
    std::ostringstream o;
    auto num = [](double v) { return format("%.4f", v); };
    const uint64_t dups = p.dup_filled + p.dup_encoded;
    // Game fps: average from the mean frame time; the 1% low from the 99th-percentile frame time.
    const double fps_avg = game_frame_avg_ms > 0 ? 1000.0 / game_frame_avg_ms : 0;
    const double fps_low = game_frame_p99_ms > 0 ? 1000.0 / game_frame_p99_ms : 0;
    o << "{\n";
    o << "  \"status\": \"" << (file_ok && error.empty() ? "OK" : "stopped by error: " + json_escape(error)) << "\",\n";
    o << "  \"software\": \"rec " << kVersionNumber << "\",\n";
    o << "  \"game\": \"" << json_escape(game) << "\",\n";
    o << "  \"backend\": \"" << backend << "\",\n";
    o << "  \"file\": \"" << json_escape(to_utf8(avi.wstring())) << "\",\n";
    o << "  \"source\": {\"width\": " << source_w << ", \"height\": " << source_h << "},\n";
    o << "  \"output\": {\"width\": " << plan.width << ", \"height\": " << plan.height << ", \"fps\": " << plan.fps << ", \"lock\": " << (lock ? "true" : "false") << "},\n";
    o << "  \"duration_s\": " << num(seconds) << ",\n";
    o << "  \"frames\": {\"output\": " << p.output_frames << ", \"I\": " << p.frames_i << ", \"P\": " << p.frames_p << ", \"DUP\": " << dups
      << ", \"DUP_filled\": " << p.dup_filled << ", \"DUP_identical\": " << p.dup_encoded << ", \"dropped\": " << (p.dropped_late + p.dropped_queue + p.dropped_error)
      << ", \"dropped_late\": " << p.dropped_late << ", \"dropped_queue\": " << p.dropped_queue << ", \"captured\": " << frames << "},\n";
    o << "  \"game_fps\": {\"avg\": " << num(fps_avg) << ", \"low_1pct\": " << num(fps_low) << "},\n";
    o << "  \"hook_cost_ms\": {\"avg\": " << num(cost_avg_ms) << ", \"p50\": " << num(cost_p50_ms) << ", \"p99\": " << num(cost_p99_ms) << ", \"max\": " << num(cost_max_ms)
      << ", \"first_frame\": " << num(first_frame_cost_ms) << "},\n";
    if (converted)
        o << "  \"convert_ms\": {\"p50\": " << num(convert_p50_ms) << ", \"p99\": " << num(convert_p99_ms) << ", \"max\": " << num(convert_max_ms) << "},\n";
    o << "  \"encode_ms\": {\"avg\": " << num(p.encode_ms_avg) << ", \"p99\": " << num(p.encode_ms_p99) << ", \"max\": " << num(p.encode_ms_max) << "},\n";
    o << "  \"compression_ratio\": {\"real_frames\": " << num(p.ratio_real) << ", \"overall\": " << num(p.ratio_overall) << "},\n";
    o << "  \"bytes_written\": " << p.file_bytes << ",\n";
    o << "  \"write_mb_s\": {\"avg\": " << num(p.disk.avg_mb_s()) << ", \"peak\": " << num(p.disk.peak_mb_s) << "},\n";
    o << "  \"slow_writes\": " << p.disk.slow_writes << ",\n  \"very_slow_writes\": " << p.disk.very_slow_writes << ",\n";
    o << "  \"seconds_at_rate_level\": [" << num(seconds) << ", 0, 0, 0, 0],\n";  // the rate controller arrives in M5: always lossless
    o << "  \"pacing\": {\"error_p50_ms\": " << num(pacing_error_p50_ms) << ", \"error_p99_ms\": " << num(pacing_error_p99_ms) << ", \"error_max_ms\": "
      << num(pacing_error_max_ms) << ", \"held_ms_total\": " << num(pacing_wait_total_ms) << "},\n";
    o << "  \"audio_discontinuities\": 0,\n  \"max_av_drift_ms\": null,\n";
    o << "  \"events\": {";
    bool first = true;
    for (size_t i = 0; i < kEventCount; ++i) {
        const EventInfo& e = kEvents[i];
        if (events_during[i] == 0 || event_level(e.id) < Level::Warn) continue;
        o << (first ? "" : ", ") << "\"" << e.code << "\": " << events_during[i];
        first = false;
    }
    o << "}\n}\n";
    return o.str();
}

RecordingSession::RecordingSession(HookLink& link, const Config& config, std::string game_name)
    : link_(link), config_(config), game_(std::move(game_name)) {}

RecordingSession::~RecordingSession() {
    if (state_ == State::Recording) request_stop();
    if (state_ == State::Stopping) {
        // Give the hook a moment to finish its read-backs, then close up.
        for (int i = 0; i < 50 && link_.control()->capture_state.load() != uint32_t(proto::CaptureState::Off); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        finish();
    }
}

bool RecordingSession::start(std::string* error) {
    if (state_ != State::Idle) {
        *error = "already recording";
        return false;
    }
    proto::ControlBlock* ctl = link_.control();
    const uint32_t backend = ctl->backend.load();
    if (proto::HookState(ctl->hook_state.load()) != proto::HookState::Hooked) {
        *error = "the hook is not active";
        return false;
    }
    if (backend != proto::kApiD3D11 && backend != proto::kApiOpenGL) {
        *error = backend ? "capture works with Direct3D 11 and OpenGL games for now" : "the game has not presented a frame yet";
        return false;
    }
    layout_ = backend == proto::kApiOpenGL ? proto::Layout::Bgra : proto::Layout::Nv12;
    if (config_.record.encoder == "hw") {
        *error = "the hardware encoder arrives in recorder milestone M9; use --encoder rcv";
        return false;
    }
    if (config_.record.format == "rgb") {
        *error = "lossless RGB recording is not implemented yet; use --format yuv420";
        return false;
    }
    source_w_ = ctl->backbuffer_width.load();
    source_h_ = ctl->backbuffer_height.load();
    if (!plan_output(config_.record, source_w_, source_h_, ctl->display_refresh_hz.load(), &plan_, error)) return false;

    // Output files: <folder>\<Game> <date> <time>-<hundredths>.avi and its companions.
    const std::filesystem::path dir = expand_env(config_.record.out_dir);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        *error = "can't create the output folder " + to_utf8(dir.wstring()) + ": " + ec.message();
        return false;
    }
    SYSTEMTIME local;
    GetLocalTime(&local);
    const std::string base = unique_base(dir, recording_basename(game_, local));
    avi_ = dir / (base + ".avi");
    csv_ = dir / (base + ".frames.csv");
    json_ = dir / (base + ".summary.json");
    log_ = dir / (base + ".log");
    std::string log_error;
    if (!logging::open_session(log_, &log_error)) logging::get(Subsystem::Disk).warn("no session log: {}", log_error);
    for (size_t i = 0; i < kEventCount; ++i) events_before_[i] = logging::event_count(kEvents[i].id);

    // The encode pipeline: encoder, packet queue, AVI writer thread.
    PipelineConfig pc;
    pc.avi_path = avi_;
    pc.csv_path = csv_;
    pc.width = plan_.width;
    pc.height = plan_.height;
    pc.fps = plan_.fps;
    pc.source_w = source_w_;
    pc.source_h = source_h_;
    pc.game = game_;
    pc.software = std::string("rec ") + kVersionNumber;
    pc.queue_mb = uint32_t(config_.record.queue_mb);
    pc.above_normal = config_.record.encoder_priority == "above-normal";
    pc.slow_write_ms = config_.log.slow_write_ms;
    pc.very_slow_write_ms = config_.log.very_slow_write_ms;
    pipeline_ = std::make_unique<EncodePipeline>();
    if (!pipeline_->start(pc, error)) {
        pipeline_.reset();
        logging::close_session();
        return false;
    }

    // The frame ring.
    const uint32_t slots = uint32_t(config_.record.frame_slots);
    const uint32_t slot_bytes = layout_ == proto::Layout::Bgra ? proto::bgra_slot_bytes(plan_.width, plan_.height) : proto::nv12_slot_bytes(plan_.width, plan_.height);
    generation_ = ctl->rec_generation.load() + 1;
    wchar_t name[64];
    proto::frame_ring_name(name, 64, link_.game_pid(), generation_, false);
    const size_t bytes = proto::frame_ring_bytes(slots, slot_bytes);
    ring_map_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, uint32_t(uint64_t(bytes) >> 32), uint32_t(bytes & 0xFFFFFFFFu), name);
    if (!ring_map_) {
        *error = "can't create the frame ring: " + win32_error_text(GetLastError());
        std::string ignored;
        pipeline_->finish(qpc_now(), &ignored);
        pipeline_.reset();
        logging::close_session();
        return false;
    }
    ring_ = static_cast<proto::FrameRingHeader*>(MapViewOfFile(ring_map_, FILE_MAP_ALL_ACCESS, 0, 0, bytes));
    proto::frame_ring_name(name, 64, link_.game_pid(), generation_, true);
    sem_ = CreateSemaphoreW(nullptr, 0, LONG(slots), name);
    if (!ring_ || !sem_) {
        *error = "can't create the frame ring: " + win32_error_text(GetLastError());
        if (ring_) UnmapViewOfFile(ring_);
        if (ring_map_) CloseHandle(ring_map_);
        if (sem_) CloseHandle(sem_);
        ring_ = nullptr;
        ring_map_ = sem_ = nullptr;
        std::string ignored;
        pipeline_->finish(qpc_now(), &ignored);
        pipeline_.reset();
        logging::close_session();
        return false;
    }
    ring_->magic = proto::kFrameRingMagic;
    ring_->version = proto::kVersion;
    ring_->slot_count = slots;
    ring_->slot_bytes = slot_bytes;
    ring_->out_w = plan_.width;
    ring_->out_h = plan_.height;
    ring_->layout = uint32_t(layout_);
    convert_ms_.clear();
    if (layout_ == proto::Layout::Bgra) {
        convert_ms_.reserve(1 << 16);
        convert_y_.assign(size_t(plan_.width) * plan_.height, 0);
        convert_uv_.assign(size_t(plan_.width) * plan_.height / 2, 128);
    }
    read_index_ = 0;

    // Reset what the receiver collects.
    for (auto* v : {&cost_ms_, &latency_ms_, &readback_frames_, &game_frame_ms_, &pacing_error_ms_}) {
        v->clear();
        v->reserve(1 << 16);
    }
    pacing_wait_total_ms_ = 0;
    first_frame_cost_ms_ = 0;
    checksum_ = last_seq_ = seq_gaps_ = 0;
    first_tick_ = last_tick_ = -1;
    tick_gaps_ = tick_repeats_ = 0;
    have_barcode_value_ = false;
    barcode_read_ = barcode_unreadable_ = barcode_out_of_order_ = barcode_skipped_ = 0;
    color_error_ = ColorError{};
    color_checked_ = 0;
    first_barcode_ = last_barcode_ = 0;
    frames_ = 0;
    cost_sum_us_ = 0;
    error_reported_ = false;
    summary_ = RecordingSummary{};
    analyze_ = ends_with_ci(game_, "rec_testapp.exe");
    base_backlog_ = ctl->gpu_backlog_skips.load();
    base_drops_ = ctl->ring_full_drops.load();
    base_errors_ = ctl->capture_errors.load();
    stop_receiver_ = false;
    thread_ = std::thread([this] { receiver(); });

    // Publish the configuration, then tell the hook to go. Tick 0 is now.
    ctl->rec_fps = plan_.fps;
    ctl->rec_out_w = plan_.width;
    ctl->rec_out_h = plan_.height;
    ctl->rec_staging_slots = uint32_t(config_.record.staging_slots);
    ctl->rec_frame_slots = slots;
    ctl->rec_slot_bytes = slot_bytes;
    ctl->rec_lock = config_.record.lock ? 1u : 0u;
    ctl->grid0_qpc.store(0);
    start_qpc_ = qpc_now();
    pipeline_->set_t0(start_qpc_);
    ctl->rec_t0_qpc = uint64_t(start_qpc_);
    ctl->capture_error_code.store(0);
    ctl->rec_generation.store(generation_, std::memory_order_release);
    ctl->host_state.store(uint32_t(proto::SessionState::Recording), std::memory_order_release);
    state_ = State::Recording;

    if (plan_.fps_capped) logging::event(Ev::DisplayRefresh, "frame rate capped to the display: {} fps", plan_.fps);
    logging::event(Ev::SessionStart, "{} D3D11 {}x{} -> {}x{} @{} fps{}, {} read-back slots, {} ring slots -> {}", game_, source_w_, source_h_, plan_.width,
                   plan_.height, plan_.fps, config_.record.lock ? " locked" : "", config_.record.staging_slots, slots, to_utf8(avi_.wstring()));
    return true;
}

void RecordingSession::request_stop() {
    if (state_ != State::Recording) return;
    link_.control()->host_state.store(uint32_t(proto::SessionState::Stopping), std::memory_order_release);
    stop_qpc_ = qpc_now();
    state_ = State::Stopping;
}

bool RecordingSession::take_error(std::string* message) {
    if (state_ != State::Recording || error_reported_) return false;
    proto::ControlBlock* ctl = link_.control();
    if (pipeline_ && pipeline_->failed()) {
        error_reported_ = true;
        *message = "the video file can't be written (" + pipeline_->error() + "); stopping";
        request_stop();
        return true;
    }
    if (ctl->capture_state.load() != uint32_t(proto::CaptureState::Error)) return false;
    error_reported_ = true;
    *message = "capture failed in the game (error " + std::to_string(ctl->capture_error_code.load()) + ", see the log); stopping";
    request_stop();
    return true;
}

bool RecordingSession::poll() {
    if (state_ != State::Stopping) return false;
    const bool hook_done = link_.control()->capture_state.load() == uint32_t(proto::CaptureState::Off);
    const bool timed_out = (qpc_now() - stop_qpc_) > 2 * qpc_freq();  // P6: stop within 2 s
    if (!hook_done && !timed_out) return false;
    if (!hook_done) logging::get(Subsystem::Transport).warn("the hook did not finish within 2 s of the stop; closing anyway (the game may have stopped presenting)");
    finish();
    return true;
}

void RecordingSession::finish_now() {
    if (state_ == State::Recording) request_stop();
    if (state_ == State::Stopping) finish();
}

LiveStats RecordingSession::live() const {
    LiveStats s;
    s.frames = frames_.load();
    if (state_ != State::Idle) s.seconds = double(qpc_now() - start_qpc_) / double(qpc_freq());
    s.avg_cost_ms = s.frames ? double(cost_sum_us_.load()) / double(s.frames) / 1000.0 : 0;
    proto::ControlBlock* ctl = const_cast<HookLink&>(link_).control();
    s.gpu_backlog = ctl->gpu_backlog_skips.load() - base_backlog_;
    s.ring_drops = ctl->ring_full_drops.load() - base_drops_;
    if (pipeline_ && state_ != State::Idle) {
        s.output_frames = pipeline_->output_frames();
        s.dup_filled = pipeline_->dup_filled();
        s.encode_ms = pipeline_->encode_ms_recent();
        s.ratio = pipeline_->ratio_running();
        s.queue_pct = pipeline_->queue_fill_pct();
        s.write_mb_s = pipeline_->write_mb_s();
        s.file_mb = double(pipeline_->file_bytes()) / 1048576.0;
        if (qpc_now() - free_checked_qpc_ > qpc_freq()) {  // once a second
            free_checked_qpc_ = qpc_now();
            ULARGE_INTEGER avail{};
            if (GetDiskFreeSpaceExW(avi_.parent_path().c_str(), &avail, nullptr, nullptr)) free_gb_ = double(avail.QuadPart) / 1073741824.0;
        }
        s.free_gb = free_gb_;
    }
    return s;
}

// One READY slot, if there is one: check it, count it, hand it to the pipeline, give it back.
bool RecordingSession::consume_one() {
    proto::SlotHeader* slot = proto::ring_slot(ring_, read_index_);
    if (slot->state.load(std::memory_order_acquire) != uint32_t(proto::SlotState::Ready)) return false;
    slot->state.store(uint32_t(proto::SlotState::Reading), std::memory_order_relaxed);
    const int64_t now = qpc_now();
    const double to_ms = 1000.0 / double(qpc_freq());

    const uint8_t* base = reinterpret_cast<const uint8_t*>(slot);
    const uint32_t w = slot->width, h = slot->height;
    const uint8_t* y = base + proto::nv12_y_offset();
    const uint8_t* uv = base + proto::nv12_uv_offset(w, h);
    uint32_t y_stride = slot->stride0, uv_stride = slot->stride1;
    double convert_ms = 0;
    if (slot->layout == uint8_t(proto::Layout::Bgra)) {  // OpenGL: the hook reads back BGRA, the encoder takes NV12
        const int64_t c0 = qpc_now();
        bgra_to_nv12(base + proto::bgra_offset(), slot->stride0, w, h, convert_y_.data(), w, convert_uv_.data(), w);
        convert_ms = double(qpc_now() - c0) * to_ms;
        convert_ms_.push_back(convert_ms);
        y = convert_y_.data();
        uv = convert_uv_.data();
        y_stride = uv_stride = w;
    }

    // Timing and loss.
    if (last_seq_ && slot->seq != last_seq_ + 1) seq_gaps_ += (slot->seq > last_seq_) ? slot->seq - last_seq_ - 1 : 1;
    last_seq_ = slot->seq;
    const int64_t tick = int64_t(slot->tick);
    if (first_tick_ < 0) first_tick_ = tick;
    if (last_tick_ >= 0) {
        if (tick == last_tick_) ++tick_repeats_;
        else if (tick > last_tick_ + 1) tick_gaps_ += uint64_t(tick - last_tick_ - 1);
    }
    last_tick_ = tick;
    const uint64_t index = frames_.load();
    if (index == 0) {
        first_frame_cost_ms_ = slot->hook_cost_us / 1000.0;
    } else {
        cost_ms_.push_back(slot->hook_cost_us / 1000.0);
    }
    latency_ms_.push_back(double(now - int64_t(slot->present_qpc)) * to_ms);
    readback_frames_.push_back(slot->readback_frames);
    if (slot->game_frame_time_us) game_frame_ms_.push_back(slot->game_frame_time_us / 1000.0);
    pacing_error_ms_.push_back(slot->pacing_error_us / 1000.0);
    pacing_wait_total_ms_ += slot->pacing_wait_us / 1000.0;
    cost_sum_us_ += slot->hook_cost_us;

    // Optional checks on the content (the test app's pattern); too costly to do on every recording.
    if (analyze_) {
        const uint64_t h1 = hash_bytes(y, size_t(y_stride) * h);
        const uint64_t h2 = hash_bytes(uv, size_t(uv_stride) * (h / 2), h1);
        checksum_ = (checksum_ << 7 | checksum_ >> 57) ^ h2;
        uint32_t value = 0;
        if (decode_barcode(y, y_stride, w, h, source_w_, source_h_, &value)) {
            ++barcode_read_;
            if (!have_barcode_value_) {
                have_barcode_value_ = true;
                first_barcode_ = value;
            } else if (value <= last_barcode_) {
                ++barcode_out_of_order_;
            } else {
                barcode_skipped_ += value - last_barcode_ - 1;
            }
            last_barcode_ = value;
            if (check_testapp_colors(y, y_stride, uv, uv_stride, w, h, source_w_, source_h_, value, &color_error_)) ++color_checked_;
        } else {
            ++barcode_unreadable_;
        }
    }
    if (!save_path_.empty() && index == save_index_) {
        std::vector<uint8_t> rgb;
        nv12_to_rgb(y, y_stride, uv, uv_stride, w, h, &rgb);
        if (!write_png_rgb(save_path_, w, h, rgb.data()))
            logging::get(Subsystem::Transport).warn("can't write {}", to_utf8(save_path_.wstring()));
        else
            logging::get(Subsystem::Transport).info("saved frame {} to {}", index, to_utf8(save_path_.wstring()));
    }

    // Into the video.
    FrameMeta meta;
    meta.tick = slot->tick;
    meta.present_qpc = int64_t(slot->present_qpc);
    meta.game_frame_time_us = slot->game_frame_time_us;
    meta.hook_cost_us = slot->hook_cost_us;
    meta.readback_frames = slot->readback_frames;
    meta.pacing_wait_us = slot->pacing_wait_us;
    meta.pacing_error_us = slot->pacing_error_us;
    meta.convert_ms = float(convert_ms);
    int ready = 0;
    for (uint32_t i = 0; i < ring_->slot_count; ++i)
        if (proto::ring_slot(ring_, i)->state.load(std::memory_order_relaxed) == uint32_t(proto::SlotState::Ready)) ++ready;
    meta.ring_fill_pct = int(ready * 100 / ring_->slot_count);
    if (config_.record.lock) {  // the hook's grid follows the game's phase: the timeline follows it
        const int64_t grid0 = int64_t(link_.control()->grid0_qpc.load(std::memory_order_acquire));
        if (grid0) pipeline_->set_t0(grid0);
    }
    pipeline_->on_frame(meta, y, y_stride, uv, uv_stride);  // encodes straight from the ring slot

    frames_.fetch_add(1);
    slot->state.store(uint32_t(proto::SlotState::Free), std::memory_order_release);
    read_index_ = (read_index_ + 1) % ring_->slot_count;
    return true;
}

void RecordingSession::receiver() {
    set_encoder_thread_priority(config_.record.encoder_priority == "above-normal");
    for (;;) {
        WaitForSingleObject(sem_, 50);
        const bool last_pass = stop_receiver_.load();
        while (consume_one()) {
        }
        if (!last_pass) pipeline_->on_idle(qpc_now());
        if (last_pass) break;
    }
}

void RecordingSession::finish() {
    proto::ControlBlock* ctl = link_.control();
    ctl->host_state.store(uint32_t(proto::SessionState::Idle), std::memory_order_release);
    stop_receiver_ = true;
    if (thread_.joinable()) thread_.join();

    // Pad the video to the stop time and close the file.
    std::string pipeline_error;
    const bool file_ok = pipeline_->finish(stop_qpc_, &pipeline_error);

    RecordingSummary s;
    s.valid = true;
    s.plan = plan_;
    s.lock = config_.record.lock;
    s.source_w = source_w_;
    s.source_h = source_h_;
    s.game = game_;
    s.backend = layout_ == proto::Layout::Bgra ? "OpenGL" : "D3D11";
    s.converted = layout_ == proto::Layout::Bgra;
    s.convert_p50_ms = percentile(convert_ms_, 50);
    s.convert_p99_ms = percentile(convert_ms_, 99);
    s.convert_max_ms = convert_ms_.empty() ? 0 : *std::max_element(convert_ms_.begin(), convert_ms_.end());
    s.avi = avi_;
    s.csv = csv_;
    s.json_file = json_;
    s.log = log_;
    s.pipeline = pipeline_->stats();
    s.file_ok = file_ok;
    s.error = pipeline_error;
    s.frames = frames_.load();
    s.seconds = double(s.pipeline.output_frames) / double(plan_.fps);
    s.tick_gaps = tick_gaps_;
    s.tick_repeats = tick_repeats_;
    s.seq_gaps = seq_gaps_;
    s.gpu_backlog = ctl->gpu_backlog_skips.load() - base_backlog_;
    s.ring_drops = ctl->ring_full_drops.load() - base_drops_;
    s.capture_errors = ctl->capture_errors.load() - base_errors_;
    s.first_frame_cost_ms = first_frame_cost_ms_;
    s.cost_p50_ms = percentile(cost_ms_, 50);
    s.cost_p99_ms = percentile(cost_ms_, 99);
    s.cost_max_ms = cost_ms_.empty() ? 0 : *std::max_element(cost_ms_.begin(), cost_ms_.end());
    if (!cost_ms_.empty()) {
        double sum = 0;
        for (double v : cost_ms_) sum += v;
        s.cost_avg_ms = sum / double(cost_ms_.size());
    }
    s.latency_p50_ms = percentile(latency_ms_, 50);
    s.latency_p99_ms = percentile(latency_ms_, 99);
    s.readback_p50_frames = percentile(readback_frames_, 50);
    s.game_frame_p50_ms = percentile(game_frame_ms_, 50);
    s.game_frame_p99_ms = percentile(game_frame_ms_, 99);
    if (!game_frame_ms_.empty()) {
        double sum = 0;
        for (double v : game_frame_ms_) sum += v;
        s.game_frame_avg_ms = sum / double(game_frame_ms_.size());
    }
    s.pacing_error_p50_ms = percentile(pacing_error_ms_, 50);
    s.pacing_error_p99_ms = percentile(pacing_error_ms_, 99);
    s.pacing_error_max_ms = pacing_error_ms_.empty() ? 0 : *std::max_element(pacing_error_ms_.begin(), pacing_error_ms_.end());
    s.pacing_wait_total_ms = pacing_wait_total_ms_;
    s.checksum = checksum_;
    s.barcode = analyze_;
    s.barcode_read = barcode_read_;
    s.barcode_unreadable = barcode_unreadable_;
    s.barcode_out_of_order = barcode_out_of_order_;
    s.barcode_source_frames_skipped = barcode_skipped_;
    s.color_error = color_error_;
    s.color_checked = color_checked_;
    s.barcode_first = first_barcode_;
    s.barcode_last = last_barcode_;

    // The pacing error is a warning when the capture of a frame regularly starts late (W1301).
    if (s.lock && s.pacing_error_p99_ms > 1.0)
        logging::event(Ev::PacingError, "p99={:.2f}ms max={:.2f}ms", s.pacing_error_p99_ms, s.pacing_error_max_ms);
    for (size_t i = 0; i < kEventCount; ++i) s.events_during[i] = logging::event_count(kEvents[i].id) - events_before_[i];
    summary_ = s;

    if (ring_) UnmapViewOfFile(ring_);
    if (ring_map_) CloseHandle(ring_map_);
    if (sem_) CloseHandle(sem_);
    ring_ = nullptr;
    ring_map_ = sem_ = nullptr;
    pipeline_.reset();
    state_ = State::Idle;

    // The summary file, and the same text in the logs.
    {
        FILE* f = nullptr;
        if (_wfopen_s(&f, json_.c_str(), L"wb") == 0 && f) {
            const std::string text = summary_.json();
            std::fwrite(text.data(), 1, text.size(), f);
            std::fclose(f);
        } else {
            logging::get(Subsystem::Disk).warn("can't write {}", to_utf8(json_.wstring()));
        }
    }
    std::string line;
    for (char ch : summary_.text()) {
        if (ch == '\n') {
            logging::get(Subsystem::Transport).info("{}", line);
            line.clear();
        } else {
            line += ch;
        }
    }
    logging::get(Subsystem::Transport).info("{}", line);
    logging::close_session();
}

}  // namespace rec
