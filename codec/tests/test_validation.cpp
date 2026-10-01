// Decoder validation, rule by rule (codec plan §6.5). Each case edits one field of a valid packet and
// rebuilds it consistently (payload size, directory), so the rule under test is the one that fires.
#include <algorithm>
#include <vector>

#include "crc32c.h"
#include "format.h"
#include "test_util.h"

using namespace tu;
using rcv::get_u32;
using rcv::put_u32;

namespace {

using Bytes = std::vector<uint8_t>;

// A packet split into its parts (§6.3).
struct Parts {
    Bytes hdr;                // 32 bytes
    Bytes map;                // P-frames only
    std::vector<Bytes> chunk;  // 3 x S, plane-major
};

Parts split(const Bytes& p, size_t map_size = 0) {
    Parts r;
    r.hdr.assign(p.begin(), p.begin() + 32);
    r.map.assign(p.begin() + 32, p.begin() + 32 + ptrdiff_t(map_size));
    const size_t n = size_t(3) * p[11];
    const size_t dir = 32 + map_size;
    size_t off = dir + 4 * n;
    for (size_t k = 0; k < n; ++k) {
        const size_t cs = get_u32(&p[dir + 4 * k]);
        r.chunk.emplace_back(p.begin() + ptrdiff_t(off), p.begin() + ptrdiff_t(off + cs));
        off += cs;
    }
    return r;
}

// Rebuilds the packet: directory from the chunk sizes (or `dir` if given), payload size, CRC.
Bytes join(const Parts& r, const std::vector<uint32_t>& dir = {}) {
    Bytes o = r.hdr;
    o.insert(o.end(), r.map.begin(), r.map.end());
    for (size_t k = 0; k < r.chunk.size(); ++k) {
        uint8_t b[4];
        put_u32(b, dir.empty() ? uint32_t(r.chunk[k].size()) : dir[k]);
        o.insert(o.end(), b, b + 4);
    }
    for (const Bytes& c : r.chunk) o.insert(o.end(), c.begin(), c.end());
    put_u32(&o[24], uint32_t(o.size() - 32));
    if (rcv::get_u16(&o[6]) & rcv::kFlagCrc) put_u32(&o[28], rcv::crc32c(&o[32], o.size() - 32));
    return o;
}

std::vector<uint32_t> sizes(const Parts& r) {
    std::vector<uint32_t> d;
    for (const Bytes& c : r.chunk) d.push_back(uint32_t(c.size()));
    return d;
}

struct Dec {
    rcv_decoder* d = nullptr;
    explicit Dec(const rcv_encoder_config& cfg) {
        uint8_t seq[32];
        rcv_write_sequence_header(&cfg, seq);
        rcv_decoder_create(seq, 1, &d);
    }
    ~Dec() { rcv_decoder_destroy(d); }
    rcv_status operator()(const Bytes& p) const {
        return rcv_decode_frame(d, p.data(), p.size(), RCV_OUT_I420, nullptr, nullptr, nullptr);
    }
};

Bytes with(Bytes p, size_t off, uint8_t v) {
    p[off] = v;
    return p;
}

// First chunk with the given mode, or -1.
int find_mode(const Parts& r, uint8_t mode) {
    for (size_t k = 0; k < r.chunk.size(); ++k)
        if (r.chunk[k][0] == mode) return int(k);
    return -1;
}

}  // namespace

TEST_CASE("validation: frame header fields (§6.5)") {
    const rcv_encoder_config cfg = config_for(64, 48, 1, 3);
    const Dec dec(cfg);
    const Bytes good = encode_one(cfg, make_yuv(64, 48, Content::Natural, 1));
    REQUIRE(!good.empty());
    REQUIRE(dec(good) == RCV_OK);
    CHECK(dec(join(split(good))) == RCV_OK);  // the editor itself round-trips

    CHECK(dec(with(good, 8, RCV_FMT_GBR)) == RCV_ERR_BITSTREAM);  // format differs from the sequence
    CHECK(dec(with(good, 8, 2)) == RCV_ERR_BITSTREAM);            // unknown format
    CHECK(dec(with(good, 14, 32)) == RCV_ERR_BITSTREAM);          // height differs
    Bytes p = with(good, 6, rcv::kFlagNear);
    CHECK(dec(with(p, 9, 4)) == RCV_ERR_BITSTREAM);  // NEAR > 3
    CHECK(dec(p) == RCV_ERR_BITSTREAM);              // near flag with NEAR 0
    CHECK(dec(with(p, 9, 2)) == RCV_OK);             // consistent: decodes (as near-lossless)
    CHECK(dec(with(good, 17, uint8_t(good[17] | 0x80))) == RCV_ERR_BITSTREAM);  // reserved colour bit
    CHECK(dec(with(good, 18, 1)) == RCV_ERR_BITSTREAM);                          // reserved
    CHECK(dec(with(good, 19, 1)) == RCV_ERR_BITSTREAM);                          // reserved
    p = good;
    p.push_back(0);  // one byte more than header + payload size
    CHECK(dec(p) == RCV_ERR_BITSTREAM);
    CHECK(dec(good) == RCV_OK);
}

TEST_CASE("validation: sequence header reserved fields (§6.1)") {
    const rcv_encoder_config cfg = config_for(64, 48);
    uint8_t seq[32];
    rcv_write_sequence_header(&cfg, seq);
    rcv_sequence_info info{};
    REQUIRE(rcv_parse_sequence_header(seq, &info) == RCV_OK);
    for (int off : {14, 15, 26, 27, 28, 29, 30, 31}) {
        uint8_t bad[32];
        std::memcpy(bad, seq, 32);
        bad[off] = uint8_t(bad[off] | 0x80);
        CHECK(rcv_parse_sequence_header(bad, &info) == RCV_ERR_BITSTREAM);
        rcv_decoder* d = nullptr;
        CHECK(rcv_decoder_create(bad, 1, &d) == RCV_ERR_BITSTREAM);
        CHECK(d == nullptr);
    }
}

TEST_CASE("validation: slice count limits (S <= 64, S <= blocks_y)") {
    const rcv_encoder_config cfg = config_for(32, 1056, 1, 64);  // 66 block rows
    const Dec dec(cfg);
    const Bytes good = encode_one(cfg, make_yuv(32, 1056, Content::Natural, 2));
    REQUIRE(!good.empty());
    CHECK(good[11] == 64);
    CHECK(dec(good) == RCV_OK);
    CHECK(dec(with(good, 11, 65)) == RCV_ERR_BITSTREAM);
    CHECK(dec(with(good, 11, 255)) == RCV_ERR_BITSTREAM);
}

TEST_CASE("validation: NEAR is rejected for GBR") {
    rcv_encoder_config cfg = config_for(32, 32);
    cfg.format = RCV_FMT_GBR;
    cfg.input_layout = RCV_IN_BGRA;
    rcv_encoder* enc = nullptr;
    REQUIRE(rcv_encoder_create(&cfg, &enc) == RCV_OK);
    Bytes px(32 * 32 * 4);
    tf::Rng rng(3);
    for (auto& b : px) b = uint8_t(rng.below(4));
    rcv_frame_in in{};
    in.plane[0] = px.data();
    in.stride[0] = 32 * 4;
    Bytes out(rcv_max_packet_size(&cfg));
    rcv_frame_info fi{};
    REQUIRE(rcv_encode_frame(enc, &in, nullptr, out.data(), out.size(), &fi) == RCV_OK);
    rcv_encoder_destroy(enc);
    out.resize(fi.packet_size);

    uint8_t seq[32];
    rcv_write_sequence_header(&cfg, seq);
    rcv_decoder* d = nullptr;
    REQUIRE(rcv_decoder_create(seq, 1, &d) == RCV_OK);
    auto decode = [&](const Bytes& p) { return rcv_decode_frame(d, p.data(), p.size(), RCV_OUT_BGRA, nullptr, nullptr, nullptr); };
    CHECK(decode(out) == RCV_OK);
    CHECK(decode(with(with(out, 6, rcv::kFlagNear), 9, 1)) == RCV_ERR_BITSTREAM);
    CHECK(decode(with(out, 8, RCV_FMT_YUV420)) == RCV_ERR_BITSTREAM);
    rcv_decoder_destroy(d);
}

TEST_CASE("validation: chunk directory (§6.3)") {
    const rcv_encoder_config cfg = config_for(64, 48, 1, 3);
    const Dec dec(cfg);
    const Bytes good = encode_one(cfg, make_yuv(64, 48, Content::Natural, 4));
    REQUIRE(!good.empty());
    const Parts r = split(good);
    const std::vector<uint32_t> d0 = sizes(r);
    CHECK(dec(join(r, d0)) == RCV_OK);

    std::vector<uint32_t> d = d0;  // same total, sizes not multiples of 4
    d[0] += 2;
    d[1] -= 2;
    CHECK(dec(join(r, d)) == RCV_ERR_BITSTREAM);
    d = d0;  // a size below the 4-byte chunk header
    d[1] += d[0];
    d[0] = 0;
    CHECK(dec(join(r, d)) == RCV_ERR_BITSTREAM);
    d = d0;  // sizes whose 32-bit sum wraps back to the right total
    d[0] += 0x80000000u;
    d[1] -= 0x80000000u;
    CHECK(dec(join(r, d)) == RCV_ERR_BITSTREAM);
    d = d0;  // total larger than the payload
    d[8] += 4;
    CHECK(dec(join(r, d)) == RCV_ERR_BITSTREAM);

    Bytes shortp(good.begin(), good.begin() + 32 + 8);  // payload smaller than the directory
    put_u32(&shortp[24], 8);
    CHECK(dec(shortp) == RCV_ERR_BITSTREAM);
    CHECK(dec(good) == RCV_OK);
}

TEST_CASE("validation: chunk modes and code-length tables (§6.4)") {
    const rcv_encoder_config cfg = config_for(64, 48, 1, 1);
    const Dec dec(cfg);
    const Bytes huff = encode_one(cfg, make_yuv(64, 48, Content::Natural, 5));
    const Bytes raw = encode_one(cfg, make_yuv(64, 48, Content::Noise, 5));
    Yuv flat = make_yuv(64, 48, Content::Zero, 5);
    for (auto& plane : flat.p) std::fill(plane.begin(), plane.end(), uint8_t(128));  // residuals all 0
    const Bytes single = encode_one(cfg, flat);
    REQUIRE(!huff.empty());
    REQUIRE(!raw.empty());
    REQUIRE(!single.empty());
    const Parts ph = split(huff), pr = split(raw), ps = split(single);
    const int kh = find_mode(ph, rcv::kChunkHuffman), kr = find_mode(pr, rcv::kChunkRaw),
              ks = find_mode(ps, rcv::kChunkSingle);
    REQUIRE(kh >= 0);
    REQUIRE(kr >= 0);
    REQUIRE(ks >= 0);
    CHECK(dec(join(ph)) == RCV_OK);
    CHECK(dec(join(pr)) == RCV_OK);
    CHECK(dec(join(ps)) == RCV_OK);

    auto edit = [&](const Parts& base, int k, auto fn) {
        Parts m = base;
        fn(m.chunk[size_t(k)]);
        return dec(join(m));
    };
    // Header bytes.
    CHECK(edit(ph, kh, [](Bytes& c) { c[0] = 4; }) == RCV_ERR_BITSTREAM);  // unknown mode
    CHECK(edit(ph, kh, [](Bytes& c) { c[1] = 1; }) == RCV_ERR_BITSTREAM);  // symbol byte on HUFFMAN
    CHECK(edit(ph, kh, [](Bytes& c) { c[2] = 1; }) == RCV_ERR_BITSTREAM);  // reserved
    CHECK(edit(ph, kh, [](Bytes& c) { c[3] = 1; }) == RCV_ERR_BITSTREAM);
    CHECK(edit(pr, kr, [](Bytes& c) { c[1] = 1; }) == RCV_ERR_BITSTREAM);

    // Code-length tables: over-subscribed, incomplete, and a complete code with a length of 13.
    auto nibble = [](Bytes& c, int s) -> int { return (c[4 + s / 2] >> (4 * (s & 1))) & 0xF; };
    auto set_len = [](Bytes& c, int s, int len) {
        uint8_t& b = c[4 + size_t(s) / 2];
        b = uint8_t((s & 1) ? (b & 0x0F) | (len << 4) : (b & 0xF0) | len);
    };
    CHECK(edit(ph, kh, [&](Bytes& c) {
              for (int s = 0; s < 256; ++s)
                  if (!nibble(c, s)) {
                      set_len(c, s, 1);
                      break;
                  }
          }) == RCV_ERR_BITSTREAM);
    CHECK(edit(ph, kh, [&](Bytes& c) {
              for (int s = 0; s < 256; ++s)
                  if (nibble(c, s)) {
                      set_len(c, s, 0);
                      break;
                  }
          }) == RCV_ERR_BITSTREAM);
    CHECK(edit(ph, kh, [&](Bytes& c) {
              std::fill(c.begin() + 4, c.begin() + 4 + 128, uint8_t(0));
              for (int s = 0; s < 12; ++s) set_len(c, s, s + 1);  // Kraft sum: 1 - 2^-12 ...
              set_len(c, 12, 13);                                  // ... + 2 x 2^-13 = 1
              set_len(c, 13, 13);
          }) == RCV_ERR_BITSTREAM);
    CHECK(edit(ph, kh, [&](Bytes& c) {  // one symbol, length 1: incomplete
              std::fill(c.begin() + 4, c.begin() + 4 + 128, uint8_t(0));
              set_len(c, 0, 1);
          }) == RCV_ERR_BITSTREAM);

    // Sizes that don't match the coded samples.
    CHECK(edit(ph, kh, [](Bytes& c) { c.resize(c.size() - 4); }) == RCV_ERR_BITSTREAM);  // bitstream ends early
    CHECK(edit(ph, kh, [](Bytes& c) { c.resize(c.size() + 4); }) == RCV_ERR_BITSTREAM);  // trailing bytes
    CHECK(edit(ph, kh, [](Bytes& c) { c.resize(4 + 128); }) == RCV_ERR_BITSTREAM);       // table only
    CHECK(edit(pr, kr, [](Bytes& c) { c.resize(c.size() + 4); }) == RCV_ERR_BITSTREAM);
    CHECK(edit(pr, kr, [](Bytes& c) { c.resize(c.size() - 4); }) == RCV_ERR_BITSTREAM);
    CHECK(edit(ps, ks, [](Bytes& c) { c.resize(8); }) == RCV_ERR_BITSTREAM);
    CHECK(edit(ps, ks, [](Bytes& c) { c[0] = rcv::kChunkEmpty, c[1] = 0; }) == RCV_ERR_BITSTREAM);  // EMPTY with samples
    CHECK(dec(huff) == RCV_OK);
}

TEST_CASE("validation: P-frame skip map and reference checks") {
    rcv_encoder_config cfg = config_for(64, 48, 1, 3);
    cfg.enable_skip = 1;
    rcv_encoder* enc = nullptr;
    REQUIRE(rcv_encoder_create(&cfg, &enc) == RCV_OK);
    const Yuv a = make_yuv(64, 48, Content::Natural, 6);
    Yuv b = a;
    for (int y = 16; y < 32; ++y)
        for (int x = 16; x < 32; ++x) b.at(0, x, y) = uint8_t(b.at(0, x, y) + 9);  // one block changes
    Bytes buf(rcv_max_packet_size(&cfg));
    std::vector<Bytes> pk;
    for (const Yuv* f : {&a, static_cast<const Yuv*>(&b)}) {
        const rcv_frame_in in = frame_in(*f);
        rcv_frame_info fi{};
        REQUIRE(rcv_encode_frame(enc, &in, nullptr, buf.data(), buf.size(), &fi) == RCV_OK);
        pk.emplace_back(buf.begin(), buf.begin() + fi.packet_size);
    }
    rcv_encoder_destroy(enc);
    REQUIRE(pk[1][5] == rcv::kFrameP);

    const Dec dec(cfg);
    REQUIRE(dec(pk[0]) == RCV_OK);
    rcv::Geometry g;
    REQUIRE(rcv::init_geometry(&g, RCV_FMT_YUV420, 64, 48, 3));
    const size_t map_size = rcv::skip_map_size(g);  // 12 blocks -> 2 bytes, padded to 4
    REQUIRE(map_size == 4);
    const Parts r = split(pk[1], map_size);
    CHECK(dec(join(r)) == RCV_OK);  // the editor round-trips a P-frame

    Parts m = r;
    m.map[1] |= 0x80;  // bit for block 15 of 12
    CHECK(dec(join(m)) == RCV_ERR_BITSTREAM);
    m = r;
    m.map[3] = 1;  // padding byte
    CHECK(dec(join(m)) == RCV_ERR_BITSTREAM);
    // Header checks run before the reference is touched: it is still valid for a DUP.
    Bytes dup(8);
    rcv::write_dup_packet(dup.data());
    CHECK(dec(dup) == RCV_OK);

    Bytes t(pk[1].begin(), pk[1].begin() + 32 + 2);  // payload shorter than the skip map
    put_u32(&t[24], 2);
    CHECK(dec(t) == RCV_ERR_BITSTREAM);
    CHECK(dec(with(pk[1], 12, 32)) == RCV_ERR_BITSTREAM);  // dimensions differ from the reference
    CHECK(dec(with(pk[1], 8, RCV_FMT_GBR)) == RCV_ERR_BITSTREAM);

    // A valid P-frame still decodes exactly afterwards.
    Yuv out = blank_like(b);
    CHECK(decode_into(dec.d, pk[1], out) == RCV_OK);
    CHECK(same_image(b, out));
}
