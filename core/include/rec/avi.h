// AVI OpenDML (AVI 2.0) container for RCV1 video (recorder plan §11.1).
//
//   RIFF 'AVI '   hdrl (avih, strl{strh, strf+extradata, indx super index}, odml{dmlh}), INFO, JUNK,
//     LIST movi   '00dc' chunks ..., 'ix00' standard index
//     idx1        legacy index of the first block
//   RIFF 'AVIX'  (one per further ~1 GB block)
//     LIST movi   '00dc' chunks ..., 'ix00'
//
// The video stream's FourCC is RCV1, its extradata the 32-byte RCV1 sequence header, AVIIF_KEYFRAME is
// set on I-frames, and DUP packets are ordinary 8-byte chunks. The header region at the start of the
// file is a multiple of 4096 bytes and is rewritten once, when the recording ends.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "rec/disk_file.h"

namespace rec {

struct AviVideoParams {
    uint32_t width = 0, height = 0;
    uint32_t fps_num = 60, fps_den = 1;
    uint8_t sequence_header[32] = {};
    uint32_t suggested_buffer = 0;  // largest packet we can write
    std::string software;           // INFO ISFT, e.g. "rec 0.4.0"
    std::string comment;            // INFO ICMT, e.g. "source=1366x745 fps=60 game=..."
};

class AviWriter {
public:
    // `file` must be open and empty. A new RIFF block starts after block_limit bytes (tests use less).
    explicit AviWriter(DiskFile& file, uint64_t block_limit = 1ull << 30) : file_(file), block_limit_(block_limit) {}

    bool begin(const AviVideoParams& params, std::string* error);
    bool add_video(const uint8_t* data, uint32_t size, bool keyframe);
    // Writes the indexes, rewrites the header region and closes the file.
    bool finish(std::string* error);

    uint64_t frames() const { return frames_; }
    uint32_t blocks() const { return uint32_t(super_.size()) + (open_block_ ? 1u : 0u); }

private:
    struct IndexEntry {
        uint32_t data_offset;  // from the block's movi data start (pointing at the chunk data)
        uint32_t size_flags;   // size, bit 31 set for a non-keyframe
        uint32_t idx1_offset;  // legacy: from the 'movi' fourcc, pointing at the chunk header
        uint32_t size;
        bool key;
    };
    struct SuperEntry {
        uint64_t offset;    // file offset of the ix00 chunk header
        uint32_t size;      // whole chunk
        uint32_t duration;  // frames
    };

    bool start_block(bool first);
    bool close_block();
    bool put(const void* data, size_t size);
    bool put32(uint32_t v) { return put(&v, 4); }
    void build_header(const AviVideoParams& p);

    DiskFile& file_;
    uint64_t block_limit_;
    std::vector<uint8_t> header_;   // the header region, patched in memory and rewritten at the end
    size_t off_riff_size_ = 0, off_avih_frames_ = 0, off_strh_length_ = 0, off_dmlh_frames_ = 0, off_indx_ = 0, off_movi_size_ = 0;
    uint32_t indx_capacity_ = 0;
    bool open_block_ = false;
    bool first_block_ = true;
    uint64_t riff_size_pos_ = 0, movi_size_pos_ = 0, movi_data_start_ = 0, block_start_ = 0;
    std::vector<IndexEntry> entries_;
    std::vector<SuperEntry> super_;
    uint64_t frames_ = 0, first_block_frames_ = 0;
    uint64_t first_riff_end_ = 0;   // for the header's RIFF size
    uint64_t first_movi_end_ = 0;
    std::string error_;
};

// ---- Reading -----------------------------------------------------------------------------------
struct AviChunk {
    uint64_t data_offset = 0;  // file offset of the chunk data
    uint32_t size = 0;
    bool key = false;          // from the standard index, if there is one
};

struct AviInfo {
    uint32_t width = 0, height = 0;
    uint32_t fps_num = 0, fps_den = 1;
    uint32_t avih_frames = 0, strh_length = 0, dmlh_frames = 0;
    uint8_t sequence_header[32] = {};
    bool has_sequence_header = false;
    std::string software, comment;
    char handler[5] = {};
};

struct AviScan {
    bool ok = false;                       // the file is an AVI we could walk (problems may remain)
    AviInfo info;
    std::vector<AviChunk> video;           // every '00dc' chunk, in file order
    uint32_t riff_blocks = 0;
    uint32_t superindex_entries = 0;
    uint64_t file_size = 0;
    std::vector<std::string> problems;     // structure and index inconsistencies
};

// Walks the whole file (chunk headers only, no payload reads) and cross-checks sizes, counts and
// the indx / ix00 / idx1 indexes against what is actually there.
AviScan scan_avi(const std::filesystem::path& path);

// Reads one chunk's payload.
class AviFile {
public:
    AviFile() = default;
    ~AviFile();
    AviFile(const AviFile&) = delete;
    AviFile& operator=(const AviFile&) = delete;
    bool open(const std::filesystem::path& path);
    bool read(uint64_t offset, uint32_t size, std::vector<uint8_t>* out);

private:
    void* file_ = nullptr;
};

}  // namespace rec
