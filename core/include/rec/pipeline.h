// The encoding pipeline of a recording (recorder plan §3.2, §6.3, §8.3, §10.3, §11):
//
//   receiver thread:  frame (NV12 in the shared ring) -> timeline (DUP filling) -> RCV1 encoder
//                     -> packet arena -> queue
//   writer thread:    queue -> AVI chunks -> unbuffered, overlapped disk writes; telemetry CSV
//
// The timeline puts exactly one output frame on every tick of the recording clock: a frame whose
// tick is ahead of the next expected one is preceded by DUP packets for the missing ticks (the game
// did not present, or the capture skipped); a stall (no frame for 250 ms) is filled with DUPs by
// on_idle(); the end of the recording is padded to the stop time by finish(). The first frame's
// tick is the start of the video (there is nothing to repeat before it).
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "rcv/rcv.h"
#include "rec/avi.h"
#include "rec/disk_file.h"
#include "rec/rate.h"

namespace rec {

struct PipelineConfig {
    std::filesystem::path avi_path;
    std::filesystem::path csv_path;       // empty: no telemetry file
    uint32_t width = 0, height = 0;       // output size (even)
    uint32_t fps = 60;                    // ticks per second
    uint32_t source_w = 0, source_h = 0;  // the game's back buffer, kept in the AVI comment
    std::string game;
    std::string software = "rec";
    int64_t t0_qpc = 0;                   // QPC value of tick 0
    int64_t qpc_frequency = 0;            // 0: query
    std::function<int64_t()> clock;       // QPC now; default QueryPerformanceCounter (tests inject their own)
    int near_level = 0;                   // 0 = lossless (the rate controller sets it in M5)
    uint32_t keyframe_interval = 0;       // 0 = the codec's default (120)
    uint32_t encoder_threads = 0;         // 0 = automatic
    bool above_normal = true;             // priority of the encoder threads (record.encoder_priority)
    uint32_t queue_mb = 256;              // packet arena
    uint32_t disk_buffer_mb = 8;
    // A write is slow (W4101) / very slow (E4102) when it took longer than these AND ran at less than
    // three times the rate the recording produces data: a healthy disk writing 8 MiB buffers takes ~60 ms
    // each, which is not a problem, while the same disk stalling for 250 ms is.
    double slow_write_ms = 50, very_slow_write_ms = 250;
    uint64_t avi_block_limit = 1ull << 30;
    uint32_t stall_ms = 250;              // no frame for this long: fill with DUPs (§6.3)
    uint32_t stall_margin_ms = 150;       // ... but only ticks older than this (frames arrive late)
    RateSettings rate;                    // the rate controller (§9)
    bool rate_control = true;
    // Fault injection (§14.3).
    int debug_encoder_delay_ms = 0;
    double debug_disk_mb_s = 0;
    int debug_stall_ms = 0, debug_stall_every_s = 0;
};

// What the hook told us about a frame.
struct FrameMeta {
    uint64_t tick = 0;
    int64_t present_qpc = 0;
    uint32_t game_frame_time_us = 0, hook_cost_us = 0, readback_frames = 0, pacing_wait_us = 0, pacing_error_us = 0;
    int ring_fill_pct = 0;
    float convert_ms = 0;  // host time spent turning the frame into NV12 (OpenGL frames arrive as BGRA)
};

struct PipelineStats {
    uint64_t frames_i = 0, frames_p = 0;
    uint64_t dup_encoded = 0;      // the codec found the frame identical to the last one
    uint64_t dup_filled = 0;       // DUPs for ticks that had no frame (gaps, stalls, the tail)
    uint64_t dup_stall = 0;        // ... of which by the stall rule
    uint64_t dropped_late = 0;     // frames that arrived for a tick already filled (W2301)
    uint64_t dropped_queue = 0;    // frames dropped because the packet queue was full (W3104)
    uint64_t dropped_error = 0;    // frames the encoder refused
    uint64_t dropped_rate = 0;     // frames turned into DUPs on purpose by the rate controller (W3101 level 4, W3102)
    uint64_t slow_encodes = 0;     // frames that took longer than a frame interval to encode (W3103)
    std::array<double, RateController::kLevels> seconds_at_level{};  // time at each rate level
    uint64_t output_frames = 0;    // chunks in the AVI
    uint64_t bytes_i = 0, bytes_p = 0, bytes_dup = 0;
    uint64_t first_tick = 0, last_tick = 0;
    uint64_t file_bytes = 0;
    double encode_ms_avg = 0, encode_ms_p99 = 0, encode_ms_max = 0;
    double ratio_real = 0;         // raw NV12 bytes / bytes of I and P packets
    double ratio_overall = 0;      // raw bytes of every output frame / file payload
    DiskFile::Stats disk;
    bool failed = false;
    std::string error;
};

class EncodePipeline {
public:
    EncodePipeline();
    ~EncodePipeline();
    EncodePipeline(const EncodePipeline&) = delete;
    EncodePipeline& operator=(const EncodePipeline&) = delete;

    // Opens the encoder and the file and starts the writer thread.
    bool start(const PipelineConfig& config, std::string* error);
    // Tick 0 is this QPC value; set before the first frame (the session picks it after the file is open).
    void set_t0(int64_t qpc) { cfg_.t0_qpc = qpc; }

    // Receiver thread only, in tick order.
    void on_frame(const FrameMeta& meta, const uint8_t* y, uint32_t y_stride, const uint8_t* uv, uint32_t uv_stride);
    void on_idle(int64_t now_qpc);  // fills a stall with DUPs
    // Pads to the stop time, writes everything, finishes the file. False if a write failed.
    bool finish(int64_t stop_qpc, std::string* error);

    // Any thread.
    uint64_t output_frames() const { return output_frames_.load(); }
    uint64_t file_bytes() const { return file_bytes_.load(); }
    int queue_fill_pct() const;
    bool failed() const { return failed_.load(); }
    std::string error() const { return writer_error_; }
    // Live figures for the status line (any thread).
    double encode_ms_recent() const { return double(encode_us_.load()) / 1000.0; }
    double ratio_running() const {
        const uint64_t b = real_bytes_.load();
        return b ? double(cfg_.width) * cfg_.height * 1.5 * double(real_frames_.load()) / double(b) : 0.0;
    }
    double write_mb_s() const { return double(write_kbps_.load()) / 1024.0; }
    uint64_t dup_filled() const { return dup_filled_live_.load(); }
    int rate_level() const { return rate_level_live_.load(); }          // 0 lossless, 1-3 NEAR, 4 dropping
    bool cpu_overloaded() const { return cpu_overload_live_.load(); }
    const PipelineStats& stats() const { return stats_; }  // complete after finish()
    uint64_t base_tick() const { return base_tick_; }

private:
    struct Row {
        uint64_t out_index = 0;
        char type = 'P';  // 'I', 'P', 'D' (DUP), 'X' (dropped)
        uint64_t tick = 0;
        int64_t present_us = -1;
        float game_ms = -1, pacing_wait_ms = -1, hook_ms = -1, pacing_error_ms = -1;
        int readback = -1, ring_fill = -1, queue_pct = -1;
        float encode_ms = -1;
        float convert_ms = 0;
        uint32_t packet_bytes = 0;
        float ratio = -1;
        int near_level = 0;
        float skipped_pct = -1;
        uint64_t stream_end = 0;
    };
    struct Item {
        uint64_t pos = 0;      // absolute arena position of the packet (modulo capacity gives the address)
        uint32_t size = 0;
        uint32_t advance = 0;  // bytes of arena this item uses, wrap padding included
        bool has_packet = false;
        bool key = false;
        Row row;
    };

    uint8_t* reserve(uint32_t timeout_ms);
    void commit(uint32_t size, bool has_packet, bool key, Row row);
    bool pop(Item* item);
    void release(const Item& item);
    void writer_main();
    void emit_dup(uint64_t tick, bool stall);
    void on_rate_level(int from, int to, const RateInputs& in);
    void on_cpu_overload(bool started, const RateInputs& in);
    void push_drop(const FrameMeta& meta, const char* why);
    void write_row(const Row& row, double latency_ms);
    void finish_rows(double latency_ms);
    void flush_completed(uint64_t completed_end, double latency_ms);
    int64_t now() const;
    uint8_t* arena_at(uint64_t pos) { return arena_ + (pos % cap_); }

    PipelineConfig cfg_;
    int64_t freq_ = 0;
    rcv_encoder* enc_ = nullptr;
    rcv_encoder_config ecfg_{};
    size_t max_packet_ = 0;

    // Rate control.
    RateController rate_;
    std::atomic<int> rate_level_live_{0};
    std::atomic<bool> cpu_overload_live_{false};
    int cur_near_ = 0;
    int64_t last_overload_log_ms_ = -100000, last_slow_encode_log_ms_ = -100000;
    uint32_t overload_suppressed_ = 0, slow_encode_suppressed_ = 0;

    // Timeline.
    bool started_ = false;
    uint64_t base_tick_ = 0, next_tick_ = 0, out_index_ = 0;
    int64_t last_frame_host_ = 0;
    std::vector<double> encode_ms_;
    PipelineStats stats_;
    uint32_t late_logged_ = 0;

    // Packet arena and queue.
    uint8_t* arena_ = nullptr;
    size_t cap_ = 0;
    uint64_t head_ = 0, tail_ = 0;
    size_t pending_skip_ = 0;
    std::deque<Item> items_;
    mutable std::mutex mutex_;
    std::condition_variable space_, ready_;
    bool closing_ = false;

    // Writer.
    DiskFile file_;
    std::unique_ptr<AviWriter> avi_;
    std::thread writer_;
    std::atomic<bool> failed_{false};
    std::atomic<uint64_t> output_frames_{0}, file_bytes_{0};
    std::atomic<uint32_t> encode_us_{0}, write_kbps_{0};
    int64_t start_qpc_ = 0;                 // when start() ran
    uint32_t slow_writes_ = 0, very_slow_writes_ = 0;
    std::atomic<uint64_t> real_frames_{0}, real_bytes_{0}, dup_filled_live_{0};
    std::string writer_error_;
    FILE* csv_ = nullptr;
    std::deque<Row> pending_rows_;
    double last_latency_ms_ = 0;
    uint64_t completed_end_ = 0;
    std::string csv_buffer_;
    bool started_ok_ = false;
};

}  // namespace rec
