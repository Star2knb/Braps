// The disk writer and the AVI OpenDML container (recorder plan §11).
#include <windows.h>
#include <winioctl.h>

#include <cstring>
#include <string>
#include <vector>

#include "rec/avi.h"
#include "rec/disk_file.h"
#include "test_util.h"
#include "testfw.h"

using namespace rec;

namespace {

std::vector<uint8_t> bytes_of(const std::filesystem::path& p) {
    const std::string s = rt::read_file(p);
    return std::vector<uint8_t>(s.begin(), s.end());
}

// Deterministic bytes.
void fill(std::vector<uint8_t>* v, uint32_t seed) {
    uint32_t x = seed * 2654435761u + 1;
    for (uint8_t& b : *v) {
        x = x * 1664525u + 1013904223u;
        b = uint8_t(x >> 24);
    }
}

}  // namespace

TEST_CASE("disk file: append, patch across buffers, truncate to the real length") {
    const rt::TempDir dir("diskfile");
    const auto path = dir.path() / "f.bin";
    DiskFile f;
    std::string error;
    REQUIRE(f.open(path, 64 * 1024, &error));  // small buffers: many flushes

    std::vector<uint8_t> model;
    const size_t sizes[] = {1, 4095, 4096, 4097, 100000, 7, 65536, 65535, 300000, 3, 1000000, 12345};
    uint32_t seed = 1;
    for (size_t n : sizes) {
        std::vector<uint8_t> chunk(n);
        fill(&chunk, seed++);
        REQUIRE(f.append(chunk.data(), chunk.size()));
        model.insert(model.end(), chunk.begin(), chunk.end());
        CHECK(f.position() == model.size());
    }
    // Patches: in the first (long written) sectors, across a buffer boundary, and in the unwritten tail.
    auto patch = [&](uint64_t off, size_t n, uint8_t v) {
        std::vector<uint8_t> p(n, v);
        REQUIRE(f.patch(off, p.data(), n));
        std::memcpy(&model[off], p.data(), n);
    };
    patch(0, 100, 0xAA);
    patch(5000, 3, 0xBB);
    patch(64 * 1024 - 10, 20, 0xCC);      // across a buffer boundary
    patch(3 * 64 * 1024 - 1, 2, 0xDD);
    patch(model.size() - 50, 50, 0xEE);   // in the buffer still being filled
    patch(model.size() - 64 * 1024 - 5, 10, 0xF0);
    uint8_t one = 1;
    CHECK(!f.patch(model.size(), &one, 1));
    REQUIRE(f.close(&error));

    const std::vector<uint8_t> got = bytes_of(path);
    REQUIRE(got.size() == model.size());  // truncated to the real length, not to the last sector
    CHECK(std::memcmp(got.data(), model.data(), model.size()) == 0);
    CHECK(f.stats().writes >= 10);
    CHECK(f.stats().bytes_written >= model.size());
}

TEST_CASE("disk file: a checkpoint writes what is buffered and appending carries on") {
    const rt::TempDir dir("diskfile2");
    const auto path = dir.path() / "g.bin";
    DiskFile f;
    std::string error;
    REQUIRE(f.open(path, 64 * 1024, &error));
    std::vector<uint8_t> a(10000), b(70000);
    fill(&a, 5);
    fill(&b, 6);
    REQUIRE(f.append(a.data(), a.size()));
    REQUIRE(f.checkpoint());
    CHECK(std::filesystem::file_size(path) >= a.size());  // on disk now (padded to a sector)
    REQUIRE(f.append(b.data(), b.size()));
    REQUIRE(f.close(&error));
    std::vector<uint8_t> model = a;
    model.insert(model.end(), b.begin(), b.end());
    const std::vector<uint8_t> got = bytes_of(path);
    REQUIRE(got.size() == model.size());
    CHECK(std::memcmp(got.data(), model.data(), model.size()) == 0);
}

TEST_CASE("disk file: a file made in an NTFS-compressed folder is written uncompressed") {
    const rt::TempDir dir("diskfile3");
    // Make the folder compressed (on NTFS; skip the checks where that is not possible).
    HANDLE d = CreateFileW(dir.path().c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    REQUIRE(d != INVALID_HANDLE_VALUE);
    USHORT format = COMPRESSION_FORMAT_DEFAULT;
    DWORD returned = 0;
    const bool compressed_folder = DeviceIoControl(d, FSCTL_SET_COMPRESSION, &format, sizeof(format), nullptr, 0, &returned, nullptr) != 0;
    CloseHandle(d);
    if (!compressed_folder) return;  // not NTFS: nothing to test

    const auto path = dir.path() / "c.bin";
    DiskFile f;
    std::string error;
    REQUIRE(f.open(path, 64 * 1024, &error));
    CHECK(f.compression_removed());
    std::vector<uint8_t> data(200000);
    fill(&data, 9);
    REQUIRE(f.append(data.data(), data.size()));
    REQUIRE(f.close(&error));
    const DWORD attributes = GetFileAttributesW(path.c_str());
    CHECK(attributes != INVALID_FILE_ATTRIBUTES);
    CHECK((attributes & FILE_ATTRIBUTE_COMPRESSED) == 0);
    const std::vector<uint8_t> got = bytes_of(path);
    CHECK(got == data);
}

namespace {

// Writes `frames` synthetic packets and returns them.
void write_avi(const std::filesystem::path& path, int frames, uint64_t block_limit, uint32_t fps_num,
               std::vector<std::vector<uint8_t>>* packets, std::vector<bool>* keys) {
    DiskFile f;
    std::string error;
    REQUIRE(f.open(path, 64 * 1024, &error));
    AviWriter w(f, block_limit);
    AviVideoParams p;
    p.width = 320;
    p.height = 180;
    p.fps_num = fps_num;
    p.fps_den = 1;
    for (int i = 0; i < 32; ++i) p.sequence_header[i] = uint8_t(i);
    p.suggested_buffer = 5000;
    p.software = "rec test";
    p.comment = "source=320x180";
    REQUIRE(w.begin(p, &error));
    for (int i = 0; i < frames; ++i) {
        const bool dup = i % 7 == 3;
        const bool key = i % 30 == 0;
        std::vector<uint8_t> pk(dup ? 8 : size_t(100 + (i * 37) % 3001));  // odd and even sizes
        fill(&pk, uint32_t(i + 100));
        REQUIRE(w.add_video(pk.data(), uint32_t(pk.size()), key));
        packets->push_back(pk);
        keys->push_back(key);
    }
    REQUIRE(w.finish(&error));
    CHECK(w.frames() == uint64_t(frames));
}

}  // namespace

TEST_CASE("avi: one block, indexes agree with the file") {
    const rt::TempDir dir("avi1");
    const auto path = dir.path() / "a.avi";
    std::vector<bool> keys;
    std::vector<std::vector<uint8_t>> packets;
    write_avi(path, 200, 1ull << 30, 60, &packets, &keys);

    const AviScan scan = scan_avi(path);
    REQUIRE(scan.ok);
    for (const std::string& p : scan.problems) std::fprintf(stderr, "problem: %s\n", p.c_str());
    CHECK(scan.problems.empty());
    CHECK(scan.riff_blocks == 1);
    CHECK(scan.info.width == 320 && scan.info.height == 180);
    CHECK(scan.info.fps_num == 60 && scan.info.fps_den == 1);
    CHECK(std::string(scan.info.handler) == "RCV1");
    CHECK(scan.info.has_sequence_header);
    for (int i = 0; i < 32; ++i) CHECK(scan.info.sequence_header[i] == i);
    CHECK(scan.info.software == "rec test");
    CHECK(scan.info.comment == "source=320x180");
    CHECK(scan.info.dmlh_frames == 200 && scan.info.strh_length == 200 && scan.info.avih_frames == 200);
    REQUIRE(scan.video.size() == packets.size());

    AviFile file;
    REQUIRE(file.open(path));
    for (size_t i = 0; i < packets.size(); ++i) {
        CHECK(scan.video[i].key == keys[i]);
        std::vector<uint8_t> got;
        REQUIRE(file.read(scan.video[i].data_offset, scan.video[i].size, &got));
        CHECK(got == packets[i]);
    }
    // The header region is a multiple of 4096 and the first chunk starts right after it.
    CHECK((scan.video[0].data_offset - 8) % 4096 == 0);
}

TEST_CASE("avi: several RIFF blocks (OpenDML) with a super index") {
    const rt::TempDir dir("avi2");
    const auto path = dir.path() / "b.avi";
    std::vector<bool> keys;
    std::vector<std::vector<uint8_t>> packets;
    write_avi(path, 400, 100 * 1024, 30, &packets, &keys);  // ~100 KB per block

    const AviScan scan = scan_avi(path);
    REQUIRE(scan.ok);
    for (const std::string& p : scan.problems) std::fprintf(stderr, "problem: %s\n", p.c_str());
    CHECK(scan.problems.empty());
    CHECK(scan.riff_blocks >= 4);
    CHECK(scan.superindex_entries == scan.riff_blocks);
    CHECK(scan.info.dmlh_frames == 400);
    CHECK(scan.info.avih_frames < 400);  // only the first block's frames
    REQUIRE(scan.video.size() == 400);
    AviFile file;
    REQUIRE(file.open(path));
    for (size_t i = 0; i < packets.size(); i += 17) {
        std::vector<uint8_t> got;
        REQUIRE(file.read(scan.video[i].data_offset, scan.video[i].size, &got));
        CHECK(got == packets[i]);
    }
}

TEST_CASE("avi: an unfinished or damaged file is reported") {
    const rt::TempDir dir("avi3");
    const auto path = dir.path() / "c.avi";
    std::vector<bool> keys;
    std::vector<std::vector<uint8_t>> packets;
    write_avi(path, 100, 1ull << 30, 60, &packets, &keys);
    std::vector<uint8_t> good = bytes_of(path);

    // Cut off in the middle (a crashed recording).
    {
        const auto cut = dir.path() / "cut.avi";
        rt::write_file(cut, std::string(good.begin(), good.begin() + good.size() / 2));
        const AviScan scan = scan_avi(cut);
        CHECK(!scan.problems.empty());
    }
    // A flipped byte in the ix00 index entries.
    {
        std::vector<uint8_t> bad = good;
        const AviScan scan = scan_avi(path);
        // ix00 follows the last video chunk: find it by searching backwards.
        size_t at = 0;
        for (size_t i = bad.size() - 4; i > 0; --i)
            if (std::memcmp(&bad[i], "ix00", 4) == 0) {
                at = i;
                break;
            }
        REQUIRE(at != 0);
        bad[at + 8 + 24 + 8 * 10] ^= 0x01;  // an offset in entry 10
        const auto mangled = dir.path() / "mangled.avi";
        rt::write_file(mangled, std::string(bad.begin(), bad.end()));
        const AviScan s2 = scan_avi(mangled);
        CHECK(!s2.problems.empty());
        (void)scan;
    }
    // Not an AVI at all.
    {
        const auto junk = dir.path() / "junk.avi";
        rt::write_file(junk, std::string(5000, 'x'));
        const AviScan scan = scan_avi(junk);
        CHECK(!scan.ok);
    }
}
