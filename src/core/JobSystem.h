// core/JobSystem.h — small worker pool for data-parallel sim work. FROZEN CONTRACT.
//
// Usage model is deliberately narrow: fork-join over index ranges, plus
// fire-and-forget tasks joined by wait_idle(). There is no task graph, no
// dependencies, no work stealing across frames. Chaff update, spatial-hash
// build, and flow-field rebake are all "split N items across K workers".
//
// DETERMINISM: parallel_for must produce identical results regardless of how
// ranges are scheduled. Callers therefore must not accumulate into shared
// floats or draw from a shared Rng — fork a per-range Rng from the range index
// (Rng::fork) instead.
#pragma once

#include "core/Types.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace immune {

class JobSystem {
public:
    /// Pass kAutoWorkers (the default) for hardware_concurrency() - 1 workers;
    /// the calling thread participates in parallel_for, so it is one of the N
    /// executors. Pass 0 for an explicitly serial pool — headless deterministic
    /// modes and unit tests rely on 0 meaning *no threads*, never "auto".
    static constexpr u32 kAutoWorkers = 0xFFFF'FFFFu;
    explicit JobSystem(u32 worker_count = kAutoWorkers);
    ~JobSystem();

    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;

    /// Number of background workers. 0 means everything runs inline (headless
    /// deterministic modes may force this for reproducible timing).
    u32 worker_count() const { return static_cast<u32>(workers_.size()); }

    /// Total executors available to parallel_for (workers + calling thread).
    u32 thread_count() const { return worker_count() + 1; }

    /// Splits [0, count) into contiguous chunks and runs `body(begin, end,
    /// range_index)` on each. Blocks until all chunks complete.
    /// grain is the minimum items per chunk; below it, runs inline.
    void parallel_for(usize count,
                      const std::function<void(usize begin, usize end, u32 range_index)>& body,
                      usize grain = 256);

    /// Enqueues a task to run on a worker. Use wait_idle() to join.
    void dispatch(std::function<void()> task);

    /// Blocks until every enqueued task has finished.
    void wait_idle();

private:
    void worker_main();

    std::vector<std::thread> workers_;
    std::vector<std::function<void()>> queue_;
    std::mutex mutex_;
    std::condition_variable cv_work_;
    std::condition_variable cv_done_;
    std::atomic<u32> pending_{0};
    bool stopping_ = false;
};

} // namespace immune
