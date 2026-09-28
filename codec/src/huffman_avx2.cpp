// AVX2-level (BMI2) build of the fast Huffman writer. Compiled with /arch:AVX2 so the variable
// shifts become shrx; only reached through the dispatch table after CPUID checks AVX2 + BMI2.
#include <stdlib.h>  // _byteswap_uint64

#include <cstring>

#include "huffman.h"

namespace rcv {

#include "huff_write_impl.inl"

size_t huff_write_avx2(const uint8_t* syms, size_t count, const HuffEncTable& t, uint8_t* dst, uint8_t* limit) {
    return huff_write_impl(syms, count, t, dst, limit);
}

}  // namespace rcv
