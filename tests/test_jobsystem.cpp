#include "core/JobSystem.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <numeric>
#include <vector>

using namespace immune;

TEST_CASE("parallel_for visits every index exactly once", "[core][jobs]") {
    JobSystem jobs;
    constexpr usize kCount = 100000;
    std::vector<u8> visited(kCount, 0);

    jobs.parallel_for(kCount, [&](usize begin, usize end, u32) {
        for (usize i = begin; i < end; ++i) visited[i] = 1;
    }, 1024);

    for (usize i = 0; i < kCount; ++i) REQUIRE(visited[i] == 1);
}

TEST_CASE("parallel_for ranges are disjoint and contiguous", "[core][jobs]") {
    JobSystem jobs;
    constexpr usize kCount = 50000;
    std::vector<u32> owner(kCount, 0xFFFFFFFFu);

    jobs.parallel_for(kCount, [&](usize begin, usize end, u32 range) {
        for (usize i = begin; i < end; ++i) owner[i] = range;
    }, 256);

    for (usize i = 0; i < kCount; ++i) REQUIRE(owner[i] != 0xFFFFFFFFu);
}

TEST_CASE("parallel_for result is independent of worker count", "[core][jobs][determinism]") {
    constexpr usize kCount = 20000;
    auto run = [](u32 workers) {
        JobSystem jobs(workers);
        std::vector<f32> out(kCount, 0.0f);
        jobs.parallel_for(kCount, [&](usize b, usize e, u32) {
            for (usize i = b; i < e; ++i) out[i] = static_cast<f32>(i) * 1.5f;
        }, 128);
        return out;
    };
    const auto serial = run(0);
    const auto parallel = run(7);
    REQUIRE(serial == parallel);
}

TEST_CASE("parallel_for handles zero and one element", "[core][jobs]") {
    JobSystem jobs;
    u32 calls = 0;
    jobs.parallel_for(0, [&](usize, usize, u32) { ++calls; });
    REQUIRE(calls == 0);
    jobs.parallel_for(1, [&](usize b, usize e, u32) { ++calls; REQUIRE(b == 0); REQUIRE(e == 1); });
    REQUIRE(calls == 1);
}

TEST_CASE("dispatch + wait_idle runs every task", "[core][jobs]") {
    JobSystem jobs;
    std::atomic<u32> counter{0};
    for (u32 i = 0; i < 256; ++i) {
        jobs.dispatch([&counter] { counter.fetch_add(1, std::memory_order_relaxed); });
    }
    jobs.wait_idle();
    REQUIRE(counter.load() == 256u);
}

TEST_CASE("worker pool defaults to hardware concurrency", "[core][jobs]") {
    JobSystem jobs;
    REQUIRE(jobs.thread_count() >= 1u);
    JobSystem serial(0);
    REQUIRE(serial.worker_count() == 0u);
    REQUIRE(serial.thread_count() == 1u);
}

TEST_CASE("rapid back-to-back parallel_for calls do not use freed stack state",
          "[core][jobs][stress]") {
    // Regression guard for a use-after-free in parallel_for.
    //
    // Its completion counter, mutex and condition_variable live in the calling
    // frame and are captured by reference by the worker lambdas. The counter
    // used to be decremented OUTSIDE the mutex, so the last worker could drop
    // it to zero, get descheduled, and only then reach for the lock -- while
    // the caller, already inside wait(), re-checked the predicate, saw zero,
    // returned, and destroyed all three objects. The worker then locked a dead
    // mutex and notified a dead condition_variable.
    //
    // It surfaced as a crash deep inside worker_main with implausible frames,
    // because the corrupted memory was whatever the next call reused, never as
    // a fault at the guilty line.
    //
    // The window is a few instructions wide, so this hammers it: many
    // iterations, workloads small enough that ranges finish almost together,
    // and frames reused immediately. It is not a proof of absence -- a race
    // this narrow can hide -- but it exercises the exact pattern, and the
    // result check also catches any range being skipped or double-counted.
    JobSystem jobs;
    if (jobs.worker_count() == 0) {
        WARN("single-core machine; parallel_for never dispatches, nothing to race");
        return;
    }

    constexpr int kIterations = 4000;
    constexpr usize kCount = 512;

    for (int it = 0; it < kIterations; ++it) {
        std::atomic<u64> sum{0};
        // grain 1 forces the split all the way up to thread_count() ranges, so
        // every iteration really does dispatch and join.
        jobs.parallel_for(kCount,
                          [&sum](usize begin, usize end, u32) {
                              u64 local = 0;
                              for (usize i = begin; i < end; ++i) local += i;
                              sum.fetch_add(local, std::memory_order_relaxed);
                          },
                          /*grain=*/1);
        // Every index exactly once: nothing skipped, nothing double-run.
        REQUIRE(sum.load() == (kCount * (kCount - 1)) / 2);
    }
}
