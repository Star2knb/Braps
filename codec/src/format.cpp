#include "format.h"

#include <cstring>

namespace rcv {

void write_seq_header(const SeqHeader& h, uint8_t* o) {
    std::memset(o, 0, kSeqHeaderSize);
    put_u32(o + 0, kSeqMagic);
    o[4] = kVersion;
    o[5] = h.format;
    put_u16(o + 6, h.coded_w);
    put_u16(o + 8, h.coded_h);
    put_u16(o + 10, h.display_w);
    put_u16(o + 12, h.display_h);
    o[14] = h.colour;
    put_u32(o + 16, h.fps_num);
    put_u32(o + 20, h.fps_den);
    put_u16(o + 24, h.keyint);
}

rcv_status parse_seq_header(const uint8_t* in, SeqHeader* h) {
    if (get_u32(in) != kSeqMagic || in[4] != kVersion) return RCV_ERR_BITSTREAM;
    h->format = in[5];
    h->coded_w = get_u16(in + 6);
    h->coded_h = get_u16(in + 8);
    h->display_w = get_u16(in + 10);
    h->display_h = get_u16(in + 12);
    h->colour = in[14];
    h->fps_num = get_u32(in + 16);
    h->fps_den = get_u32(in + 20);
    h->keyint = get_u16(in + 24);
    if (h->format > RCV_FMT_GBR) return RCV_ERR_BITSTREAM;
    if (!valid_dimensions(h->format, h->coded_w, h->coded_h)) return RCV_ERR_BITSTREAM;
    if (h->display_w == 0 || h->display_h == 0 || h->display_w > h->coded_w ||
        h->display_h > h->coded_h)
        return RCV_ERR_BITSTREAM;
    return RCV_OK;
}

void write_frame_header(const FrameHeader& h, uint8_t* o) {
    put_u32(o + 0, kFrameMagic);
    o[4] = kVersion;
    o[5] = h.type;
    put_u16(o + 6, h.flags);
    o[8] = h.format;
    o[9] = h.near_level;
    o[10] = h.predictor;
    o[11] = h.slices;
    put_u16(o + 12, h.width);
    put_u16(o + 14, h.height);
    o[16] = kBlockLog2;
    o[17] = h.colour;
    o[18] = 0;
    o[19] = 0;
    put_u32(o + 20, h.frame_number);
    put_u32(o + 24, h.payload_size);
    put_u32(o + 28, h.crc);
}

void write_dup_packet(uint8_t* o) {
    put_u32(o + 0, kFrameMagic);
    o[4] = kVersion;
    o[5] = kFrameDup;
    put_u16(o + 6, 0);
}

bool valid_dimensions(int format, int width, int height) {
    if (width < kMinDim || width > kMaxDim || height < kMinDim || height > kMaxDim) return false;
    if (format == RCV_FMT_YUV420 && ((width | height) & 1)) return false;
    return true;
}

bool init_geometry(Geometry* g, int format, int width, int height, int slices) {
    if (format != RCV_FMT_YUV420 && format != RCV_FMT_GBR) return false;
    if (!valid_dimensions(format, width, height)) return false;
    g->format = format;
    g->width = width;
    g->height = height;
    g->blocks_x = (width + (1 << kBlockLog2) - 1) >> kBlockLog2;
    g->blocks_y = (height + (1 << kBlockLog2) - 1) >> kBlockLog2;
    if (slices == 0) slices = g->blocks_y < kDefaultSlices ? g->blocks_y : kDefaultSlices;
    if (slices < 1 || slices > kMaxSlices || slices > g->blocks_y) return false;
    g->num_slices = slices;

    if (format == RCV_FMT_YUV420) {
        g->plane_w[0] = width;
        g->plane_h[0] = height;
        g->block_shift[0] = kBlockLog2;
        for (int p = 1; p < 3; ++p) {
            g->plane_w[p] = width / 2;
            g->plane_h[p] = height / 2;
            g->block_shift[p] = kBlockLog2 - 1;
        }
    } else {
        for (int p = 0; p < 3; ++p) {
            g->plane_w[p] = width;
            g->plane_h[p] = height;
            g->block_shift[p] = kBlockLog2;
        }
    }
    for (int s = 0; s <= slices; ++s) g->slice_block_row[s] = s * g->blocks_y / slices;
    return true;
}

size_t skip_map_size(const Geometry& g) {
    return align4((size_t(g.blocks_x) * size_t(g.blocks_y) + 7) / 8);
}

size_t max_packet_size(const Geometry& g) {
    size_t n = kFrameHeaderSize + skip_map_size(g) + size_t(3) * size_t(g.num_slices) * 4;
    for (int p = 0; p < 3; ++p)
        for (int s = 0; s < g.num_slices; ++s) n += kChunkHeaderSize + align4(g.chunk_samples(p, s));
    return n;
}

size_t max_chunk_samples(const Geometry& g) {
    size_t m = 0;
    for (int p = 0; p < 3; ++p)
        for (int s = 0; s < g.num_slices; ++s) {
            const size_t n = g.chunk_samples(p, s);
            if (n > m) m = n;
        }
    return m;
}

}  // namespace rcv
