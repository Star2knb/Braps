// `rec verify` and `rec convert` (recorder plan §12.1, §14.1): reading recordings back.
#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace rec {

struct VerifyOptions {
    bool testapp = false;       // read rec_testapp's frame-counter barcode and judge continuity (§14.1)
    uint32_t source_w = 0, source_h = 0;  // the game's window size for the barcode (default: from the AVI comment, else the video size)
    int threads = 2;
};

struct VerifyReport {
    bool ok = false;                      // structure and every frame fine (test-pattern findings are separate)
    std::vector<std::string> problems;    // anything wrong with the file
    std::vector<std::string> findings;    // test-pattern findings that mean a recording problem (missing, repeated, out of order)
    uint64_t frames = 0, frames_i = 0, frames_p = 0, frames_dup = 0;
    uint64_t keyframes = 0;
    uint64_t packet_bytes = 0;
    uint32_t width = 0, height = 0, fps_num = 0, fps_den = 1;
    double seconds = 0;
    double ratio = 0;                     // raw NV12 bytes of the I and P frames / their packet bytes
    uint64_t file_bytes = 0;
    // Test pattern.
    uint64_t barcode_read = 0, barcode_unreadable = 0, barcode_missing = 0, barcode_repeated = 0, barcode_out_of_order = 0;
    uint64_t barcode_lost_at_stall = 0;   // frames that were still in the GPU read-back when the game stopped presenting (see DECISIONS D-078)
    uint64_t dup_with_new_picture = 0;    // a DUP whose picture differs from the one before: must not happen
    // Spacing against the tick grid, from the .frames.csv beside the file.
    bool have_csv = false;
    double pacing_error_p50_ms = 0, pacing_error_p99_ms = 0, pacing_error_max_ms = 0;
    uint64_t csv_rows = 0, csv_ticks_missing = 0;
    std::string text;                     // the report as printed
};

// Walks the structure, decodes every frame and (optionally) reads the barcodes. `progress` is called
// now and then with (frames done, frames total).
VerifyReport verify_avi(const std::filesystem::path& path, const VerifyOptions& options,
                        const std::function<void(uint64_t, uint64_t)>& progress = {});

struct ConvertOptions {
    std::string container = "mp4";  // mp4 | mkv
    int crf = 16;
    std::filesystem::path output;   // empty: next to the input, with the container's extension
    int threads = 2;
};

// Decodes the recording and pipes the frames to FFmpeg (found on PATH). Video only for now.
bool convert_avi(const std::filesystem::path& input, const ConvertOptions& options, std::string* message,
                 const std::function<void(uint64_t, uint64_t)>& progress = {});

}  // namespace rec
