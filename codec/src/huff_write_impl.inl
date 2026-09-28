// Body of the fast Huffman writer, compiled once per ISA level (huffman.cpp at the x64 baseline,
// huffman_avx2.cpp with /arch:AVX2 so shifts become BMI2 shrx). Internal linkage on purpose:
// each translation unit gets its own copy, so the linker can never mix the two.
// Needs <cstring>, <stdlib.h> and huffman.h included first.

namespace {

size_t huff_write_impl(const uint8_t* syms, size_t count, const rcv::HuffEncTable& t, uint8_t* dst,
                       uint8_t* limit) {
    // Left-aligned accumulator: `bits` valid bits at the top, zeros below. Codes are stored
    // left-aligned, so a symbol costs one shift, one OR and one add; the only loop-carried
    // dependencies are the OR into `acc` and the add into `bits`.
    uint8_t* p = dst;
    uint64_t acc = 0;
    unsigned bits = 0;
    auto put = [&](uint8_t s) {
        acc |= t.code[s] >> bits;
        bits += t.len[s];
    };
    // Stores the whole bytes (plus the partial one) and keeps < 8 bits pending.
    auto flush = [&]() {
        if (limit - p >= 8) {
            const uint64_t be = _byteswap_uint64(acc);
            std::memcpy(p, &be, 8);
        } else {
            const unsigned nbytes = (bits + 7) >> 3;
            for (unsigned k = 0; k < nbytes; ++k) p[k] = uint8_t(acc >> (56 - 8 * k));
        }
        p += bits >> 3;
        acc <<= (bits & ~7u);
        bits &= 7;
    };
    size_t k = 0;
    // <= 7 pending bits + 4 x 12-bit codes = 55 bits: always fits in the accumulator.
    for (; k + 4 <= count; k += 4) {
        put(syms[k]);
        put(syms[k + 1]);
        put(syms[k + 2]);
        put(syms[k + 3]);
        flush();
    }
    for (; k < count; ++k) {
        put(syms[k]);
        flush();
    }
    // The last flush already stored the partial byte (zero-padded) at p[0].
    return size_t(p - dst) + (bits ? 1 : 0);
}

}  // namespace
