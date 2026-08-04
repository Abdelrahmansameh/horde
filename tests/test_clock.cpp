#include "core/Clock.h"

#include <catch2/catch_test_macros.hpp>

using namespace immune;

TEST_CASE("fixed clock yields exactly 60 ticks per simulated second", "[core][clock][determinism]") {
    FixedClock clock;
    clock.advance_manual(1.0);
    u32 ticks = 0;
    while (clock.consume_tick()) ++ticks;
    REQUIRE(ticks == kTicksPerSecond);
    REQUIRE(clock.tick() == kTicksPerSecond);
}

TEST_CASE("partial time accumulates instead of being lost", "[core][clock]") {
    FixedClock clock;
    // Ten frames of 1/100 s = 0.1 s = exactly 6 ticks.
    u32 ticks = 0;
    for (int i = 0; i < 10; ++i) {
        clock.advance_manual(0.01);
        while (clock.consume_tick()) ++ticks;
    }
    REQUIRE(ticks == 6);
}

TEST_CASE("alpha is the fraction into the next tick", "[core][clock]") {
    FixedClock clock;
    clock.advance_manual(kFixedDt * 0.5);
    REQUIRE(!clock.consume_tick());
    REQUIRE(clock.alpha() > 0.45f);
    REQUIRE(clock.alpha() < 0.55f);
}

TEST_CASE("tick count is independent of frame rate", "[core][clock][determinism]") {
    FixedClock fast, slow;
    // 1 second at 240 Hz vs 1 second at 30 Hz.
    for (int i = 0; i < 240; ++i) fast.advance_manual(1.0 / 240.0);
    for (int i = 0; i < 30; ++i) slow.advance_manual(1.0 / 30.0);
    u32 a = 0, b = 0;
    while (fast.consume_tick()) ++a;
    while (slow.consume_tick()) ++b;
    REQUIRE(a == b);
}

TEST_CASE("reset clears the accumulator and tick counter", "[core][clock]") {
    FixedClock clock;
    clock.advance_manual(1.0);
    while (clock.consume_tick()) {}
    REQUIRE(clock.tick() > 0);
    clock.reset();
    REQUIRE(clock.tick() == 0);
    REQUIRE(!clock.consume_tick());
}

TEST_CASE("scoped timer records a non-negative duration", "[core][clock]") {
    f64 ms = -1.0;
    {
        ScopedTimer timer(ms);
        volatile int sink = 0;
        for (int i = 0; i < 10000; ++i) sink += i;
        (void)sink;
    }
    REQUIRE(ms >= 0.0);
}
