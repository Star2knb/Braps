// A11: no heap allocation inside rcv_encode_frame, rcv_encode_duplicate or rcv_decode_frame
// (codec plan §0 rule 3, §11.4). Every global operator new/delete of this test binary is replaced
// by a counting version; the codec allocates only through operator new (aligned_buffer.h), and is
// linked statically. Debug builds also count every debug-CRT heap allocation (malloc,
// _aligned_malloc, realloc, from any module on the CRT) through _CrtSetAllocHook.
#include <atomic>
#include <cstdlib>
#include <malloc.h>
#include <new>
#include <vector>

#if defined(_DEBUG)
#include <crtdbg.h>
#define NOMINMAX
#include <windows.h>
#endif

#include "test_util.h"

namespace {

std::atomic<long long> g_news{0};

void* counted(size_t n) {
    g_news.fetch_add(1, std::memory_order_relaxed);
    return std::malloc(n ? n : 1);
}
void* counted_aligned(size_t n, std::align_val_t a) {
    g_news.fetch_add(1, std::memory_order_relaxed);
    return _aligned_malloc(n ? n : 1, size_t(a));
}

}  // namespace

void* operator new(size_t n) {
    if (void* p = counted(n)) return p;
    throw std::bad_alloc();
}
void* operator new[](size_t n) {
    if (void* p = counted(n)) return p;
    throw std::bad_alloc();
}
void* operator new(size_t n, const std::nothrow_t&) noexcept { return counted(n); }
void* operator new[](size_t n, const std::nothrow_t&) noexcept { return counted(n); }
void* operator new(size_t n, std::align_val_t a) {
    if (void* p = counted_aligned(n, a)) return p;
    throw std::bad_alloc();
}
void* operator new[](size_t n, std::align_val_t a) {
    if (void* p = counted_aligned(n, a)) return p;
    throw std::bad_alloc();
}
void* operator new(size_t n, std::align_val_t a, const std::nothrow_t&) noexcept { return counted_aligned(n, a); }
void* operator new[](size_t n, std::align_val_t a, const std::nothrow_t&) noexcept { return counted_aligned(n, a); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { _aligned_free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { _aligned_free(p); }
void operator delete(void* p, size_t, std::align_val_t) noexcept { _aligned_free(p); }
void operator delete[](void* p, size_t, std::align_val_t) noexcept { _aligned_free(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { _aligned_free(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { _aligned_free(p); }

using namespace tu;

namespace {

using Bytes = std::vector<uint8_t>;

#if defined(_DEBUG)
std::atomic<long long> g_crt{0};
// The first few allocations seen, for the failure message.
struct Seen {
    size_t size;
    int block_type;
    unsigned long thread;
};
Seen g_seen[8];
int __cdecl crt_hook(int type, void*, size_t size, int block_type, long, const unsigned char*, int) {
    if (type == _HOOK_ALLOC || type == _HOOK_REALLOC) {
        const long long k = g_crt.fetch_add(1, std::memory_order_relaxed);
        if (k < 8) g_seen[k] = {size, block_type, GetCurrentThreadId()};
    }
    return 1;  // allow the allocation
}
#endif

// Counts allocations on every thread (the codec's workers too) between construction and count().
class Watch {
public:
    Watch() {
#if defined(_DEBUG)
        g_crt.store(0);  // nothing is allocating now: the codec's workers are idle
        prev_ = _CrtSetAllocHook(crt_hook);
#endif
        new0_ = g_news.load();
    }
    ~Watch() {
#if defined(_DEBUG)
        _CrtSetAllocHook(prev_);
#endif
    }
    // Number of allocations so far; if any, describes the first few on stderr.
    long long count() const {
        const long long n = g_news.load() - new0_;
        if (n > 0) std::fprintf(stderr, "  %lld operator new call(s)\n", n);
#if defined(_DEBUG)
        const long long c = g_crt.load();
        for (long long k = 0; k < c && k < 8; ++k)  // block type 2 is the CRT's own (_CRT_BLOCK)
            std::fprintf(stderr, "  CRT allocation %lld: %zu bytes, block type %d, thread %lu (caller %lu)\n", k,
                         g_seen[k].size, g_seen[k].block_type, g_seen[k].thread, GetCurrentThreadId());
        return n + c;
#else
        return n;
#endif
    }

private:
    long long new0_ = 0;
#if defined(_DEBUG)
    _CRT_ALLOC_HOOK prev_ = nullptr;
#endif
};

// Encodes a frame sequence through one encoder, then a duplicate and two rejected calls. Returns
// the number of allocations inside those calls (-1 if a call failed); packets go to *out.
long long encode_counted(const rcv_encoder_config& cfg, const std::vector<rcv_frame_in>& ins,
                         const std::vector<rcv_encode_params>& params, std::vector<Bytes>* out) {
    rcv_encoder* enc = nullptr;
    if (rcv_encoder_create(&cfg, &enc) != RCV_OK) return -1;
    const size_t cap = rcv_max_packet_size(&cfg);
    std::vector<Bytes> buf(ins.size() + 1, Bytes(cap));
    std::vector<uint32_t> len(ins.size() + 1, 0);
    rcv_encode_params bad{};
    bad.near_level = 9;
    bool ok = true;
    long long n = 0;
    {
        const Watch w;
        rcv_frame_info info{};
        for (size_t i = 0; i < ins.size(); ++i) {
            if (rcv_encode_frame(enc, &ins[i], &params[i], buf[i].data(), cap, &info) != RCV_OK) ok = false;
            len[i] = info.packet_size;
        }
        if (rcv_encode_duplicate(enc, buf.back().data(), cap, &info) != RCV_OK) ok = false;
        len.back() = info.packet_size;
        if (rcv_encode_frame(enc, &ins[0], &bad, buf[0].data(), cap, nullptr) != RCV_ERR_INVALID_ARG) ok = false;
        if (rcv_encode_frame(enc, &ins[0], nullptr, buf[0].data(), cap - 1, nullptr) != RCV_ERR_BUFFER_TOO_SMALL)
            ok = false;
        n = w.count();
    }
    rcv_encoder_destroy(enc);
    if (!ok) return -1;
    for (size_t i = 0; i < buf.size(); ++i) {
        buf[i].resize(len[i]);
        out->push_back(std::move(buf[i]));
    }
    return n;
}

// Decodes the packets (all must succeed), then corrupted copies, a DUP after a reset, and the
// first packet again. Returns the allocations inside those calls (-1 if a valid packet failed).
long long decode_counted(const rcv_encoder_config& cfg, int threads, const std::vector<Bytes>& pkts,
                         rcv_output_layout layout) {
    uint8_t seq[32];
    rcv_write_sequence_header(&cfg, seq);
    rcv_decoder* d = nullptr;
    if (rcv_decoder_create(seq, uint8_t(threads), &d) != RCV_OK) return -1;
    const int w = cfg.coded_width, h = cfg.coded_height;
    Bytes p0, p1, p2;
    int32_t stride[3] = {};
    if (layout == RCV_OUT_BGRA) {
        p0.resize(size_t(4) * w * h);
        stride[0] = 4 * w;
    } else if (layout == RCV_OUT_NV12) {
        p0.resize(size_t(w) * h);
        p1.resize(size_t(w) * (h / 2));
        stride[0] = stride[1] = w;
    } else {
        p0.resize(size_t(w) * h);
        p1.resize(size_t(w / 2) * (h / 2));
        p2.resize(size_t(w / 2) * (h / 2));
        stride[0] = w;
        stride[1] = stride[2] = w / 2;
    }
    uint8_t* planes[3] = {p0.data(), p1.empty() ? nullptr : p1.data(), p2.empty() ? nullptr : p2.data()};
    std::vector<Bytes> bad;
    tf::Rng rng(11);
    for (const Bytes& p : pkts) {
        Bytes m = p;
        m[rng.below(uint32_t(m.size()))] ^= 0x5A;
        bad.push_back(m);
        m.resize(m.size() / 2);
        bad.push_back(m);
    }
    const Bytes dup = pkts.back();  // the sequences end with rcv_encode_duplicate
    bool ok = true;
    long long n = 0;
    {
        const Watch wt;
        rcv_frame_info info{};
        for (const Bytes& p : pkts) {
            if (rcv_decode_frame(d, p.data(), p.size(), layout, planes, stride, &info) != RCV_OK) ok = false;
            if (rcv_decode_frame(d, dup.data(), dup.size(), layout, nullptr, nullptr, nullptr) != RCV_OK) ok = false;
        }
        for (const Bytes& p : bad) rcv_decode_frame(d, p.data(), p.size(), layout, planes, stride, &info);
        rcv_decoder_reset(d);
        if (rcv_decode_frame(d, dup.data(), dup.size(), layout, planes, stride, nullptr) != RCV_ERR_NO_REFERENCE)
            ok = false;
        if (rcv_decode_frame(d, pkts[0].data(), pkts[0].size(), layout, planes, stride, &info) != RCV_OK) ok = false;
        n = wt.count();
    }
    rcv_decoder_destroy(d);
    return ok ? n : -1;
}

Yuv changed(const Yuv& f, int bx, int by, int delta) {
    Yuv o = f;
    for (int y = by * 16; y < by * 16 + 16 && y < f.h; ++y)
        for (int x = bx * 16; x < bx * 16 + 16 && x < f.w; ++x) o.at(0, x, y) = uint8_t(o.at(0, x, y) + delta);
    return o;
}

// NV12 copy of an I420 frame: Y plane, then interleaved Cb/Cr.
struct Nv12 {
    Bytes y, uv;
    int w = 0;
};
Nv12 to_nv12(const Yuv& f) {
    Nv12 o;
    o.w = f.w;
    o.y.resize(size_t(f.w) * f.h);
    o.uv.resize(size_t(f.w) * (f.h / 2));
    for (int y = 0; y < f.h; ++y)
        for (int x = 0; x < f.w; ++x) o.y[size_t(y) * f.w + x] = f.at(0, x, y);
    for (int y = 0; y < f.h / 2; ++y)
        for (int x = 0; x < f.w / 2; ++x) {
            o.uv[size_t(y) * f.w + 2 * x] = f.at(1, x, y);
            o.uv[size_t(y) * f.w + 2 * x + 1] = f.at(2, x, y);
        }
    return o;
}

}  // namespace

TEST_CASE("alloc (A11): the replacement operator new is the one in use") {
    const long long before = g_news.load();
    int* p = new int(5);
    delete p;
    void* q = ::operator new(64, std::align_val_t(64), std::nothrow);
    ::operator delete(q, std::align_val_t(64));
    CHECK(g_news.load() - before == 2);
#if defined(_DEBUG)
    const Watch w;  // the CRT hook sees malloc too
    void* m = std::malloc(10);
    std::free(m);
    CHECK(g_crt.load() == 1);
#endif
}

TEST_CASE("alloc (A11): YUV420 encode and decode make no heap allocation") {
    const int w = 96, h = 64;
    const Yuv f0 = make_yuv(w, h, Content::Natural, 1);
    const Yuv f1 = changed(f0, 2, 1, 7);
    const Yuv f4 = changed(f1, 0, 3, 20);
    const Yuv f5 = changed(f4, 5, 0, 40);
    const Yuv noise = make_yuv(w, h, Content::Noise, 2);
    const Yuv zero = make_yuv(w, h, Content::Zero, 3);
    // I, P, DUP, forced I, near P/I frames, noise (RAW chunks), flat (SINGLE chunks).
    const std::vector<const Yuv*> frames = {&f0, &f1, &f1, &f1, &f4, &f5, &noise, &noise, &zero};
    std::vector<rcv_encode_params> params(frames.size());
    params[3].force_keyframe = 1;
    params[4].near_level = 1;
    params[5].near_level = 2;
    params[6].near_level = 3;

    std::vector<Nv12> nv;
    for (const Yuv* f : frames) nv.push_back(to_nv12(*f));
    for (int layout = 0; layout < 2; ++layout)
        for (int threads : {1, 4}) {
            rcv_encoder_config cfg = config_for(w, h, 1, 4);
            cfg.num_threads = uint8_t(threads);
            cfg.enable_crc = uint8_t(threads == 4);
            cfg.input_layout = layout ? RCV_IN_NV12 : RCV_IN_I420;
            std::vector<rcv_frame_in> ins;
            for (size_t i = 0; i < frames.size(); ++i) {
                rcv_frame_in in = frame_in(*frames[i]);
                if (layout) {
                    in = rcv_frame_in{};
                    in.plane[0] = nv[i].y.data();
                    in.plane[1] = nv[i].uv.data();
                    in.stride[0] = in.stride[1] = w;
                }
                ins.push_back(in);
            }
            std::vector<Bytes> pkts;
            CHECK(encode_counted(cfg, ins, params, &pkts) == 0);
            REQUIRE(pkts.size() == frames.size() + 1);
            CHECK(pkts[2].size() == RCV_DUP_PACKET_SIZE);
            CHECK(pkts[1][5] == 2);  // P
            for (rcv_output_layout out : {RCV_OUT_I420, RCV_OUT_NV12})
                CHECK(decode_counted(cfg, threads, pkts, out) == 0);
        }
}

TEST_CASE("alloc (A11): GBR encode and decode make no heap allocation") {
    const int w = 50, h = 34;
    auto bgra = [&](uint64_t seed, int kind) {
        tf::Rng rng(seed);
        Bytes px(size_t(4) * w * h);
        for (size_t i = 0; i < px.size(); ++i) px[i] = kind == 0 ? uint8_t(i % 251 / 3) : kind == 1 ? rng.byte() : 0;
        return px;
    };
    const Bytes a = bgra(1, 0), noise = bgra(2, 1), zero = bgra(3, 2);
    Bytes b = a;
    for (int y = 0; y < 16; ++y) b[size_t(y) * 4 * w + 8] ^= 0x40;  // one block column changes
    const std::vector<const Bytes*> frames = {&a, &b, &b, &b, &noise, &zero};
    std::vector<rcv_encode_params> params(frames.size());
    params[3].force_keyframe = 1;
    for (int threads : {1, 4}) {
        rcv_encoder_config cfg = config_for(w, h, 1, 2);
        cfg.format = RCV_FMT_GBR;
        cfg.input_layout = RCV_IN_BGRA;
        cfg.num_threads = uint8_t(threads);
        cfg.enable_crc = uint8_t(threads == 1);
        std::vector<rcv_frame_in> ins;
        for (const Bytes* f : frames) {
            rcv_frame_in in{};
            in.plane[0] = f->data();
            in.stride[0] = 4 * w;
            ins.push_back(in);
        }
        std::vector<Bytes> pkts;
        CHECK(encode_counted(cfg, ins, params, &pkts) == 0);
        REQUIRE(pkts.size() == frames.size() + 1);
        CHECK(pkts[1][5] == 2);  // P
        CHECK(pkts[2].size() == RCV_DUP_PACKET_SIZE);
        CHECK(decode_counted(cfg, threads, pkts, RCV_OUT_BGRA) == 0);
    }
}
