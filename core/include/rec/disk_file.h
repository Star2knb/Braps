// The recorder's disk writer (recorder plan §11.2): a file opened unbuffered and overlapped, filled
// through two sector-aligned 8 MB buffers so one write is on its way to the disk while the next
// fills. The caller sees an append-only byte stream plus the ability to patch bytes already written
// (the AVI headers are rewritten when the recording ends). All calls come from one thread.
#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace rec {

class DiskFile {
public:
    struct Stats {
        uint64_t bytes_written = 0;   // bytes handed to the disk (padding included)
        uint64_t writes = 0;
        double busy_seconds = 0;      // sum of write latencies
        double max_latency_ms = 0;
        double peak_mb_s = 0;         // best single write
        uint32_t slow_writes = 0;     // latency above slow_ms
        uint32_t very_slow_writes = 0;
        double avg_mb_s() const { return busy_seconds > 0 ? double(bytes_written) / 1048576.0 / busy_seconds : 0; }
    };

    DiskFile() = default;
    ~DiskFile();
    DiskFile(const DiskFile&) = delete;
    DiskFile& operator=(const DiskFile&) = delete;

    static constexpr uint32_t kSector = 4096;  // aligns for 512-byte and 4K-native disks alike

    // Creates the file (replacing an existing one). buffer_bytes is rounded up to a multiple of the sector.
    bool open(const std::filesystem::path& path, size_t buffer_bytes, std::string* error);

    // Appends to the stream. False after a write error (see error()).
    bool append(const void* data, size_t size);
    // Bytes appended so far: where the next byte goes.
    uint64_t position() const { return position_; }

    // Overwrites bytes that are already in the stream (buffered, in flight or on disk).
    bool patch(uint64_t offset, const void* data, size_t size);

    // Writes everything appended so far to the disk (the unfinished sector is padded and rewritten
    // by the next write), and waits for it. Used for crash-safety checkpoints.
    bool checkpoint();

    // Writes the rest, waits for every write, cuts the file to position() bytes and closes it.
    bool close(std::string* error);
    bool is_open() const { return file_ != INVALID_HANDLE_VALUE; }
    // True if the folder made the new file NTFS-compressed and open() switched that off. The video is
    // compressed already; letting NTFS compress it again costs the CPU in the kernel on every write and
    // cut a recording's disk speed from ~400 MB/s to ~27 MB/s (D-086).
    bool compression_removed() const { return compression_removed_; }
    bool encrypted() const { return encrypted_; }
    const std::string& error() const { return error_; }

    // Write latencies above these are counted as slow (W4101 / E4102 thresholds, §10.4).
    void set_thresholds(double slow_ms, double very_slow_ms) {
        slow_ms_ = slow_ms;
        very_slow_ms_ = very_slow_ms;
    }
    // Called on the owning thread for each completed write: where in the file it went, how many bytes
    // (padding included), and its latency in milliseconds.
    std::function<void(uint64_t file_offset, uint32_t bytes, double latency_ms)> on_write;

    const Stats& stats() const { return stats_; }

private:
    struct Slot {
        uint8_t* data = nullptr;
        OVERLAPPED ov{};
        HANDLE event = nullptr;
        HANDLE wait = nullptr;       // thread-pool wait that timestamps the completion
        std::atomic<uint64_t> done_qpc{0};
        uint64_t submit_qpc = 0;
        uint64_t file_offset = 0;
        uint32_t bytes = 0;
        bool in_flight = false;
    };

    bool submit(Slot& s, uint32_t bytes);
    bool wait_for(Slot& s);
    bool drain();
    bool fail(const std::string& what, DWORD err);
    static VOID CALLBACK on_complete(PVOID context, BOOLEAN timed_out);

    HANDLE file_ = INVALID_HANDLE_VALUE;
    HANDLE patch_file_ = INVALID_HANDLE_VALUE;  // synchronous handle for read-modify-write of old sectors
    std::filesystem::path path_;
    size_t buffer_bytes_ = 0;
    Slot slots_[2];
    int cur_ = 0;
    size_t fill_ = 0;             // bytes in the current buffer
    uint64_t base_ = 0;           // file offset of the current buffer's first byte
    uint64_t position_ = 0;       // bytes appended
    uint64_t allocated_ = 0;      // preallocated size
    uint8_t* sector_buf_ = nullptr;
    double slow_ms_ = 50, very_slow_ms_ = 250;
    Stats stats_;
    std::string error_;
    bool compression_removed_ = false;
    bool encrypted_ = false;
};

}  // namespace rec
