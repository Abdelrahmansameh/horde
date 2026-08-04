#include "core/Rng.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace immune;

TEST_CASE("same seed produces the same stream", "[core][rng][determinism]") {
    Rng a(12345);
    Rng b(12345);
    for (int i = 0; i < 1000; ++i) {
        REQUIRE(a.next_u32() == b.next_u32());
    }
}

TEST_CASE("different seeds diverge", "[core][rng]") {
    Rng a(1);
    Rng b(2);
    bool differs = false;
    for (int i = 0; i < 64 && !differs; ++i) {
        if (a.next_u32() != b.next_u32()) differs = true;
    }
    REQUIRE(differs);
}

TEST_CASE("next_f32 stays in [0,1)", "[core][rng]") {
    Rng rng(999);
    for (int i = 0; i < 100000; ++i) {
        const f32 v = rng.next_f32();
        REQUIRE(v >= 0.0f);
        REQUIRE(v < 1.0f);
    }
}

TEST_CASE("next_below respects its bound and covers it", "[core][rng]") {
    Rng rng(7);
    std::vector<int> hits(10, 0);
    for (int i = 0; i < 20000; ++i) {
        const u32 v = rng.next_below(10);
        REQUIRE(v < 10u);
        ++hits[v];
    }
    for (int count : hits) REQUIRE(count > 0);
    REQUIRE(rng.next_below(0) == 0u);
}

TEST_CASE("range_i is inclusive on both ends", "[core][rng]") {
    Rng rng(3);
    bool saw_lo = false, saw_hi = false;
    for (int i = 0; i < 5000; ++i) {
        const i32 v = rng.range_i(-3, 3);
        REQUIRE(v >= -3);
        REQUIRE(v <= 3);
        if (v == -3) saw_lo = true;
        if (v == 3) saw_hi = true;
    }
    REQUIRE(saw_lo);
    REQUIRE(saw_hi);
}

TEST_CASE("unit_disc stays inside the unit circle", "[core][rng]") {
    Rng rng(42);
    for (int i = 0; i < 10000; ++i) {
        const Vec2 p = rng.unit_disc();
        REQUIRE(p.x * p.x + p.y * p.y <= 1.0f);
    }
}

TEST_CASE("forked streams are deterministic and independent", "[core][rng][determinism]") {
    Rng base(555);
    Rng f1 = base.fork(1);
    Rng f2 = base.fork(2);
    Rng f1_again = base.fork(1);

    for (int i = 0; i < 100; ++i) {
        REQUIRE(f1.next_u32() == f1_again.next_u32());
    }
    // Forking must not advance the parent.
    Rng base2(555);
    REQUIRE(base.next_u32() == base2.next_u32());

    bool differs = false;
    Rng g1 = base2.fork(1);
    for (int i = 0; i < 64 && !differs; ++i) {
        if (g1.next_u32() != f2.next_u32()) differs = true;
    }
    REQUIRE(differs);
}
