// Tests for the burrowing, slithering Parasite (sim/burrow/Burrow.h).
//
// The lane here is a straight walkable corridor, 1-unit cells, running +x to
// a goal at its far end, so "further along the lane" is simply "larger x" and
// cost-to-goal falls one unit per unit of x.
#include "sim/burrow/Burrow.h"
#include "sim/chaff/ChaffBuffers.h"
#include "sim/chaff/ChaffSystem.h"
#include "sim/squad/Squads.h"
#include "sim/flowfield/FlowField.h"
#include "sim/spatial/SpatialHash.h"
#include "sim/SimWorld.h"
#include "sim/ecs/Components.h"

#include "core/Math.h"
#include "core/Rng.h"
#include "game/enemies/EnemyConfigApply.h"
#include "render/ChaffBatcher.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

using namespace immune;
using namespace immune::sim;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;
constexpr u32 kParasite = static_cast<u32>(PathogenFamily::Parasite);

/// A corridor x in [0, 200), walkable for y in [5, 35), goal at the far end.
struct Lane {
    Rect bounds{Vec2{0.0f, 0.0f}, Vec2{200.0f, 40.0f}};
    TissueMask mask;
    DistanceField sdf;
    FlowField flow;
    SpatialHash hash;

    Lane() {
        mask.resize(200, 40, 1.0f, bounds.min);
        for (i32 y = 5; y < 35; ++y)
            for (i32 x = 0; x < 200; ++x) mask.set_walkable(x, y, true);
        sdf.bake(mask);
        FlowFieldBakeDesc desc;
        desc.goals = {FlowGoal{mask.world_to_cell(Vec2{198.0f, 20.0f})}};
        flow.bake(mask, desc);
        SpatialHashDesc hd;
        hd.bounds = bounds;
        hd.cell_size = 4.0f;
        hash.configure(hd);
    }

    void rehash(const ChaffBuffers& chaff) {
        hash.rebuild(chaff.pos_x.data(), chaff.pos_y.data(), chaff.count());
    }
};

BurrowParams test_burrow() {
    BurrowParams bp;
    bp.enabled = true;
    bp.cooldown = 1.0f;
    bp.cooldown_jitter = 0.0f;
    bp.dive_duration = 0.5f;
    bp.underground_duration = 1.0f;
    bp.telegraph_duration = 0.4f;
    bp.emerge_duration = 0.5f;
    bp.min_range = 14.0f;
    bp.max_range = 42.0f;
    bp.cone_half_angle = 40.0f;
    bp.candidate_count = 24;
    bp.min_progress = 8.0f;
    bp.goal_standoff = 20.0f;
    bp.wall_clearance = 2.0f;
    return bp;
}

BurrowSystem make_system(const BurrowParams& bp) {
    BurrowTuning t;
    t.burrow[kParasite] = bp;
    t.slither[kParasite].enabled = true;
    BurrowSystem sys;
    sys.set_tuning(t);
    return sys;
}

usize spawn_parasite(ChaffBuffers& chaff, Vec2 at) {
    ChaffSpawnParams p;
    p.position = at;
    p.family = PathogenFamily::Parasite;
    p.density = 2.0f;
    chaff.spawn(p);
    return chaff.count() - 1;
}

void move_parasite(ChaffBuffers& chaff, usize i, Vec2 displacement) {
    chaff.prev_pos_x[i] = chaff.pos_x[i];
    chaff.prev_pos_y[i] = chaff.pos_y[i];
    chaff.pos_x[i] += displacement.x;
    chaff.pos_y[i] += displacement.y;
}

struct ScopedParasiteSlither {
    SlitherParams previous = family_slither(PathogenFamily::Parasite);
    explicit ScopedParasiteSlither(const SlitherParams& params) {
        set_family_slither(PathogenFamily::Parasite, params);
    }
    ~ScopedParasiteSlither() { set_family_slither(PathogenFamily::Parasite, previous); }
};

/// Ticks until the agent leaves `state` (or `max_ticks` pass). Returns ticks.
u32 tick_until_not(BurrowSystem& sys, Lane& lane, ChaffBuffers& chaff, usize i, u8 state,
                   const std::vector<BurrowThreat>& threats, Rng& rng, u32 max_ticks) {
    u32 n = 0;
    while (chaff.burrow_state[i] == state && n < max_ticks) {
        lane.rehash(chaff);
        sys.update(chaff, lane.flow, lane.sdf, lane.mask, lane.hash, threats, rng, kDt);
        ++n;
    }
    return n;
}

} // namespace

TEST_CASE("a parasite burrows when ready near a threat and resurfaces further down the lane",
          "[sim][burrow]") {
    Lane lane;
    ChaffBuffers chaff;
    chaff.reserve(16);
    const BurrowParams bp = test_burrow();
    BurrowSystem sys = make_system(bp);
    Rng rng(7);
    const std::vector<BurrowThreat> threats{BurrowThreat{Vec2{40.0f, 20.0f}, 2.0f}};

    const Vec2 start{40.0f, 20.0f};
    const usize i = spawn_parasite(chaff, start);
    const f32 start_cost = lane.flow.sample_cost(start);

    // Arms on the first tick, then waits out the cooldown on the surface.
    const u32 surface_ticks =
        tick_until_not(sys, lane, chaff, i, burrow_state::kSurface, threats, rng, 600);
    REQUIRE(chaff.burrow_state[i] == burrow_state::kDiving);
    CHECK(static_cast<f32>(surface_ticks) * kDt == Catch::Approx(bp.cooldown).margin(3.0f * kDt));
    CHECK((chaff.flags[i] & chaff_flags::kHidden) != 0);
    // It sinks where it stood.
    CHECK(chaff.pos_x[i] == start.x);
    CHECK(chaff.pos_y[i] == start.y);

    // The exit it chose: inside the cone, inside the range, forward.
    const Vec2 exit{chaff.burrow_target_x[i], chaff.burrow_target_y[i]};
    const Vec2 d = exit - start;
    const f32 dist = math::length(d);
    CHECK(dist >= bp.min_range - 1e-3f);
    CHECK(dist <= bp.max_range + 1e-3f);
    const f32 angle_deg = std::fabs(std::atan2(d.y, d.x)) * 180.0f / math::kPi;
    CHECK(angle_deg <= bp.cone_half_angle + 1.0f);
    CHECK(start_cost - lane.flow.sample_cost(exit) >= bp.min_progress);
    CHECK(lane.sdf.sample(exit) >= bp.wall_clearance);

    // Under: parked at the exit, still hidden.
    tick_until_not(sys, lane, chaff, i, burrow_state::kDiving, threats, rng, 600);
    REQUIRE(chaff.burrow_state[i] == burrow_state::kUnderground);
    CHECK(chaff.pos_x[i] == exit.x);
    CHECK(chaff.pos_y[i] == exit.y);
    CHECK(chaff.prev_pos_x[i] == exit.x);

    tick_until_not(sys, lane, chaff, i, burrow_state::kUnderground, threats, rng, 600);
    REQUIRE(chaff.burrow_state[i] == burrow_state::kEmerging);
    tick_until_not(sys, lane, chaff, i, burrow_state::kEmerging, threats, rng, 600);
    REQUIRE(chaff.burrow_state[i] == burrow_state::kSurface);
    CHECK((chaff.flags[i] & chaff_flags::kHidden) == 0);
    // Re-armed for the next one.
    CHECK(chaff.burrow_timer[i] == Catch::Approx(bp.cooldown).margin(1e-4f));
}

TEST_CASE("a ready parasite saves its burrow until a threat is ahead", "[sim][burrow]") {
    Lane lane;
    ChaffBuffers chaff;
    chaff.reserve(4);
    const BurrowParams bp = test_burrow();
    BurrowSystem sys = make_system(bp);
    Rng rng(71);
    const usize i = spawn_parasite(chaff, Vec2{40.0f, 20.0f});
    const std::vector<BurrowThreat> no_threats;

    // Once ready, the cooldown stays available throughout a safe approach.
    for (u32 t = 0; t < 180; ++t) {
        lane.rehash(chaff);
        sys.update(chaff, lane.flow, lane.sdf, lane.mask, lane.hash, no_threats, rng, kDt);
    }
    REQUIRE(chaff.burrow_state[i] == burrow_state::kSurface);
    CHECK(chaff.burrow_timer[i] == Catch::Approx(0.0f).margin(1e-5f));

    // Coverage safely behind the parasite is no reason to spend it.
    const std::vector<BurrowThreat> behind{BurrowThreat{Vec2{25.0f, 20.0f}, 3.0f}};
    for (u32 t = 0; t < 60; ++t) {
        lane.rehash(chaff);
        sys.update(chaff, lane.flow, lane.sdf, lane.mask, lane.hash, behind, rng, kDt);
    }
    CHECK(chaff.burrow_state[i] == burrow_state::kSurface);
    CHECK(chaff.burrow_timer[i] == Catch::Approx(0.0f).margin(1e-5f));

    // A threat on its immediate path triggers the saved burrow without waiting
    // through another cooldown, and the exit avoids that threat's coverage.
    const std::vector<BurrowThreat> ahead{BurrowThreat{Vec2{50.0f, 20.0f}, 3.0f}};
    lane.rehash(chaff);
    sys.update(chaff, lane.flow, lane.sdf, lane.mask, lane.hash, ahead, rng, kDt);
    REQUIRE(chaff.burrow_state[i] == burrow_state::kDiving);
    const Vec2 exit{chaff.burrow_target_x[i], chaff.burrow_target_y[i]};
    CHECK(math::length(exit - ahead[0].position) > ahead[0].radius);
    CHECK(lane.flow.sample_cost(exit) < lane.flow.sample_cost(Vec2{40.0f, 20.0f}));
}

TEST_CASE("a burrowed parasite takes no damage", "[sim][burrow]") {
    ChaffBuffers chaff;
    chaff.reserve(4);
    const usize i = spawn_parasite(chaff, Vec2{10.0f, 10.0f});
    const f32 before = chaff.density[i];

    for (u8 state : {burrow_state::kDiving, burrow_state::kUnderground, burrow_state::kEmerging}) {
        chaff.burrow_state[i] = state;
        chaff.apply_density_loss(i, 1000.0f);
        CHECK(chaff.density[i] == before);
        CHECK((chaff.flags[i] & chaff_flags::kPendingKill) == 0);
    }
    chaff.burrow_state[i] = burrow_state::kSurface;
    chaff.apply_density_loss(i, 0.5f);
    CHECK(chaff.density[i] == Catch::Approx(before - 0.5f));
}

TEST_CASE("a parasite surfaces beyond tower coverage when its range allows", "[sim][burrow]") {
    Lane lane;
    ChaffBuffers chaff;
    chaff.reserve(4);
    BurrowParams bp = test_burrow();
    bp.cone_half_angle = 20.0f;
    BurrowSystem sys = make_system(bp);
    Rng rng(11);

    // A tower whose reach covers the whole near half of the cone.
    const Vec2 start{40.0f, 20.0f};
    const std::vector<BurrowThreat> threats{BurrowThreat{Vec2{58.0f, 20.0f}, 14.0f}};
    const usize i = spawn_parasite(chaff, start);
    tick_until_not(sys, lane, chaff, i, burrow_state::kSurface, threats, rng, 600);
    REQUIRE(chaff.burrow_state[i] == burrow_state::kDiving);
    const Vec2 exit{chaff.burrow_target_x[i], chaff.burrow_target_y[i]};
    CHECK(math::length(exit - threats[0].position) > threats[0].radius);
}

TEST_CASE("a parasite prefers open ground to a crowd", "[sim][burrow]") {
    Lane lane;
    ChaffBuffers chaff;
    chaff.reserve(512);
    BurrowParams bp = test_burrow();
    bp.cone_half_angle = 0.0f;       // a line: only the distance varies
    bp.min_range = 14.0f;
    bp.max_range = 40.0f;
    bp.progress_weight = 0.0f;
    bp.tower_weight = 0.0f;       // isolate the crowd term
    bp.crowd_weight = 5.0f;
    bp.crowd_radius = 6.0f;
    BurrowSystem sys = make_system(bp);
    Rng rng(3);

    const Vec2 start{40.0f, 20.0f};
    const usize i = spawn_parasite(chaff, start);
    // A packed crowd over the far half of the line.
    for (f32 x = 66.0f; x <= 86.0f; x += 1.5f)
        for (f32 y = 16.0f; y <= 24.0f; y += 1.5f) {
            ChaffSpawnParams p;
            p.position = Vec2{x, y};
            p.family = PathogenFamily::Virus;
            chaff.spawn(p);
        }
    const std::vector<BurrowThreat> threats{BurrowThreat{Vec2{40.0f, 20.0f}, 2.0f}};
    tick_until_not(sys, lane, chaff, i, burrow_state::kSurface, threats, rng, 600);
    REQUIRE(chaff.burrow_state[i] == burrow_state::kDiving);
    CHECK(chaff.burrow_target_x[i] < 66.0f - bp.crowd_radius);
}

TEST_CASE("with no legal exit a parasite stays up and retries", "[sim][burrow]") {
    Lane lane;
    ChaffBuffers chaff;
    chaff.reserve(4);
    BurrowParams bp = test_burrow();
    bp.goal_standoff = 1000.0f;      // nowhere is far enough from the goal
    bp.retry_delay = 0.25f;
    BurrowSystem sys = make_system(bp);
    Rng rng(5);
    const std::vector<BurrowThreat> threats{BurrowThreat{Vec2{40.0f, 20.0f}, 2.0f}};
    const usize i = spawn_parasite(chaff, Vec2{40.0f, 20.0f});

    BurrowStats total{};
    for (u32 t = 0; t < 120; ++t) {
        lane.rehash(chaff);
        const BurrowStats s =
            sys.update(chaff, lane.flow, lane.sdf, lane.mask, lane.hash, threats, rng, kDt);
        total.failed += s.failed;
        total.dives += s.dives;
    }
    CHECK(total.dives == 0);
    CHECK(total.failed >= 2);
    CHECK(chaff.burrow_state[i] == burrow_state::kSurface);
    CHECK((chaff.flags[i] & chaff_flags::kHidden) == 0);
}

TEST_CASE("a Macrophage's captive does not burrow out of its grip", "[sim][burrow]") {
    Lane lane;
    ChaffBuffers chaff;
    chaff.reserve(4);
    BurrowSystem sys = make_system(test_burrow());
    Rng rng(9);
    const std::vector<BurrowThreat> none;
    const usize i = spawn_parasite(chaff, Vec2{40.0f, 20.0f});
    chaff.flags[i] |= chaff_flags::kHidden;   // what a grabbing arm does
    for (u32 t = 0; t < 300; ++t) {
        lane.rehash(chaff);
        sys.update(chaff, lane.flow, lane.sdf, lane.mask, lane.hash, none, rng, kDt);
    }
    CHECK(chaff.burrow_state[i] == burrow_state::kSurface);
}

TEST_CASE("slither advances the wave with distance and turns the body gradually",
          "[sim][burrow][slither]") {
    Lane lane;
    ChaffBuffers chaff;
    chaff.reserve(4);
    BurrowParams off;
    BurrowTuning t;
    t.burrow[kParasite] = off;
    t.slither[kParasite].enabled = true;
    t.slither[kParasite].wavelength = 10.0f;
    t.slither[kParasite].idle_rate = 0.0f;
    t.slither[kParasite].turn_rate = 1.0f;
    BurrowSystem sys;
    sys.set_tuning(t);
    Rng rng(1);
    const std::vector<BurrowThreat> none;
    const usize i = spawn_parasite(chaff, Vec2{40.0f, 20.0f});

    lane.rehash(chaff);
    sys.update(chaff, lane.flow, lane.sdf, lane.mask, lane.hash, none, rng, kDt);
    const f32 phase0 = chaff.slither_phase[i];
    REQUIRE(phase0 >= 0.0f);
    // Initialised facing down the lane (+x).
    CHECK(chaff.body_heading[i] == Catch::Approx(0.0f).margin(0.05f));
    const f32 initial_heading = chaff.body_heading[i];

    // The visible displacement drives the wave, even when velocity disagrees.
    chaff.vel_x[i] = 0.0f;
    chaff.vel_y[i] = 6.0f;
    move_parasite(chaff, i, Vec2{0.1f, 0.0f});
    sys.update(chaff, lane.flow, lane.sdf, lane.mask, lane.hash, none, rng, kDt);
    const f32 advanced = std::fmod(chaff.slither_phase[i] - phase0 + kSlitherPhasePeriod,
                                    kSlitherPhasePeriod);
    CHECK(advanced == Catch::Approx(math::kTwoPi * 0.1f / 10.0f).margin(1e-4f));
    // Travel corrects the small baked-flow offset gradually rather than
    // snapping the body's tangent to every new displacement direction.
    CHECK(std::fabs(chaff.body_heading[i]) < std::fabs(initial_heading));
    const f32 aligned_heading = chaff.body_heading[i];

    // A perpendicular step asks it to turn, bounded to one tick's turn rate.
    move_parasite(chaff, i, Vec2{0.0f, 0.1f});
    sys.update(chaff, lane.flow, lane.sdf, lane.mask, lane.hash, none, rng, kDt);
    CHECK(chaff.body_heading[i] - aligned_heading == Catch::Approx(1.0f * kDt).margin(1e-4f));
}

TEST_CASE("slither runs the ground track with signed displacement and no travelling idle",
          "[sim][burrow][slither]") {
    Lane lane;
    ChaffBuffers chaff;
    chaff.reserve(4);
    BurrowTuning tuning;
    SlitherParams& slither = tuning.slither[kParasite];
    slither.enabled = true;
    slither.wavelength = 8.0f;
    slither.idle_rate = 0.25f;
    BurrowSystem sys;
    Rng rng(27);
    const std::vector<BurrowThreat> none;
    const usize i = spawn_parasite(chaff, Vec2{40.0f, 20.0f});
    chaff.slither_phase[i] = 1.0f;
    chaff.body_heading[i] = 0.0f;

    SECTION("movement with zero stored velocity still drives exactly its distance") {
        chaff.slither_phase[i] = kSlitherPhasePeriod - 0.01f;
        move_parasite(chaff, i, Vec2{0.1f, 0.0f});
        sys.set_tuning(tuning);
        sys.update(chaff, lane.flow, lane.sdf, lane.mask, lane.hash, none, rng, kDt);
        CHECK(chaff.slither_phase_delta[i] == Catch::Approx(math::kTwoPi * 0.1f / 8.0f).margin(2e-6f));
        CHECK(chaff.slither_phase[i] == Catch::Approx(math::kTwoPi * 0.1f / 8.0f - 0.01f).margin(3e-6f));
    }
    SECTION("a blocked agent does not animate stored forward velocity") {
        slither.idle_rate = 0.0f;
        chaff.vel_x[i] = 100.0f;
        sys.set_tuning(tuning);
        sys.update(chaff, lane.flow, lane.sdf, lane.mask, lane.hash, none, rng, kDt);
        CHECK(chaff.slither_phase[i] == 1.0f);
        CHECK(chaff.slither_phase_delta[i] == 0.0f);
    }
    SECTION("a backwards shove reverses the wave without turning the body around") {
        chaff.slither_phase[i] = 0.01f;
        move_parasite(chaff, i, Vec2{-0.1f, 0.0f});
        chaff.vel_x[i] = 100.0f;
        sys.set_tuning(tuning);
        sys.update(chaff, lane.flow, lane.sdf, lane.mask, lane.hash, none, rng, kDt);
        CHECK(chaff.slither_phase_delta[i] == Catch::Approx(-math::kTwoPi * 0.1f / 8.0f).margin(2e-6f));
        CHECK(chaff.slither_phase[i] == Catch::Approx(kSlitherPhasePeriod + 0.01f - math::kTwoPi * 0.1f / 8.0f).margin(3e-6f));
        CHECK(chaff.body_heading[i] == Catch::Approx(0.0f).margin(1e-5f));
    }
    SECTION("a forward corner beyond ninety degrees does not reverse the wave") {
        const f32 angle = 100.0f * math::kPi / 180.0f;
        move_parasite(chaff, i, Vec2{std::cos(angle), std::sin(angle)} * 0.1f);
        sys.set_tuning(tuning);
        sys.update(chaff, lane.flow, lane.sdf, lane.mask, lane.hash, none, rng, kDt);
        CHECK(chaff.body_heading_delta[i] > 0.0f);
        CHECK(chaff.slither_phase_delta[i] > 0.0f);
    }
    SECTION("an existing reverse run holds direction through the hysteresis band") {
        chaff.slither_phase_delta[i] = -0.1f;
        const f32 angle = 100.0f * math::kPi / 180.0f;
        move_parasite(chaff, i, Vec2{std::cos(angle), std::sin(angle)} * 0.1f);
        sys.set_tuning(tuning);
        sys.update(chaff, lane.flow, lane.sdf, lane.mask, lane.hash, none, rng, kDt);
        CHECK(chaff.body_heading_delta[i] < 0.0f);
        CHECK(chaff.slither_phase_delta[i] < 0.0f);
    }
    SECTION("a stationary free body retains its authored idle motion") {
        sys.set_tuning(tuning);
        sys.update(chaff, lane.flow, lane.sdf, lane.mask, lane.hash, none, rng, kDt);
        CHECK(chaff.slither_phase_delta[i] == Catch::Approx(math::kTwoPi * 0.25f * kDt).margin(1e-6f));
    }
    SECTION("a captive clears interpolation deltas and holds its track") {
        chaff.flags[i] |= chaff_flags::kHidden;
        chaff.body_heading_delta[i] = 0.1f;
        chaff.slither_phase_delta[i] = 0.3f;
        chaff.slither_curvature[i] = 0.04f;
        chaff.slither_curvature_delta[i] = 0.02f;
        move_parasite(chaff, i, Vec2{0.1f, 0.0f});
        sys.set_tuning(tuning);
        sys.update(chaff, lane.flow, lane.sdf, lane.mask, lane.hash, none, rng, kDt);
        CHECK(chaff.slither_phase[i] == 1.0f);
        CHECK(chaff.body_heading_delta[i] == 0.0f);
        CHECK(chaff.slither_phase_delta[i] == 0.0f);
        CHECK(chaff.slither_curvature[i] == 0.04f);
        CHECK(chaff.slither_curvature_delta[i] == 0.0f);
    }
}

TEST_CASE("slither follows a steady circular turn with the matching arc length",
          "[sim][burrow][slither]") {
    Lane lane;
    ChaffBuffers chaff;
    chaff.reserve(4);
    BurrowTuning tuning;
    tuning.slither[kParasite].enabled = true;
    tuning.slither[kParasite].wavelength = 8.0f;
    tuning.slither[kParasite].idle_rate = 0.25f;
    tuning.body_half_length[kParasite] = 4.5f;
    BurrowSystem sys;
    sys.set_tuning(tuning);
    Rng rng(28);
    const std::vector<BurrowThreat> none;
    const usize i = spawn_parasite(chaff, Vec2{40.0f, 20.0f});
    chaff.slither_phase[i] = 1.3f;
    chaff.body_heading[i] = 0.0f;
    constexpr f32 radius = 12.0f;
    constexpr f32 speed = 6.0f;
    constexpr u32 ticks = 360;
    constexpr f32 angle_per_tick = speed * kDt / radius;
    const f32 heading_response = 1.0f - std::exp(-kDt / 0.10f);
    // For a constant turn, the endpoint estimate follows the true tangent.
    // A sampled first-order heading filter has this steady angular lag.
    const f32 expected_heading_lag = (1.0f / heading_response - 1.0f) * angle_per_tick;
    f32 travelled_phase = 0.0f;
    f32 settled_curvature = 0.0f;
    u32 settled_samples = 0;
    for (u32 tick = 1; tick <= ticks; ++tick) {
        const f32 angle = speed * kDt * static_cast<f32>(tick) / radius;
        chaff.prev_pos_x[i] = chaff.pos_x[i];
        chaff.prev_pos_y[i] = chaff.pos_y[i];
        chaff.pos_x[i] = 40.0f + radius * std::sin(angle);
        chaff.pos_y[i] = 20.0f + radius * (1.0f - std::cos(angle));
        // The displacement contract also covers positional corrections not in velocity.
        chaff.vel_x[i] = chaff.vel_y[i] = 0.0f;
        sys.update(chaff, lane.flow, lane.sdf, lane.mask, lane.hash, none, rng, kDt);
        travelled_phase += chaff.slither_phase_delta[i];
        if (tick >= 240) {
            INFO("tick " << tick);
            CHECK(chaff.body_heading[i] == Catch::Approx(angle - expected_heading_lag).margin(2e-4f));
            CHECK(chaff.slither_curvature[i] == Catch::Approx(1.0f / radius).margin(2e-4f));
            settled_curvature += chaff.slither_curvature[i];
            ++settled_samples;
            CHECK(chaff.slither_phase_delta[i] == Catch::Approx(math::kTwoPi * speed * kDt / 8.0f).margin(1e-5f));
        }
    }
    CHECK(settled_curvature / static_cast<f32>(settled_samples) ==
          Catch::Approx(1.0f / radius).margin(3e-5f));
    CHECK(travelled_phase == Catch::Approx(math::kTwoPi * speed * kDt * ticks / 8.0f).margin(3e-4f));
}

TEST_CASE("the batcher interpolates slither position heading and four pi phase together",
          "[render][burrow][slither]") {
    SlitherParams slither;
    slither.enabled = true;
    const ScopedParasiteSlither scoped_slither(slither);
    ChaffBuffers chaff;
    chaff.reserve(4);
    const usize i = spawn_parasite(chaff, Vec2{40.0f, 20.0f});
    move_parasite(chaff, i, Vec2{0.1f, 0.0f});
    chaff.body_heading[i] = -math::kPi + 0.05f;
    chaff.body_heading_delta[i] = 0.1f;
    chaff.slither_phase[i] = 0.03f;
    chaff.slither_phase_delta[i] = 0.1f;
    chaff.slither_curvature[i] = 0.04f;
    chaff.slither_curvature_delta[i] = 0.02f;
    chaff.size_scale[i] = 1.25f;
    render::ChaffBatchParams params;
    params.lod_blob_enabled = false;
    params.per_family_capacity = 4;
    const render::OccupancyGrid occ{};
    std::vector<render::ChaffInstance> dest(kFamilyCount * params.per_family_capacity);
    for (const f32 alpha : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
        params.interpolation_alpha = alpha;
        const auto result = render::build_chaff_batches(chaff, occ, params, dest.data(), nullptr);
        REQUIRE(result.family_counts[kParasite] == 1);
        const auto& instance = dest[kParasite * params.per_family_capacity];
        CHECK(instance.x == Catch::Approx(40.0f + 0.1f * alpha).margin(1e-5f));
        // Compare periodic geometry across the angle and 4*pi phase boundaries.
        const f32 heading = math::kPi - 0.05f + 0.1f * alpha;
        CHECK(std::sin(instance.rotation) == Catch::Approx(std::sin(heading)).margin(1e-6f));
        CHECK(std::cos(instance.rotation) == Catch::Approx(std::cos(heading)).margin(1e-6f));
        const f32 phase = kSlitherPhasePeriod - 0.07f + 0.1f * alpha;
        CHECK(std::sin(0.5f * instance.anim_phase) == Catch::Approx(std::sin(0.5f * phase)).margin(1e-6f));
        CHECK(std::cos(0.5f * instance.anim_phase) == Catch::Approx(std::cos(0.5f * phase)).margin(1e-6f));
        CHECK(instance.curvature / instance.scale == Catch::Approx(0.02f + 0.02f * alpha).margin(1e-6f));
    }
}

TEST_CASE("alternating steering noise cannot snap the slither backbone",
          "[sim][burrow][slither]") {
    Lane lane;
    for (const f32 speed : {0.3f, 3.0f, 10.125f}) {
        INFO("speed " << speed);
        ChaffBuffers chaff;
        chaff.reserve(4);
        BurrowTuning tuning;
        auto& slither = tuning.slither[kParasite];
        slither.enabled = true;
        tuning.body_half_length[kParasite] = 4.5f;
        BurrowSystem sys;
        sys.set_tuning(tuning);
        const usize i = spawn_parasite(chaff, Vec2{40.0f, 20.0f});
        chaff.slither_phase[i] = 1.0f;
        const f32 half = tuning.body_half_length[kParasite] * chaff.size_scale[i];
        const f32 scale = 2.0f * half / slither.body_length;
        const f32 max_wave = scale * (slither.amplitude * slither.head_amplitude +
                                      0.5f * slither.curl + slither.probe);
        // The four corners of the moving curve's envelope give a physical
        // displacement check, independent of the curvature filter itself.
        const auto envelope_point = [](f32 curvature, f32 s, f32 lateral) {
            if (std::fabs(curvature) < 1e-7f) return Vec2{s, lateral};
            const f32 angle = curvature * s;
            const f32 sn = std::sin(angle), cs = std::cos(angle);
            const f32 half_sn = std::sin(0.5f * angle);
            return Vec2{sn / curvature - lateral * sn,
                        2.0f * half_sn * half_sn / curvature + lateral * cs};
        };
        f32 maximum_bend_motion = 0.0f;
        for (u32 tick = 0; tick < 240; ++tick) {
            INFO("tick " << tick);
            const f32 angle = (tick % 2 == 0 ? 1.0f : -1.0f) * 5.0f * math::kPi / 180.0f;
            const f32 old_curvature = chaff.slither_curvature[i];
            move_parasite(chaff, i, Vec2{std::cos(angle), std::sin(angle)} * (speed * kDt));
            const f32 distance = math::length(Vec2{chaff.pos_x[i] - chaff.prev_pos_x[i],
                                                   chaff.pos_y[i] - chaff.prev_pos_y[i]});
            sys.update_slither(chaff, lane.flow, kDt);
            CHECK(std::fabs(chaff.body_heading_delta[i]) < math::kPi / 180.0f);
            CHECK(chaff.slither_curvature_delta[i] ==
                  Catch::Approx(chaff.slither_curvature[i] - old_curvature).margin(1e-7f));
            const f32 budget = 0.25f * math::min(distance, math::max(slither.wavelength, 1.0f) * kDt);
            for (const f32 s : {-half, half}) {
                for (const f32 lateral : {-max_wave, max_wave}) {
                    const f32 movement = math::length(
                        envelope_point(chaff.slither_curvature[i], s, lateral) -
                        envelope_point(old_curvature, s, lateral));
                    maximum_bend_motion = math::max(maximum_bend_motion, movement);
                    CHECK(movement <= budget + 2e-5f);
                }
            }
        }
        // Normal travel used to flip between the curvature limits here,
        // throwing a tip over two world units sideways in one tick.
        CHECK(maximum_bend_motion < 0.04f);
    }
}

TEST_CASE("deferred slither includes movement corrections after the burrow pass",
          "[sim][burrow][slither]") {
    Lane lane;
    ChaffBuffers chaff;
    chaff.reserve(4);
    BurrowTuning tuning;
    tuning.slither[kParasite].enabled = true;
    tuning.slither[kParasite].idle_rate = 0.0f;
    BurrowSystem sys;
    sys.set_tuning(tuning);
    Rng rng(29);
    const std::vector<BurrowThreat> none;
    const usize i = spawn_parasite(chaff, Vec2{40.0f, 20.0f});
    chaff.slither_phase[i] = 1.0f;
    move_parasite(chaff, i, Vec2{0.1f, 0.0f});
    sys.update(chaff, lane.flow, lane.sdf, lane.mask, lane.hash, none, rng, kDt, Rect{}, false);
    CHECK(chaff.slither_phase[i] == 1.0f);
    // A friendly body can displace a pathogen after its navigation step.
    chaff.pos_y[i] += 0.075f;
    sys.update_slither(chaff, lane.flow, kDt);
    const f32 path_distance = chaff.slither_phase_delta[i] *
                              tuning.slither[kParasite].wavelength / math::kTwoPi;
    CHECK(path_distance == Catch::Approx(0.125f).margin(3e-5f));
    CHECK(path_distance > 0.12f);
}

TEST_CASE("slither pose survives compaction and resets recycled and cleared slots",
          "[sim][burrow][slither]") {
    ChaffBuffers chaff;
    chaff.reserve(4);
    const usize dead = spawn_parasite(chaff, Vec2{10.0f, 20.0f});
    const usize live = spawn_parasite(chaff, Vec2{20.0f, 20.0f});
    chaff.body_heading[live] = 0.7f;
    chaff.slither_phase[live] = 10.0f;
    chaff.body_heading_delta[live] = 0.02f;
    chaff.slither_phase_delta[live] = 0.3f;
    chaff.slither_curvature[live] = -0.04f;
    chaff.slither_curvature_delta[live] = -0.01f;
    chaff.kill(dead);
    REQUIRE(chaff.compact() == 1);
    REQUIRE(chaff.count() == 1);
    CHECK(chaff.body_heading[0] == 0.7f);
    CHECK(chaff.slither_phase[0] == 10.0f);
    CHECK(chaff.body_heading_delta[0] == 0.02f);
    CHECK(chaff.slither_phase_delta[0] == 0.3f);
    CHECK(chaff.slither_curvature[0] == -0.04f);
    CHECK(chaff.slither_curvature_delta[0] == -0.01f);
    CHECK(chaff.body_heading_delta[1] == 0.0f);
    CHECK(chaff.slither_phase_delta[1] == 0.0f);
    CHECK(chaff.slither_curvature[1] == 0.0f);
    CHECK(chaff.slither_curvature_delta[1] == 0.0f);

    const usize fresh = spawn_parasite(chaff, Vec2{30.0f, 20.0f});
    CHECK(chaff.slither_phase[fresh] < 0.0f);
    CHECK(chaff.body_heading_delta[fresh] == 0.0f);
    CHECK(chaff.slither_phase_delta[fresh] == 0.0f);
    CHECK(chaff.slither_curvature[fresh] == 0.0f);
    CHECK(chaff.slither_curvature_delta[fresh] == 0.0f);
    chaff.clear();
    for (usize slot = 0; slot < 4; ++slot) {
        CHECK(chaff.slither_phase[slot] < 0.0f);
        CHECK(chaff.body_heading_delta[slot] == 0.0f);
        CHECK(chaff.slither_phase_delta[slot] == 0.0f);
        CHECK(chaff.slither_curvature[slot] == 0.0f);
        CHECK(chaff.slither_curvature_delta[slot] == 0.0f);
    }
}

TEST_CASE("the batcher hides a parasite underground until its exit mound rises",
          "[render][burrow]") {
    SlitherParams sl;
    sl.enabled = true;
    set_family_slither(PathogenFamily::Parasite, sl);

    ChaffBuffers chaff;
    chaff.reserve(8);
    const usize a = spawn_parasite(chaff, Vec2{10.0f, 10.0f});
    const usize b = spawn_parasite(chaff, Vec2{20.0f, 10.0f});
    const usize c = spawn_parasite(chaff, Vec2{30.0f, 10.0f});
    chaff.burrow_state[a] = burrow_state::kUnderground;
    chaff.burrow_anim[a] = 0.0f;                  // not yet telegraphing
    chaff.burrow_state[b] = burrow_state::kUnderground;
    chaff.burrow_anim[b] = 0.5f;                  // mound half up
    chaff.burrow_state[c] = burrow_state::kDiving;
    chaff.burrow_anim[c] = 0.25f;
    chaff.flags[c] |= chaff_flags::kHidden;
    chaff.body_heading[c] = 1.25f;

    render::ChaffBatchParams params;
    params.lod_blob_enabled = false;
    params.per_family_capacity = 8;
    std::vector<render::ChaffInstance> dest(kFamilyCount * params.per_family_capacity);
    const render::OccupancyGrid occ{};
    const render::ChaffBatchResult r =
        render::build_chaff_batches(chaff, occ, params, dest.data(), nullptr);

    REQUIRE(r.family_counts[kParasite] == 2);
    const render::ChaffInstance* inst = &dest[kParasite * params.per_family_capacity];
    // Emitted in slot order: b then c.
    CHECK(inst[0].pad == Catch::Approx(2.0f * burrow_state::kUnderground + 0.5f));
    CHECK(inst[1].pad == Catch::Approx(2.0f * burrow_state::kDiving + 0.25f));
    CHECK(inst[1].rotation == Catch::Approx(1.25f));
    // The burrow draws itself; the generic hidden dimming stays off.
    CHECK((inst[1].flags & chaff_flags::kHidden) == 0u);

    set_family_slither(PathogenFamily::Parasite, SlitherParams{});
}

TEST_CASE("parasites burrow in a running world, deterministically", "[sim][burrow]") {
    auto run = [](u64 seed, u64& burrows) {
        SimDesc desc;
        desc.seed = seed;
        desc.max_chaff = 256;
        desc.world_bounds = Rect{Vec2{0.0f, 0.0f}, Vec2{200.0f, 40.0f}};
        desc.chaff_tuning.family[kParasite].radius = 1.6f;
        desc.chaff_tuning.family[kParasite].max_speed = 8.0f;
        desc.burrow_tuning.burrow[kParasite] = test_burrow();
        desc.burrow_tuning.burrow[kParasite].cooldown_jitter = 0.4f;
        desc.burrow_tuning.slither[kParasite].enabled = true;
        SimWorld world;
        world.init(desc, nullptr);
        TissueMask& mask = world.tissue();
        mask.resize(200, 40, 1.0f, Vec2{0.0f, 0.0f});
        for (i32 y = 5; y < 35; ++y)
            for (i32 x = 0; x < 200; ++x) mask.set_walkable(x, y, true);
        world.sdf().bake(mask);
        FlowFieldBakeDesc fd;
        fd.goals = {FlowGoal{mask.world_to_cell(Vec2{198.0f, 20.0f})}};
        world.flow().bake(mask, fd);
        // A live defence gives the parasites a reason to use their burrow.
        auto& registry = world.ecs().registry();
        const auto tower = registry.create();
        registry.emplace<comp::Transform>(tower, comp::Transform{Vec2{30.0f, 20.0f}});
        registry.emplace<comp::Health>(tower);
        registry.emplace<comp::Tower>(tower, comp::Tower{TowerType::Macrophage, 10.0f});
        Rng rng(seed);
        world.chaff_system().spawn_burst(world.chaff(), PathogenFamily::Parasite,
                                         Vec2{20.0f, 20.0f}, 6.0f, 12, rng);
        for (u32 t = 0; t < 240; ++t) world.tick();
        burrows = world.snapshot().burrows_total;
        return world.state_hash();
    };
    u64 burrows_a = 0, burrows_b = 0;
    const u64 a = run(42, burrows_a);
    const u64 b = run(42, burrows_b);
    CHECK(burrows_a > 0);
    CHECK(burrows_a == burrows_b);
    CHECK(a == b);
}

TEST_CASE("the roster ships the parasite burrowing and slithering", "[game][burrow]") {
    const sim::BurrowTuning t = game::burrow_tuning();
    CHECK(t.burrow[kParasite].enabled);
    CHECK(t.slither[kParasite].enabled);
    CHECK(t.body_half_length[kParasite] > 0.0f);
    CHECK_FALSE(t.burrow[static_cast<u32>(PathogenFamily::Virus)].enabled);
    CHECK_FALSE(t.burrow[static_cast<u32>(PathogenFamily::Bacteria)].enabled);
}

TEST_CASE("a family with collisions off neither shoves nor is shoved", "[sim][burrow][collision]") {
    Lane lane;
    ChaffBuffers chaff;
    chaff.reserve(8);
    ChaffTuning tuning;
    for (u32 f = 0; f < kFamilyCount; ++f) {
        tuning.family[f].radius = 1.0f;
        tuning.family[f].jitter = 0.0f;
        tuning.family[f].max_speed = 0.0f;   // isolate the positional contact push
    }
    tuning.family[kParasite].collides = false;
    ChaffSystem sys;
    sys.set_tuning(tuning);
    sys.set_world_bounds(lane.bounds);
    sys.set_goal(Vec2{0.0f, 0.0f}, Vec2{0.0f, 0.0f});

    // A worm lying on a virus, and two viruses lying on each other.
    const usize worm = spawn_parasite(chaff, Vec2{40.0f, 20.0f});
    ChaffSpawnParams v;
    v.family = PathogenFamily::Virus;
    v.position = Vec2{40.5f, 20.0f};
    const usize on_worm = (chaff.spawn(v), chaff.count() - 1);
    v.position = Vec2{80.0f, 20.0f};
    const usize a = (chaff.spawn(v), chaff.count() - 1);
    v.position = Vec2{80.5f, 20.0f};
    const usize b = (chaff.spawn(v), chaff.count() - 1);

    const SquadRegistry squads;
    Rng rng(2);
    lane.rehash(chaff);
    sys.update(chaff, lane.flow, DistanceField{}, TissueMask{}, lane.hash, squads, rng, kDt, nullptr);

    CHECK(chaff.pos_x[worm] == Catch::Approx(40.0f));
    CHECK(chaff.pos_x[on_worm] == Catch::Approx(40.5f));
    // Colliding families still separate.
    CHECK(chaff.pos_x[b] - chaff.pos_x[a] > 0.5f + 1e-3f);
}

TEST_CASE("parasite spacing relaxes at a bounded rate without changing solid contacts",
          "[sim][burrow][collision][slither]") {
    Lane lane;
    struct ContactRun {
        f32 separation;
        f32 maximum_step;
    };
    const auto run = [&](PathogenFamily family, bool collides, f32 dt, u32 ticks) {
        ChaffBuffers chaff;
        chaff.reserve(4);
        ChaffTuning tuning;
        for (auto& fp : tuning.family) {
            fp.radius = 1.6f;
            fp.max_speed = 10.0f;
            fp.acceleration = 0.0f;
            fp.separation_radius = 0.0f;
            fp.separation_strength = 0.0f;
            fp.alignment_radius = 0.0f;
            fp.alignment_strength = 0.0f;
            fp.jitter = 0.0f;
            fp.crowd_relief = 0.0f;
            fp.contact_spacing = 4.5f;
            fp.contact_stiffness = 1.0f;
        }
        tuning.family[static_cast<u32>(family)].collides = collides;
        ChaffSystem sys;
        sys.set_tuning(tuning);
        sys.set_world_bounds(lane.bounds);
        sys.set_goal(Vec2{}, Vec2{});
        ChaffSpawnParams spawn;
        spawn.family = family;
        spawn.position = {40.0f, 20.0f};
        REQUIRE(chaff.spawn(spawn).valid());
        spawn.position = {40.5f, 20.0f};
        REQUIRE(chaff.spawn(spawn).valid());
        const SquadRegistry squads;
        Rng rng(32);
        f32 maximum_step = 0.0f;
        f32 previous_separation = 0.5f;
        for (u32 tick = 0; tick < ticks; ++tick) {
            lane.rehash(chaff);
            const f32 left = chaff.pos_x[0], right = chaff.pos_x[1];
            sys.update(chaff, lane.flow, DistanceField{}, TissueMask{}, lane.hash,
                       squads, rng, dt, nullptr);
            const f32 step = math::max(std::fabs(chaff.pos_x[0] - left),
                                       std::fabs(chaff.pos_x[1] - right));
            maximum_step = math::max(maximum_step, step);
            const f32 separation = chaff.pos_x[1] - chaff.pos_x[0];
            CHECK(separation > previous_separation);
            previous_separation = separation;
            if (!collides) CHECK(step <= 10.0f * dt * 0.25f + 3e-6f);
        }
        return ContactRun{previous_separation, maximum_step};
    };

    // The pair remains overlapped throughout this interval. Its spacing
    // increases smoothly at the same physical rate at both tick sizes.
    const auto sixty = run(PathogenFamily::Parasite, false, 1.0f / 60.0f, 12);
    const auto one_twenty = run(PathogenFamily::Parasite, false, 1.0f / 120.0f, 24);
    CHECK(sixty.separation == Catch::Approx(1.5f).margin(2e-4f));
    CHECK(one_twenty.separation == Catch::Approx(sixty.separation).margin(2e-4f));

    // Solid families retain their radius-sized contact response. This is
    // ordinary overlap resolution, not the parasite's gentle self-spacing.
    const auto solid = run(PathogenFamily::Bacteria, true, kDt, 1);
    CHECK(solid.maximum_step == Catch::Approx(1.6f).margin(3e-6f));
    CHECK(solid.maximum_step > sixty.maximum_step * 10.0f);
}

TEST_CASE("parasites only burrow inside the play area", "[sim][burrow]") {
    Lane lane;
    ChaffBuffers chaff;
    chaff.reserve(4);
    BurrowParams bp = test_burrow();
    bp.edge_margin = 8.0f;
    BurrowSystem sys = make_system(bp);
    Rng rng(13);
    const std::vector<BurrowThreat> threats{BurrowThreat{Vec2{30.0f, 20.0f}, 2.0f},
                                         BurrowThreat{Vec2{80.0f, 20.0f}, 2.0f}};
    // The play area is only the middle of the corridor; the lane runs on
    // either side of it off-screen, the way a level's spawn points do.
    const Rect play{Vec2{60.0f, 0.0f}, Vec2{140.0f, 40.0f}};

    // Off-screen: never dives, however long it waits.
    const usize off = spawn_parasite(chaff, Vec2{30.0f, 20.0f});
    for (u32 t = 0; t < 300; ++t) {
        lane.rehash(chaff);
        sys.update(chaff, lane.flow, lane.sdf, lane.mask, lane.hash, threats, rng, kDt, play);
    }
    CHECK(chaff.burrow_state[off] == burrow_state::kSurface);

    // On-screen: dives, and the exit stays inside the margin.
    const usize on = spawn_parasite(chaff, Vec2{80.0f, 20.0f});
    u32 n = 0;
    while (chaff.burrow_state[on] == burrow_state::kSurface && n++ < 600) {
        lane.rehash(chaff);
        sys.update(chaff, lane.flow, lane.sdf, lane.mask, lane.hash, threats, rng, kDt, play);
    }
    REQUIRE(chaff.burrow_state[on] == burrow_state::kDiving);
    CHECK(chaff.burrow_target_x[on] <= play.max.x - bp.edge_margin);
    CHECK(chaff.burrow_target_x[on] >= play.min.x + bp.edge_margin);
}

TEST_CASE("a parasite does not dive with its head over the lane wall", "[sim][burrow]") {
    Lane lane;
    ChaffBuffers chaff;
    chaff.reserve(4);
    BurrowTuning t;
    t.burrow[kParasite] = test_burrow();
    t.slither[kParasite].enabled = true;
    t.body_half_length[kParasite] = 8.0f;
    BurrowSystem sys;
    sys.set_tuning(t);
    Rng rng(17);
    const std::vector<BurrowThreat> threats{BurrowThreat{Vec2{60.0f, 10.0f}, 2.0f}};

    // Lying across the lane near its lower wall (y = 5), head pointing into it.
    const usize i = spawn_parasite(chaff, Vec2{60.0f, 10.0f});
    chaff.slither_phase[i] = 0.0f;              // skip first-sight heading setup
    chaff.body_heading[i] = -0.5f * math::kPi;  // facing -y: head at y = 2
    for (u32 n = 0; n < 300; ++n) {
        lane.rehash(chaff);
        sys.update(chaff, lane.flow, lane.sdf, lane.mask, lane.hash, threats, rng, kDt);
    }
    CHECK(chaff.burrow_state[i] == burrow_state::kSurface);

    // Turned back down the lane, it goes.
    chaff.body_heading[i] = 0.0f;
    u32 n = 0;
    while (chaff.burrow_state[i] == burrow_state::kSurface && n++ < 600) {
        lane.rehash(chaff);
        sys.update(chaff, lane.flow, lane.sdf, lane.mask, lane.hash, threats, rng, kDt);
    }
    CHECK(chaff.burrow_state[i] == burrow_state::kDiving);
}

TEST_CASE("an exit keeps the whole surfacing body, tail hole included, on the lane",
          "[sim][burrow]") {
    Lane lane;
    // A pillar of lining from the lower wall to mid-lane, just ahead.
    for (i32 y = 5; y < 22; ++y)
        for (i32 x = 70; x < 76; ++x) lane.mask.set_walkable(x, y, false);
    lane.sdf.bake(lane.mask);
    FlowFieldBakeDesc fd;
    fd.goals = {FlowGoal{lane.mask.world_to_cell(Vec2{198.0f, 20.0f})}};
    lane.flow.bake(lane.mask, fd);

    const f32 half = 8.0f;
    BurrowTuning t;
    t.burrow[kParasite] = test_burrow();
    t.burrow[kParasite].cone_half_angle = 60.0f;
    t.slither[kParasite].enabled = true;
    t.body_half_length[kParasite] = half;
    BurrowSystem sys;
    sys.set_tuning(t);
    const std::vector<BurrowThreat> threats{BurrowThreat{Vec2{50.0f, 14.0f}, 4.0f}};

    u32 dives = 0;
    for (u64 seed = 1; seed <= 20; ++seed) {
        ChaffBuffers chaff;
        chaff.reserve(4);
        Rng rng(seed);
        const usize i = spawn_parasite(chaff, Vec2{50.0f, 14.0f});
        tick_until_not(sys, lane, chaff, i, burrow_state::kSurface, threats, rng, 600);
        if (chaff.burrow_state[i] != burrow_state::kDiving) continue;
        ++dives;
        const Vec2 exit{chaff.burrow_target_x[i], chaff.burrow_target_y[i]};
        const Vec2 fwd = math::normalize_safe(lane.flow.sample(exit));
        INFO("seed " << seed << " exit " << exit.x << "," << exit.y);
        for (const Vec2 end : {exit - fwd * half, exit + fwd * half}) {
            const IVec2 c = lane.mask.world_to_cell(end);
            CHECK(lane.mask.walkable(c.x, c.y));
            CHECK(lane.sdf.sample(end) >= t.burrow[kParasite].wall_clearance);
        }
    }
    CHECK(dives >= 10);
}
