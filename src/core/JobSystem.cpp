#include "core/JobSystem.h"

#include <algorithm>

namespace immune {

JobSystem::JobSystem(u32 worker_count) {
    if (worker_count == kAutoWorkers) {
        const u32 hw = std::max(1u, std::thread::hardware_concurrency());
        worker_count = hw > 1 ? hw - 1 : 0;
    }
    workers_.reserve(worker_count);
    for (u32 i = 0; i < worker_count; ++i) {
        workers_.emplace_back([this] { worker_main(); });
    }
}

JobSystem::~JobSystem() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    cv_work_.notify_all();
    for (auto& t : workers_) {
        if (t.joinable()) t.join();
    }
}

void JobSystem::worker_main() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_work_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_ && queue_.empty()) return;
            task = std::move(queue_.back());
            queue_.pop_back();
        }
        task();
        if (pending_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            std::lock_guard<std::mutex> lock(mutex_);
            cv_done_.notify_all();
        }
    }
}

void JobSystem::dispatch(std::function<void()> task) {
    if (workers_.empty()) {
        task();
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back(std::move(task));
        pending_.fetch_add(1, std::memory_order_release);
    }
    cv_work_.notify_one();
}

void JobSystem::wait_idle() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_done_.wait(lock, [this] { return pending_.load(std::memory_order_acquire) == 0; });
}

void JobSystem::parallel_for(usize count,
                             const std::function<void(usize, usize, u32)>& body,
                             usize grain) {
    if (count == 0) return;
    if (grain == 0) grain = 1;

    const usize max_ranges = std::max<usize>(1, (count + grain - 1) / grain);
    const usize ranges = std::min<usize>(max_ranges, thread_count());

    if (ranges <= 1 || workers_.empty()) {
        body(0, count, 0);
        return;
    }

    const usize chunk = (count + ranges - 1) / ranges;

    // Ranges [1, ranges) go to workers; range 0 runs on the calling thread so
    // we never idle it.
    std::atomic<u32> remaining{static_cast<u32>(ranges - 1)};
    std::mutex done_mutex;
    std::condition_variable done_cv;

    for (usize r = 1; r < ranges; ++r) {
        const usize begin = r * chunk;
        const usize end = std::min(begin + chunk, count);
        const u32 index = static_cast<u32>(r);
        dispatch([&body, begin, end, index, &remaining, &done_mutex, &done_cv] {
            if (begin < end) body(begin, end, index);
            if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                std::lock_guard<std::mutex> lock(done_mutex);
                done_cv.notify_all();
            }
        });
    }

    body(0, std::min(chunk, count), 0);

    std::unique_lock<std::mutex> lock(done_mutex);
    done_cv.wait(lock, [&remaining] { return remaining.load(std::memory_order_acquire) == 0; });
}

} // namespace immune
