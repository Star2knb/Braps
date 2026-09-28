// rcv_bench - speed and compression benchmark for RCV1 (codec plan §11.5).
//
// Streams a raw I420 corpus once. For every frame and every configured RCV1 encoder it
// encodes (a frame identical to the previous one becomes a DUP packet, as the recorder will
// emit it), decodes, verifies bit-exactness and records sizes and timings. Per-frame packet
// sizes of other codecs (FRAPS, Ut Video, FFV1, ...) can be added with --compare so all
// codecs are measured with the same statistics on the same frames.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <fcntl.h>
#include <intrin.h>
#include <io.h>
#include <powerbase.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "profile.h"
#include "rcv/rcv.h"

namespace {

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

void usage() {
    std::fprintf(stderr,
                 "usage: rcv_bench -i corpus.raw|- -s WxH [-r FPS] [--frames N] [--format yuv420|gbr]\n"
                 "                 (yuv420: planar I420 frames; gbr: packed BGRA frames; '-' reads stdin)\n"
                 "                 [--predictor med|left|both] [--slices 8[,1,...]] (0 = auto)\n"
                 "                 [--isa auto|scalar|sse41|avx2|all[,...]] [--threads 1[,2,...]]\n"
                 "                 [--skip on|off|both]   (temporal skip / P-frames; default on)\n"
                 "                 [--cpu N | --cpu -1] [--no-verify] [--no-decode] [--csv frames.csv]\n"
                 "                 [--compare NAME=sizes.txt]...\n"
                 "sizes.txt: one packet size per line, in frame order ('# enc_ms=X' line optional).\n");
}

struct Options {
    std::string input, csv;
    int w = 0, h = 0;
    double fps = 60;
    long max_frames = 0;
    std::vector<int> predictors{1};
    std::vector<int> slices{8};
    std::vector<rcv_isa> isas{RCV_ISA_AUTO};
    std::vector<int> threads{1};
    std::vector<int> skip{1};  // temporal skip on / off
    bool rgb = false;          // --format gbr: input is packed BGRA, coded as GBR
    int cpu = -2;  // -2: last logical CPU, -1: no pinning
    bool verify = true;
    bool decode = true;  // --no-decode: encoder only, like the recorder (keeps its reference in cache)
    std::vector<std::pair<std::string, std::string>> compare;
};

std::vector<int> parse_int_list(const std::string& s) {
    std::vector<int> v;
    size_t pos = 0;
    while (pos <= s.size()) {
        const size_t comma = s.find(',', pos);
        const std::string item = s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        if (!item.empty()) v.push_back(std::atoi(item.c_str()));
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return v;
}

bool parse_options(int argc, char** argv, Options* o) {
    for (int i = 1; i < argc; ++i) {
        const std::string k = argv[i];
        if (k == "--no-verify") {
            o->verify = false;
            continue;
        }
        if (k == "--no-decode") {
            o->decode = false;
            o->verify = false;
            continue;
        }
        if (i + 1 >= argc) return false;
        const std::string v = argv[++i];
        if (k == "-i") o->input = v;
        else if (k == "-s") {
            if (sscanf_s(v.c_str(), "%dx%d", &o->w, &o->h) != 2) return false;
        } else if (k == "-r") o->fps = std::atof(v.c_str());
        else if (k == "--frames") o->max_frames = std::atol(v.c_str());
        else if (k == "--predictor") {
            if (v == "med") o->predictors = {1};
            else if (v == "left") o->predictors = {0};
            else if (v == "both") o->predictors = {1, 0};
            else return false;
        } else if (k == "--slices") o->slices = parse_int_list(v);
        else if (k == "--threads") o->threads = parse_int_list(v);
        else if (k == "--format") {
            if (v == "yuv420") o->rgb = false;
            else if (v == "gbr" || v == "rgb") o->rgb = true;
            else return false;
        }
        else if (k == "--skip") {
            if (v == "on") o->skip = {1};
            else if (v == "off") o->skip = {0};
            else if (v == "both") o->skip = {1, 0};
            else return false;
        }
        else if (k == "--isa") {
            o->isas.clear();
            size_t pos = 0;
            while (pos <= v.size()) {
                const size_t comma = v.find(',', pos);
                const std::string name = v.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
                if (name == "all") {
                    for (int lvl = RCV_ISA_SCALAR; lvl <= int(rcv_cpu_isa()); ++lvl) o->isas.push_back(rcv_isa(lvl));
                } else if (name == "auto") o->isas.push_back(RCV_ISA_AUTO);
                else if (name == "scalar") o->isas.push_back(RCV_ISA_SCALAR);
                else if (name == "sse41") o->isas.push_back(RCV_ISA_SSE41);
                else if (name == "avx2") o->isas.push_back(RCV_ISA_AVX2);
                else return false;
                if (comma == std::string::npos) break;
                pos = comma + 1;
            }
        }
        else if (k == "--cpu") o->cpu = std::atoi(v.c_str());
        else if (k == "--csv") o->csv = v;
        else if (k == "--compare") {
            const size_t eq = v.find('=');
            if (eq == std::string::npos || eq == 0) return false;
            o->compare.emplace_back(v.substr(0, eq), v.substr(eq + 1));
        } else return false;
    }
    return !o->input.empty() && o->w > 0 && o->h > 0 && o->fps > 0 && !o->slices.empty();
}

// ---------------------------------------------------------------- system info

std::string cpu_brand() {
    int r[4];
    char brand[49] = {};
    __cpuid(r, int(0x80000000));
    if (unsigned(r[0]) >= 0x80000004u)
        for (int i = 0; i < 3; ++i) {
            __cpuid(r, int(0x80000002 + i));
            std::memcpy(brand + 16 * i, r, 16);
        }
    std::string s = brand;
    s.erase(0, s.find_first_not_of(' '));
    return s.empty() ? "unknown CPU" : s;
}

struct CpuClock {
    unsigned long current = 0, max = 0;
};

// As reported by Windows; on many laptops "current" stays at the base clock even in turbo.
CpuClock cpu_clock() {
    struct ProcessorPowerInformation {  // PROCESSOR_POWER_INFORMATION (documented, not in the SDK headers)
        ULONG Number, MaxMhz, CurrentMhz, MhzLimit, MaxIdleState, CurrentIdleState;
    };
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    std::vector<ProcessorPowerInformation> v(si.dwNumberOfProcessors);
    if (CallNtPowerInformation(ProcessorInformation, nullptr, 0, v.data(),
                               ULONG(v.size() * sizeof(ProcessorPowerInformation))) != 0)
        return {};
    return {v[0].CurrentMhz, v[0].MaxMhz};
}

std::string power_state() {
    SYSTEM_POWER_STATUS ps;
    if (!GetSystemPowerStatus(&ps)) return "power: unknown";
    std::string s = ps.ACLineStatus == 1 ? "AC power" : ps.ACLineStatus == 0 ? "on battery" : "power: unknown";
    if (ps.BatteryLifePercent <= 100) s += ", battery " + std::to_string(ps.BatteryLifePercent) + "%";
    if (ps.SystemStatusFlag == 1) s += ", battery saver ON";
    return s;
}

// ---------------------------------------------------------------- statistics

double median_sorted(const std::vector<double>& v) {
    if (v.empty()) return 0;
    const size_t n = v.size();
    return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

double pct_sorted(const std::vector<double>& v, double q) {
    if (v.empty()) return 0;
    return v[std::min(v.size() - 1, size_t(q * double(v.size() - 1) + 0.5))];
}

struct Dist {
    double mean = 0, p50 = 0, p99 = 0, max = 0;
    size_t n = 0;
};

Dist distribution(std::vector<double> v) {
    Dist d;
    if (v.empty()) return d;
    std::sort(v.begin(), v.end());
    double sum = 0;
    for (double x : v) sum += x;
    d.n = v.size();
    d.mean = sum / double(v.size());
    d.p50 = median_sorted(v);
    d.p99 = pct_sorted(v, 0.99);
    d.max = v.back();
    return d;
}

// One codec's per-frame results. enc_ms/dec_ms are empty for external codecs.
struct Series {
    std::string name;
    std::vector<uint32_t> bytes;
    std::vector<double> enc_ms, dec_ms;
    std::vector<uint8_t> types;          // RCV1: frame type per packet (0 DUP, 1 I, 2 P)
    std::vector<double> skip_pct;        // RCV1 P-frames: % of blocks skipped
    std::vector<double> skip_ms;         // RCV1: phase A wall time, frames where it ran
    std::vector<double> skip_ms_full;    // ... of those, frames that turned out unchanged (whole frame compared)
    double external_enc_ms = -1;  // from '# enc_ms=' in a --compare file
    rcv::StageTimes stages;
    long verify_failures = 0;
};

struct Metrics {
    size_t frames = 0, real = 0;
    double median = 0, worst = 0, best = 0, aggregate = 0, overall = 0, avg_mbs = 0, peak_mbs = 0;
    Dist enc, dec;
};

// Packets this small are duplicate-frame markers (FRAPS: 8 bytes, RCV1 DUP: 8 bytes).
constexpr uint32_t kMarkerMaxBytes = 64;

// Ratios use "real" frames only. A codec that stores duplicates as marker packets is measured on
// the frames it actually coded; a codec without markers on the frames that differ from their
// predecessor. (The two can disagree by a slot: FFmpeg re-times FRAPS duplicates on decode.)
Metrics compute(const Series& s, const std::vector<uint8_t>& unique, double raw, double fps) {
    Metrics m;
    const size_t n = std::min(s.bytes.size(), unique.size());
    m.frames = n;
    const bool has_markers =
        std::any_of(s.bytes.begin(), s.bytes.begin() + ptrdiff_t(n), [](uint32_t b) { return b <= kMarkerMaxBytes; });
    std::vector<double> ratios, enc, dec;
    double real_bytes = 0, total = 0;
    for (size_t i = 0; i < n; ++i) {
        total += s.bytes[i];
        const bool real = has_markers ? s.bytes[i] > kMarkerMaxBytes : unique[i] != 0;
        if (!real || s.bytes[i] == 0) continue;
        ratios.push_back(raw / s.bytes[i]);
        real_bytes += s.bytes[i];
        if (i < s.enc_ms.size()) enc.push_back(s.enc_ms[i]);
        if (i < s.dec_ms.size()) dec.push_back(s.dec_ms[i]);
    }
    std::sort(ratios.begin(), ratios.end());
    m.real = ratios.size();
    if (!ratios.empty()) {
        m.median = median_sorted(ratios);
        m.worst = ratios.front();
        m.best = ratios.back();
        m.aggregate = raw * double(ratios.size()) / real_bytes;
    }
    if (total > 0) {
        m.overall = raw * double(n) / total;
        m.avg_mbs = total / (double(n) / fps) / 1e6;
    }
    // Peak write rate over any window of one second of frames.
    const size_t win = std::max<size_t>(1, std::min(n, size_t(fps + 0.5)));
    double sum = 0, peak = 0;
    for (size_t i = 0; i < n; ++i) {
        sum += s.bytes[i];
        if (i >= win) sum -= s.bytes[i - win];
        peak = std::max(peak, sum);
    }
    m.peak_mbs = n ? peak * (fps / double(win)) / 1e6 : 0;
    m.enc = distribution(enc);
    m.dec = distribution(dec);
    return m;
}

bool load_sizes(const std::string& name, const std::string& path, Series* s) {
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) {
        std::fprintf(stderr, "cannot open %s\n", path.c_str());
        return false;
    }
    s->name = name;
    char line[256];
    while (std::fgets(line, sizeof(line), f)) {
        if (line[0] == '#') {
            const char* p = std::strstr(line, "enc_ms=");
            if (p) s->external_enc_ms = std::atof(p + 7);
            continue;
        }
        char* end = nullptr;
        const unsigned long v = std::strtoul(line, &end, 10);
        if (end != line) s->bytes.push_back(uint32_t(v));
    }
    std::fclose(f);
    return true;
}

// ---------------------------------------------------------------- RCV1 runs

// Logical CPUs for the bench thread (worker 0) and encoder workers 1..n-1: the bench CPU first,
// then one per other physical core (Windows numbers hyper-thread siblings adjacently, so stepping
// by 2 changes core), then the siblings. On a 2C/4T CPU with the bench on 3: 3, 1, 2, 0.
struct PinPlan {
    int cpus[64];
    int count = 0;
};

PinPlan make_pin_plan(int first, int logical) {
    PinPlan plan;
    if (first < 0) return plan;
    for (int parity = 0; parity < 2; ++parity)
        for (int k = 0; k < logical && plan.count < 64; k += 2) {
            const int c = ((first - parity - k) % logical + logical) % logical;
            bool seen = false;
            for (int i = 0; i < plan.count; ++i) seen |= plan.cpus[i] == c;
            if (!seen) plan.cpus[plan.count++] = c;
        }
    return plan;
}

// rcv_encoder_config::on_worker_start: pin encoder worker `worker` like the bench thread.
void pin_worker(void* user, int worker) {
    const PinPlan* plan = static_cast<const PinPlan*>(user);
    if (worker < plan->count) SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR(1) << plan->cpus[worker]);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
}

class RcvRun {
public:
    RcvRun(int w, int h, bool rgb, double fps, int predictor, int slices, rcv_isa isa, int threads, bool skip,
           const PinPlan* pins)
        : w_(w), h_(h), threads_(threads), skip_(skip), rgb_(rgb) {
        rcv_encoder_config_init(&cfg_);
        cfg_.coded_width = uint16_t(w);
        cfg_.coded_height = uint16_t(h);
        if (rgb) {
            cfg_.format = RCV_FMT_GBR;
            cfg_.input_layout = RCV_IN_BGRA;
        }
        cfg_.fps_num = uint32_t(fps + 0.5);
        cfg_.fps_den = 1;
        cfg_.predictor = uint8_t(predictor);
        cfg_.num_slices = uint8_t(slices);
        cfg_.isa = isa;
        cfg_.num_threads = uint8_t(threads);
        cfg_.enable_skip = skip ? 1 : 0;
        if (pins && pins->count) {
            cfg_.on_worker_start = pin_worker;
            cfg_.user = const_cast<PinPlan*>(pins);
        }
        static const char* kIsa[] = {"auto", "scalar", "SSE4.1", "AVX2"};
        const rcv_isa shown = isa == RCV_ISA_AUTO ? rcv_cpu_isa() : isa;
        series.name = std::string("RCV1 ") + (predictor ? "MED" : "LEFT") +
                      (slices ? " S=" + std::to_string(slices) : std::string(" S=auto")) + " " + kIsa[shown] +
                      " T=" + std::to_string(threads) + (skip ? "" : " noskip");
    }
    int threads() const { return threads_; }
    bool skip() const { return skip_; }
    ~RcvRun() {
        rcv_encoder_destroy(enc_);
        rcv_decoder_destroy(dec_);
    }
    RcvRun(const RcvRun&) = delete;
    RcvRun& operator=(const RcvRun&) = delete;

    rcv_status init() {
        rcv_status st = rcv_encoder_create(&cfg_, &enc_);
        if (st != RCV_OK) return st;
        uint8_t seq[32];
        rcv_write_sequence_header(&cfg_, seq);
        st = rcv_decoder_create(seq, uint8_t(threads_), &dec_);  // decoder workers are not pinned
        if (st != RCV_OK) return st;
        rcv::set_stage_profile(enc_, &series.stages);
        pkt_.resize(rcv_max_packet_size(&cfg_));
        out_.resize(rgb_ ? size_t(w_) * h_ * 4 : size_t(w_) * h_ * 3 / 2);
        return RCV_OK;
    }

    // Returns false on a codec error (which is fatal for the benchmark).
    // With skip on, every frame goes through rcv_encode_frame and the encoder itself turns
    // unchanged frames into DUPs (phase A). With skip off, frames identical to the previous one are
    // sent as rcv_encode_duplicate, as the recorder host does for timeline gaps (and as in M2-M4).
    bool frame(const uint8_t* src, bool identical, bool decode, bool verify, long index) {
        const bool dup = identical && !skip_;
        const size_t ysize = size_t(w_) * h_, csize = size_t(w_ / 2) * (h_ / 2);
        rcv_frame_in in{};
        if (rgb_) {  // packed BGRA
            in.plane[0] = src;
            in.stride[0] = 4 * w_;
        } else {
            in.plane[0] = src;
            in.plane[1] = src + ysize;
            in.plane[2] = src + ysize + csize;
            in.stride[0] = w_;
            in.stride[1] = in.stride[2] = w_ / 2;
        }

        rcv_frame_info info{};
        const Clock::time_point t0 = Clock::now();
        rcv_status st = dup ? rcv_encode_duplicate(enc_, pkt_.data(), pkt_.size(), &info)
                            : rcv_encode_frame(enc_, &in, nullptr, pkt_.data(), pkt_.size(), &info);
        const double enc_ms = ms_since(t0);
        if (st != RCV_OK) {
            std::fprintf(stderr, "\n%s: frame %ld: encode failed: %s\n", series.name.c_str(), index,
                         rcv_status_string(st));
            return false;
        }
        double dec_ms = 0;
        if (decode) {
            uint8_t* planes[3] = {out_.data(), out_.data() + ysize, out_.data() + ysize + csize};
            int32_t strides[3] = {w_, w_ / 2, w_ / 2};
            if (rgb_) strides[0] = 4 * w_;
            const Clock::time_point t1 = Clock::now();
            st = rcv_decode_frame(dec_, pkt_.data(), info.packet_size, rgb_ ? RCV_OUT_BGRA : RCV_OUT_I420, planes,
                                  strides, nullptr);
            dec_ms = ms_since(t1);
            if (st != RCV_OK) {
                std::fprintf(stderr, "\n%s: frame %ld: decode failed: %s\n", series.name.c_str(), index,
                             rcv_status_string(st));
                return false;
            }
        }
        series.bytes.push_back(info.packet_size);
        series.types.push_back(info.frame_type);
        if (info.frame_type == 2 && info.blocks_total)
            series.skip_pct.push_back(100.0 * info.blocks_skipped / info.blocks_total);
        if (skip_ && !dup && (info.frame_type != 1 || info.time_skip_us)) {
            series.skip_ms.push_back(info.time_skip_us / 1000.0);
            if (info.frame_type == 0) series.skip_ms_full.push_back(info.time_skip_us / 1000.0);
        }
        series.enc_ms.push_back(enc_ms);
        if (decode) series.dec_ms.push_back(dec_ms);
        if (verify && !same_output(src)) series.verify_failures++;
        return true;
    }

    // Decoded frame equals the source; for BGRA only colour counts (alpha isn't stored).
    bool same_output(const uint8_t* src) const {
        if (!rgb_) return std::memcmp(out_.data(), src, out_.size()) == 0;
        for (size_t k = 0; k < out_.size(); k += 4) {
            uint32_t a, b;
            std::memcpy(&a, src + k, 4);
            std::memcpy(&b, out_.data() + k, 4);
            if ((a ^ b) & 0x00FFFFFFu) return false;
        }
        return true;
    }

    Series series;

private:
    int w_, h_, threads_;
    bool skip_, rgb_;
    rcv_encoder_config cfg_{};
    rcv_encoder* enc_ = nullptr;
    rcv_decoder* dec_ = nullptr;
    std::vector<uint8_t> pkt_, out_;
};

void print_stage_table(const std::vector<std::unique_ptr<RcvRun>>& runs) {
    std::printf("\nEncoder stage breakdown (mean ms per coded I/P frame, single-thread runs only; skip compare "
                "per frame it ran on):\n\n");
    std::printf("| Config | skip compare | load | predict + histogram | table build | entropy write | other | total |\n");
    std::printf("|---|---|---|---|---|---|---|---|\n");
    for (const auto& r : runs) {
        const rcv::StageTimes& t = r->series.stages;
        if (!t.frames || r->threads() != 1) continue;  // with threads, stage times are summed CPU time
        const double k = 1e-6 / double(t.frames);
        const double skip = t.skip_frames ? double(t.skip_ns) * 1e-6 / double(t.skip_frames) : 0.0;
        // Phase A of coded frames is inside total; phase A of frames that became DUP is not.
        const double other = double(t.total_ns) - double(t.load_ns + t.predict_ns + t.table_ns + t.entropy_ns) -
                             skip * 1e6 * double(t.frames);
        std::printf("| %s | %.3f | %.2f | %.2f | %.2f | %.2f | %.2f | %.2f |\n", r->series.name.c_str(), skip,
                    t.load_ns * k, t.predict_ns * k, t.table_ns * k, t.entropy_ns * k, other > 0 ? other * k : 0.0,
                    t.total_ns * k);
    }
}

void print_frame_types(const std::vector<std::unique_ptr<RcvRun>>& runs) {
    std::printf("\nFrame types and temporal skip:\n\n");
    std::printf("| Config | I | P | DUP | blocks skipped in P (mean) | skip compare ms p50 / p99 / max (wall) "
                "| ... on unchanged frames (whole frame compared) |\n");
    std::printf("|---|---|---|---|---|---|---|\n");
    for (const auto& r : runs) {
        const Series& s = r->series;
        long counts[3] = {};
        for (uint8_t t : s.types) counts[t < 3 ? t : 0]++;
        double pct = 0;
        for (double v : s.skip_pct) pct += v;
        const Dist sk = distribution(s.skip_ms), full = distribution(s.skip_ms_full);
        char skip[64] = "-", skip_full[64] = "-";
        if (sk.n) std::snprintf(skip, sizeof(skip), "%.3f / %.3f / %.3f", sk.p50, sk.p99, sk.max);
        if (full.n) std::snprintf(skip_full, sizeof(skip_full), "%.3f / %.3f / %.3f", full.p50, full.p99, full.max);
        std::printf("| %s | %ld | %ld | %ld | %.2f%% | %s | %s |\n", s.name.c_str(), counts[1], counts[2], counts[0],
                    s.skip_pct.empty() ? 0.0 : pct / double(s.skip_pct.size()), skip, skip_full);
    }
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parse_options(argc, argv, &opt)) {
        usage();
        return 2;
    }
    if (!opt.rgb && ((opt.w | opt.h) & 1)) {
        std::fprintf(stderr, "YUV420 needs even dimensions\n");
        return 2;
    }

    // Stable timing: one fixed logical CPU, high thread priority.
    const DWORD logical = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    const int cpu = opt.cpu == -2 ? int(logical) - 1 : opt.cpu;
    if (cpu >= 0) SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR(1) << cpu);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    const CpuClock clk_before = cpu_clock();

    const PinPlan pins = make_pin_plan(cpu, int(logical));
    std::vector<std::unique_ptr<RcvRun>> runs;
    for (int p : opt.predictors)
        for (int s : opt.slices)
            for (rcv_isa isa : opt.isas)
                for (int t : opt.threads)
                    for (int sk : opt.skip) {
                        auto r = std::make_unique<RcvRun>(opt.w, opt.h, opt.rgb, opt.fps, p, s, isa, t, sk != 0,
                                                          &pins);
                        const rcv_status st = r->init();
                        if (st != RCV_OK) {
                            std::fprintf(stderr, "%s: init failed: %s\n", r->series.name.c_str(),
                                         rcv_status_string(st));
                            return 1;
                        }
                        runs.push_back(std::move(r));
                    }
    std::vector<Series> externals(opt.compare.size());
    for (size_t i = 0; i < opt.compare.size(); ++i)
        if (!load_sizes(opt.compare[i].first, opt.compare[i].second, &externals[i])) return 1;

    FILE* in = nullptr;
    if (opt.input == "-") {  // e.g. piped from FFmpeg, so large RGB corpora need no disk space
        in = stdin;
        _setmode(_fileno(stdin), _O_BINARY);
    } else if (fopen_s(&in, opt.input.c_str(), "rb") != 0 || !in) {
        std::fprintf(stderr, "cannot open %s\n", opt.input.c_str());
        return 1;
    }
    // Input frame bytes; ratios use the coded bytes (I420: the same; BGRA: 3 per pixel, no alpha).
    const size_t fs = opt.rgb ? size_t(opt.w) * opt.h * 4 : size_t(opt.w) * opt.h * 3 / 2;
    std::vector<uint8_t> cur(fs), prev(fs);
    std::vector<uint8_t> unique;
    const Clock::time_point start = Clock::now();
    for (long n = 0; opt.max_frames == 0 || n < opt.max_frames; ++n) {
        const size_t got = std::fread(cur.data(), 1, fs, in);
        if (got == 0) break;
        if (got != fs) {
            std::fprintf(stderr, "warning: ignoring partial frame at the end\n");
            break;
        }
        const bool dup = !unique.empty() && std::memcmp(cur.data(), prev.data(), fs) == 0;
        unique.push_back(dup ? 0 : 1);
        for (auto& r : runs)
            if (!r->frame(cur.data(), dup, opt.decode, opt.verify, n)) return 1;
        std::swap(cur, prev);
        if (n % 50 == 0) std::fprintf(stderr, "\r  frame %ld", n);
    }
    if (in != stdin) std::fclose(in);
    const double wall_s = ms_since(start) / 1000.0;
    const CpuClock clk_after = cpu_clock();
    std::fprintf(stderr, "\r  %zu frames in %.1f s\n", unique.size(), wall_s);
    if (unique.empty()) {
        std::fprintf(stderr, "no frames read\n");
        return 1;
    }

    size_t n_unique = 0;
    for (uint8_t u : unique) n_unique += u;
    const double raw = opt.rgb ? 3.0 * opt.w * opt.h : double(fs);

    std::printf("## RCV1 benchmark\n\n");
    std::printf("- Corpus: `%s`, %dx%d %s @ %.3g fps, %zu frames: %zu real, %zu identical to the previous "
                "frame (coded as DUP by RCV1)\n",
                opt.input == "-" ? "stdin" : opt.input.c_str(), opt.w, opt.h,
                opt.rgb ? "BGRA -> GBR (lossless RGB)" : "YUV 4:2:0", opt.fps, unique.size(), n_unique,
                unique.size() - n_unique);
    std::printf("- Machine: %s, %lu logical CPUs, %s; Windows-reported clock %lu/%lu MHz before, %lu MHz after\n",
                cpu_brand().c_str(), logical, power_state().c_str(), clk_before.current, clk_before.max,
                clk_after.current);
    std::printf("- Build: %s, MSVC %d; bench thread %s, priority HIGHEST; encoder workers pinned to further "
                "physical cores first (T = encoder and decoder threads)\n",
#ifdef NDEBUG
                "Release",
#else
                "DEBUG (timings not meaningful)",
#endif
                _MSC_FULL_VER, cpu >= 0 ? ("pinned to logical CPU " + std::to_string(cpu)).c_str() : "not pinned");
    std::printf("- Ratios are raw frame size (%.0f bytes) / packet size over real frames: frames a codec coded in "
                "full if it stores duplicates as marker packets (FRAPS, RCV1), otherwise frames that differ from "
                "the previous one. \"Overall\" includes duplicate frames as each codec stores them\n\n",
                raw);

    std::printf("| Codec | Real frames | Real-frame ratio (median) | Worst | Best | Aggregate | Overall | Avg MB/s "
                "| Peak MB/s | Encode ms p50 / p99 / max | Decode ms p50 / p99 |\n");
    std::printf("|---|---|---|---|---|---|---|---|---|---|---|\n");
    auto row = [&](const Series& s) {
        if (s.bytes.size() < unique.size())
            std::fprintf(stderr, "warning: %s has %zu sizes for %zu frames\n", s.name.c_str(), s.bytes.size(),
                         unique.size());
        const Metrics m = compute(s, unique, raw, opt.fps);
        char enc[64] = "-", dec[64] = "-";
        if (m.enc.n) std::snprintf(enc, sizeof(enc), "%.2f / %.2f / %.2f", m.enc.p50, m.enc.p99, m.enc.max);
        else if (s.external_enc_ms >= 0) std::snprintf(enc, sizeof(enc), "~%.2f (mean, ffmpeg)", s.external_enc_ms);
        if (m.dec.n) std::snprintf(dec, sizeof(dec), "%.2f / %.2f", m.dec.p50, m.dec.p99);
        std::printf("| %s | %zu | **%.3f** | %.3f | %.3f | %.3f | %.3f | %.1f | %.1f | %s | %s |\n", s.name.c_str(),
                    m.real, m.median, m.worst, m.best, m.aggregate, m.overall, m.avg_mbs, m.peak_mbs, enc, dec);
    };
    for (const auto& r : runs) row(r->series);
    for (const Series& s : externals) row(s);

    print_frame_types(runs);
    print_stage_table(runs);

    long failures = 0;
    std::printf("\nRound trip:");
    for (const auto& r : runs) {
        failures += r->series.verify_failures;
        std::printf(" %s %s;", r->series.name.c_str(),
                    !opt.verify ? "not verified"
                    : r->series.verify_failures ? "FAILED"
                                                : "bit-exact");
    }
    std::printf("\n");

    if (!opt.csv.empty()) {
        FILE* csv = nullptr;
        if (fopen_s(&csv, opt.csv.c_str(), "wb") == 0 && csv) {
            std::fprintf(csv, "frame,codec,real,bytes,enc_ms,dec_ms\n");
            for (const auto& r : runs)
                for (size_t i = 0; i < r->series.bytes.size(); ++i)
                    std::fprintf(csv, "%zu,%s,%d,%u,%.4f,%.4f\n", i, r->series.name.c_str(), unique[i],
                                 r->series.bytes[i], r->series.enc_ms[i],
                                 i < r->series.dec_ms.size() ? r->series.dec_ms[i] : 0.0);
            for (const Series& s : externals)
                for (size_t i = 0; i < s.bytes.size() && i < unique.size(); ++i)
                    std::fprintf(csv, "%zu,%s,%d,%u,,\n", i, s.name.c_str(), unique[i], s.bytes[i]);
            std::fclose(csv);
        } else {
            std::fprintf(stderr, "cannot write %s\n", opt.csv.c_str());
        }
    }
    return failures ? 1 : 0;
}
