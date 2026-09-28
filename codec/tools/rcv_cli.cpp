// rcv_cli - encode / decode / verify / stats for the RCV1 codec (codec plan §11.1).
//
// .rcv test container: 32-byte sequence header, then [u32 packet_size][packet] repeated.
#include <fcntl.h>
#include <io.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "rcv/rcv.h"

namespace {

void usage() {
    std::fprintf(stderr,
                 "usage:\n"
                 "  rcv_cli encode -i in.yuv -s WxH [-f yuv420] [-r FPS[/DEN]] [--near N] [--slices S]\n"
                 "                 [--threads T] [--predictor med|left] [--no-skip] [--keyint K] [--crc]\n"
                 "                 [--isa scalar|sse41|avx2] [--frames N] -o out.rcv\n"
                 "  rcv_cli decode -i in.rcv -o out.yuv            (raw I420; '-' = stdout)\n"
                 "  rcv_cli verify -i in.yuv -c in.rcv             (bit-exact round-trip check)\n"
                 "  rcv_cli stats  -i in.rcv [-v]                  (frame types, sizes, ratios)\n"
                 "Input/output '-' means stdin/stdout.\n");
}

struct Args {
    std::map<std::string, std::string> opt;
    bool has(const char* k) const { return opt.count(k) != 0; }
    std::string get(const char* k, const char* def = "") const {
        auto it = opt.find(k);
        return it == opt.end() ? def : it->second;
    }
};

bool parse_args(int argc, char** argv, Args* a) {
    static const char* kFlags[] = {"--no-skip", "--crc", "-v"};
    for (int i = 2; i < argc; ++i) {
        const std::string k = argv[i];
        if (std::find_if(std::begin(kFlags), std::end(kFlags), [&](const char* f) { return k == f; }) !=
            std::end(kFlags)) {
            a->opt[k] = "1";
            continue;
        }
        if (k.size() < 2 || k[0] != '-' || i + 1 >= argc) {
            std::fprintf(stderr, "bad argument: %s\n", k.c_str());
            return false;
        }
        a->opt[k] = argv[++i];
    }
    return true;
}

struct FileCloser {
    void operator()(FILE* f) const {
        if (f && f != stdin && f != stdout) std::fclose(f);
    }
};
using File = std::unique_ptr<FILE, FileCloser>;

File open_file(const std::string& path, bool write) {
    if (path == "-") {
        FILE* f = write ? stdout : stdin;
        _setmode(_fileno(f), _O_BINARY);
        return File(f);
    }
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), write ? "wb" : "rb") != 0) f = nullptr;
    if (!f) std::fprintf(stderr, "cannot open %s\n", path.c_str());
    return File(f);
}

bool read_exact(FILE* f, void* dst, size_t n) { return std::fread(dst, 1, n, f) == n; }
bool write_exact(FILE* f, const void* src, size_t n) { return std::fwrite(src, 1, n, f) == n; }

uint32_t load_u32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

bool parse_size(const std::string& s, int* w, int* h) {
    return sscanf_s(s.c_str(), "%dx%d", w, h) == 2 && *w > 0 && *h > 0;
}

double median(std::vector<double> v) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

double percentile(std::vector<double> v, double q) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, size_t(q * double(v.size() - 1) + 0.5))];
}

// Reads the container's sequence header and the next packet.
struct RcvReader {
    File f;
    uint8_t seq[32] = {};
    rcv_sequence_info info{};
    std::vector<uint8_t> pkt;

    bool open(const std::string& path) {
        f = open_file(path, false);
        if (!f) return false;
        if (!read_exact(f.get(), seq, 32)) {
            std::fprintf(stderr, "%s: missing sequence header\n", path.c_str());
            return false;
        }
        const rcv_status st = rcv_parse_sequence_header(seq, &info);
        if (st != RCV_OK) {
            std::fprintf(stderr, "%s: bad sequence header (%s)\n", path.c_str(), rcv_status_string(st));
            return false;
        }
        return true;
    }
    // 1 = packet read, 0 = clean end of file, -1 = error.
    int next() {
        uint8_t len[4];
        const size_t got = std::fread(len, 1, 4, f.get());
        if (got == 0) return 0;
        if (got != 4) return -1;
        const uint32_t n = load_u32(len);
        const size_t limit = size_t(info.coded_width) * info.coded_height * 4 + 65536;
        if (n == 0 || n > limit) return -1;
        pkt.resize(n);
        return read_exact(f.get(), pkt.data(), n) ? 1 : -1;
    }
};

size_t i420_size(int w, int h) { return size_t(w) * h + 2 * size_t(w / 2) * (h / 2); }

int cmd_encode(const Args& a) {
    int w = 0, h = 0;
    if (!a.has("-i") || !a.has("-o") || !parse_size(a.get("-s"), &w, &h)) {
        usage();
        return 2;
    }
    if (a.get("-f", "yuv420") != "yuv420") {
        std::fprintf(stderr, "only -f yuv420 is implemented so far (RGB/GBR arrives in M6)\n");
        return 2;
    }

    rcv_encoder_config cfg;
    rcv_encoder_config_init(&cfg);
    cfg.coded_width = uint16_t(w);
    cfg.coded_height = uint16_t(h);
    unsigned fps_num = 60, fps_den = 1;
    if (a.has("-r") && sscanf_s(a.get("-r").c_str(), "%u/%u", &fps_num, &fps_den) < 1) {
        std::fprintf(stderr, "bad -r\n");
        return 2;
    }
    cfg.fps_num = fps_num;
    cfg.fps_den = fps_den ? fps_den : 1;
    cfg.num_slices = uint8_t(std::atoi(a.get("--slices", "0").c_str()));
    cfg.num_threads = uint8_t(std::atoi(a.get("--threads", "0").c_str()));
    cfg.keyframe_interval = uint16_t(std::atoi(a.get("--keyint", "120").c_str()));
    cfg.enable_skip = a.has("--no-skip") ? 0 : 1;
    cfg.enable_crc = a.has("--crc") ? 1 : 0;
    const std::string pred = a.get("--predictor", "med");
    if (pred != "med" && pred != "left") {
        std::fprintf(stderr, "bad --predictor\n");
        return 2;
    }
    cfg.predictor = pred == "med" ? 1 : 0;
    std::string isa = a.get("--isa", "");
    if (isa.empty()) {  // RCV_FORCE_ISA is read only by tests and this CLI (plan §3.3)
        char* env = nullptr;
        size_t len = 0;
        if (_dupenv_s(&env, &len, "RCV_FORCE_ISA") == 0 && env) {
            isa = env;
            std::free(env);
        }
    }
    cfg.isa = isa == "scalar" ? RCV_ISA_SCALAR : isa == "sse41" ? RCV_ISA_SSE41 : isa == "avx2" ? RCV_ISA_AVX2 : RCV_ISA_AUTO;
    rcv_encode_params params{};
    params.near_level = uint8_t(std::atoi(a.get("--near", "0").c_str()));
    const long max_frames = std::atol(a.get("--frames", "0").c_str());

    rcv_encoder* enc = nullptr;
    rcv_status st = rcv_encoder_create(&cfg, &enc);
    if (st != RCV_OK) {
        std::fprintf(stderr, "encoder_create: %s\n", rcv_status_string(st));
        return 1;
    }
    std::unique_ptr<rcv_encoder, void (*)(rcv_encoder*)> enc_guard(enc, rcv_encoder_destroy);
    static const char* kIsaNames[] = {"auto", "scalar", "SSE4.1", "AVX2"};
    std::fprintf(stderr, "kernels: %s\n", kIsaNames[cfg.isa == RCV_ISA_AUTO ? rcv_cpu_isa() : cfg.isa]);

    File in = open_file(a.get("-i"), false);
    File out = open_file(a.get("-o"), true);
    if (!in || !out) return 1;

    uint8_t seq[32];
    rcv_write_sequence_header(&cfg, seq);
    if (!write_exact(out.get(), seq, 32)) return 1;

    const size_t frame_size = i420_size(w, h);
    std::vector<uint8_t> frame(frame_size), pkt(rcv_max_packet_size(&cfg));
    rcv_frame_in fin{};
    fin.plane[0] = frame.data();
    fin.plane[1] = frame.data() + size_t(w) * h;
    fin.plane[2] = fin.plane[1] + size_t(w / 2) * (h / 2);
    fin.stride[0] = w;
    fin.stride[1] = fin.stride[2] = w / 2;

    long frames = 0;
    uint64_t bytes_out = 32;
    std::vector<double> real_ratio, enc_ms;
    long type_count[3] = {};
    while (max_frames == 0 || frames < max_frames) {
        const size_t got = std::fread(frame.data(), 1, frame_size, in.get());
        if (got == 0) break;
        if (got != frame_size) {
            std::fprintf(stderr, "warning: ignoring partial frame at the end (%zu bytes)\n", got);
            break;
        }
        rcv_frame_info info{};
        st = rcv_encode_frame(enc, &fin, &params, pkt.data(), pkt.size(), &info);
        if (st != RCV_OK) {
            std::fprintf(stderr, "frame %ld: encode failed: %s\n", frames, rcv_status_string(st));
            return 1;
        }
        uint8_t len[4] = {uint8_t(info.packet_size), uint8_t(info.packet_size >> 8),
                          uint8_t(info.packet_size >> 16), uint8_t(info.packet_size >> 24)};
        if (!write_exact(out.get(), len, 4) || !write_exact(out.get(), pkt.data(), info.packet_size)) {
            std::fprintf(stderr, "write failed\n");
            return 1;
        }
        bytes_out += 4 + info.packet_size;
        type_count[info.frame_type]++;
        if (info.frame_type != 0) real_ratio.push_back(double(frame_size) / info.packet_size);
        enc_ms.push_back(info.time_total_us / 1000.0);
        ++frames;
    }

    const double raw = double(frame_size) * frames;
    std::fprintf(stderr,
                 "encoded %ld frames (I %ld, P %ld, DUP %ld) %dx%d\n"
                 "  in  %.1f MB, out %.1f MB, overall ratio %.3f:1\n"
                 "  real-frame ratio: median %.3f, min %.3f, max %.3f\n"
                 "  encode ms/frame: mean %.2f, p50 %.2f, p99 %.2f, max %.2f\n",
                 frames, type_count[1], type_count[2], type_count[0], w, h, raw / 1e6, bytes_out / 1e6,
                 bytes_out ? raw / double(bytes_out) : 0.0, median(real_ratio),
                 real_ratio.empty() ? 0 : *std::min_element(real_ratio.begin(), real_ratio.end()),
                 real_ratio.empty() ? 0 : *std::max_element(real_ratio.begin(), real_ratio.end()),
                 enc_ms.empty() ? 0 : [&] { double s = 0; for (double v : enc_ms) s += v; return s / enc_ms.size(); }(),
                 percentile(enc_ms, 0.5), percentile(enc_ms, 0.99), percentile(enc_ms, 1.0));
    return 0;
}

int cmd_decode(const Args& a) {
    if (!a.has("-i") || !a.has("-o")) {
        usage();
        return 2;
    }
    RcvReader r;
    if (!r.open(a.get("-i"))) return 1;
    rcv_decoder* dec = nullptr;
    rcv_status st = rcv_decoder_create(r.seq, 0, &dec);
    if (st != RCV_OK) {
        std::fprintf(stderr, "decoder_create: %s\n", rcv_status_string(st));
        return 1;
    }
    std::unique_ptr<rcv_decoder, void (*)(rcv_decoder*)> guard(dec, rcv_decoder_destroy);
    File out = open_file(a.get("-o"), true);
    if (!out) return 1;

    const int w = r.info.coded_width, h = r.info.coded_height;
    std::vector<uint8_t> frame(i420_size(w, h));
    uint8_t* planes[3] = {frame.data(), frame.data() + size_t(w) * h, frame.data() + size_t(w) * h + size_t(w / 2) * (h / 2)};
    const int32_t strides[3] = {w, w / 2, w / 2};
    long n = 0;
    for (int rc; (rc = r.next()) != 0; ++n) {
        if (rc < 0) {
            std::fprintf(stderr, "packet %ld: truncated or corrupt container\n", n);
            return 1;
        }
        st = rcv_decode_frame(dec, r.pkt.data(), r.pkt.size(), RCV_OUT_I420, planes, strides, nullptr);
        if (st != RCV_OK) {
            std::fprintf(stderr, "packet %ld: decode failed: %s\n", n, rcv_status_string(st));
            return 1;
        }
        if (!write_exact(out.get(), frame.data(), frame.size())) {
            std::fprintf(stderr, "write failed\n");
            return 1;
        }
    }
    std::fprintf(stderr, "decoded %ld frames (%dx%d I420)\n", n, w, h);
    return 0;
}

int cmd_verify(const Args& a) {
    if (!a.has("-i") || !a.has("-c")) {
        usage();
        return 2;
    }
    RcvReader r;
    if (!r.open(a.get("-c"))) return 1;
    File src = open_file(a.get("-i"), false);
    if (!src) return 1;
    rcv_decoder* dec = nullptr;
    rcv_status st = rcv_decoder_create(r.seq, 0, &dec);
    if (st != RCV_OK) {
        std::fprintf(stderr, "decoder_create: %s\n", rcv_status_string(st));
        return 1;
    }
    std::unique_ptr<rcv_decoder, void (*)(rcv_decoder*)> guard(dec, rcv_decoder_destroy);

    const int w = r.info.coded_width, h = r.info.coded_height;
    const size_t fs = i420_size(w, h);
    const size_t plane_off[4] = {0, size_t(w) * h, size_t(w) * h + size_t(w / 2) * (h / 2), fs};
    std::vector<uint8_t> frame(fs), ref(fs);
    uint8_t* planes[3] = {frame.data(), frame.data() + plane_off[1], frame.data() + plane_off[2]};
    const int32_t strides[3] = {w, w / 2, w / 2};

    long n = 0, bad_frames = 0;
    int max_err[3] = {};
    for (int rc; (rc = r.next()) != 0; ++n) {
        if (rc < 0) {
            std::fprintf(stderr, "packet %ld: truncated or corrupt container\n", n);
            return 1;
        }
        st = rcv_decode_frame(dec, r.pkt.data(), r.pkt.size(), RCV_OUT_I420, planes, strides, nullptr);
        if (st != RCV_OK) {
            std::fprintf(stderr, "packet %ld: decode failed: %s\n", n, rcv_status_string(st));
            return 1;
        }
        if (!read_exact(src.get(), ref.data(), fs)) {
            std::fprintf(stderr, "FAIL: source has fewer frames (%ld) than the .rcv file\n", n);
            return 1;
        }
        bool frame_ok = true;
        for (int p = 0; p < 3; ++p)
            for (size_t k = plane_off[p]; k < plane_off[p + 1]; ++k) {
                const int e = std::abs(int(frame[k]) - int(ref[k]));
                if (e) {
                    frame_ok = false;
                    max_err[p] = std::max(max_err[p], e);
                }
            }
        if (!frame_ok && ++bad_frames <= 10) std::fprintf(stderr, "frame %ld differs\n", n);
    }
    if (bad_frames) {
        std::fprintf(stderr, "FAIL: %ld of %ld frames differ; max error Y %d, Cb %d, Cr %d\n", bad_frames, n,
                     max_err[0], max_err[1], max_err[2]);
        return 1;
    }
    std::fprintf(stderr, "PASS: %ld frames bit-exact (%dx%d)\n", n, w, h);
    return 0;
}

int cmd_stats(const Args& a) {
    if (!a.has("-i")) {
        usage();
        return 2;
    }
    RcvReader r;
    if (!r.open(a.get("-i"))) return 1;
    const bool verbose = a.has("-v");
    const int w = r.info.coded_width, h = r.info.coded_height;
    const double raw = double(i420_size(w, h));
    static const char* kTypes[] = {"DUP", "I", "P"};
    static const char* kModes[] = {"HUFFMAN", "SINGLE", "RAW", "EMPTY"};
    long n = 0, type_count[3] = {}, mode_count[4] = {};
    uint64_t total = 32;
    std::vector<double> real_ratio, p_skip_pct;
    const size_t blocks = size_t((w + 15) / 16) * size_t((h + 15) / 16);
    if (verbose) std::printf("frame,type,bytes,ratio\n");
    for (int rc; (rc = r.next()) != 0; ++n) {
        if (rc < 0 || r.pkt.size() < 8 || r.pkt[5] > 2) {
            std::fprintf(stderr, "packet %ld: truncated or corrupt\n", n);
            return 1;
        }
        const int type = r.pkt[5];
        type_count[type]++;
        total += 4 + r.pkt.size();
        const double ratio = raw / double(r.pkt.size());
        if (type != 0) real_ratio.push_back(ratio);
        if (type != 0 && r.pkt.size() >= 32) {  // tally chunk modes; P-frames start with the skip map
            const size_t S = r.pkt[11];
            size_t map = 0;
            if (type == 2) {
                map = ((blocks + 7) / 8 + 3) & ~size_t(3);
                size_t skipped = 0;
                for (size_t b = 0; b < blocks && 32 + (b >> 3) < r.pkt.size(); ++b)
                    skipped += (r.pkt[32 + (b >> 3)] >> (b & 7)) & 1;
                p_skip_pct.push_back(100.0 * double(skipped) / double(blocks));
            }
            const uint8_t* dir = r.pkt.data() + 32 + map;
            size_t off = 32 + map + 12 * S;
            for (size_t k = 0; k < 3 * S && off < r.pkt.size(); ++k) {
                if (r.pkt[off] < 4) mode_count[r.pkt[off]]++;
                off += load_u32(dir + 4 * k);
            }
        }
        if (verbose) std::printf("%ld,%s,%zu,%.3f\n", n, kTypes[type], r.pkt.size(), ratio);
    }
    std::printf("%ld frames %dx%d @ %u/%u fps: I %ld, P %ld, DUP %ld\n", n, w, h, r.info.fps_num, r.info.fps_den,
                type_count[1], type_count[2], type_count[0]);
    std::printf("total %.1f MB, overall ratio %.3f:1\n", total / 1e6, n ? raw * n / double(total) : 0.0);
    if (!real_ratio.empty())
        std::printf("real-frame ratio: median %.3f, min %.3f, max %.3f\n", median(real_ratio),
                    *std::min_element(real_ratio.begin(), real_ratio.end()),
                    *std::max_element(real_ratio.begin(), real_ratio.end()));
    if (!p_skip_pct.empty()) {
        double sum = 0;
        for (double v : p_skip_pct) sum += v;
        std::printf("P-frames: %.1f%% of blocks skipped on average\n", sum / double(p_skip_pct.size()));
    }
    std::printf("chunk modes:");
    for (int m = 0; m < 4; ++m) std::printf(" %s %ld", kModes[m], mode_count[m]);
    std::printf("\n");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return 2;
    }
    Args a;
    if (!parse_args(argc, argv, &a)) return 2;
    const std::string cmd = argv[1];
    if (cmd == "encode") return cmd_encode(a);
    if (cmd == "decode") return cmd_decode(a);
    if (cmd == "verify") return cmd_verify(a);
    if (cmd == "stats") return cmd_stats(a);
    usage();
    return 2;
}
