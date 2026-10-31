#include "sim/chaff/ChaffSystem.h"
#include "sim/chaff/ChaffBuffers.h"
#include "core/Rng.h"
#include "sim/flowfield/FlowField.h"
#include "sim/flowfield/TissueRaster.h"
#include "sim/spatial/SpatialHash.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace immune;
using namespace immune::sim;

namespace {
ChaffTuning budget_tuning() {
    ChaffTuning tuning;
    tuning.max_neighbors_sampled = 4;
    for (auto& family : tuning.family) {
        family.acceleration = 0;
        family.max_speed = 8;
        family.jitter = 0;
        family.replication_rate = 0;
        family.separation_radius = 2;
        family.separation_strength = 0;
        family.alignment_radius = 2;
        family.alignment_strength = 0;
        family.pressure_gain = 0;
        family.pressure_max = 0;
        family.radius = 1;
        family.contact_spacing = 1;
        family.contact_stiffness = 1;
    }
    return tuning;
}
ChaffUpdateStats step(ChaffBuffers& chaff) {
    const Rect bounds{Vec2{0.0f,0.0f},Vec2{64.0f,64.0f}};
    SpatialHash hash; hash.configure({bounds,4.0f});
    hash.rebuild(chaff.pos_x.data(),chaff.pos_y.data(),chaff.count());
    ChaffSystem system;system.set_tuning(budget_tuning());system.set_world_bounds(bounds);
    system.set_goal(Vec2{},Vec2{});
    Rng rng(123);
    return system.update(chaff,FlowField{},DistanceField{},TissueMask{},hash,SquadRegistry{},rng,kFixedDt,nullptr);
}
}

TEST_CASE("coincident candidates consume the strict neighbour walk budget", "[chaff][capacity][candidate-budget]") {
    ChaffBuffers chaff;chaff.reserve(2048);
    ChaffSpawnParams p;p.position=Vec2{20.0f,20.0f};
    for (u32 i=0;i<2048;++i) REQUIRE(chaff.spawn(p).valid());
    const auto stats=step(chaff);
    // Four ordinary samples retain the existing 8x eligible-work budget;
    // one extra CSR slot accommodates self without shortening normal scans.
    CHECK(stats.max_neighbour_candidates_per_agent==33);
    CHECK(stats.neighbour_candidate_entries==2048u*33u);
    CHECK(chaff.pos_x[0]==20.0f);
}

TEST_CASE("underground piles are ignored for contact but still charged to candidate work", "[chaff][capacity][candidate-budget]") {
    ChaffBuffers chaff;chaff.reserve(4096);
    ChaffSpawnParams p;p.position=Vec2{20.0f,20.0f};
    REQUIRE(chaff.spawn(p).valid());
    p.family=PathogenFamily::Parasite;p.position.x=20.2f;
    for (u32 i=1;i<4096;++i) {
        REQUIRE(chaff.spawn(p).valid());
        chaff.burrow_state[i]=burrow_state::kUnderground;
        chaff.flags[i]|=chaff_flags::kHidden;
    }
    const auto stats=step(chaff);
    CHECK(stats.max_neighbour_candidates_per_agent==33);
    CHECK(stats.neighbour_candidate_entries==33);
    // Charging a ghost does not turn it into a collision/target participant.
    CHECK(chaff.pos_x[0]==20.0f);
    CHECK(chaff.pos_y[0]==20.0f);
}

TEST_CASE("ordinary pair contact keeps the existing complete neighbour sample", "[chaff][contact][candidate-budget]") {
    ChaffBuffers chaff;chaff.reserve(2);
    ChaffSpawnParams p;p.position=Vec2{20.0f,20.0f};REQUIRE(chaff.spawn(p).valid());
    p.position.x=20.5f;REQUIRE(chaff.spawn(p).valid());
    const auto stats=step(chaff);
    CHECK(stats.neighbour_candidate_entries==4);
    CHECK(stats.max_neighbour_candidates_per_agent==2);
    CHECK(chaff.pos_x[0]<20.0f);
    CHECK(chaff.pos_x[1]>20.5f);
    CHECK(chaff.pos_x[0]+chaff.pos_x[1]==Catch::Approx(40.5f).margin(0.0001f));
}
