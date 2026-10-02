#include "rec/pipeline.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "rec/frame_tools.h"
#include "rec/log.h"
#include "rec/paths.h"
#include "rec/scheduling.h"

namespace rec {
namespace {

int64_t qpc_now() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

int64_t qpc_frequency() {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return f.QuadPart;
}

constexpr uint64_t kNoIndex = ~0ull;

}  // namespace

EncodePipeline::EncodePipeline() = default;

EncodePipeline::~EncodePipeline() {
    if (started_ok_ && writer_.joinable()) {
        std::string ignored;
        finish(now(), &ignored);
    }
    if (enc_) rcv_encoder_destroy(enc_);
    if (arena_) VirtualFree(arena_, 0, MEM_RELEASE);
}

int64_t EncodePipeline::now() const { return cfg_.clock ? cfg_.clock() : qpc_now(); }

int EncodePipeline::queue_fill_pct() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cap_ ? int((head_ - tail_) * 100 / cap_) : 0;
}

bool EncodePipeline::start(const PipelineConfig& config, std::string* error) {
    cfg_ = config;
    freq_ = cfg_.qpc_frequency ? cfg_.qpc_frequency : qpc_frequency();
    start_qpc_ = qpc_now();
    if (cfg_.width < 16 || cfg_.height < 16 || (cfg_.width & 1) || (cfg_.height & 1) || cfg_.fps < 1) {
        *error = "bad output size or frame rate";
        return false;
    }
    rcv_encoder_config_init(&ecfg_);
    ecfg_.format = RCV_FMT_YUV420;
    ecfg_.input_layout = RCV_IN_NV12;
    ecfg_.coded_width = uint16_t(cfg_.width);
    ecfg_.coded_height = uint16_t(cfg_.height);
    ecfg_.fps_num = cfg_.fps;
    ecfg_.fps_den = 1;
    if (cfg_.keyframe_interval) ecfg_.keyframe_interval = uint16_t(cfg_.keyframe_interval);
    ecfg_.num_threads = uint8_t(cfg_.encoder_threads);
    // The codec's worker threads: same priority as the thread that calls it (the caller sets it, codec plan §8).
    ecfg_.on_worker_start = [](void* user, int) { set_encoder_thread_priority(*static_cast<bool*>(user)); };
    ecfg_.user = &cfg_.above_normal;
    const rcv_status st = rcv_encoder_create(&ecfg_, &enc_);
    if (st != RCV_OK) {
        *error = std::string("can't create the encoder: ") + rcv_status_string(st);
        return false;
    }
    max_packet_ = rcv_max_packet_size(&ecfg_);

    cap_ = (std::max)(size_t(cfg_.queue_mb) << 20, 4 * max_packet_);
    arena_ = static_cast<uint8_t*>(VirtualAlloc(nullptr, cap_, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!arena_) {
        *error = "out of memory for the packet queue (" + std::to_string(cap_ >> 20) + " MB)";
        return false;
    }

    file_.set_thresholds(cfg_.slow_write_ms, cfg_.very_slow_write_ms);
    file_.on_write = [this](uint64_t offset, uint32_t bytes, double latency_ms) {
        completed_end_ = (std::max)(completed_end_, offset + bytes);
        last_latency_ms_ = latency_ms;
        const double mb_s = latency_ms > 0 ? double(bytes) / 1048576.0 / (latency_ms / 1000.0) : 0;
        write_kbps_.store(uint32_t(mb_s * 1024.0));
        // Compared with the rate the recording needs (after the first second, when there is a rate to speak of).
        const double elapsed_s = double(qpc_now() - start_qpc_) / double(freq_);
        const double needed_mb_s = elapsed_s >= 1.0 ? double(file_bytes_.load()) / 1048576.0 / elapsed_s : 0.0;
        const bool lagging = needed_mb_s > 0 && mb_s < 3.0 * needed_mb_s;
        if (lagging && latency_ms > cfg_.very_slow_write_ms) {
            ++very_slow_writes_;
            logging::event(Ev::VerySlowWrite, "latency={:.0f}ms size={}MiB rate={:.0f}MB/s queue={}%", latency_ms, bytes >> 20, mb_s, queue_fill_pct());
        } else if (lagging && latency_ms > cfg_.slow_write_ms) {
            ++slow_writes_;
            logging::event(Ev::SlowWrite, "latency={:.0f}ms size={}MiB rate={:.0f}MB/s queue={}%", latency_ms, bytes >> 20, mb_s, queue_fill_pct());
        }
        flush_completed(completed_end_, latency_ms);
    };
    if (!file_.open(cfg_.avi_path, size_t(cfg_.disk_buffer_mb) << 20, error)) return false;
    if (file_.compression_removed())
        logging::get(Subsystem::Disk).info("the output folder is NTFS-compressed; the recording is written uncompressed (the video is compressed already)");
    if (file_.encrypted())
        logging::get(Subsystem::Disk).warn("the output folder is encrypted (EFS): writes will be slow; choose another folder (record.out_dir)");

    AviVideoParams vp;
    vp.width = cfg_.width;
    vp.height = cfg_.height;
    vp.fps_num = cfg_.fps;
    vp.fps_den = 1;
    rcv_write_sequence_header(&ecfg_, vp.sequence_header);
    vp.suggested_buffer = uint32_t(max_packet_);
    vp.software = cfg_.software;
    vp.comment = "game=" + cfg_.game + " source=" + std::to_string(cfg_.source_w) + "x" + std::to_string(cfg_.source_h) +
                 " fps=" + std::to_string(cfg_.fps) + " encoder=rcv";
    avi_ = std::make_unique<AviWriter>(file_, cfg_.avi_block_limit);
    if (!avi_->begin(vp, error)) return false;

    if (!cfg_.csv_path.empty()) {
        if (_wfopen_s(&csv_, cfg_.csv_path.c_str(), L"wb") != 0) csv_ = nullptr;
        if (csv_)
            std::fputs("out_index,type,tick,present_qpc_us,game_frame_ms,pacing_wait_ms,hook_cost_ms,readback_latency_frames,map_copy_ms,"
                       "ring_fill_pct,convert_ms,encode_ms,packet_bytes,ratio,near,blocks_skipped_pct,packet_queue_pct,write_latency_ms,"
                       "audio_drift_ms,pacing_error_ms\n",
                       csv_);
        else
            logging::get(Subsystem::Disk).warn("can't create {}; no telemetry file", to_utf8(cfg_.csv_path.wstring()));
    }

    encode_ms_.reserve(1 << 16);
    started_ok_ = true;
    writer_ = std::thread([this] { writer_main(); });
    return true;
}

// ---- Packet queue ------------------------------------------------------------------------------------
uint8_t* EncodePipeline::reserve(uint32_t timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        const uint64_t used = head_ - tail_;
        const size_t offset = size_t(head_ % cap_);
        const size_t skip = (cap_ - offset < max_packet_) ? cap_ - offset : 0;  // keep each packet contiguous
        if (cap_ - used >= skip + max_packet_) {
            pending_skip_ = skip;
            return arena_ + (skip ? 0 : offset);
        }
        if (space_.wait_until(lock, deadline) == std::cv_status::timeout) return nullptr;
    }
}

void EncodePipeline::commit(uint32_t size, bool has_packet, bool key, Row row) {
    std::lock_guard<std::mutex> lock(mutex_);
    Item it;
    it.pos = head_ + pending_skip_;
    it.size = size;
    it.advance = uint32_t(pending_skip_) + size;
    it.has_packet = has_packet;
    it.key = key;
    row.queue_pct = int((head_ - tail_) * 100 / cap_);
    it.row = row;
    head_ += it.advance;
    pending_skip_ = 0;
    items_.push_back(std::move(it));
    ready_.notify_one();
}

bool EncodePipeline::pop(Item* item) {
    std::unique_lock<std::mutex> lock(mutex_);
    ready_.wait(lock, [this] { return !items_.empty() || closing_; });
    if (items_.empty()) return false;
    *item = std::move(items_.front());
    items_.pop_front();
    return true;
}

void EncodePipeline::release(const Item& item) {
    std::lock_guard<std::mutex> lock(mutex_);
    tail_ += item.advance;
    space_.notify_one();
}

// ---- Timeline and encoding (receiver thread) -----------------------------------------------------------
void EncodePipeline::emit_dup(uint64_t tick, bool stall) {
    uint8_t* out = reserve(1000);
    if (!out) {
        ++stats_.dropped_queue;
        logging::event(Ev::PacketQueueFull, "no room for a DUP at tick {}", tick);
        return;
    }
    rcv_frame_info info{};
    const rcv_status st = rcv_encode_duplicate(enc_, out, max_packet_, &info);
    if (st != RCV_OK) {
        ++stats_.dropped_error;
        return;
    }
    Row row;
    row.out_index = out_index_++;
    row.type = 'D';
    row.tick = tick;
    row.packet_bytes = info.packet_size;
    row.near_level = cfg_.near_level;
    commit(info.packet_size, true, false, row);
    ++stats_.dup_filled;
    dup_filled_live_.fetch_add(1);
    if (stall) ++stats_.dup_stall;
    stats_.bytes_dup += info.packet_size;
}

void EncodePipeline::push_drop(const FrameMeta& meta, const char*) {
    Row row;
    row.out_index = kNoIndex;
    row.type = 'X';
    row.tick = meta.tick;
    row.present_us = (meta.present_qpc - cfg_.t0_qpc) * 1000000 / freq_;
    row.game_ms = float(meta.game_frame_time_us) / 1000.0f;
    row.hook_ms = float(meta.hook_cost_us) / 1000.0f;
    std::lock_guard<std::mutex> lock(mutex_);
    Item it;
    it.pos = head_;
    row.queue_pct = int((head_ - tail_) * 100 / cap_);
    it.row = row;
    items_.push_back(std::move(it));
    ready_.notify_one();
}

void EncodePipeline::on_frame(const FrameMeta& meta, const uint8_t* y, uint32_t y_stride, const uint8_t* uv, uint32_t uv_stride) {
    last_frame_host_ = now();
    if (failed_.load()) return;
    if (!started_) {
        started_ = true;
        base_tick_ = next_tick_ = meta.tick;
        stats_.first_tick = meta.tick;
    }
    if (meta.tick < next_tick_) {  // its tick was filled already: it arrived too late (W2301)
        ++stats_.dropped_late;
        if (late_logged_++ < 5) logging::event(Ev::LateFrameDropped, "tick {} < expected {}", meta.tick, next_tick_);
        push_drop(meta, "late");
        return;
    }
    while (next_tick_ < meta.tick) emit_dup(next_tick_++, false);

    uint8_t* out = reserve(1000);
    if (!out) {  // the writer is far behind: the tick is filled with a DUP by the next frame
        ++stats_.dropped_queue;
        logging::event(Ev::PacketQueueFull, "frame at tick {} dropped", meta.tick);
        push_drop(meta, "queue");
        return;
    }
    rcv_frame_in in{};
    in.plane[0] = y;
    in.plane[1] = uv;
    in.stride[0] = int32_t(y_stride);
    in.stride[1] = int32_t(uv_stride);
    rcv_encode_params params{};
    params.near_level = uint8_t(cfg_.near_level);
    rcv_frame_info info{};
    const int64_t t0 = qpc_now();
    const rcv_status st = rcv_encode_frame(enc_, &in, &params, out, max_packet_, &info);
    const double encode_ms = double(qpc_now() - t0) * 1000.0 / double(qpc_frequency());
    if (st != RCV_OK) {
        ++stats_.dropped_error;
        logging::get(Subsystem::Encoder).warn("encoder refused the frame at tick {}: {}", meta.tick, rcv_status_string(st));
        push_drop(meta, "encoder");
        return;
    }
    encode_ms_.push_back(encode_ms);
    {
        const uint32_t now_us = uint32_t(encode_ms * 1000.0), old_us = encode_us_.load();
        encode_us_.store(old_us ? (old_us * 7 + now_us) / 8 : now_us);  // smoothed over ~8 frames
    }

    Row row;
    row.out_index = out_index_++;
    row.type = info.frame_type == 1 ? 'I' : info.frame_type == 2 ? 'P' : 'D';
    row.tick = meta.tick;
    row.present_us = (meta.present_qpc - cfg_.t0_qpc) * 1000000 / freq_;
    row.game_ms = float(meta.game_frame_time_us) / 1000.0f;
    row.pacing_wait_ms = float(meta.pacing_wait_us) / 1000.0f;
    row.pacing_error_ms = float(meta.pacing_error_us) / 1000.0f;
    row.hook_ms = float(meta.hook_cost_us) / 1000.0f;
    row.readback = int(meta.readback_frames);
    row.ring_fill = meta.ring_fill_pct;
    row.encode_ms = float(encode_ms);
    row.convert_ms = meta.convert_ms;
    row.packet_bytes = info.packet_size;
    row.ratio = info.packet_size ? float(double(cfg_.width) * cfg_.height * 1.5 / info.packet_size) : -1.0f;
    row.near_level = cfg_.near_level;
    row.skipped_pct = info.blocks_total ? 100.0f * float(info.blocks_skipped) / float(info.blocks_total) : -1.0f;
    commit(info.packet_size, true, info.is_keyframe != 0, row);
    next_tick_ = meta.tick + 1;
    if (info.frame_type == 1 || info.frame_type == 2) {
        real_frames_.fetch_add(1);
        real_bytes_.fetch_add(info.packet_size);
    }
    if (info.frame_type == 1) {
        ++stats_.frames_i;
        stats_.bytes_i += info.packet_size;
    } else if (info.frame_type == 2) {
        ++stats_.frames_p;
        stats_.bytes_p += info.packet_size;
    } else {
        ++stats_.dup_encoded;
        stats_.bytes_dup += info.packet_size;
    }
}

void EncodePipeline::on_idle(int64_t now_qpc) {
    if (!started_ || failed_.load()) return;
    const int64_t quiet_ms = (now_qpc - last_frame_host_) * 1000 / freq_;
    if (quiet_ms < cfg_.stall_ms) return;
    // Fill the ticks that are certainly over: frames reach the host up to ~100 ms after their tick.
    const int64_t horizon = now_qpc - int64_t(cfg_.stall_margin_ms) * freq_ / 1000 - cfg_.t0_qpc;
    if (horizon < 0) return;
    const uint64_t limit = uint64_t(horizon) * cfg_.fps / uint64_t(freq_);
    while (next_tick_ < limit) emit_dup(next_tick_++, true);
}

bool EncodePipeline::finish(int64_t stop_qpc, std::string* error) {
    if (!started_ok_) return false;
    if (started_ && !failed_.load() && stop_qpc > cfg_.t0_qpc) {
        const uint64_t stop_tick = uint64_t((stop_qpc - cfg_.t0_qpc) * int64_t(cfg_.fps) + freq_ / 2) / uint64_t(freq_);
        while (next_tick_ < stop_tick) emit_dup(next_tick_++, false);
    }
    stats_.last_tick = started_ && next_tick_ > 0 ? next_tick_ - 1 : 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closing_ = true;
        ready_.notify_all();
    }
    if (writer_.joinable()) writer_.join();
    started_ok_ = false;

    stats_.output_frames = output_frames_.load();
    stats_.file_bytes = file_bytes_.load();
    stats_.disk = file_.stats();
    stats_.disk.slow_writes = slow_writes_;  // counted against the recording's needs, see PipelineConfig
    stats_.disk.very_slow_writes = very_slow_writes_;
    stats_.failed = failed_.load();
    stats_.error = writer_error_;
    if (!encode_ms_.empty()) {
        double sum = 0;
        for (double v : encode_ms_) sum += v;
        stats_.encode_ms_avg = sum / double(encode_ms_.size());
        stats_.encode_ms_p99 = percentile(encode_ms_, 99);
        stats_.encode_ms_max = *std::max_element(encode_ms_.begin(), encode_ms_.end());
    }
    const double raw = double(cfg_.width) * cfg_.height * 1.5;
    const uint64_t real_frames = stats_.frames_i + stats_.frames_p;
    if (real_frames && stats_.bytes_i + stats_.bytes_p) stats_.ratio_real = raw * double(real_frames) / double(stats_.bytes_i + stats_.bytes_p);
    const uint64_t payload = stats_.bytes_i + stats_.bytes_p + stats_.bytes_dup;
    if (payload) stats_.ratio_overall = raw * double(stats_.output_frames) / double(payload);
    if (stats_.failed && error) *error = stats_.error;
    return !stats_.failed;
}

// ---- Writer thread ---------------------------------------------------------------------------------------
void EncodePipeline::writer_main() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    Item it;
    while (pop(&it)) {
        if (it.has_packet && !failed_.load()) {
            if (!avi_->add_video(arena_at(it.pos), it.size, it.key)) {
                writer_error_ = file_.error().empty() ? "the AVI writer failed" : file_.error();
                failed_ = true;
                logging::event(Ev::WriteFailed, "{}", writer_error_);
            } else {
                output_frames_.fetch_add(1);
            }
        }
        file_bytes_.store(file_.position());
        it.row.stream_end = file_.position();
        pending_rows_.push_back(it.row);
        release(it);
        flush_completed(completed_end_, last_latency_ms_);
    }
    std::string error;
    if (!failed_.load()) {
        if (!avi_->finish(&error)) {
            writer_error_ = error;
            failed_ = true;
            logging::event(Ev::WriteFailed, "{}", error);
        }
    } else {
        file_.close(&error);
    }
    file_bytes_.store(file_.position());
    finish_rows(last_latency_ms_);
    if (csv_) {
        std::fwrite(csv_buffer_.data(), 1, csv_buffer_.size(), csv_);
        std::fclose(csv_);
        csv_ = nullptr;
    }
}

void EncodePipeline::flush_completed(uint64_t completed_end, double latency_ms) {
    while (!pending_rows_.empty() && pending_rows_.front().stream_end <= completed_end) {
        write_row(pending_rows_.front(), latency_ms);
        pending_rows_.pop_front();
    }
}

void EncodePipeline::finish_rows(double latency_ms) {
    for (const Row& r : pending_rows_) write_row(r, latency_ms);
    pending_rows_.clear();
}

void EncodePipeline::write_row(const Row& r, double latency_ms) {
    if (!csv_) return;
    char line[512];
    auto num = [](char* dst, size_t n, float v, const char* fmt) {
        if (v < 0) dst[0] = 0;
        else std::snprintf(dst, n, fmt, double(v));
    };
    char idx[24] = "", present[24] = "", game[24], wait[24], hook[24], enc[24], ratio[24], skipped[24];
    if (r.out_index != kNoIndex) std::snprintf(idx, sizeof(idx), "%llu", (unsigned long long)r.out_index);
    if (r.present_us >= 0) std::snprintf(present, sizeof(present), "%lld", (long long)r.present_us);
    num(game, sizeof(game), r.game_ms, "%.3f");
    num(wait, sizeof(wait), r.pacing_wait_ms, "%.3f");
    num(hook, sizeof(hook), r.hook_ms, "%.3f");
    char perr[24];
    num(perr, sizeof(perr), r.pacing_error_ms, "%.3f");
    num(enc, sizeof(enc), r.encode_ms, "%.3f");
    num(ratio, sizeof(ratio), r.ratio, "%.2f");
    num(skipped, sizeof(skipped), r.skipped_pct, "%.1f");
    const char* type = r.type == 'I' ? "I" : r.type == 'P' ? "P" : r.type == 'D' ? "DUP" : "DROP";
    char readback[16] = "", ring[16] = "", queue[16] = "", convert[24] = "0";
    if (r.convert_ms > 0) std::snprintf(convert, sizeof(convert), "%.3f", r.convert_ms);
    if (r.readback >= 0) std::snprintf(readback, sizeof(readback), "%d", r.readback);
    if (r.ring_fill >= 0) std::snprintf(ring, sizeof(ring), "%d", r.ring_fill);
    if (r.queue_pct >= 0) std::snprintf(queue, sizeof(queue), "%d", r.queue_pct);
    // out_index, type, tick, present_qpc_us, game_frame_ms, pacing_wait_ms, hook_cost_ms, readback_latency_frames,
    // map_copy_ms (not measured: empty), ring_fill_pct, convert_ms (0 for NV12 input), encode_ms, packet_bytes, ratio,
    // near, blocks_skipped_pct, packet_queue_pct, write_latency_ms, audio_drift_ms (audio arrives in M6: empty),
    // pacing_error_ms (added: how late after its tick the capture began, lock mode)
    std::snprintf(line, sizeof(line), "%s,%s,%llu,%s,%s,%s,%s,%s,,%s,%s,%s,%u,%s,%d,%s,%s,%.2f,,%s\n", idx, type, (unsigned long long)r.tick, present, game,
                  wait, hook, readback, ring, convert, enc, r.packet_bytes, ratio, r.near_level, skipped, queue, latency_ms, perr);
    csv_buffer_ += line;
    if (csv_buffer_.size() >= (1u << 16)) {
        std::fwrite(csv_buffer_.data(), 1, csv_buffer_.size(), csv_);
        csv_buffer_.clear();
    }
}

}  // namespace rec
