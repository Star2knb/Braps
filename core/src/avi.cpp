#include "rec/avi.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "rec/paths.h"

namespace rec {
namespace {

// ---- A growable little-endian byte buffer with RIFF chunk helpers ------------------------------------
struct Buf {
    std::vector<uint8_t> b;
    size_t size() const { return b.size(); }
    void u8(uint8_t v) { b.push_back(v); }
    void u16(uint16_t v) { put(&v, 2); }
    void u32(uint32_t v) { put(&v, 4); }
    void u64(uint64_t v) { put(&v, 8); }
    void four(const char* s) { put(s, 4); }
    void zeros(size_t n) { b.insert(b.end(), n, 0); }
    void put(const void* p, size_t n) {
        const uint8_t* q = static_cast<const uint8_t*>(p);
        b.insert(b.end(), q, q + n);
    }
    void patch32(size_t at, uint32_t v) { std::memcpy(&b[at], &v, 4); }
    // A chunk: id, size (patched at end), data. Returns the position of the size field.
    size_t begin_chunk(const char* id) {
        four(id);
        const size_t at = b.size();
        u32(0);
        return at;
    }
    void end_chunk(size_t size_at) {
        const size_t data = size_at + 4;
        patch32(size_at, uint32_t(b.size() - data));
        if ((b.size() - data) & 1) u8(0);
    }
    // A LIST: 'LIST', size (patched at end), type.
    size_t begin_list(const char* type) {
        four("LIST");
        const size_t at = b.size();
        u32(0);
        four(type);
        return at;
    }
    void end_list(size_t size_at) { patch32(size_at, uint32_t(b.size() - (size_at + 4))); }
};

uint32_t rd32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}
uint16_t rd16(const uint8_t* p) {
    uint16_t v;
    std::memcpy(&v, p, 2);
    return v;
}
uint64_t rd64(const uint8_t* p) {
    uint64_t v;
    std::memcpy(&v, p, 8);
    return v;
}

constexpr uint32_t kIndexCapacity = 256;  // super-index entries: 256 blocks of ~1 GB
constexpr uint32_t kAviifKeyframe = 0x10;
constexpr uint32_t kHeaderAlign = 4096;

}  // namespace

// ---- Writer ------------------------------------------------------------------------------------------
bool AviWriter::put(const void* data, size_t size) {
    if (!file_.append(data, size)) {
        error_ = file_.error();
        return false;
    }
    return true;
}

void AviWriter::build_header(const AviVideoParams& p) {
    Buf h;
    h.four("RIFF");
    off_riff_size_ = h.size();
    h.u32(0);
    h.four("AVI ");

    const size_t hdrl = h.begin_list("hdrl");
    {
        const size_t avih = h.begin_chunk("avih");
        h.u32(uint32_t(1000000ull * p.fps_den / p.fps_num));  // dwMicroSecPerFrame
        h.u32(uint32_t(uint64_t(p.suggested_buffer) * p.fps_num / p.fps_den));  // dwMaxBytesPerSec
        h.u32(0);                                             // dwPaddingGranularity
        h.u32(0x10 | 0x100);                                  // AVIF_HASINDEX | AVIF_ISINTERLEAVED
        off_avih_frames_ = h.size();
        h.u32(0);                                             // dwTotalFrames (first RIFF)
        h.u32(0);                                             // dwInitialFrames
        h.u32(1);                                             // dwStreams
        h.u32(p.suggested_buffer);
        h.u32(p.width);
        h.u32(p.height);
        h.zeros(16);
        h.end_chunk(avih);

        const size_t strl = h.begin_list("strl");
        {
            const size_t strh = h.begin_chunk("strh");
            h.four("vids");
            h.four("RCV1");
            h.u32(0);                // dwFlags
            h.u16(0);                // wPriority
            h.u16(0);                // wLanguage
            h.u32(0);                // dwInitialFrames
            h.u32(p.fps_den);        // dwScale
            h.u32(p.fps_num);        // dwRate
            h.u32(0);                // dwStart
            off_strh_length_ = h.size();
            h.u32(0);                // dwLength
            h.u32(p.suggested_buffer);
            h.u32(0xFFFFFFFFu);      // dwQuality
            h.u32(0);                // dwSampleSize
            h.u16(0);
            h.u16(0);
            h.u16(uint16_t(p.width));
            h.u16(uint16_t(p.height));
            h.end_chunk(strh);

            const size_t strf = h.begin_chunk("strf");
            h.u32(40);                                  // BITMAPINFOHEADER
            h.u32(p.width);
            h.u32(p.height);
            h.u16(1);
            h.u16(12);
            h.four("RCV1");
            h.u32(p.width * p.height * 3 / 2);
            h.zeros(16);                                // pels per metre, colours
            h.put(p.sequence_header, 32);               // extradata
            h.end_chunk(strf);

            const size_t indx = h.begin_chunk("indx");
            off_indx_ = h.size();
            h.u16(4);        // wLongsPerEntry
            h.u8(0);         // bIndexSubType
            h.u8(0);         // bIndexType: AVI_INDEX_OF_INDEXES
            h.u32(0);        // nEntriesInUse
            h.four("00dc");  // dwChunkId
            h.zeros(12);     // dwReserved[3]
            h.zeros(size_t(kIndexCapacity) * 16);
            h.end_chunk(indx);
        }
        h.end_list(strl);

        const size_t odml = h.begin_list("odml");
        {
            const size_t dmlh = h.begin_chunk("dmlh");
            off_dmlh_frames_ = h.size();
            h.u32(0);
            h.zeros(244);
            h.end_chunk(dmlh);
        }
        h.end_list(odml);
    }
    h.end_list(hdrl);

    const size_t info = h.begin_list("INFO");
    {
        std::string sw = p.software;
        sw.push_back('\0');
        const size_t isft = h.begin_chunk("ISFT");
        h.put(sw.data(), sw.size());
        h.end_chunk(isft);
        if (!p.comment.empty()) {
            std::string cm = p.comment;
            cm.push_back('\0');
            const size_t icmt = h.begin_chunk("ICMT");
            h.put(cm.data(), cm.size());
            h.end_chunk(icmt);
        }
    }
    h.end_list(info);

    // JUNK so that the header region, including the 12-byte 'LIST movi' header, ends on a 4096 boundary.
    size_t total = h.size() + 8 + 12;
    total = (total + kHeaderAlign - 1) / kHeaderAlign * kHeaderAlign;
    const size_t junk = h.begin_chunk("JUNK");
    h.zeros(total - 12 - h.size());
    h.end_chunk(junk);

    h.four("LIST");
    off_movi_size_ = h.size();
    h.u32(0);
    h.four("movi");
    header_ = std::move(h.b);
}

bool AviWriter::begin(const AviVideoParams& p, std::string* error) {
    if (p.fps_num == 0 || p.fps_den == 0 || p.width == 0 || p.height == 0) {
        *error = "bad video parameters";
        return false;
    }
    build_header(p);
    if (!put(header_.data(), header_.size())) {
        *error = error_;
        return false;
    }
    first_block_ = true;
    open_block_ = true;
    block_start_ = 0;
    riff_size_pos_ = 4;
    movi_size_pos_ = off_movi_size_;
    movi_data_start_ = header_.size();
    entries_.clear();
    super_.clear();
    frames_ = first_block_frames_ = 0;
    return true;
}

bool AviWriter::start_block(bool first) {
    first_block_ = first;
    open_block_ = true;
    block_start_ = file_.position();
    riff_size_pos_ = block_start_ + 4;
    Buf b;
    b.four("RIFF");
    b.u32(0);
    b.four("AVIX");
    b.four("LIST");
    b.u32(0);
    b.four("movi");
    movi_size_pos_ = block_start_ + 16;
    if (!put(b.b.data(), b.size())) return false;
    movi_data_start_ = file_.position();
    entries_.clear();
    return true;
}

bool AviWriter::add_video(const uint8_t* data, uint32_t size, bool keyframe) {
    if (!open_block_ || !error_.empty()) return false;
    const uint64_t header_pos = file_.position();
    uint8_t head[8] = {'0', '0', 'd', 'c'};
    std::memcpy(head + 4, &size, 4);
    if (!put(head, 8) || !put(data, size)) return false;
    if (size & 1) {
        const uint8_t pad = 0;
        if (!put(&pad, 1)) return false;
    }
    IndexEntry e;
    e.data_offset = uint32_t(header_pos + 8 - movi_data_start_);
    e.size = size;
    e.size_flags = size | (keyframe ? 0u : 0x80000000u);
    e.idx1_offset = uint32_t(header_pos - (movi_data_start_ - 4));
    e.key = keyframe;
    entries_.push_back(e);
    ++frames_;
    if (file_.position() - block_start_ >= block_limit_ && super_.size() + 1 < kIndexCapacity) {
        if (!close_block() || !start_block(false)) return false;
    }
    return true;
}

bool AviWriter::close_block() {
    // Standard index of this block.
    const uint64_t ix_pos = file_.position();
    Buf ix;
    ix.four("ix00");
    ix.u32(uint32_t(24 + 8 * entries_.size()));
    ix.u16(2);       // wLongsPerEntry
    ix.u8(0);        // bIndexSubType
    ix.u8(1);        // bIndexType: AVI_INDEX_OF_CHUNKS
    ix.u32(uint32_t(entries_.size()));
    ix.four("00dc");
    ix.u64(movi_data_start_);  // qwBaseOffset
    ix.u32(0);
    for (const IndexEntry& e : entries_) {
        ix.u32(e.data_offset);
        ix.u32(e.size_flags);
    }
    if (!put(ix.b.data(), ix.size())) return false;
    const uint64_t movi_end = file_.position();

    // The movi LIST ends here; the first block then carries the legacy idx1.
    const uint32_t movi_size = uint32_t(movi_end - (movi_size_pos_ + 4));
    uint64_t riff_end = movi_end;
    if (first_block_) {
        Buf idx;
        idx.four("idx1");
        idx.u32(uint32_t(16 * entries_.size()));
        for (const IndexEntry& e : entries_) {
            idx.four("00dc");
            idx.u32(e.key ? kAviifKeyframe : 0);
            idx.u32(e.idx1_offset);
            idx.u32(e.size);
        }
        if (!put(idx.b.data(), idx.size())) return false;
        riff_end = file_.position();
        first_block_frames_ = entries_.size();
    }
    const uint32_t riff_size = uint32_t(riff_end - (riff_size_pos_ + 4));
    if (first_block_) {
        std::memcpy(&header_[off_movi_size_], &movi_size, 4);  // rewritten with the whole header at the end
        std::memcpy(&header_[off_riff_size_], &riff_size, 4);
    } else if (!file_.patch(movi_size_pos_, &movi_size, 4) || !file_.patch(riff_size_pos_, &riff_size, 4)) {
        error_ = file_.error();
        return false;
    }
    super_.push_back({ix_pos, uint32_t(8 + 24 + 8 * entries_.size()), uint32_t(entries_.size())});
    open_block_ = false;
    return true;
}

bool AviWriter::finish(std::string* error) {
    bool ok = error_.empty();
    if (ok && open_block_) ok = close_block();
    if (ok) {
        const uint32_t first = uint32_t(first_block_frames_), total = uint32_t(frames_);
        std::memcpy(&header_[off_avih_frames_], &first, 4);
        std::memcpy(&header_[off_strh_length_], &total, 4);
        std::memcpy(&header_[off_dmlh_frames_], &total, 4);
        const uint32_t used = uint32_t(super_.size());
        std::memcpy(&header_[off_indx_ + 4], &used, 4);
        size_t at = off_indx_ + 24;
        for (const SuperEntry& s : super_) {
            std::memcpy(&header_[at], &s.offset, 8);
            std::memcpy(&header_[at + 8], &s.size, 4);
            std::memcpy(&header_[at + 12], &s.duration, 4);
            at += 16;
        }
        ok = file_.patch(0, header_.data(), header_.size());
        if (!ok) error_ = file_.error();
    }
    std::string close_error;
    const bool closed = file_.close(&close_error);
    if (!ok || !closed) {
        *error = !error_.empty() ? error_ : close_error;
        return false;
    }
    return true;
}

// ---- Reading -----------------------------------------------------------------------------------------
namespace {

class Reader {
public:
    explicit Reader(const std::filesystem::path& path) {
        if (_wfopen_s(&f_, path.c_str(), L"rb") != 0) f_ = nullptr;
        if (f_) {
            std::setvbuf(f_, nullptr, _IOFBF, 1 << 20);
            _fseeki64(f_, 0, SEEK_END);
            size_ = uint64_t(_ftelli64(f_));
        }
    }
    ~Reader() {
        if (f_) std::fclose(f_);
    }
    bool ok() const { return f_ != nullptr; }
    uint64_t size() const { return size_; }
    bool read(uint64_t off, void* dst, size_t n) {
        if (off + n > size_) return false;
        if (_fseeki64(f_, int64_t(off), SEEK_SET) != 0) return false;
        return std::fread(dst, 1, n, f_) == n;
    }

private:
    FILE* f_ = nullptr;
    uint64_t size_ = 0;
};

struct ChunkHead {
    char id[5] = {};
    uint32_t size = 0;
};

bool read_head(Reader& r, uint64_t off, ChunkHead* h) {
    uint8_t b[8];
    if (!r.read(off, b, 8)) return false;
    std::memcpy(h->id, b, 4);
    h->id[4] = 0;
    h->size = rd32(b + 4);
    return true;
}

struct BlockIndex {
    uint64_t movi_data_start = 0;
    uint64_t first_chunk = 0, chunk_count = 0;  // range in AviScan::video
    uint64_t ix_pos = 0;                         // 0 = no ix00 found
    struct Entry {
        uint32_t off, size_flags;
    };
    std::vector<Entry> ix;
    uint64_t ix_base = 0;
};

}  // namespace

AviScan scan_avi(const std::filesystem::path& path) {
    AviScan scan;
    Reader r(path);
    if (!r.ok()) {
        scan.problems.push_back("can't open the file");
        return scan;
    }
    scan.file_size = r.size();
    auto problem = [&](const std::string& s) { scan.problems.push_back(s); };

    std::vector<BlockIndex> blocks;
    struct Idx1Entry {
        uint32_t flags, offset, size;
    };
    std::vector<Idx1Entry> idx1;
    bool have_idx1 = false;
    struct Super {
        uint64_t off;
        uint32_t size, duration;
    };
    std::vector<Super> super;
    uint64_t first_movi_fourcc = 0;

    uint64_t pos = 0;
    bool first_riff = true;
    while (pos + 12 <= r.size()) {
        uint8_t rh[12];
        if (!r.read(pos, rh, 12) || std::memcmp(rh, "RIFF", 4) != 0) {
            if (first_riff) {
                problem("not a RIFF file");
                return scan;
            }
            problem("unexpected data after the last RIFF block at offset " + std::to_string(pos));
            break;
        }
        const uint32_t riff_size = rd32(rh + 4);
        if (first_riff && std::memcmp(rh + 8, "AVI ", 4) != 0) {
            problem("RIFF type is not 'AVI '");
            return scan;
        }
        if (!first_riff && std::memcmp(rh + 8, "AVIX", 4) != 0) problem("extension RIFF block is not 'AVIX' at offset " + std::to_string(pos));
        uint64_t riff_end = pos + 8 + riff_size;
        if (riff_end > r.size()) {
            problem("RIFF block at " + std::to_string(pos) + " claims to end at " + std::to_string(riff_end) + " but the file ends at " +
                    std::to_string(r.size()) + " (unfinished recording?)");
            riff_end = r.size();
        }
        ++scan.riff_blocks;

        uint64_t p = pos + 12;
        while (p + 8 <= riff_end) {
            ChunkHead h;
            if (!read_head(r, p, &h)) break;
            const uint64_t body = p + 8;
            const uint64_t next = body + h.size + (h.size & 1);
            if (std::memcmp(h.id, "LIST", 4) == 0) {
                char type[5] = {};
                r.read(body, type, 4);
                const uint64_t list_end = (std::min)(body + h.size, riff_end);
                if (std::strcmp(type, "hdrl") == 0 && first_riff) {
                    uint64_t q = body + 4;
                    while (q + 8 <= list_end) {
                        ChunkHead c;
                        if (!read_head(r, q, &c)) break;
                        const uint64_t cb = q + 8;
                        if (std::strcmp(c.id, "avih") == 0 && c.size >= 56) {
                            uint8_t a[56];
                            r.read(cb, a, 56);
                            scan.info.avih_frames = rd32(a + 16);
                        } else if (std::strcmp(c.id, "LIST") == 0) {
                            char t2[5] = {};
                            r.read(cb, t2, 4);
                            uint64_t s = cb + 4;
                            const uint64_t se = cb + c.size;
                            while (s + 8 <= se) {
                                ChunkHead sc;
                                if (!read_head(r, s, &sc)) break;
                                const uint64_t sb = s + 8;
                                if (std::strcmp(t2, "strl") == 0) {
                                    if (std::strcmp(sc.id, "strh") == 0 && sc.size >= 56) {
                                        uint8_t a[56];
                                        r.read(sb, a, 56);
                                        std::memcpy(scan.info.handler, a + 4, 4);
                                        scan.info.fps_den = rd32(a + 20);
                                        scan.info.fps_num = rd32(a + 24);
                                        scan.info.strh_length = rd32(a + 32);
                                    } else if (std::strcmp(sc.id, "strf") == 0 && sc.size >= 40) {
                                        std::vector<uint8_t> a(sc.size);
                                        r.read(sb, a.data(), sc.size);
                                        scan.info.width = rd32(a.data() + 4);
                                        scan.info.height = rd32(a.data() + 8);
                                        if (sc.size >= 72) {
                                            std::memcpy(scan.info.sequence_header, a.data() + 40, 32);
                                            scan.info.has_sequence_header = true;
                                        }
                                    } else if (std::strcmp(sc.id, "indx") == 0 && sc.size >= 24) {
                                        std::vector<uint8_t> a(sc.size);
                                        r.read(sb, a.data(), sc.size);
                                        const uint32_t n = rd32(a.data() + 4);
                                        scan.superindex_entries = n;
                                        for (uint32_t i = 0; i < n && 24 + size_t(i) * 16 + 16 <= a.size(); ++i)
                                            super.push_back({rd64(a.data() + 24 + i * 16), rd32(a.data() + 32 + i * 16), rd32(a.data() + 36 + i * 16)});
                                    }
                                } else if (std::strcmp(t2, "odml") == 0 && std::strcmp(sc.id, "dmlh") == 0 && sc.size >= 4) {
                                    uint8_t a[4];
                                    r.read(sb, a, 4);
                                    scan.info.dmlh_frames = rd32(a);
                                }
                                s = sb + sc.size + (sc.size & 1);
                            }
                        }
                        q = cb + c.size + (c.size & 1);
                    }
                } else if (std::strcmp(type, "INFO") == 0 && first_riff) {
                    uint64_t q = body + 4;
                    while (q + 8 <= list_end) {
                        ChunkHead c;
                        if (!read_head(r, q, &c)) break;
                        std::string text(c.size, '\0');
                        r.read(q + 8, text.data(), c.size);
                        while (!text.empty() && text.back() == '\0') text.pop_back();
                        if (std::strcmp(c.id, "ISFT") == 0) scan.info.software = text;
                        if (std::strcmp(c.id, "ICMT") == 0) scan.info.comment = text;
                        q += 8 + c.size + (c.size & 1);
                    }
                } else if (std::strcmp(type, "movi") == 0) {
                    BlockIndex blk;
                    blk.movi_data_start = body + 4;
                    blk.first_chunk = scan.video.size();
                    if (first_riff) first_movi_fourcc = body;
                    uint64_t q = body + 4;
                    while (q + 8 <= list_end) {
                        ChunkHead c;
                        if (!read_head(r, q, &c)) break;
                        const uint64_t cb = q + 8;
                        if (cb + c.size > list_end) {
                            problem("chunk '" + std::string(c.id) + "' at " + std::to_string(q) + " runs past the end of its list (truncated?)");
                            break;
                        }
                        if (std::strcmp(c.id, "00dc") == 0 || std::strcmp(c.id, "00db") == 0) {
                            AviChunk ch;
                            ch.data_offset = cb;
                            ch.size = c.size;
                            scan.video.push_back(ch);
                        } else if (std::strcmp(c.id, "ix00") == 0 && c.size >= 24) {
                            blk.ix_pos = q;
                            std::vector<uint8_t> a(c.size);
                            r.read(cb, a.data(), c.size);
                            const uint32_t n = rd32(a.data() + 4);
                            blk.ix_base = rd64(a.data() + 12);
                            for (uint32_t i = 0; i < n && 24 + size_t(i) * 8 + 8 <= a.size(); ++i)
                                blk.ix.push_back({rd32(a.data() + 24 + i * 8), rd32(a.data() + 28 + i * 8)});
                        }
                        q = cb + c.size + (c.size & 1);
                    }
                    blk.chunk_count = scan.video.size() - blk.first_chunk;
                    blocks.push_back(std::move(blk));
                }
            } else if (std::strcmp(h.id, "idx1") == 0 && first_riff) {
                have_idx1 = true;
                std::vector<uint8_t> a(h.size);
                r.read(body, a.data(), h.size);
                for (size_t i = 0; i + 16 <= a.size(); i += 16) idx1.push_back({rd32(a.data() + i + 4), rd32(a.data() + i + 8), rd32(a.data() + i + 12)});
            }
            if (next <= p) break;
            p = next;
        }
        first_riff = false;
        pos = riff_end;
        if (riff_end >= r.size()) break;
    }

    scan.ok = scan.info.width != 0 && !blocks.empty();
    if (!scan.ok) {
        problem("no usable header or movi list found");
        return scan;
    }
    if (std::strcmp(scan.info.handler, "RCV1") != 0) problem(std::string("video codec is '") + scan.info.handler + "', not RCV1");

    // Cross-checks: counts.
    const uint64_t total = scan.video.size();
    if (scan.info.dmlh_frames != total) problem("dmlh says " + std::to_string(scan.info.dmlh_frames) + " frames, the file has " + std::to_string(total));
    if (scan.info.strh_length != total) problem("strh says " + std::to_string(scan.info.strh_length) + " frames, the file has " + std::to_string(total));
    if (scan.info.avih_frames != blocks[0].chunk_count)
        problem("avih says " + std::to_string(scan.info.avih_frames) + " frames in the first block, it has " + std::to_string(blocks[0].chunk_count));

    // Super index against the blocks.
    if (super.size() != blocks.size()) {
        problem("super index has " + std::to_string(super.size()) + " entries for " + std::to_string(blocks.size()) + " blocks");
    }
    for (size_t i = 0; i < blocks.size() && i < super.size(); ++i) {
        if (blocks[i].ix_pos != super[i].off) problem("super index entry " + std::to_string(i) + " points at " + std::to_string(super[i].off) + ", the ix00 is at " + std::to_string(blocks[i].ix_pos));
        if (super[i].duration != blocks[i].chunk_count) problem("super index entry " + std::to_string(i) + " says " + std::to_string(super[i].duration) + " frames, the block has " + std::to_string(blocks[i].chunk_count));
    }

    // Standard indexes against the chunks (and take the keyframe flags from them).
    for (size_t b = 0; b < blocks.size(); ++b) {
        const BlockIndex& blk = blocks[b];
        if (blk.ix_pos == 0) {
            problem("block " + std::to_string(b) + " has no ix00 index");
            continue;
        }
        if (blk.ix.size() != blk.chunk_count) {
            problem("block " + std::to_string(b) + ": ix00 lists " + std::to_string(blk.ix.size()) + " chunks, the block has " + std::to_string(blk.chunk_count));
            continue;
        }
        for (uint64_t i = 0; i < blk.chunk_count; ++i) {
            AviChunk& ch = scan.video[blk.first_chunk + i];
            const BlockIndex::Entry& e = blk.ix[i];
            const uint32_t size = e.size_flags & 0x7FFFFFFFu;
            if (blk.ix_base + e.off != ch.data_offset || size != ch.size) {
                problem("block " + std::to_string(b) + ": ix00 entry " + std::to_string(i) + " does not match the chunk in the file");
                break;
            }
            ch.key = !(e.size_flags & 0x80000000u);
        }
    }

    // Legacy index against the first block.
    if (!have_idx1) {
        problem("no idx1 index");
    } else if (idx1.size() != blocks[0].chunk_count) {
        problem("idx1 lists " + std::to_string(idx1.size()) + " chunks, the first block has " + std::to_string(blocks[0].chunk_count));
    } else {
        for (uint64_t i = 0; i < blocks[0].chunk_count; ++i) {
            const AviChunk& ch = scan.video[i];
            const uint64_t expect = ch.data_offset - 8 - first_movi_fourcc;
            if (idx1[i].offset != expect || idx1[i].size != ch.size || bool(idx1[i].flags & kAviifKeyframe) != ch.key) {
                problem("idx1 entry " + std::to_string(i) + " does not match the chunk in the file");
                break;
            }
        }
    }
    return scan;
}

AviFile::~AviFile() {
    if (file_) std::fclose(static_cast<FILE*>(file_));
}

bool AviFile::open(const std::filesystem::path& path) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return false;
    std::setvbuf(f, nullptr, _IOFBF, 1 << 20);
    file_ = f;
    return true;
}

bool AviFile::read(uint64_t offset, uint32_t size, std::vector<uint8_t>* out) {
    FILE* f = static_cast<FILE*>(file_);
    out->resize(size);
    if (_fseeki64(f, int64_t(offset), SEEK_SET) != 0) return false;
    return std::fread(out->data(), 1, size, f) == size;
}

}  // namespace rec
