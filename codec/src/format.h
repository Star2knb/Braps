// Bitstream constants, header (de)serialisation and frame geometry (codec plan §4.6, §6).
#pragma once

#include <cstddef>
#include <cstdint>

#include "rcv/rcv.h"

namespace rcv {

constexpr uint8_t kVersion = 1;
constexpr uint32_t kSeqMagic = 0x53564352;    // "RCVS" read as little-endian u32
constexpr uint32_t kFrameMagic = 0x31564352;  // "RCV1"

constexpr size_t kSeqHeaderSize = 32;
constexpr size_t kFrameHeaderSize = 32;
constexpr size_t kDupPacketSize = 8;
constexpr size_t kChunkHeaderSize = 4;
constexpr size_t kHuffTableSize = 128;

constexpr int kBlockLog2 = 4;
constexpr int kMaxSlices = 64;
constexpr int kDefaultSlices = 8;
constexpr int kMinDim = 2;
constexpr int kMaxDim = 8192;
constexpr int kMaxCodeLen = 12;
constexpr uint16_t kDefaultKeyint = 120;

enum FrameType : uint8_t { kFrameDup = 0, kFrameI = 1, kFrameP = 2 };
enum ChunkMode : uint8_t { kChunkHuffman = 0, kChunkSingle = 1, kChunkRaw = 2, kChunkEmpty = 3 };
constexpr uint16_t kFlagCrc = 1u << 0;
constexpr uint16_t kFlagNear = 1u << 1;
constexpr uint16_t kKnownFlags = kFlagCrc | kFlagNear;

inline void put_u16(uint8_t* p, uint16_t v) {
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
}
inline void put_u32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
    p[2] = uint8_t(v >> 16);
    p[3] = uint8_t(v >> 24);
}
inline uint16_t get_u16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
inline uint32_t get_u32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

constexpr size_t align4(size_t n) { return (n + 3) & ~size_t(3); }

// Colour byte: bits 0-3 matrix, bit 4 full range, bits 5-6 chroma siting (§6.1 offset 14).
constexpr uint8_t pack_colour(unsigned matrix, unsigned full_range, unsigned siting) {
    return uint8_t((matrix & 0xF) | ((full_range & 1) << 4) | ((siting & 3) << 5));
}

struct SeqHeader {
    uint8_t format;
    uint16_t coded_w, coded_h;
    uint16_t display_w, display_h;
    uint8_t colour;
    uint32_t fps_num, fps_den;
    uint16_t keyint;
};

void write_seq_header(const SeqHeader& h, uint8_t* out32);
// Validates magic, version, format, dimensions and display size.
rcv_status parse_seq_header(const uint8_t* in32, SeqHeader* out);

struct FrameHeader {
    uint8_t type;
    uint16_t flags;
    uint8_t format;
    uint8_t near_level;
    uint8_t predictor;
    uint8_t slices;
    uint16_t width, height;
    uint8_t colour;
    uint32_t frame_number;
    uint32_t payload_size;
    uint32_t crc;
};

void write_frame_header(const FrameHeader& h, uint8_t* out32);  // I and P packets
void write_dup_packet(uint8_t* out8);

bool valid_dimensions(int format, int width, int height);

// Plane sizes, blocks and slice bounds for one frame (§4.6).
struct Geometry {
    int format;
    int width, height;           // coded size
    int blocks_x, blocks_y;
    int num_slices;
    int plane_w[3], plane_h[3];
    int block_shift[3];          // log2 of the block height in each plane
    int slice_block_row[kMaxSlices + 1];

    // First sample row of slice s in plane p (s == num_slices gives the plane height).
    int slice_row(int p, int s) const {
        const int r = slice_block_row[s] << block_shift[p];
        return r < plane_h[p] ? r : plane_h[p];
    }
    size_t chunk_samples(int p, int s) const {
        return size_t(plane_w[p]) * size_t(slice_row(p, s + 1) - slice_row(p, s));
    }
};

// slices == 0 selects the default min(8, blocks_y). Returns false for invalid input.
bool init_geometry(Geometry* g, int format, int width, int height, int slices);
size_t skip_map_size(const Geometry& g);
size_t max_packet_size(const Geometry& g);   // §6.6
size_t max_chunk_samples(const Geometry& g);

}  // namespace rcv
