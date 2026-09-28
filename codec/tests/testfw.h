// Minimal self-contained test harness (see docs/DECISIONS.md, D-003).
#pragma once

#include <cstdint>
#include <cstdio>
#include <vector>

namespace tf {

struct Test {
    const char* name;
    void (*fn)();
};

inline std::vector<Test>& registry() {
    static std::vector<Test> r;
    return r;
}
inline int& failures() {
    static int f = 0;
    return f;
}
inline void report(const char* file, int line, const char* expr) {
    if (++failures() <= 50) std::fprintf(stderr, "%s(%d): CHECK failed: %s\n", file, line, expr);
}

struct Registrar {
    Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

// Deterministic PRNG (splitmix64) so every run tests the same data.
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed) {}
    uint64_t next() {
        uint64_t z = (s += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    uint32_t below(uint32_t n) { return uint32_t(next() % n); }
    uint8_t byte() { return uint8_t(next()); }
};

}  // namespace tf

#define TF_CAT2(a, b) a##b
#define TF_CAT(a, b) TF_CAT2(a, b)
#define TEST_CASE(name)                                                                  \
    static void TF_CAT(tf_test_, __LINE__)();                                            \
    static tf::Registrar TF_CAT(tf_reg_, __LINE__)(name, TF_CAT(tf_test_, __LINE__));    \
    static void TF_CAT(tf_test_, __LINE__)()

#define CHECK(cond)                                          \
    do {                                                     \
        if (!(cond)) tf::report(__FILE__, __LINE__, #cond);  \
    } while (0)

#define REQUIRE(cond)                                   \
    do {                                                \
        if (!(cond)) {                                  \
            tf::report(__FILE__, __LINE__, #cond);      \
            return;                                     \
        }                                               \
    } while (0)
