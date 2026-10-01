#include "crc32c.h"

#include <array>

#include "cpu.h"

namespace rcv {
namespace {

constexpr std::array<uint32_t, 256> make_table() {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? (c >> 1) ^ 0x82F63B78u : c >> 1;
        t[i] = c;
    }
    return t;
}

constexpr std::array<uint32_t, 256> kTable = make_table();

// Chosen once at load time; the checksum is the same either way.
const bool kHasSse42 = cpu_has_sse42();

}  // namespace

uint32_t crc32c_scalar(const uint8_t* data, size_t size) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) c = kTable[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return ~c;
}

uint32_t crc32c(const uint8_t* data, size_t size) {
    return kHasSse42 ? crc32c_sse42(data, size) : crc32c_scalar(data, size);
}

}  // namespace rcv
