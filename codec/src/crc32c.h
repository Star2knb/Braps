// CRC-32C (Castagnoli), used for the optional payload checksum (§6.2).
#pragma once

#include <cstddef>
#include <cstdint>

namespace rcv {

// Best implementation for this CPU: the SSE4.2 crc32 instruction, else the table version.
uint32_t crc32c(const uint8_t* data, size_t size);

uint32_t crc32c_scalar(const uint8_t* data, size_t size);
uint32_t crc32c_sse42(const uint8_t* data, size_t size);  // needs SSE4.2 (cpu_has_sse42)

}  // namespace rcv
