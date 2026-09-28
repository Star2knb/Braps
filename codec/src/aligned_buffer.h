// 64-byte aligned allocations, only ever made in *_create (codec plan §9).
#pragma once

#include <cstddef>
#include <new>

namespace rcv {

constexpr size_t kAlign = 64;

constexpr size_t align64(size_t n) { return (n + kAlign - 1) & ~(kAlign - 1); }

inline void* aligned_alloc64(size_t size) {
    return ::operator new(size, std::align_val_t(kAlign), std::nothrow);
}

inline void aligned_free64(void* p) {
    if (p) ::operator delete(p, std::align_val_t(kAlign));
}

}  // namespace rcv
