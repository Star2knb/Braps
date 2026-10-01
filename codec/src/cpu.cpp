#include "cpu.h"

#include <intrin.h>
#include <immintrin.h>

namespace rcv {
namespace {

// XGETBV(0): which register states the OS saves on context switch.
#if defined(__clang__)
__attribute__((target("xsave")))
#endif
unsigned long long read_xcr0() {
    return _xgetbv(0);
}

}  // namespace

rcv_isa detect_isa() {
    int r[4];
    __cpuid(r, 0);
    const int max_leaf = r[0];
    __cpuid(r, 1);
    const bool sse41 = (r[2] >> 19) & 1;
    const bool osxsave = (r[2] >> 27) & 1;
    const bool avx = (r[2] >> 28) & 1;
    bool avx2 = false;
    if (max_leaf >= 7 && osxsave && avx && (read_xcr0() & 6) == 6) {  // XMM and YMM state enabled
        __cpuidex(r, 7, 0);
        // The AVX2 level also uses BMI1/BMI2 (the /arch:AVX2 Huffman writer emits shrx). Every
        // AVX2 CPU from Intel (Haswell+) and AMD (Excavator+) has them; check anyway.
        avx2 = ((r[1] >> 5) & 1) && ((r[1] >> 3) & 1) && ((r[1] >> 8) & 1);
    }
    if (avx2 && sse41) return RCV_ISA_AVX2;
    if (sse41) return RCV_ISA_SSE41;
    return RCV_ISA_SCALAR;
}

bool cpu_has_sse42() {
    int r[4];
    __cpuid(r, 1);
    return (r[2] >> 20) & 1;
}

rcv_status resolve_isa(rcv_isa requested, rcv_isa* out) {
    const rcv_isa best = detect_isa();
    if (requested == RCV_ISA_AUTO) {
        *out = best;
        return RCV_OK;
    }
    if (unsigned(requested) > unsigned(RCV_ISA_AVX2)) return RCV_ERR_INVALID_ARG;
    if (unsigned(requested) > unsigned(best)) return RCV_ERR_UNSUPPORTED;
    *out = requested;
    return RCV_OK;
}

}  // namespace rcv

extern "C" RCV_API rcv_isa rcv_cpu_isa(void) { return rcv::detect_isa(); }
