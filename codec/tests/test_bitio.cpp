#include <vector>

#include "bitio.h"
#include "crc32c.h"
#include "testfw.h"

using namespace rcv;

TEST_CASE("bitio: MSB-first layout") {
    uint8_t buf[8] = {};
    BitWriter bw(buf);
    bw.put(1, 1);
    bw.put(0, 1);
    bw.put(0x3F, 6);   // byte 0 = 1011 1111
    bw.put(0x5, 3);    // byte 1 = 101 + padding 00000
    CHECK(bw.finish() == 2);
    CHECK(buf[0] == 0xBF);
    CHECK(buf[1] == 0xA0);
}

TEST_CASE("bitio: random round trip") {
    tf::Rng rng(1);
    const int n = 20000;
    std::vector<uint32_t> codes(n);
    std::vector<int> lens(n);
    size_t total_bits = 0;
    for (int i = 0; i < n; ++i) {
        lens[i] = 1 + int(rng.below(12));
        codes[i] = uint32_t(rng.next()) & ((1u << lens[i]) - 1);
        total_bits += size_t(lens[i]);
    }
    std::vector<uint8_t> buf(total_bits / 8 + 16, 0xCC);
    BitWriter bw(buf.data());
    for (int i = 0; i < n; ++i) bw.put(codes[i], lens[i]);
    const size_t bytes = bw.finish();
    CHECK(bytes == (total_bits + 7) / 8);
    if (total_bits % 8) CHECK((buf[bytes - 1] & ((1u << (8 - total_bits % 8)) - 1)) == 0);

    BitReader br(buf.data(), buf.data() + bytes);
    for (int i = 0; i < n; ++i) {
        if (br.count() < 12) br.refill();
        const uint32_t v = br.peek12() >> (12 - lens[i]);
        CHECK(v == codes[i]);
        CHECK(br.consume(lens[i]));
    }
    CHECK(br.bits_consumed() == total_bits);
}

TEST_CASE("bitio: reader never runs past the end") {
    const uint8_t buf[2] = {0xFF, 0xFF};
    BitReader br(buf, buf + 2);
    br.refill();
    CHECK(br.count() == 16);
    CHECK(br.consume(12));
    CHECK(!br.consume(5));
    CHECK(br.consume(4));
    br.refill();
    CHECK(br.count() == 0);
    CHECK(!br.consume(1));
}

TEST_CASE("crc32c: check value") {
    const uint8_t s[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    CHECK(crc32c(s, sizeof(s)) == 0xE3069283u);
    CHECK(crc32c(s, 0) == 0);
}
