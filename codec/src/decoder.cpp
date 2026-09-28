// RCV1 decoder (codec plan §5.7, §6, Appendix A.2).
// M1 scope: scalar, single-threaded; I-frames and DUP; I420 / NV12 output.
// The decoder must never crash on bad input: every length is checked before use (§6.5).
#include <chrono>
#include <cstring>
#include <new>

#include "aligned_buffer.h"
#include "bitio.h"
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
    uint8_t* resid;            // decoded symbols of one chunk (sized for S = 1)
    HuffDecEntry lut[kLutSize];
    bool has_ref;
};

namespace {

using Clock = std::chrono::steady_clock;

uint32_t elapsed_us(Clock::time_point t0) {
    return uint32_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count());
}

// Decodes one chunk's symbols into d->resid. `count` is derived from geometry, never stored.
rcv_status decode_chunk(rcv_decoder* d, const uint8_t* c, size_t size, size_t count) {
    if (size < kChunkHeaderSize || c[2] != 0 || c[3] != 0) return RCV_ERR_BITSTREAM;
    switch (c[0]) {
    case kChunkHuffman: {
        if (c[1] != 0 || count == 0 || size < kChunkHeaderSize + kHuffTableSize) return RCV_ERR_BITSTREAM;
        uint8_t len[256];
        huff_unpack_lengths(c + kChunkHeaderSize, len);
        if (!huff_build_decode_lut(len, d->lut)) return RCV_ERR_BITSTREAM;
        BitReader br(c + kChunkHeaderSize + kHuffTableSize, c + size);
        for (size_t k = 0; k < count; ++k) {
            if (br.count() < kLutBits) br.refill();
            const HuffDecEntry e = d->lut[br.peek12()];
            if (!br.consume(e.len)) return RCV_ERR_BITSTREAM;  // bitstream ended early
            d->resid[k] = e.sym;
        }
        // The chunk must be exactly the bitstream padded to a multiple of 4 bytes.
        const size_t used = (br.bits_consumed() + 7) / 8;
        if (size != kChunkHeaderSize + kHuffTableSize + align4(used)) return RCV_ERR_BITSTREAM;
        return RCV_OK;
    }
    case kChunkSingle:
        if (count == 0 || size != kChunkHeaderSize) return RCV_ERR_BITSTREAM;
        std::memset(d->resid, c[1], count);
        return RCV_OK;
    case kChunkRaw:
        if (c[1] != 0 || count == 0 || size != kChunkHeaderSize + align4(count)) return RCV_ERR_BITSTREAM;
        std::memcpy(d->resid, c + kChunkHeaderSize, count);
        return RCV_OK;
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
        for (int s = 0; s < slices; ++s) {
            const size_t cs = get_u32(payload + 4 * (size_t(pl) * slices + size_t(s)));
            const int r0 = g.slice_row(pl, s);
            const int r1 = g.slice_row(pl, s + 1);
            const rcv_status st = decode_chunk(d, chunk, cs, g.chunk_samples(pl, s));
            if (st != RCV_OK) return st;
            reconstruct_lossless_scalar(d->ref_plane[pl], d->ref_stride[pl], g.plane_w[pl], r0, r1,
                                        predictor, d->resid);
            chunk += cs;
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
    d->resid = static_cast<uint8_t*>(aligned_alloc64(align64(max_chunk_samples(d->geo))));
    if (!d->ref_mem || !d->resid) {
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
    aligned_free64(d->resid);
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
