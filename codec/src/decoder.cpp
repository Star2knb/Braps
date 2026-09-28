// RCV1 decoder (codec plan §5.7, §6, Appendix A.2).
// Scope so far: single-threaded; I-frames and DUP; I420 / NV12 output.
// Entropy decoding and reconstruction run fused in one serial pass per chunk (the decoder is
// inherently serial along a row, §5.4).
// The decoder must never crash on bad input: every length is checked before use (§6.5).
#include <stdlib.h>  // _byteswap_uint64

#include <chrono>
#include <cstring>
#include <new>

#include "aligned_buffer.h"
#include "crc32c.h"
#include "format.h"
#include "huffman.h"
#include "predict.h"
#include "rcv/rcv.h"

using namespace rcv;

struct rcv_decoder {
    SeqHeader seq;
    Geometry geo;              // plane sizes; slice bounds are recomputed per frame
    uint8_t* ref_mem;
    uint8_t* ref_plane[3];
    ptrdiff_t ref_stride[3];
    HuffDecTable lut[2];       // two, for decoding a pair of chunks in lockstep
    bool has_ref;
};

namespace {

using Clock = std::chrono::steady_clock;

uint32_t elapsed_us(Clock::time_point t0) {
    return uint32_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count());
}

// Symbol sources for the fused reconstruct loop.
struct SingleSource {
    uint8_t sym;
    uint8_t operator()() const { return sym; }
};

struct RawSource {
    const uint8_t* p;
    uint8_t operator()() { return *p++; }
};

// MSB-first Huffman source with a 12-bit LUT. Bits are left-aligned in `buf`; `count` of them are
// real. Refills 8 bytes at a time while at least 8 bytes of the chunk remain, byte by byte after
// that, and never reads past the chunk. Any bits below `count` are the stream's next bits (or 0),
// so re-reading them is harmless.
//
// Past the end of the chunk the buffer simply supplies zero bits and `count` goes negative. That
// is only possible once every byte has been loaded (a refill always tops up to > 12 bits while
// bytes remain, and a code is <= 12 bits), and it is checked once after the chunk (overrun()),
// so the per-symbol path has no bounds branch.
struct HuffSource {
    const HuffDecTable* lut;
    const uint8_t* start;
    const uint8_t* p;
    const uint8_t* end;
    uint64_t buf = 0;
    int count = 0;

    void refill() {
        if (end - p >= 8) {
            uint64_t v;
            std::memcpy(&v, p, 8);
            buf |= _byteswap_uint64(v) >> count;  // 0 <= count < 12 here
            p += (63 - count) >> 3;
            count |= 56;
        } else {
            while (count <= 56 && p < end) {  // count >= 0 whenever bytes remain
                buf |= uint64_t(*p++) << (56 - count);
                count += 8;
            }
        }
    }
    uint8_t operator()() {
        if (count < kLutBits) refill();
        const unsigned idx = unsigned(buf >> 52);
        const unsigned len = lut->len[idx];
        buf <<= len;
        count -= int(len);
        return lut->sym[idx];
    }
    bool overrun() const { return count < 0; }  // a symbol needed bits past the end of the chunk
    size_t bytes_used() const { return (size_t(p - start) * 8 - size_t(count) + 7) / 8; }
};

// Row reconstruction (inverse of §5.4 / Appendix A.2). The first row of a slice starts from 128
// and uses the left neighbour; other rows start from the sample above.
template <class Source>
inline void row_first(uint8_t* x, int width, Source& next) {
    uint8_t a = 128;
    for (int i = 0; i < width; ++i) {
        a = uint8_t(a + next());
        x[i] = a;
    }
}

template <class Source>
inline void row_rest(uint8_t* x, const uint8_t* up, int width, int predictor, Source& next) {
    int a = uint8_t(up[0] + next());
    x[0] = uint8_t(a);
    if (predictor == kPredMed) {
        int c = up[0];
        for (int i = 1; i < width; ++i) {
            const int b = up[i];
            a = (predict_med_clamp(a, b, c) + next()) & 0xFF;
            x[i] = uint8_t(a);
            c = b;
        }
    } else {
        for (int i = 1; i < width; ++i) {
            a = (a + next()) & 0xFF;
            x[i] = uint8_t(a);
        }
    }
}

// Rebuilds rows [r0, r1) of a plane (one slice) from a symbol source.
// The source is copied to a local first: pixel stores go through uint8_t*, which may alias
// anything, so a source reached by reference would be reloaded from memory on every symbol.
template <class Source>
void reconstruct(uint8_t* plane, ptrdiff_t stride, int width, int r0, int r1, int predictor, Source& src) {
    Source next = src;
    row_first(plane + r0 * stride, width, next);
    for (int j = r0 + 1; j < r1; ++j) row_rest(plane + j * stride, plane + (j - 1) * stride, width, predictor, next);
    src = next;
}

// Two slices of one plane rebuilt in lockstep. Each slice is a serial chain (Huffman bit
// position, then left neighbour), but the two chains are independent, so interleaving them
// lets the CPU overlap them on one core. Rows beyond the shorter slice are finished alone.
void reconstruct_pair(uint8_t* plane, ptrdiff_t stride, int width, int r0a, int r1a, int r0b, int r1b,
                      int predictor, HuffSource& src_a, HuffSource& src_b) {
    HuffSource sa = src_a, sb = src_b;  // locals, kept in registers (see reconstruct)
    const int rows = (r1a - r0a) < (r1b - r0b) ? (r1a - r0a) : (r1b - r0b);
    {
        uint8_t* xa = plane + r0a * stride;
        uint8_t* xb = plane + r0b * stride;
        uint8_t a = 128, b = 128;
        for (int i = 0; i < width; ++i) {
            a = uint8_t(a + sa());
            xa[i] = a;
            b = uint8_t(b + sb());
            xb[i] = b;
        }
    }
    for (int k = 1; k < rows; ++k) {
        uint8_t* xa = plane + (r0a + k) * stride;
        uint8_t* xb = plane + (r0b + k) * stride;
        const uint8_t* ua = xa - stride;
        const uint8_t* ub = xb - stride;
        int a = uint8_t(ua[0] + sa());
        int b = uint8_t(ub[0] + sb());
        xa[0] = uint8_t(a);
        xb[0] = uint8_t(b);
        if (predictor == kPredMed) {
            int ca = ua[0], cb = ub[0];
            for (int i = 1; i < width; ++i) {
                const int ba = ua[i], bb = ub[i];
                a = (predict_med_clamp(a, ba, ca) + sa()) & 0xFF;
                b = (predict_med_clamp(b, bb, cb) + sb()) & 0xFF;
                xa[i] = uint8_t(a);
                xb[i] = uint8_t(b);
                ca = ba;
                cb = bb;
            }
        } else {
            for (int i = 1; i < width; ++i) {
                a = (a + sa()) & 0xFF;
                b = (b + sb()) & 0xFF;
                xa[i] = uint8_t(a);
                xb[i] = uint8_t(b);
            }
        }
    }
    for (int j = r0a + rows; j < r1a; ++j)
        row_rest(plane + j * stride, plane + (j - 1) * stride, width, predictor, sa);
    for (int j = r0b + rows; j < r1b; ++j)
        row_rest(plane + j * stride, plane + (j - 1) * stride, width, predictor, sb);
    src_a = sa;
    src_b = sb;
}

bool chunk_header_ok(const uint8_t* c, size_t size) {
    return size >= kChunkHeaderSize && c[2] == 0 && c[3] == 0;
}

// Validates a HUFFMAN chunk's header and code lengths and sets up its bit source.
rcv_status open_huffman(const uint8_t* c, size_t size, size_t count, HuffDecTable* lut, HuffSource* src) {
    if (c[1] != 0 || count == 0 || size < kChunkHeaderSize + kHuffTableSize) return RCV_ERR_BITSTREAM;
    uint8_t len[256];
    huff_unpack_lengths(c + kChunkHeaderSize, len);
    if (!huff_build_decode_lut(len, lut)) return RCV_ERR_BITSTREAM;
    const uint8_t* bits = c + kChunkHeaderSize + kHuffTableSize;
    *src = HuffSource{lut, bits, bits, c + size};
    return RCV_OK;
}

// After decoding: no symbol ran past the chunk, and the chunk is exactly the bitstream padded to a
// multiple of 4 bytes.
rcv_status close_huffman(const HuffSource& src, size_t size) {
    if (src.overrun()) return RCV_ERR_BITSTREAM;
    if (size != kChunkHeaderSize + kHuffTableSize + align4(src.bytes_used())) return RCV_ERR_BITSTREAM;
    return RCV_OK;
}

// Decodes two neighbouring HUFFMAN chunks (slices s and s+1 of one plane) in lockstep.
rcv_status decode_huffman_pair(rcv_decoder* d, const uint8_t* ca, size_t size_a, const uint8_t* cb, size_t size_b,
                               int plane, const Geometry& g, int s, int predictor) {
    if (!chunk_header_ok(ca, size_a) || !chunk_header_ok(cb, size_b)) return RCV_ERR_BITSTREAM;
    HuffSource a, b;
    rcv_status st = open_huffman(ca, size_a, g.chunk_samples(plane, s), &d->lut[0], &a);
    if (st != RCV_OK) return st;
    st = open_huffman(cb, size_b, g.chunk_samples(plane, s + 1), &d->lut[1], &b);
    if (st != RCV_OK) return st;
    reconstruct_pair(d->ref_plane[plane], d->ref_stride[plane], g.plane_w[plane], g.slice_row(plane, s),
                     g.slice_row(plane, s + 1), g.slice_row(plane, s + 1), g.slice_row(plane, s + 2), predictor, a, b);
    st = close_huffman(a, size_a);
    if (st != RCV_OK) return st;
    return close_huffman(b, size_b);
}

// Decodes one chunk straight into the reference plane. The sample count comes from geometry,
// never from the packet.
rcv_status decode_chunk(rcv_decoder* d, const uint8_t* c, size_t size, int plane, const Geometry& g, int slice,
                        int predictor) {
    if (!chunk_header_ok(c, size)) return RCV_ERR_BITSTREAM;
    const size_t count = g.chunk_samples(plane, slice);
    uint8_t* dst = d->ref_plane[plane];
    const ptrdiff_t stride = d->ref_stride[plane];
    const int w = g.plane_w[plane], r0 = g.slice_row(plane, slice), r1 = g.slice_row(plane, slice + 1);
    switch (c[0]) {
    case kChunkHuffman: {
        HuffSource src;
        const rcv_status st = open_huffman(c, size, count, &d->lut[0], &src);
        if (st != RCV_OK) return st;
        reconstruct(dst, stride, w, r0, r1, predictor, src);
        return close_huffman(src, size);
    }
    case kChunkSingle: {
        if (count == 0 || size != kChunkHeaderSize) return RCV_ERR_BITSTREAM;
        SingleSource src{c[1]};
        reconstruct(dst, stride, w, r0, r1, predictor, src);
        return RCV_OK;
    }
    case kChunkRaw: {
        if (c[1] != 0 || count == 0 || size != kChunkHeaderSize + align4(count)) return RCV_ERR_BITSTREAM;
        RawSource src{c + kChunkHeaderSize};
        reconstruct(dst, stride, w, r0, r1, predictor, src);
        return RCV_OK;
    }
    case kChunkEmpty:
        if (c[1] != 0 || count != 0 || size != kChunkHeaderSize) return RCV_ERR_BITSTREAM;
        return RCV_OK;
    default:
        return RCV_ERR_BITSTREAM;
    }
}

rcv_status decode_i_frame(rcv_decoder* d, const uint8_t* pkt, size_t size) {
    if (size < kFrameHeaderSize) return RCV_ERR_BITSTREAM;
    const uint16_t flags = get_u16(pkt + 6);
    const uint8_t format = pkt[8];
    const uint8_t near_level = pkt[9];
    const uint8_t predictor = pkt[10];
    const uint8_t slices = pkt[11];
    const uint16_t width = get_u16(pkt + 12);
    const uint16_t height = get_u16(pkt + 14);
    const uint8_t block_log2 = pkt[16];
    const uint32_t payload_size = get_u32(pkt + 24);
    const uint32_t crc = get_u32(pkt + 28);

    if (flags & ~kKnownFlags) return RCV_ERR_BITSTREAM;
    if (format > RCV_FMT_GBR || format != d->seq.format) return RCV_ERR_BITSTREAM;
    if (near_level > 3 || (near_level != 0) != ((flags & kFlagNear) != 0)) return RCV_ERR_BITSTREAM;
    if (near_level != 0 && format == RCV_FMT_GBR) return RCV_ERR_BITSTREAM;
    if (predictor > kPredMed || block_log2 != kBlockLog2) return RCV_ERR_BITSTREAM;
    if (width != d->seq.coded_w || height != d->seq.coded_h) return RCV_ERR_BITSTREAM;
    if (slices == 0) return RCV_ERR_BITSTREAM;
    Geometry g;
    if (!init_geometry(&g, format, width, height, slices)) return RCV_ERR_BITSTREAM;
    if (payload_size != size - kFrameHeaderSize) return RCV_ERR_BITSTREAM;
    const uint8_t* payload = pkt + kFrameHeaderSize;
    if (flags & kFlagCrc) {
        if (crc32c(payload, payload_size) != crc) return RCV_ERR_BITSTREAM;
    } else if (crc != 0) {
        return RCV_ERR_BITSTREAM;
    }
    if (near_level != 0) return RCV_ERR_UNSUPPORTED;  // M7

    const size_t num_chunks = size_t(3) * slices;
    const size_t dir_size = num_chunks * 4;
    if (dir_size > payload_size) return RCV_ERR_BITSTREAM;
    uint64_t sum = 0;
    for (size_t k = 0; k < num_chunks; ++k) {
        const uint32_t cs = get_u32(payload + 4 * k);
        if (cs < kChunkHeaderSize || (cs & 3)) return RCV_ERR_BITSTREAM;
        sum += cs;
    }
    if (sum != payload_size - dir_size) return RCV_ERR_BITSTREAM;

    // From here on the reference is overwritten; it is only valid again on success.
    d->has_ref = false;
    const uint8_t* chunk = payload + dir_size;
    for (int pl = 0; pl < 3; ++pl) {
        auto chunk_size = [&](int s) { return size_t(get_u32(payload + 4 * (size_t(pl) * slices + size_t(s)))); };
        for (int s = 0; s < slices;) {
            const size_t cs = chunk_size(s);
            rcv_status st;
            if (s + 1 < slices && chunk[0] == kChunkHuffman && chunk[cs] == kChunkHuffman) {
                const size_t cs2 = chunk_size(s + 1);  // sizes are >= 4 (checked above)
                st = decode_huffman_pair(d, chunk, cs, chunk + cs, cs2, pl, g, s, predictor);
                chunk += cs + cs2;
                s += 2;
            } else {
                st = decode_chunk(d, chunk, cs, pl, g, s, predictor);
                chunk += cs;
                s += 1;
            }
            if (st != RCV_OK) return st;
        }
    }
    d->has_ref = true;
    return RCV_OK;
}

rcv_status check_output(const rcv_decoder* d, rcv_output_layout layout, uint8_t* const plane[3],
                        const int32_t stride[3]) {
    if (!plane || !plane[0]) return RCV_OK;  // decode only
    if (!stride) return RCV_ERR_INVALID_ARG;
    const Geometry& g = d->geo;
    switch (layout) {
    case RCV_OUT_I420:
        for (int p = 0; p < 3; ++p)
            if (!plane[p] || stride[p] < g.plane_w[p]) return RCV_ERR_INVALID_ARG;
        return RCV_OK;
    case RCV_OUT_NV12:
        if (!plane[1] || stride[0] < g.width || stride[1] < g.width) return RCV_ERR_INVALID_ARG;
        return RCV_OK;
    case RCV_OUT_BGRA:
        return RCV_ERR_UNSUPPORTED;  // M6
    default:
        return RCV_ERR_INVALID_ARG;
    }
}

void write_output(const rcv_decoder* d, rcv_output_layout layout, uint8_t* const plane[3],
                  const int32_t stride[3]) {
    if (!plane || !plane[0]) return;
    const Geometry& g = d->geo;
    if (layout == RCV_OUT_I420) {
        for (int p = 0; p < 3; ++p)
            for (int y = 0; y < g.plane_h[p]; ++y)
                std::memcpy(plane[p] + ptrdiff_t(y) * stride[p], d->ref_plane[p] + y * d->ref_stride[p],
                            size_t(g.plane_w[p]));
        return;
    }
    for (int y = 0; y < g.plane_h[0]; ++y)
        std::memcpy(plane[0] + ptrdiff_t(y) * stride[0], d->ref_plane[0] + y * d->ref_stride[0],
                    size_t(g.plane_w[0]));
    for (int y = 0; y < g.plane_h[1]; ++y) {
        uint8_t* uv = plane[1] + ptrdiff_t(y) * stride[1];
        const uint8_t* u = d->ref_plane[1] + y * d->ref_stride[1];
        const uint8_t* v = d->ref_plane[2] + y * d->ref_stride[2];
        for (int x = 0; x < g.plane_w[1]; ++x) {
            uv[2 * x] = u[x];
            uv[2 * x + 1] = v[x];
        }
    }
}

}  // namespace

extern "C" {

const char* rcv_status_string(rcv_status s) {
    switch (s) {
    case RCV_OK: return "ok";
    case RCV_ERR_INVALID_ARG: return "invalid argument";
    case RCV_ERR_UNSUPPORTED: return "unsupported";
    case RCV_ERR_BUFFER_TOO_SMALL: return "buffer too small";
    case RCV_ERR_BITSTREAM: return "invalid bitstream";
    case RCV_ERR_NO_REFERENCE: return "no reference frame";
    case RCV_ERR_OUT_OF_MEMORY: return "out of memory";
    }
    return "unknown status";
}

rcv_status rcv_parse_sequence_header(const uint8_t seq_header[32], rcv_sequence_info* out) {
    if (!seq_header || !out) return RCV_ERR_INVALID_ARG;
    SeqHeader h;
    const rcv_status st = parse_seq_header(seq_header, &h);
    if (st != RCV_OK) return st;
    out->format = rcv_format(h.format);
    out->coded_width = h.coded_w;
    out->coded_height = h.coded_h;
    out->display_width = h.display_w;
    out->display_height = h.display_h;
    out->colour_matrix = uint8_t(h.colour & 0xF);
    out->full_range = uint8_t((h.colour >> 4) & 1);
    out->chroma_siting = uint8_t((h.colour >> 5) & 3);
    out->keyframe_interval = h.keyint;
    out->fps_num = h.fps_num;
    out->fps_den = h.fps_den;
    return RCV_OK;
}

rcv_status rcv_decoder_create(const uint8_t seq_header[32], uint8_t num_threads, rcv_decoder** out) {
    (void)num_threads;  // M4
    if (!seq_header || !out) return RCV_ERR_INVALID_ARG;
    *out = nullptr;
    SeqHeader h;
    rcv_status st = parse_seq_header(seq_header, &h);
    if (st != RCV_OK) return st;
    if (h.format == RCV_FMT_GBR) return RCV_ERR_UNSUPPORTED;  // M6

    rcv_decoder* d = new (std::nothrow) rcv_decoder{};
    if (!d) return RCV_ERR_OUT_OF_MEMORY;
    d->seq = h;
    init_geometry(&d->geo, h.format, h.coded_w, h.coded_h, 1);
    size_t total = 0;
    for (int p = 0; p < 3; ++p) {
        d->ref_stride[p] = ptrdiff_t(align64(size_t(d->geo.plane_w[p])));
        total += size_t(d->ref_stride[p]) * size_t(d->geo.plane_h[p]);
    }
    d->ref_mem = static_cast<uint8_t*>(aligned_alloc64(total));
    if (!d->ref_mem) {
        rcv_decoder_destroy(d);
        return RCV_ERR_OUT_OF_MEMORY;
    }
    std::memset(d->ref_mem, 0, total);
    uint8_t* p = d->ref_mem;
    for (int i = 0; i < 3; ++i) {
        d->ref_plane[i] = p;
        p += size_t(d->ref_stride[i]) * size_t(d->geo.plane_h[i]);
    }
    *out = d;
    return RCV_OK;
}

void rcv_decoder_destroy(rcv_decoder* d) {
    if (!d) return;
    aligned_free64(d->ref_mem);
    delete d;
}

void rcv_decoder_reset(rcv_decoder* d) {
    if (d) d->has_ref = false;
}

rcv_status rcv_decode_frame(rcv_decoder* d, const uint8_t* packet, size_t size, rcv_output_layout layout,
                            uint8_t* const plane[3], const int32_t stride[3], rcv_frame_info* info) {
    const Clock::time_point t0 = Clock::now();
    if (!d || !packet) return RCV_ERR_INVALID_ARG;
    rcv_status st = check_output(d, layout, plane, stride);
    if (st != RCV_OK) return st;

    if (size < kDupPacketSize || get_u32(packet) != kFrameMagic || packet[4] != kVersion)
        return RCV_ERR_BITSTREAM;
    const uint8_t type = packet[5];
    switch (type) {
    case kFrameDup:
        if (size != kDupPacketSize || get_u16(packet + 6) != 0) return RCV_ERR_BITSTREAM;
        if (!d->has_ref) return RCV_ERR_NO_REFERENCE;
        break;
    case kFrameI:
        st = decode_i_frame(d, packet, size);
        if (st != RCV_OK) return st;
        break;
    case kFrameP:
        if (size < kFrameHeaderSize) return RCV_ERR_BITSTREAM;
        if (!d->has_ref) return RCV_ERR_NO_REFERENCE;
        return RCV_ERR_UNSUPPORTED;  // M5
    default:
        return RCV_ERR_BITSTREAM;
    }
    write_output(d, layout, plane, stride);

    if (info) {
        std::memset(info, 0, sizeof(*info));
        info->packet_size = uint32_t(size);
        info->frame_type = type;
        info->is_keyframe = type == kFrameI;
        info->blocks_total = uint32_t(d->geo.blocks_x * d->geo.blocks_y);
        if (type == kFrameDup) info->blocks_skipped = info->blocks_total;
        info->time_total_us = elapsed_us(t0);
        info->time_encode_us = info->time_total_us;
    }
    return RCV_OK;
}

}  // extern "C"
