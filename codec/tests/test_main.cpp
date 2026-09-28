// Runs every registered test. Optional argument: substring filter on test names.
#include <chrono>
#include <cstring>

#include "testfw.h"

int main(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int run = 0, failed_tests = 0;
    for (const tf::Test& t : tf::registry()) {
        if (filter && !std::strstr(t.name, filter)) continue;
        const int before = tf::failures();
        const auto t0 = std::chrono::steady_clock::now();
        t.fn();
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        const bool ok = tf::failures() == before;
        std::printf("[%s] %s (%.0f ms)\n", ok ? " OK " : "FAIL", t.name, ms);
        ++run;
        if (!ok) ++failed_tests;
    }
    std::printf("\n%d test(s), %d failed, %d failed check(s)\n", run, failed_tests, tf::failures());
    return failed_tests ? 1 : 0;
}
