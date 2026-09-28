// CRC-32C (Castagnoli), used for the optional payload checksum (§6.2).
#pragma once

#include <cstddef>
#include <cstdint>

namespace rcv {

// Scalar table-driven version; an SSE4.2 path comes with the hardening milestone.
uint32_t crc32c(const uint8_t* data, size_t size);

}  // namespace rcv
