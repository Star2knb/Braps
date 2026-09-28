// RCV1 encoder (codec plan §5, §6, §7).
// M1 scope: scalar, single-threaded, lossless YUV420 I-frames, plus DUP packets.
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

struct rcv_encoder {
    rcv_encoder_config cfg;   // with defaults resolved
    Geometry geo;
    uint8_t colour;
    uint8_t* ref_mem;         // reference frame = last reconstructed frame (§5.7)
    uint8_t* ref_plane[3];
    ptrdiff_t ref_stride[3];
    uint8_t* resid;           // residual symbols of one chunk
    size_t max_packet;
    uint32_t frame_number;
    uint32_t frames_since_i;
    bool has_ref;
};

namespace {

using Clock = std::chrono::steady_clock;

uint32_t elapsed_us(Clock::time_point t0) {
    return uint32_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count());
}

rcv_status validate_config(const rcv_encoder_config* c, Geometry* g) {
    if (!c) return RCV_ERR_INVALID_ARG;
    if (c->format == RCV_FMT_GBR) return RCV_ERR_UNSUPPORTED;  // M6
    if (c->format != RCV_FMT_YUV420) return RCV_ERR_INVALID_ARG;
    if (c->input_layout != RCV_IN_I420 && c->input_layout != RCV_IN_NV12) return RCV_ERR_INVALID_ARG;
    if (c->num_slices > kMaxSlices) return RCV_ERR_INVALID_ARG;
    if (!init_geometry(g, c->format, c->coded_width, c->coded_height, c->num_slices))
        return RCV_ERR_INVALID_ARG;
    if (c->display_width > c->coded_width || c->display_height > c->coded_height)
        return RCV_ERR_INVALID_ARG;
    if (c->predictor > kPredMed || c->colour_matrix > 1 || c->full_range > 1)
        return RCV_ERR_INVALID_ARG;
    if (unsigned(c->isa) > unsigned(RCV_ISA_AVX2)) return RCV_ERR_INVALID_ARG;
    return RCV_OK;
}

rcv_encoder_config resolve_defaults(const rcv_encoder_config& c) {
    rcv_encoder_config r = c;
    if (r.display_width == 0) r.display_width = r.coded_width;
    if (r.display_height == 0) r.display_height = r.coded_height;
    if (r.keyframe_interval == 0) r.keyframe_interval = kDefaultKeyint;
    return r;
}

// Copies the input into the reference buffer (source == reconstruction when lossless).
rcv_status load_input(rcv_encoder* e, const rcv_frame_in* in) {
    const Geometry& g = e->geo;
    if (e->cfg.input_layout == RCV_IN_I420) {
        for (int p = 0; p < 3; ++p)
            if (!in->plane[p] || in->stride[p] < g.plane_w[p]) return RCV_ERR_INVALID_ARG;
        for (int p = 0; p < 3; ++p)
            for (int y = 0; y < g.plane_h[p]; ++y)
                std::memcpy(e->ref_plane[p] + y * e->ref_stride[p],
                            in->plane[p] + ptrdiff_t(y) * in->stride[p], size_t(g.plane_w[p]));
        return RCV_OK;
    }
    // NV12: Y plane + interleaved CbCr plane (width bytes per chroma row).
    if (!in->plane[0] || !in->plane[1] || in->stride[0] < g.width || in->stride[1] < g.width)
        return RCV_ERR_INVALID_ARG;
    for (int y = 0; y < g.plane_h[0]; ++y)
        std::memcpy(e->ref_plane[0] + y * e->ref_stride[0], in->plane[0] + ptrdiff_t(y) * in->stride[0],
                    size_t(g.plane_w[0]));
    for (int y = 0; y < g.plane_h[1]; ++y) {
        const uint8_t* uv = in->plane[1] + ptrdiff_t(y) * in->stride[1];
        uint8_t* u = e->ref_plane[1] + y * e->ref_stride[1];
        uint8_t* v = e->ref_plane[2] + y * e->ref_stride[2];
        for (int x = 0; x < g.plane_w[1]; ++x) {
            u[x] = uv[2 * x];
            v[x] = uv[2 * x + 1];
        }
    }
    return RCV_OK;
}

// Writes one chunk (§6.4) and returns its size (a multiple of 4).
// `out` must have room for the RAW bound: 4 + align4(count).
size_t encode_chunk(uint8_t* out, const uint8_t* syms, size_t count, const uint32_t hist[256]) {
    std::memset(out, 0, kChunkHeaderSize);
    if (count == 0) {
        out[0] = kChunkEmpty;
        return kChunkHeaderSize;
    }
    int distinct = 0, last = 0;
    for (int s = 0; s < 256; ++s)
        if (hist[s]) {
            ++distinct;
            last = s;
        }
    if (distinct == 1) {
        out[0] = kChunkSingle;
        out[1] = uint8_t(last);
        return kChunkHeaderSize;
    }

    uint8_t len[256];
    huff_build_lengths(hist, len);
    uint64_t bits = 0;
    for (int s = 0; s < 256; ++s) bits += uint64_t(hist[s]) * len[s];
    const size_t huff_size = kChunkHeaderSize + kHuffTableSize + align4(size_t((bits + 7) / 8));
    const size_t raw_size = kChunkHeaderSize + align4(count);

    if (huff_size > raw_size) {
        out[0] = kChunkRaw;
        std::memcpy(out + kChunkHeaderSize, syms, count);
        std::memset(out + kChunkHeaderSize + count, 0, raw_size - kChunkHeaderSize - count);
        return raw_size;
    }

    out[0] = kChunkHuffman;
    huff_pack_lengths(len, out + kChunkHeaderSize);
    uint16_t code[256];
    huff_canonical_codes(len, code);  // always valid for lengths from huff_build_lengths
    uint8_t* bitstream = out + kChunkHeaderSize + kHuffTableSize;
    BitWriter bw(bitstream);
    for (size_t k = 0; k < count; ++k) bw.put(code[syms[k]], len[syms[k]]);
    const size_t n = bw.finish();
    std::memset(bitstream + n, 0, align4(n) - n);
    return huff_size;
}

void fill_info(rcv_frame_info* info, uint32_t size, uint8_t type, uint32_t blocks_total,
               uint32_t encode_us, uint32_t total_us) {
    if (!info) return;
    std::memset(info, 0, sizeof(*info));
    info->packet_size = size;
    info->frame_type = type;
    info->is_keyframe = type == kFrameI;
    info->blocks_total = blocks_total;
    info->time_encode_us = encode_us;
    info->time_total_us = total_us;
}

}  // namespace

extern "C" {

void rcv_encoder_config_init(rcv_encoder_config* c) {
    if (!c) return;
    std::memset(c, 0, sizeof(*c));
    c->format = RCV_FMT_YUV420;
    c->input_layout = RCV_IN_I420;
    c->colour_matrix = 0;
    c->full_range = 1;
    c->predictor = kPredMed;
    c->enable_skip = 1;
    c->keyframe_interval = kDefaultKeyint;
    c->fps_num = 60;
    c->fps_den = 1;
    c->isa = RCV_ISA_AUTO;
}

size_t rcv_max_packet_size(const rcv_encoder_config* cfg) {
    Geometry g;
    if (validate_config(cfg, &g) != RCV_OK) return 0;
    return max_packet_size(g);
}

void rcv_write_sequence_header(const rcv_encoder_config* cfg, uint8_t out[32]) {
    if (!out) return;
    std::memset(out, 0, kSeqHeaderSize);
    if (!cfg) return;
    const rcv_encoder_config c = resolve_defaults(*cfg);
    SeqHeader h{};
    h.format = uint8_t(c.format);
    h.coded_w = c.coded_width;
    h.coded_h = c.coded_height;
    h.display_w = c.display_width;
    h.display_h = c.display_height;
    h.colour = pack_colour(c.colour_matrix, c.full_range, 0);
    h.fps_num = c.fps_num;
    h.fps_den = c.fps_den;
    h.keyint = c.keyframe_interval;
    write_seq_header(h, out);
}

rcv_status rcv_encoder_create(const rcv_encoder_config* cfg, rcv_encoder** out) {
    if (!out) return RCV_ERR_INVALID_ARG;
    *out = nullptr;
    Geometry g;
    const rcv_status st = validate_config(cfg, &g);
    if (st != RCV_OK) return st;

    rcv_encoder* e = new (std::nothrow) rcv_encoder{};
    if (!e) return RCV_ERR_OUT_OF_MEMORY;
    e->cfg = resolve_defaults(*cfg);
    e->geo = g;
    e->colour = pack_colour(e->cfg.colour_matrix, e->cfg.full_range, 0);
    e->max_packet = max_packet_size(g);

    size_t total = 0;
    for (int p = 0; p < 3; ++p) {
        e->ref_stride[p] = ptrdiff_t(align64(size_t(g.plane_w[p])));
        total += size_t(e->ref_stride[p]) * size_t(g.plane_h[p]);
    }
    e->ref_mem = static_cast<uint8_t*>(aligned_alloc64(total));
    e->resid = static_cast<uint8_t*>(aligned_alloc64(align64(max_chunk_samples(g))));
    if (!e->ref_mem || !e->resid) {
        rcv_encoder_destroy(e);
        return RCV_ERR_OUT_OF_MEMORY;
    }
    std::memset(e->ref_mem, 0, total);
    uint8_t* p = e->ref_mem;
    for (int i = 0; i < 3; ++i) {
        e->ref_plane[i] = p;
        p += size_t(e->ref_stride[i]) * size_t(g.plane_h[i]);
    }
    *out = e;
    return RCV_OK;
}

void rcv_encoder_destroy(rcv_encoder* e) {
    if (!e) return;
    aligned_free64(e->ref_mem);
    aligned_free64(e->resid);
    delete e;
}

rcv_status rcv_encode_frame(rcv_encoder* e, const rcv_frame_in* in, const rcv_encode_params* params,
                            uint8_t* out, size_t out_capacity, rcv_frame_info* info) {
    const Clock::time_point t0 = Clock::now();
    if (!e || !in || !out) return RCV_ERR_INVALID_ARG;
    if (out_capacity < e->max_packet) return RCV_ERR_BUFFER_TOO_SMALL;
    const uint8_t near_level = params ? params->near : 0;
    if (near_level > 3) return RCV_ERR_INVALID_ARG;
    if (near_level != 0) return RCV_ERR_UNSUPPORTED;  // M7

    rcv_status st = load_input(e, in);
    if (st != RCV_OK) return st;

    // M1: every coded frame is an I-frame (temporal skip / P-frames arrive in M5).
    const Geometry& g = e->geo;
    const int S = g.num_slices;
    uint8_t* payload = out + kFrameHeaderSize;
    uint8_t* dir = payload;
    uint8_t* p = dir + size_t(3) * size_t(S) * 4;
    for (int pl = 0; pl < 3; ++pl) {
        for (int s = 0; s < S; ++s) {
            uint32_t hist[256] = {};
            const size_t n = residuals_lossless_scalar(e->ref_plane[pl], e->ref_stride[pl], g.plane_w[pl],
                                                       g.slice_row(pl, s), g.slice_row(pl, s + 1),
                                                       e->cfg.predictor, e->resid, hist);
            const size_t cs = encode_chunk(p, e->resid, n, hist);
            put_u32(dir + 4 * (size_t(pl) * size_t(S) + size_t(s)), uint32_t(cs));
            p += cs;
        }
    }
    const size_t payload_size = size_t(p - payload);

    FrameHeader h{};
    h.type = kFrameI;
    h.flags = e->cfg.enable_crc ? kFlagCrc : 0;
    h.format = uint8_t(e->cfg.format);
    h.near_level = 0;
    h.predictor = e->cfg.predictor;
    h.slices = uint8_t(S);
    h.width = uint16_t(g.width);
    h.height = uint16_t(g.height);
    h.colour = e->colour;
    h.frame_number = e->frame_number;
    h.payload_size = uint32_t(payload_size);
    h.crc = e->cfg.enable_crc ? crc32c(payload, payload_size) : 0;
    write_frame_header(h, out);

    e->has_ref = true;
    e->frames_since_i = 1;
    e->frame_number++;

    const uint32_t us = elapsed_us(t0);
    fill_info(info, uint32_t(kFrameHeaderSize + payload_size), kFrameI,
              uint32_t(g.blocks_x * g.blocks_y), us, us);
    return RCV_OK;
}

rcv_status rcv_encode_duplicate(rcv_encoder* e, uint8_t* out, size_t out_capacity, rcv_frame_info* info) {
    const Clock::time_point t0 = Clock::now();
    if (!e || !out) return RCV_ERR_INVALID_ARG;
    if (out_capacity < kDupPacketSize) return RCV_ERR_BUFFER_TOO_SMALL;
    if (!e->has_ref) return RCV_ERR_NO_REFERENCE;
    write_dup_packet(out);
    e->frames_since_i++;
    e->frame_number++;
    const uint32_t blocks = uint32_t(e->geo.blocks_x * e->geo.blocks_y);
    fill_info(info, uint32_t(kDupPacketSize), kFrameDup, blocks, 0, elapsed_us(t0));
    if (info) info->blocks_skipped = blocks;
    return RCV_OK;
}

}  // extern "C"
