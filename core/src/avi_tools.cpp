#include "rec/avi_tools.h"

#include <windows.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include "rcv/rcv.h"
#include "rec/avi.h"
#include "rec/frame_tools.h"
#include "rec/paths.h"

namespace rec {
namespace {

std::string format(const char* fmt, ...) {
    char buf[768];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    return buf;
}

// "source=1366x745" in the AVI comment.
bool parse_source(const std::string& comment, uint32_t* w, uint32_t* h) {
    const size_t at = comment.find("source=");
    if (at == std::string::npos) return false;
    unsigned a = 0, b = 0;
    if (sscanf_s(comment.c_str() + at + 7, "%ux%u", &a, &b) != 2 || !a || !b) return false;
    *w = a;
    *h = b;
    return true;
}

std::vector<std::string> split_csv(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : line) {
        if (c == ',') {
            out.push_back(cur);
            cur.clear();
        } else if (c != '\r') {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

}  // namespace

VerifyReport verify_avi(const std::filesystem::path& path, const VerifyOptions& options, const std::function<void(uint64_t, uint64_t)>& progress) {
    VerifyReport r;
    std::ostringstream out;
    const std::string name = to_utf8(path.filename().wstring());

    const AviScan scan = scan_avi(path);
    r.problems = scan.problems;
    r.file_bytes = scan.file_size;
    if (!scan.ok) {
        out << "rec verify " << name << "\n  FAIL  not a readable recording\n";
        for (const std::string& p : r.problems) out << "        - " << p << "\n";
        r.text = out.str();
        return r;
    }
    r.width = scan.info.width;
    r.height = scan.info.height;
    r.fps_num = scan.info.fps_num;
    r.fps_den = scan.info.fps_den ? scan.info.fps_den : 1;
    r.frames = scan.video.size();
    r.seconds = r.fps_num ? double(r.frames) * r.fps_den / r.fps_num : 0;

    rcv_sequence_info seq{};
    if (!scan.info.has_sequence_header || rcv_parse_sequence_header(scan.info.sequence_header, &seq) != RCV_OK) {
        r.problems.push_back("the RCV1 sequence header is missing or invalid");
    } else if (seq.coded_width != r.width || seq.coded_height != r.height) {
        r.problems.push_back("the sequence header says " + std::to_string(seq.coded_width) + "x" + std::to_string(seq.coded_height) + ", the AVI says " +
                             std::to_string(r.width) + "x" + std::to_string(r.height));
    }

    uint32_t src_w = options.source_w, src_h = options.source_h;
    if (!src_w || !src_h) {
        if (!parse_source(scan.info.comment, &src_w, &src_h)) {
            src_w = r.width;
            src_h = r.height;
        }
    }

    rcv_decoder* dec = nullptr;
    AviFile file;
    if (r.problems.empty() || scan.info.has_sequence_header) {
        if (rcv_decoder_create(scan.info.sequence_header, uint8_t(options.threads), &dec) != RCV_OK) {
            dec = nullptr;
            r.problems.push_back("can't create a decoder for this stream");
        }
    }
    if (dec && !file.open(path)) r.problems.push_back("can't read the file");

    if (dec) {
        const size_t y_size = size_t(r.width) * r.height, uv_size = y_size / 2;
        std::vector<uint8_t> y(y_size), uv(uv_size), packet;
        uint64_t prev_hash = 0;
        bool have_prev_hash = false, have_value = false;
        uint32_t prev_value = 0;
        uint64_t dup_run = 0;  // consecutive DUPs just before this frame
        constexpr uint64_t kStallDups = 15;  // 250 ms at 60 fps: the stall rule's threshold
        constexpr uint32_t kInFlightFrames = 8;  // the most frames the hook can hold in its read-back pipeline
        uint64_t decode_errors = 0, key_mismatch = 0;
        uint64_t raw_real = 0, bytes_real = 0;
        for (size_t i = 0; i < scan.video.size(); ++i) {
            const AviChunk& c = scan.video[i];
            if (!file.read(c.data_offset, c.size, &packet)) {
                r.problems.push_back("frame " + std::to_string(i) + ": can't read the packet");
                break;
            }
            uint8_t* planes[3] = {y.data(), uv.data(), nullptr};
            const int32_t strides[3] = {int32_t(r.width), int32_t(r.width), 0};
            rcv_frame_info info{};
            const rcv_status st = rcv_decode_frame(dec, packet.data(), packet.size(), RCV_OUT_NV12, planes, strides, &info);
            if (st != RCV_OK) {
                if (decode_errors++ < 5) r.problems.push_back("frame " + std::to_string(i) + ": " + rcv_status_string(st));
                continue;
            }
            if (i == 0 && info.frame_type != 1) r.problems.push_back("the first frame is not an I-frame");
            if (bool(info.is_keyframe) != c.key && key_mismatch++ < 5) r.problems.push_back("frame " + std::to_string(i) + ": the index key flag disagrees with the packet");
            if (info.is_keyframe) ++r.keyframes;
            if (info.frame_type == 1) ++r.frames_i;
            else if (info.frame_type == 2) ++r.frames_p;
            else ++r.frames_dup;
            r.packet_bytes += c.size;
            if (info.frame_type != 0) {
                raw_real += y_size + uv_size;
                bytes_real += c.size;
            }

            const uint64_t h = hash_bytes(uv.data(), uv_size, hash_bytes(y.data(), y_size));
            if (info.frame_type == 0 && have_prev_hash && h != prev_hash) ++r.dup_with_new_picture;
            prev_hash = h;
            have_prev_hash = true;

            if (options.testapp) {
                uint32_t value = 0;
                if (decode_barcode(y.data(), r.width, r.width, r.height, src_w, src_h, &value)) {
                    ++r.barcode_read;
                    if (info.frame_type != 0) {  // a DUP repeats the picture, its counter too (checked through the hash)
                        if (have_value) {
                            if (value == prev_value) {
                                ++r.barcode_repeated;
                            } else if (value < prev_value) {
                                ++r.barcode_out_of_order;
                            } else {
                                const uint64_t gap = value - prev_value - 1;
                                // After a stall (a long run of DUPs) the frames the hook still held in its read-back
                                // pipeline arrived too late and were dropped: explained, and counted apart.
                                if (gap > 0 && dup_run >= kStallDups && gap <= kInFlightFrames) r.barcode_lost_at_stall += gap;
                                else r.barcode_missing += gap;
                            }
                        }
                        prev_value = value;
                        have_value = true;
                    }
                } else {
                    ++r.barcode_unreadable;
                }
            }
            dup_run = (info.frame_type == 0) ? dup_run + 1 : 0;
            if (progress && (i % 60 == 0)) progress(i, scan.video.size());
        }
        r.ratio = bytes_real ? double(raw_real) / double(bytes_real) : 0;
        if (decode_errors) r.problems.push_back(std::to_string(decode_errors) + " frame(s) failed to decode");
        rcv_decoder_destroy(dec);
    }

    // Telemetry beside the file: the ticks must be consecutive and the capture close to its tick.
    {
        std::filesystem::path csv = path;
        csv.replace_extension(".frames.csv");
        std::ifstream in(csv);
        if (in) {
            r.have_csv = true;
            std::string line;
            std::getline(in, line);
            const std::vector<std::string> head = split_csv(line);
            int c_type = -1, c_tick = -1, c_present = -1, c_pacing = -1;
            for (size_t i = 0; i < head.size(); ++i) {
                if (head[i] == "type") c_type = int(i);
                if (head[i] == "tick") c_tick = int(i);
                if (head[i] == "present_qpc_us") c_present = int(i);
                if (head[i] == "pacing_error_ms") c_pacing = int(i);
            }
            std::vector<double> late_ms;
            bool have_tick = false;
            uint64_t prev_tick = 0;
            while (std::getline(in, line)) {
                const std::vector<std::string> f = split_csv(line);
                if (c_type < 0 || c_tick < 0 || c_present < 0 || f.size() <= size_t(std::max({c_type, c_tick, c_present}))) continue;
                if (f[size_t(c_type)] == "DROP") continue;
                ++r.csv_rows;
                const uint64_t tick = std::strtoull(f[size_t(c_tick)].c_str(), nullptr, 10);
                if (have_tick && tick != prev_tick + 1) ++r.csv_ticks_missing;
                prev_tick = tick;
                have_tick = true;
                if (c_pacing >= 0 && f.size() > size_t(c_pacing)) {
                    // lock mode: how late after its tick the hook began the capture (the grid follows the game's phase)
                    if (!f[size_t(c_pacing)].empty()) late_ms.push_back(std::strtod(f[size_t(c_pacing)].c_str(), nullptr));
                } else if (!f[size_t(c_present)].empty() && r.fps_num) {
                    const double present_ms = std::strtod(f[size_t(c_present)].c_str(), nullptr) / 1000.0;
                    const double tick_ms = double(tick) * 1000.0 * r.fps_den / r.fps_num;
                    late_ms.push_back((std::max)(0.0, present_ms - tick_ms));
                }
            }
            r.pacing_error_p50_ms = percentile(late_ms, 50);
            r.pacing_error_p99_ms = percentile(late_ms, 99);
            r.pacing_error_max_ms = late_ms.empty() ? 0 : *std::max_element(late_ms.begin(), late_ms.end());
            if (r.csv_rows != r.frames) r.problems.push_back("the telemetry file has " + std::to_string(r.csv_rows) + " rows for " + std::to_string(r.frames) + " frames");
            if (r.csv_ticks_missing) r.problems.push_back("the telemetry file's ticks are not consecutive (" + std::to_string(r.csv_ticks_missing) + " jumps)");
        }
    }
    if (r.dup_with_new_picture) r.problems.push_back(std::to_string(r.dup_with_new_picture) + " DUP frame(s) whose picture differs from the one before");

    if (options.testapp) {
        if (r.barcode_read == 0) r.findings.push_back("no test-pattern barcode could be read (is this a rec_testapp recording?)");
        if (r.barcode_missing) r.findings.push_back(std::to_string(r.barcode_missing) + " game frame(s) missing between consecutive captured frames");
        if (r.barcode_repeated) r.findings.push_back(std::to_string(r.barcode_repeated) + " frame(s) repeated that are not DUPs");
        if (r.barcode_out_of_order) r.findings.push_back(std::to_string(r.barcode_out_of_order) + " frame(s) out of order");
        if (r.barcode_unreadable) r.findings.push_back(std::to_string(r.barcode_unreadable) + " frame(s) with an unreadable barcode");
    }

    r.ok = r.problems.empty();
    out << "rec verify " << name << "\n";
    out << format("  %ux%u, %u/%u fps, %llu frames = %.2f s, %.1f MB; %u RIFF block(s)\n", r.width, r.height, r.fps_num, r.fps_den,
                  (unsigned long long)r.frames, r.seconds, double(r.file_bytes) / 1048576.0, scan.riff_blocks);
    out << format("  I %llu (%llu keyframes), P %llu, DUP %llu; compression %.2f:1 on real frames\n", (unsigned long long)r.frames_i, (unsigned long long)r.keyframes,
                  (unsigned long long)r.frames_p, (unsigned long long)r.frames_dup, r.ratio);
    if (!scan.info.software.empty()) out << "  written by " << scan.info.software << (scan.info.comment.empty() ? "" : "; " + scan.info.comment) << "\n";
    if (r.have_csv)
        out << format("  telemetry: %llu rows, ticks %s; capture began at most %.2f ms after its tick (median %.2f, p99 %.2f)\n", (unsigned long long)r.csv_rows,
                      r.csv_ticks_missing ? "NOT consecutive" : "consecutive", r.pacing_error_max_ms, r.pacing_error_p50_ms, r.pacing_error_p99_ms);
    if (options.testapp)
        out << format("  test pattern: %llu barcodes read; missing %llu, repeated %llu, out of order %llu, unreadable %llu; lost to stall latency %llu; "
                      "DUP with a new picture %llu\n",
                      (unsigned long long)r.barcode_read, (unsigned long long)r.barcode_missing, (unsigned long long)r.barcode_repeated,
                      (unsigned long long)r.barcode_out_of_order, (unsigned long long)r.barcode_unreadable, (unsigned long long)r.barcode_lost_at_stall,
                      (unsigned long long)r.dup_with_new_picture);
    for (const std::string& p : r.problems) out << "  PROBLEM: " << p << "\n";
    for (const std::string& f : r.findings) out << "  FINDING: " << f << "\n";
    out << (r.ok ? (r.findings.empty() ? "  PASS" : "  PASS (file is sound; see the findings)") : "  FAIL") << "\n";
    r.text = out.str();
    return r;
}

// ---- convert ------------------------------------------------------------------------------------------
bool convert_avi(const std::filesystem::path& input, const ConvertOptions& options, std::string* message, const std::function<void(uint64_t, uint64_t)>& progress) {
    if (options.container != "mp4" && options.container != "mkv") {
        *message = "--to must be mp4 or mkv";
        return false;
    }
    const AviScan scan = scan_avi(input);
    if (!scan.ok) {
        *message = "not a readable recording" + (scan.problems.empty() ? std::string() : ": " + scan.problems.front());
        return false;
    }

    wchar_t ffmpeg[MAX_PATH] = {};
    if (!SearchPathW(nullptr, L"ffmpeg", L".exe", MAX_PATH, ffmpeg, nullptr)) {
        *message = "ffmpeg.exe was not found on PATH (needed by rec convert)";
        return false;
    }
    std::filesystem::path output = options.output;
    if (output.empty()) {
        output = input;
        output.replace_extension("." + options.container);
    }
    const uint32_t w = scan.info.width, h = scan.info.height;
    const uint32_t fps_num = scan.info.fps_num, fps_den = scan.info.fps_den ? scan.info.fps_den : 1;

    // Full-range BT.601 YUV in, limited-range BT.601 H.264 out (the recorder's colour; plan §4.3).
    std::wstring cmd = L"\"" + std::wstring(ffmpeg) + L"\" -hide_banner -loglevel error -y -f rawvideo -pix_fmt yuvj420p -s " + std::to_wstring(w) + L"x" +
                       std::to_wstring(h) + L" -r " + std::to_wstring(fps_num) + L"/" + std::to_wstring(fps_den) +
                       L" -i - -an -vf scale=in_range=pc:out_range=tv:in_color_matrix=bt601:out_color_matrix=bt601,format=yuv420p"
                       L" -c:v libx264 -preset medium -crf " + std::to_wstring(options.crf) +
                       L" -colorspace smpte170m -color_primaries smpte170m -color_trc smpte170m -color_range tv" +
                       (options.container == "mp4" ? L" -movflags +faststart" : L"") + L" \"" + output.wstring() + L"\"";

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE pipe_read = nullptr, pipe_write = nullptr;
    if (!CreatePipe(&pipe_read, &pipe_write, &sa, 4 << 20)) {
        *message = "can't create a pipe: " + win32_error_text(GetLastError());
        return false;
    }
    SetHandleInformation(pipe_write, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = pipe_read;
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) {
        *message = "can't start ffmpeg: " + win32_error_text(GetLastError());
        CloseHandle(pipe_read);
        CloseHandle(pipe_write);
        return false;
    }
    CloseHandle(pipe_read);
    CloseHandle(pi.hThread);

    bool ok = true;
    rcv_decoder* dec = nullptr;
    AviFile file;
    if (rcv_decoder_create(scan.info.sequence_header, uint8_t(options.threads), &dec) != RCV_OK || !file.open(input)) {
        *message = "can't open the recording for decoding";
        ok = false;
    }
    const size_t y_size = size_t(w) * h, c_size = y_size / 4;
    std::vector<uint8_t> frame(y_size + 2 * c_size), packet;
    for (size_t i = 0; ok && i < scan.video.size(); ++i) {
        const AviChunk& c = scan.video[i];
        uint8_t* planes[3] = {frame.data(), frame.data() + y_size, frame.data() + y_size + c_size};
        const int32_t strides[3] = {int32_t(w), int32_t(w / 2), int32_t(w / 2)};
        rcv_frame_info info{};
        if (!file.read(c.data_offset, c.size, &packet)) {
            *message = "frame " + std::to_string(i) + ": can't read the packet";
            ok = false;
        } else if (rcv_decode_frame(dec, packet.data(), packet.size(), RCV_OUT_I420, planes, strides, &info) != RCV_OK) {
            *message = "frame " + std::to_string(i) + ": it does not decode";
            ok = false;
        } else {
            size_t done = 0;
            while (done < frame.size()) {
                DWORD n = 0;
                if (!WriteFile(pipe_write, frame.data() + done, DWORD(frame.size() - done), &n, nullptr)) {
                    *message = "ffmpeg stopped reading (see its message above)";
                    ok = false;
                    break;
                }
                done += n;
            }
        }
        if (progress && i % 120 == 0) progress(i, scan.video.size());
    }
    if (dec) rcv_decoder_destroy(dec);
    CloseHandle(pipe_write);
    WaitForSingleObject(pi.hProcess, 600000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    if (ok && code != 0) {
        *message = "ffmpeg failed (exit code " + std::to_string(code) + ")";
        ok = false;
    }
    if (ok) {
        std::error_code ec;
        const auto size = std::filesystem::file_size(output, ec);
        *message = "wrote " + to_utf8(output.wstring()) + " (" + format("%.1f", double(size) / 1048576.0) + " MB)";
    }
    return ok;
}

}  // namespace rec
