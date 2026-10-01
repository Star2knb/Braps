#include "threadpool.h"

#include <immintrin.h>

#include <chrono>

namespace rcv {
namespace {

// Spins for about 2 µs waiting for `a` to change from `old` (plan §8: avoids wake-up latency when
// the next dispatch or the last job is imminent). Returns true if it changed.
template <class T>
bool spin_until_changed(const std::atomic<T>& a, T old) {
    using Clock = std::chrono::steady_clock;
    const Clock::time_point t0 = Clock::now();
    do {
        for (int k = 0; k < 16; ++k) {
            if (a.load(std::memory_order_acquire) != old) return true;
            _mm_pause();
        }
    } while (Clock::now() - t0 < std::chrono::microseconds(2));
    return false;
}

}  // namespace

int resolve_thread_count(int requested) {
    if (requested <= 0) return std::thread::hardware_concurrency() >= 4 ? 2 : 1;
    return requested > kMaxThreads ? kMaxThreads : requested;
}

bool ThreadPool::start(int threads, StartFn on_start, void* user) {
    stop();
    threads_ = threads < 1 ? 1 : threads > kMaxThreads ? kMaxThreads : threads;
    quit_.store(false);
    started_.store(0);
    try {
        workers_.reserve(size_t(threads_ - 1));
        for (int i = 1; i < threads_; ++i) workers_.emplace_back(&ThreadPool::worker_main, this, i, on_start, user);
    } catch (...) {  // std::system_error / std::bad_alloc: the codec never throws
        stop();
        threads_ = 1;
        return false;
    }
    // Wait until every worker is running: a new thread's start-up allocates (the CRT's per-thread
    // data), and that must be over before *_create returns, not during the first frame (A11).
    for (int s; (s = started_.load()) != threads_ - 1;) started_.wait(s);
    return true;
}

void ThreadPool::stop() {
    if (workers_.empty()) return;
    quit_.store(true);
    wake_.fetch_add(1);
    wake_.notify_all();
    for (std::thread& t : workers_) t.join();
    workers_.clear();
}

void ThreadPool::worker_main(int index, StartFn on_start, void* user) {
    if (on_start) on_start(user, index);
    started_.fetch_add(1);
    started_.notify_all();
    uint32_t seen = 0;
    for (;;) {
        if (!spin_until_changed(wake_, seen)) wake_.wait(seen);
        seen = wake_.load();
        if (quit_.load()) return;
        work(index);
    }
}

// Claims and runs jobs until the current dispatch has none left.
//
// Why a claim is safe: the index and the job count come from the same state word, so a successful
// compare-exchange from (g, count, i) with i < count claims a job that really exists in dispatch g,
// exactly once. Job i of g was unclaimed until that moment, so dispatch g was unfinished for the
// whole time between loading `cur` and the claim; the next run() only starts after every job of g
// has finished, so fn_/ctx_ - loaded in between - are g's (all operations sequentially consistent).
// (An earlier version read the count from a separate atomic, so a late worker could pair an old
// index with a new count and claim a job that didn't exist, corrupting `remaining_`.)
void ThreadPool::work(int worker) {
    uint64_t cur = state_.load();
    for (;;) {
        const uint32_t index = uint32_t(cur & 0xFFFF);
        const uint32_t jobs = uint32_t((cur >> 16) & 0xFFFF);
        if (index >= jobs) return;
        const JobFn fn = fn_.load();
        void* const ctx = ctx_.load();
        if (!state_.compare_exchange_weak(cur, cur + 1)) continue;  // `cur` reloaded; retry
        fn(ctx, int(index), worker);
        if (remaining_.fetch_sub(1) == 1) remaining_.notify_all();
        cur = state_.load();
    }
}

void ThreadPool::run(JobFn fn, void* ctx, int jobs) {
    if (jobs <= 0) return;
    if (jobs > kMaxJobs) jobs = kMaxJobs;  // callers stay far below (3 x 64 slices)
    if (threads_ == 1) {
        for (int j = 0; j < jobs; ++j) fn(ctx, j, 0);
        return;
    }
    // The previous dispatch has fully finished here, so no worker holds a job of it.
    remaining_.store(jobs);
    fn_.store(fn);
    ctx_.store(ctx);
    // Publishes the dispatch: new generation, its job count, next index 0.
    state_.store((uint64_t(++generation_) << 32) | (uint64_t(jobs) << 16));
    wake_.fetch_add(1);
    wake_.notify_all();

    work(0);

    // Jobs may still be running on workers: spin briefly, then sleep until the last one finishes.
    int r = remaining_.load();
    if (r != 0) spin_until_changed(remaining_, r);
    while ((r = remaining_.load()) != 0) remaining_.wait(r);
}

}  // namespace rcv
