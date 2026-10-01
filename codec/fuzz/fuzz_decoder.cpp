// libFuzzer target for the decoder (codec plan §11.4, acceptance A12). Built by the clang-debug
// preset with ASan + UBSan: build\clang-debug\codec\rcv_fuzz.exe corpus_dir seed_dir
//
// Input: a stream, as a container would deliver it.
//   bytes 0-31  sequence header (§6.1)
//   byte 32     options: bit 0 = two decoder threads; bits 1-2 = output (0 none, 1 I420 or BGRA,
//               2 NV12 or BGRA, 3 I420 or BGRA with padded strides)
//   then up to 16 packets, each a little-endian u32 length followed by that many bytes (a length
//   past the end of the input takes what is left).
// Every packet and output plane is copied into its own exactly-sized heap block, so ASan reports
// any read or write outside it.
#include <cstdint>
#include <cstring>
#include <memory>

#include "rcv/rcv.h"

namespace {

constexpr size_t kMaxSamples = 256 * 256;  // keeps each input fast; the code paths don't depend on size
constexpr int kMaxPackets = 16;

uint32_t get_u32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

struct Output {
    std::unique_ptr<uint8_t[]> plane[3];
    uint8_t* ptr[3] = {};
    int32_t stride[3] = {};
    rcv_output_layout layout = RCV_OUT_I420;

    void alloc(int p, int32_t stride_bytes, int rows) {
        stride[p] = stride_bytes;
        plane[p].reset(new uint8_t[size_t(stride_bytes) * size_t(rows)]);
        ptr[p] = plane[p].get();
    }
};

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 33) return -1;
    rcv_sequence_info info{};
    uint8_t seq[32];
    std::memcpy(seq, data, 32);
    if (rcv_parse_sequence_header(seq, &info) != RCV_OK) {
        rcv_decoder* d = nullptr;
        if (rcv_decoder_create(seq, 1, &d) == RCV_OK) __builtin_trap();  // parse and create must agree
        return 0;
    }
    const int w = info.coded_width, h = info.coded_height;
    if (size_t(w) * size_t(h) > kMaxSamples) return -1;

    const uint8_t opt = data[32];
    rcv_decoder* d = nullptr;
    if (rcv_decoder_create(seq, uint8_t(1 + (opt & 1)), &d) != RCV_OK) __builtin_trap();

    Output out;
    const int mode = (opt >> 1) & 3;
    const int pad = mode == 3 ? 7 : 0;
    if (mode != 0) {
        if (info.format == RCV_FMT_GBR) {
            out.layout = RCV_OUT_BGRA;
            out.alloc(0, 4 * w + pad, h);
        } else if (mode == 2) {
            out.layout = RCV_OUT_NV12;
            out.alloc(0, w, h);
            out.alloc(1, w, h / 2);
        } else {
            out.layout = RCV_OUT_I420;
            out.alloc(0, w + pad, h);
            out.alloc(1, w / 2 + pad, h / 2);
            out.alloc(2, w / 2 + pad, h / 2);
        }
    }

    size_t pos = 33;
    for (int n = 0; n < kMaxPackets && pos + 4 <= size; ++n) {
        size_t len = get_u32(data + pos);
        pos += 4;
        if (len > size - pos) len = size - pos;
        std::unique_ptr<uint8_t[]> pkt(new uint8_t[len ? len : 1]);
        std::memcpy(pkt.get(), data + pos, len);
        pos += len;
        rcv_frame_info fi{};
        const rcv_status st =
            rcv_decode_frame(d, pkt.get(), len, out.layout, mode ? out.ptr : nullptr, mode ? out.stride : nullptr, &fi);
        if (st != RCV_OK && st != RCV_ERR_BITSTREAM && st != RCV_ERR_NO_REFERENCE) __builtin_trap();
        if (st == RCV_OK && fi.packet_size != len) __builtin_trap();
    }
    rcv_decoder_destroy(d);
    return 0;
}
