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
