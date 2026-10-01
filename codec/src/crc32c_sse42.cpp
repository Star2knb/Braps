// SSE4.2 CRC-32C. A single stream of 8-byte crc32 instructions (3-cycle latency) runs at
// ~8 bytes per 3 cycles, about 0.1 ms for a 1 MB payload; the checksum is off by default, so
// the 3-stream interleave with CRC combining is not worth its complexity (D-042).
#include <cstring>
#include <nmmintrin.h>

#include "crc32c.h"

namespace rcv {

uint32_t crc32c_sse42(const uint8_t* data, size_t size) {
    uint32_t c = 0xFFFFFFFFu;
    for (; size && (reinterpret_cast<uintptr_t>(data) & 7); --size) c = _mm_crc32_u8(c, *data++);
    uint64_t c64 = c;
    for (; size >= 8; size -= 8, data += 8) {
        uint64_t v;
        std::memcpy(&v, data, 8);
        c64 = _mm_crc32_u64(c64, v);
    }
    c = uint32_t(c64);
    for (; size; --size) c = _mm_crc32_u8(c, *data++);
    return ~c;
}

}  // namespace rcv
