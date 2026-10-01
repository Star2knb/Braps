// Persistent worker pool for slice-parallel encode/decode (codec plan §8).
#pragma once

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

namespace rcv {

constexpr int kMaxThreads = 32;

// Resolves a thread-count setting: 0 = auto (2 if >= 4 logical CPUs, else 1), capped at kMaxThreads.
int resolve_thread_count(int requested);

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324)  // padded due to alignas: intended, keeps hot atomics on separate cache lines
#endif

class ThreadPool {
public:
    using JobFn = void (*)(void* ctx, int job, int worker);
    using StartFn = void (*)(void* user, int worker);

    ThreadPool() = default;
    ~ThreadPool() { stop(); }
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // Starts threads - 1 workers; the thread calling run() is worker 0. on_start runs first on
    // each worker thread (workers 1..threads-1), e.g. to set priority and affinity. Returns once
    // every worker has started and its on_start has returned.
    // Allocates and may fail: call only from *_create. Returns false if a thread can't be created.
    bool start(int threads, StartFn on_start, void* user);
    void stop();
    int threads() const { return threads_; }

    // Runs fn(ctx, j, worker) once for every j in [0, jobs), on the workers and the calling thread,
    // and returns when all jobs have finished. Jobs are claimed in index order. No allocation.
    // One caller at a time. jobs <= kMaxJobs.
    static constexpr int kMaxJobs = 0xFFFF;
    void run(JobFn fn, void* ctx, int jobs);

private:
    void worker_main(int index, StartFn on_start, void* user);
    void work(int worker);

    int threads_ = 1;
    uint32_t generation_ = 0;  // owned by the caller of run()
    std::vector<std::thread> workers_;

    // generation (32 bits) | job count (16) | next job index (16). Claiming a job is a
    // compare-exchange on the whole word, and the index is compared with the count *from the same
    // word*, so a claim is always consistent with exactly one dispatch: a worker that is late for
    // one dispatch can never claim a job of another, or a job index that doesn't exist.
    alignas(64) std::atomic<uint64_t> state_{0};
    std::atomic<JobFn> fn_{nullptr};
    std::atomic<void*> ctx_{nullptr};
    alignas(64) std::atomic<int> remaining_{0};  // jobs of the current dispatch not yet finished
    alignas(64) std::atomic<uint32_t> wake_{0};  // bumped per dispatch; idle workers wait on it
    std::atomic<bool> quit_{false};
    std::atomic<int> started_{0};  // workers that have finished starting up
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif

}  // namespace rcv
