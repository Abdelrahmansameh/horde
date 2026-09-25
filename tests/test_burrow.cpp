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

TEST_CASE("a parasite burrows on its cooldown and resurfaces further down the lane",
          "[sim][burrow]") {
    Lane lane;
    ChaffBuffers chaff;
    chaff.reserve(16);
    const BurrowParams bp = test_burrow();
    BurrowSystem sys = make_system(bp);
    Rng rng(7);
    const std::vector<BurrowThreat> none;

    const Vec2 start{40.0f, 20.0f};
    const usize i = spawn_parasite(chaff, start);
    const f32 start_cost = lane.flow.sample_cost(start);

    // Arms on the first tick, then waits out the cooldown on the surface.
    const u32 surface_ticks =
        tick_until_not(sys, lane, chaff, i, burrow_state::kSurface, none, rng, 600);
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
    tick_until_not(sys, lane, chaff, i, burrow_state::kDiving, none, rng, 600);
    REQUIRE(chaff.burrow_state[i] == burrow_state::kUnderground);
    CHECK(chaff.pos_x[i] == exit.x);
    CHECK(chaff.pos_y[i] == exit.y);
    CHECK(chaff.prev_pos_x[i] == exit.x);

    tick_until_not(sys, lane, chaff, i, burrow_state::kUnderground, none, rng, 600);
    REQUIRE(chaff.burrow_state[i] == burrow_state::kEmerging);
    tick_until_not(sys, lane, chaff, i, burrow_state::kEmerging, none, rng, 600);
    REQUIRE(chaff.burrow_state[i] == burrow_state::kSurface);
    CHECK((chaff.flags[i] & chaff_flags::kHidden) == 0);
    // Re-armed for the next one.
    CHECK(chaff.burrow_timer[i] == Catch::Approx(bp.cooldown).margin(1e-4f));
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
    bp.progress_weight = 0.0f;       // isolate the crowd term
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
    const std::vector<BurrowThreat> none;
    tick_until_not(sys, lane, chaff, i, burrow_state::kSurface, none, rng, 600);
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
    const std::vector<BurrowThreat> none;
    const usize i = spawn_parasite(chaff, Vec2{40.0f, 20.0f});

    BurrowStats total{};
    for (u32 t = 0; t < 120; ++t) {
        lane.rehash(chaff);
        const BurrowStats s =
            sys.update(chaff, lane.flow, lane.sdf, lane.mask, lane.hash, none, rng, kDt);
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
    const f32 heading0 = chaff.body_heading[i];

    // Moving at 6 units/s for one tick is 0.1 units: 2pi/100 of phase.
    chaff.vel_x[i] = 0.0f;
    chaff.vel_y[i] = 6.0f;   // and asking to face straight up
    sys.update(chaff, lane.flow, lane.sdf, lane.mask, lane.hash, none, rng, kDt);
    const f32 advanced = std::fmod(chaff.slither_phase[i] - phase0 + math::kTwoPi, math::kTwoPi);
    CHECK(advanced == Catch::Approx(math::kTwoPi * 0.1f / 10.0f).margin(1e-4f));
    // ...but only turned one tick's worth toward it.
    CHECK(chaff.body_heading[i] - heading0 == Catch::Approx(1.0f * kDt).margin(1e-4f));
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
