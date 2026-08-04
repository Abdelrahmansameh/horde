// Guards the SoA invariants documented in sim/chaff/ChaffBuffers.h. Wave 1B
// must keep these passing while it adds the movement kernel.
#include "sim/chaff/ChaffBuffers.h"

#include <catch2/catch_test_macros.hpp>

using namespace immune;
using namespace immune::sim;

namespace {
ChaffSpawnParams make(f32 x, f32 y, PathogenFamily f = PathogenFamily::Virus, f32 density = 1.0f) {
    ChaffSpawnParams p;
    p.position = Vec2{x, y};
    p.family = f;
    p.density = density;
    return p;
}
} // namespace

TEST_CASE("chaff streams stay parallel and sized to capacity", "[sim][chaff][soa]") {
    ChaffBuffers b;
    b.reserve(1000);
    REQUIRE(b.capacity() == 1000);
    REQUIRE(b.count() == 0);
    REQUIRE(b.pos_x.size() == 1000);
    REQUIRE(b.pos_y.size() == b.pos_x.size());
    REQUIRE(b.vel_x.size() == b.pos_x.size());
    REQUIRE(b.vel_y.size() == b.pos_x.size());
    REQUIRE(b.family.size() == b.pos_x.size());
    REQUIRE(b.density.size() == b.pos_x.size());
    REQUIRE(b.flags.size() == b.pos_x.size());
}

TEST_CASE("spawn fills the streams and never reallocates", "[sim][chaff][soa]") {
    ChaffBuffers b;
    b.reserve(4);
    const f32* base = b.pos_x.data();

    for (int i = 0; i < 4; ++i) {
        REQUIRE(b.spawn(make(static_cast<f32>(i), 1.0f)).valid());
    }
    REQUIRE(b.count() == 4);
    REQUIRE(b.full());
    REQUIRE(b.pos_x.data() == base);       // no reallocation
    REQUIRE(!b.spawn(make(9.0f, 9.0f)).valid()); // capacity refusal
    REQUIRE(b.count() == 4);

    for (usize i = 0; i < b.count(); ++i) {
        REQUIRE((b.flags[i] & chaff_flags::kAlive) != 0);
    }
}

TEST_CASE("family counts and total density track spawns", "[sim][chaff][soa]") {
    ChaffBuffers b;
    b.reserve(16);
    b.spawn(make(0, 0, PathogenFamily::Virus, 2.0f));
    b.spawn(make(1, 0, PathogenFamily::Virus, 3.0f));
    b.spawn(make(2, 0, PathogenFamily::Bacteria, 5.0f));

    REQUIRE(b.family_count(PathogenFamily::Virus) == 2);
    REQUIRE(b.family_count(PathogenFamily::Bacteria) == 1);
    REQUIRE(b.family_count(PathogenFamily::Parasite) == 0);
    REQUIRE(b.total_density() == 10.0f);
}

TEST_CASE("density loss kills at zero and is the only damage path", "[sim][chaff][damage]") {
    ChaffBuffers b;
    b.reserve(8);
    b.spawn(make(0, 0, PathogenFamily::Virus, 4.0f));

    b.apply_density_loss(0, 1.5f);
    REQUIRE(b.density[0] == 2.5f);
    REQUIRE((b.flags[0] & chaff_flags::kPendingKill) == 0);
    REQUIRE(b.total_density() == 2.5f);

    b.apply_density_loss(0, 10.0f);   // over-kill must clamp, not go negative
    REQUIRE(b.density[0] == 0.0f);
    REQUIRE((b.flags[0] & chaff_flags::kPendingKill) != 0);
    REQUIRE(b.total_density() == 0.0f);
}

TEST_CASE("compact swap-removes dead agents and keeps the array dense",
          "[sim][chaff][soa]") {
    ChaffBuffers b;
    b.reserve(16);
    for (int i = 0; i < 10; ++i) b.spawn(make(static_cast<f32>(i), 0.0f));

    b.kill(2);
    b.kill(5);
    b.kill(9);
    const usize removed = b.compact();

    REQUIRE(removed == 3);
    REQUIRE(b.count() == 7);
    for (usize i = 0; i < b.count(); ++i) {
        REQUIRE((b.flags[i] & chaff_flags::kAlive) != 0);
        REQUIRE((b.flags[i] & chaff_flags::kPendingKill) == 0);
        REQUIRE(b.density[i] > 0.0f);           // invariant I4
    }
}

TEST_CASE("compact handles all-dead and none-dead", "[sim][chaff][soa]") {
    ChaffBuffers b;
    b.reserve(8);
    for (int i = 0; i < 5; ++i) b.spawn(make(static_cast<f32>(i), 0.0f));
    REQUIRE(b.compact() == 0);
    REQUIRE(b.count() == 5);

    for (usize i = 0; i < b.count(); ++i) b.kill(i);
    REQUIRE(b.compact() == 5);
    REQUIRE(b.count() == 0);
    REQUIRE(b.total_density() == 0.0f);
}

TEST_CASE("clear resets counts without dropping capacity", "[sim][chaff][soa]") {
    ChaffBuffers b;
    b.reserve(32);
    for (int i = 0; i < 20; ++i) b.spawn(make(static_cast<f32>(i), 0.0f));
    b.clear();
    REQUIRE(b.count() == 0);
    REQUIRE(b.capacity() == 32);
    REQUIRE(b.total_density() == 0.0f);
    REQUIRE(b.family_count(PathogenFamily::Virus) == 0);
}
