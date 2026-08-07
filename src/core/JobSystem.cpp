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
    // THE COUNTER IS GUARDED BY done_mutex, NOT ATOMIC, AND THE NOTIFY HAPPENS
    // WHILE HOLDING THE LOCK. Both of those are load-bearing; this was a
    // use-after-free of stack memory.
    //
    // These three objects live in THIS stack frame, and the worker lambdas
    // capture them by reference. The previous version decremented an atomic
    // counter OUTSIDE the mutex and only then took the lock to notify:
    //
    //     if (remaining.fetch_sub(1) == 1) {        // worker, no lock held
    //         std::lock_guard lock(done_mutex);     // <-- window
    //         done_cv.notify_all();
    //     }
    //
    // which allows:
    //   1. the last worker decrements `remaining` to 0 and is then descheduled
    //      before taking the lock;
    //   2. the calling thread, already holding done_mutex inside wait(),
    //      re-evaluates the predicate, sees 0, and returns WITHOUT EVER
    //      BLOCKING;
    //   3. parallel_for returns, and done_mutex/done_cv/remaining are destroyed
    //      with the frame;
    //   4. the worker resumes and locks a destroyed mutex, then notifies a
    //      destroyed condition_variable.
    //
    // The corrupted memory is whatever stack the next call reuses, so this
    // surfaces as an implausible crash inside worker_main with nonsense frames
    // (e.g. vector<thread> reallocation) rather than anywhere near here.
    //
    // Mutating the counter under the lock closes it: the waiter can only leave
    // wait() by re-acquiring done_mutex, which the notifying worker still
    // holds, so by the time the frame can be destroyed the worker has finished
    // touching all three objects. Notifying with the lock held costs the woken
    // thread one extra block, which is irrelevant at a few calls per tick.
    u32 remaining = static_cast<u32>(ranges - 1);
    std::mutex done_mutex;
    std::condition_variable done_cv;

    for (usize r = 1; r < ranges; ++r) {
        const usize begin = r * chunk;
        const usize end = std::min(begin + chunk, count);
        const u32 index = static_cast<u32>(r);
        dispatch([&body, begin, end, index, &remaining, &done_mutex, &done_cv] {
            if (begin < end) body(begin, end, index);
            std::lock_guard<std::mutex> lock(done_mutex);
            if (--remaining == 0) done_cv.notify_all();
        });
    }

    body(0, std::min(chunk, count), 0);

    std::unique_lock<std::mutex> lock(done_mutex);
    done_cv.wait(lock, [&remaining] { return remaining == 0; });
}

} // namespace immune
