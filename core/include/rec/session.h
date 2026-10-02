// One recording of one hooked game (recorder plan §3.3, §7). The host asks the hook to capture, the
// receiver thread takes the frames from the shared ring, checks them, and hands them to the encode
// pipeline (timeline, RCV1, AVI writer). Each recording produces
//   <Game> YYYY-MM-DD HH-MM-SS-cc.avi / .frames.csv / .summary.json / .log in the output folder.
//
//   IDLE --start()--> RECORDING --request_stop()--> STOPPING --poll()--> IDLE
#pragma once

#include <windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "rec/config.h"
#include "rec/events.h"
#include "rec/frame_tools.h"
#include "rec/hooklink.h"
#include "rec/monitor.h"
#include "rec/pipeline.h"

namespace rec {

// What gets recorded: output size and tick rate, from the settings and what the game offers.
struct OutputPlan {
    uint32_t width = 0, height = 0, fps = 0;
    bool size_capped = false;  // asked for more than the back buffer has (R4)
    bool fps_capped = false;   // asked for more than the display refreshes (R5)
};

// Output size: the configured one (or the back buffer's for "native"), no larger than the back buffer
// in either direction (aspect kept), rounded down to even numbers. Rate: the configured one, at most
// the display's refresh rate (when known).
bool plan_output(const Config::Record& settings, uint32_t backbuffer_w, uint32_t backbuffer_h, uint32_t refresh_hz, OutputPlan* plan,
                 std::string* error);

// "Minecraft.Windows 2026-09-28 15-15-15-36" for a game exe and a local time (hundredths of a second last).
std::string recording_basename(const std::string& exe_name, const SYSTEMTIME& local_time);

struct RecordingSummary {
    bool valid = false;
    OutputPlan plan;
    bool lock = false;
    uint32_t source_w = 0, source_h = 0;
    std::string game;
    std::string backend;       // "D3D11", "OpenGL"
    bool converted = false;    // frames arrived as BGRA and were converted to NV12 here (OpenGL)
    double convert_p50_ms = 0, convert_p99_ms = 0, convert_max_ms = 0;
    std::filesystem::path avi, csv, json_file, log;

    // Frames from the hook.
    double seconds = 0;   // length of the video (output frames / rate)
    uint64_t frames = 0;  // captured frames received
    uint64_t tick_gaps = 0;     // ticks without a captured frame (they became DUP frames)
    uint64_t tick_repeats = 0;  // two frames for one tick: must not happen
    uint64_t seq_gaps = 0;      // frames lost between hook and host: must not happen
    uint64_t gpu_backlog = 0;   // W1201: ticks skipped because the GPU was behind
    uint64_t ring_drops = 0;    // W1202: frames dropped because the host was behind
    uint64_t capture_errors = 0;
    double cost_p50_ms = 0, cost_p99_ms = 0, cost_max_ms = 0, cost_avg_ms = 0;  // hook time per captured frame, first frame excluded
    double first_frame_cost_ms = 0;                                          // the first one creates the GPU objects
    double latency_p50_ms = 0, latency_p99_ms = 0;                           // Present to frame in the host's hands
    double readback_p50_frames = 0;
    double game_frame_avg_ms = 0, game_frame_p50_ms = 0, game_frame_p99_ms = 0;
    double pacing_error_p50_ms = 0, pacing_error_p99_ms = 0, pacing_error_max_ms = 0;
    double pacing_wait_total_ms = 0;  // time the game was held to the grid (lock mode)

    // The encoded video.
    PipelineStats pipeline;
    bool file_ok = false;
    std::string error;
    std::array<uint64_t, kEventCount> events_during{};  // events logged during the recording, by id

    // Checks on the frames (rec_testapp's barcode and colour bars; the checksum folds every frame).
    uint64_t checksum = 0;
    bool barcode = false;
    uint64_t barcode_read = 0, barcode_unreadable = 0, barcode_out_of_order = 0, barcode_source_frames_skipped = 0;
    uint32_t barcode_first = 0, barcode_last = 0;
    ColorError color_error;
    uint64_t color_checked = 0;

    std::string text() const;  // several lines for the console and the log
    std::string json() const;  // the summary file (§10.5)
};

struct LiveStats {
    uint64_t frames = 0;
    double seconds = 0;
    double avg_cost_ms = 0;
    uint64_t gpu_backlog = 0, ring_drops = 0;
    // The video being written.
    uint64_t output_frames = 0, dup_filled = 0;
    double encode_ms = 0, ratio = 0;
    int queue_pct = 0;
    double write_mb_s = 0, file_mb = 0, free_gb = -1;
    int rate_level = 0;           // 0 lossless, 1-3 NEAR, 4 dropping frames (disk too slow)
    bool cpu_overloaded = false;  // dropping frames because the CPU can't keep up
};

class RecordingSession {
public:
    enum class State { Idle, Recording, Stopping };

    RecordingSession(HookLink& link, const Config& config, std::string game_name);
    ~RecordingSession();
    RecordingSession(const RecordingSession&) = delete;
    RecordingSession& operator=(const RecordingSession&) = delete;

    State state() const { return state_; }

    // IDLE -> RECORDING. False with the reason if the game isn't ready for it or the file can't be made.
    bool start(std::string* error);
    // RECORDING -> STOPPING (the hook finishes the read-backs in flight). Returns at once.
    void request_stop();
    // Call a few times a second. Completes STOPPING -> IDLE; true when a recording just ended.
    bool poll();
    // Ends a recording now without waiting for the hook (the game is gone). Builds the summary.
    void finish_now();
    // True once if capture failed in the game or the file can't be written; the session has begun to stop.
    bool take_error(std::string* message);

    LiveStats live() const;
    const OutputPlan& plan() const { return plan_; }
    const RecordingSummary& summary() const { return summary_; }
    bool locked() const { return config_.record.lock; }

    // Test aid: write captured frame number `index` (0-based) of the next recording as a PNG.
    void save_frame_to(const std::filesystem::path& png, uint64_t index) {
        save_path_ = png;
        save_index_ = index;
    }

private:
    void receiver();
    bool consume_one();
    void finish();

    HookLink& link_;
    Config config_;
    std::string game_;
    State state_ = State::Idle;
    OutputPlan plan_;
    uint32_t source_w_ = 0, source_h_ = 0;
    bool analyze_ = false;  // hash frames and check the test pattern (rec_testapp only: costs host CPU)

    // The shared ring of the current recording.
    HANDLE ring_map_ = nullptr;
    HANDLE sem_ = nullptr;
    proto::FrameRingHeader* ring_ = nullptr;
    uint32_t read_index_ = 0;
    uint32_t generation_ = 0;
    proto::Layout layout_ = proto::Layout::Nv12;  // what the hook writes into the ring (D3D11: NV12, OpenGL: BGRA)
    std::vector<uint8_t> convert_y_, convert_uv_;  // BGRA frames are converted into these
    std::vector<double> convert_ms_;

    std::unique_ptr<EncodePipeline> pipeline_;
    std::filesystem::path avi_, csv_, json_, log_;
    std::array<uint64_t, kEventCount> events_before_{};

    int64_t start_qpc_ = 0, stop_qpc_ = 0;
    uint64_t base_backlog_ = 0, base_drops_ = 0, base_errors_ = 0;
    bool error_reported_ = false;

    std::thread thread_;
    std::atomic<bool> stop_receiver_{false};
    std::atomic<uint64_t> frames_{0};
    std::atomic<uint64_t> cost_sum_us_{0};

    // Watching (receiver thread, once a second): resources, slow hooks, game stalls, the I6002 line.
    void second_tick();
    ResourceMonitor monitor_;
    std::atomic<bool> critical_space_{false};
    bool critical_reported_ = false;
    int64_t last_tick_qpc_ = 0;
    uint64_t sec_frames_ = 0, sec_cost_sum_us_ = 0, sec_game_sum_us_ = 0, sec_game_n_ = 0;
    uint32_t sec_cost_max_us_ = 0, sec_slow_hooks_ = 0, sec_stalls_ = 0;
    uint32_t sec_stall_worst_ms_ = 0;
    uint64_t last_dup_filled_ = 0, last_drops_ = 0;
    mutable int64_t free_checked_qpc_ = 0;
    mutable double free_gb_ = -1;

    // Filled by the receiver thread, read after it has been joined.
    std::vector<double> cost_ms_, latency_ms_, readback_frames_, game_frame_ms_, pacing_error_ms_;
    double pacing_wait_total_ms_ = 0;
    double first_frame_cost_ms_ = 0;
    uint64_t checksum_ = 0, last_seq_ = 0, seq_gaps_ = 0;
    int64_t first_tick_ = -1, last_tick_ = -1;
    uint64_t tick_gaps_ = 0, tick_repeats_ = 0;
    bool have_barcode_value_ = false;
    uint32_t last_barcode_ = 0, first_barcode_ = 0;
    uint64_t barcode_read_ = 0, barcode_unreadable_ = 0, barcode_out_of_order_ = 0, barcode_skipped_ = 0;
    ColorError color_error_;
    uint64_t color_checked_ = 0;

    std::filesystem::path save_path_;
    uint64_t save_index_ = 0;

    RecordingSummary summary_;
};

}  // namespace rec
