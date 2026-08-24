// Tests for the chaff movement kernel. Owner: Wave 1B.
//
// FlowField's bake() is Wave 1A's contract; rather than block on it or reach
// into private state, these tests bake a small synthetic "radial toward a
// point" field over a fully-walkable box through the real public API
// (TissueMask -> FlowField::bake). That is exactly the kind of field the
// gameplay levels produce, just tiny and goal-only.
#include "sim/chaff/ChaffBuffers.h"
#include "sim/chaff/ChaffSystem.h"
#include "sim/flowfield/FlowField.h"
#include "sim/flowfield/TissueRaster.h"
#include "sim/spatial/SpatialHash.h"
#include "sim/squad/Squads.h"

#include "core/JobSystem.h"
#include "core/Math.h"
#include "core/Rng.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace immune;
using namespace immune::sim;

namespace {

/// Squad-free registry: no paths, so ChaffSystem's squad terms collapse and the
/// kernel behaves exactly as it did before the squad layer existed. Every test
/// in this file is about the base movement rules, so they all steer through
/// this. Squad behaviour has its own file, tests/test_squads.cpp.
const SquadRegistry& no_squads() {
    static const SquadRegistry empty;
    return empty;
}

FlowField make_radial_flow(Rect bounds, Vec2 goal_world, f32 cell = 1.0f) {
    TissueMask mask;
    const i32 w = static_cast<i32>(bounds.size().x / cell);
    const i32 h = static_cast<i32>(bounds.size().y / cell);
    mask.resize(w, h, cell, bounds.min);
    for (i32 y = 0; y < h; ++y)
        for (i32 x = 0; x < w; ++x) mask.set_walkable(x, y, true);

    FlowField flow;
    FlowFieldBakeDesc desc;
    desc.goal_cells = {mask.world_to_cell(goal_world)};
    flow.bake(mask, desc);
    return flow;
}

SpatialHash make_hash(Rect bounds, f32 cell_size = 4.0f) {
    SpatialHash hash;
    SpatialHashDesc d;
    d.bounds = bounds;
    d.cell_size = cell_size;
    hash.configure(d);
    return hash;
}

void rebuild(SpatialHash& hash, ChaffBuffers& b) {
    hash.rebuild(b.pos_x.data(), b.pos_y.data(), b.count(), nullptr);
}

ChaffTuning flat_tuning(f32 accel, f32 max_speed, f32 sep_radius, f32 sep_strength,
                        f32 jitter, f32 replication_rate = 0.0f) {
    ChaffTuning t;
    for (u32 f = 0; f < kFamilyCount; ++f) {
        t.family[f].acceleration = accel;
        t.family[f].max_speed = max_speed;
        t.family[f].separation_radius = sep_radius;
        t.family[f].separation_strength = sep_strength;
        t.family[f].jitter = jitter;
        t.family[f].base_density = 1.0f;
        t.family[f].replication_rate = replication_rate;
    }
    return t;
}

} // namespace

TEST_CASE("flow acceleration steers an agent toward the goal", "[sim][chaff][movement]") {
    const Rect bounds{Vec2{0.0f, 0.0f}, Vec2{50.0f, 50.0f}};
    const Vec2 goal{40.0f, 25.0f};
    FlowField flow = make_radial_flow(bounds, goal);
    DistanceField sdf;
    SpatialHash hash = make_hash(bounds);

    ChaffBuffers buffers;
    buffers.reserve(8);
    ChaffSpawnParams p;
    p.position = Vec2{5.0f, 25.0f};
    buffers.spawn(p);

    ChaffSystem sys;
    sys.set_tuning(flat_tuning(/*accel*/ 20.0f, /*max_speed*/ 8.0f, /*sep_radius*/ 0.0f,
                               /*sep_strength*/ 0.0f, /*jitter*/ 0.0f));
    sys.set_world_bounds(bounds);
    sys.set_goal(goal, 0.0f);   // disabled: radius 0 never triggers despawn here

    Rng rng(1);
    for (int i = 0; i < 30; ++i) {
        rebuild(hash, buffers);
        sys.update(buffers, flow, sdf, TissueMask{}, hash, no_squads(), rng, kFixedDt, nullptr);
    }

    REQUIRE(buffers.count() == 1);
    REQUIRE(buffers.pos_x[0] > 5.0f);   // moved toward the goal, i.e. +x
}

TEST_CASE("kDrifting agents ignore the flow field and follow ambient drift",
          "[sim][chaff][movement][flags]") {
    const Rect bounds{Vec2{0.0f, 0.0f}, Vec2{50.0f, 50.0f}};
    const Vec2 goal{45.0f, 25.0f};   // strongly to the +x
    FlowField flow = make_radial_flow(bounds, goal);
    DistanceField sdf;
    SpatialHash hash = make_hash(bounds);

    ChaffBuffers buffers;
    buffers.reserve(8);
    ChaffSpawnParams p;
    p.position = Vec2{25.0f, 25.0f};
    p.flags = chaff_flags::kDrifting;
    buffers.spawn(p);

    ChaffSystem sys;
    ChaffTuning tuning = flat_tuning(20.0f, 8.0f, 0.0f, 0.0f, 0.0f);
    tuning.ambient_drift = Vec2{0.0f, -5.0f};   // straight down
    sys.set_tuning(tuning);
    sys.set_world_bounds(bounds);
    sys.set_goal(goal, 0.0f);

    Rng rng(2);
    for (int i = 0; i < 20; ++i) {
        rebuild(hash, buffers);
        sys.update(buffers, flow, sdf, TissueMask{}, hash, no_squads(), rng, kFixedDt, nullptr);
    }

    REQUIRE(buffers.count() == 1);
    REQUIRE(buffers.pos_y[0] < 25.0f);            // drifted down
    REQUIRE(buffers.pos_x[0] == Catch::Approx(25.0f).margin(0.01f)); // NOT pulled toward goal
}

TEST_CASE("kSlowed lowers the effective max speed", "[sim][chaff][movement][flags]") {
    const Rect bounds{Vec2{0.0f, 0.0f}, Vec2{200.0f, 200.0f}};
    FlowField flow = make_radial_flow(bounds, Vec2{190.0f, 100.0f}, 2.0f);
    DistanceField sdf;
    SpatialHash hash = make_hash(bounds, 4.0f);

    ChaffBuffers buffers;
    buffers.reserve(4);
    {
        ChaffSpawnParams p; p.position = Vec2{10.0f, 100.0f};   // agent 0: normal
        buffers.spawn(p);
    }
    {
        ChaffSpawnParams p; p.position = Vec2{10.0f, 100.0f};   // agent 1: slowed
        p.flags = chaff_flags::kSlowed;
        buffers.spawn(p);
    }

    ChaffSystem sys;
    // Huge acceleration so both agents saturate to max_speed within one tick,
    // isolating the kSlowed multiplier as the only remaining difference.
    sys.set_tuning(flat_tuning(/*accel*/ 10000.0f, /*max_speed*/ 10.0f, 0.0f, 0.0f, 0.0f));
    sys.set_world_bounds(bounds);
    sys.set_goal(Vec2{190.0f, 100.0f}, 0.0f);

    Rng rng(4);
    rebuild(hash, buffers);
    sys.update(buffers, flow, sdf, TissueMask{}, hash, no_squads(), rng, kFixedDt, nullptr);

    const f32 speed_normal = std::sqrt(buffers.vel_x[0] * buffers.vel_x[0] +
                                       buffers.vel_y[0] * buffers.vel_y[0]);
    const f32 speed_slowed = std::sqrt(buffers.vel_x[1] * buffers.vel_x[1] +
                                       buffers.vel_y[1] * buffers.vel_y[1]);

    REQUIRE(speed_normal == Catch::Approx(10.0f).margin(0.05f));
    REQUIRE(speed_slowed < speed_normal);
    REQUIRE(speed_slowed > 0.0f);
}

TEST_CASE("kHidden agents do not move", "[sim][chaff][movement][flags]") {
    const Rect bounds{Vec2{0.0f, 0.0f}, Vec2{50.0f, 50.0f}};
    FlowField flow = make_radial_flow(bounds, Vec2{45.0f, 25.0f});
    DistanceField sdf;
    SpatialHash hash = make_hash(bounds);

    ChaffBuffers buffers;
    buffers.reserve(4);
    ChaffSpawnParams p;
    p.position = Vec2{25.0f, 25.0f};
    p.velocity = Vec2{3.0f, -2.0f};   // pre-existing motion; must be zeroed
    p.flags = chaff_flags::kHidden;
    buffers.spawn(p);

    ChaffSystem sys;
    sys.set_tuning(flat_tuning(20.0f, 8.0f, 1.0f, 8.0f, 1.0f));
    sys.set_world_bounds(bounds);
    sys.set_goal(Vec2{45.0f, 25.0f}, 0.0f);

    Rng rng(5);
    for (int i = 0; i < 10; ++i) {
        rebuild(hash, buffers);
        sys.update(buffers, flow, sdf, TissueMask{}, hash, no_squads(), rng, kFixedDt, nullptr);
        REQUIRE(buffers.pos_x[0] == Catch::Approx(25.0f));
        REQUIRE(buffers.pos_y[0] == Catch::Approx(25.0f));
        REQUIRE(buffers.vel_x[0] == 0.0f);
        REQUIRE(buffers.vel_y[0] == 0.0f);
    }
}

TEST_CASE("replication respects the per-tick global cap", "[sim][chaff][replication]") {
    const Rect bounds{Vec2{0.0f, 0.0f}, Vec2{50.0f, 50.0f}};
    FlowField flow = make_radial_flow(bounds, Vec2{25.0f, 25.0f});
    DistanceField sdf;
    SpatialHash hash = make_hash(bounds);

    ChaffBuffers buffers;
    buffers.reserve(200);
    for (int i = 0; i < 50; ++i) {
        ChaffSpawnParams p;
        p.position = Vec2{5.0f + static_cast<f32>(i) * 0.1f, 25.0f};
        buffers.spawn(p);
    }

    ChaffSystem sys;
    // replication_rate * dt >= 1 makes Rng::chance() succeed unconditionally
    // (next_f32() in [0,1) is always < a probability >= 1), so the cap itself
    // is the only thing limiting the outcome — no dependence on RNG draws.
    ChaffTuning tuning = flat_tuning(0.0f, 5.0f, 0.0f, 0.0f, 0.0f, /*replication*/ 10000.0f);
    tuning.max_replications_per_tick = 3;
    sys.set_tuning(tuning);
    sys.set_world_bounds(bounds);
    sys.set_goal(Vec2{25.0f, 25.0f}, 0.0f);

    Rng rng(6);
    rebuild(hash, buffers);
    const ChaffUpdateStats stats = sys.update(buffers, flow, sdf, TissueMask{}, hash, no_squads(), rng, kFixedDt, nullptr);

    REQUIRE(stats.replicated == 3);
    REQUIRE(buffers.count() == 53);
}

TEST_CASE("agents leaving the world bounds are flagged for despawn", "[sim][chaff][despawn]") {
    const Rect bounds{Vec2{0.0f, 0.0f}, Vec2{20.0f, 20.0f}};
    FlowField flow = make_radial_flow(bounds, Vec2{10.0f, 10.0f});
    DistanceField sdf;
    SpatialHash hash = make_hash(bounds);

    ChaffBuffers buffers;
    buffers.reserve(4);
    ChaffSpawnParams p;
    p.position = Vec2{19.9f, 10.0f};
    p.velocity = Vec2{50.0f, 0.0f};   // guaranteed to exit this tick
    buffers.spawn(p);

    ChaffSystem sys;
    sys.set_tuning(flat_tuning(0.0f, 100.0f, 0.0f, 0.0f, 0.0f));
    sys.set_world_bounds(bounds);
    sys.set_goal(Vec2{10.0f, 10.0f}, 0.0f);   // disabled

    Rng rng(7);
    rebuild(hash, buffers);
    const ChaffUpdateStats stats = sys.update(buffers, flow, sdf, TissueMask{}, hash, no_squads(), rng, kFixedDt, nullptr);

    REQUIRE(stats.despawned_out_of_bounds == 1);
    REQUIRE((buffers.flags[0] & chaff_flags::kPendingKill) != 0);
    REQUIRE(buffers.compact() == 1);
    REQUIRE(buffers.count() == 0);
}

TEST_CASE("agents reaching the goal are flagged for despawn", "[sim][chaff][despawn]") {
    const Rect bounds{Vec2{0.0f, 0.0f}, Vec2{50.0f, 50.0f}};
    const Vec2 goal{25.0f, 25.0f};
    FlowField flow = make_radial_flow(bounds, goal);
    DistanceField sdf;
    SpatialHash hash = make_hash(bounds);

    ChaffBuffers buffers;
    buffers.reserve(4);
    ChaffSpawnParams p;
    p.position = Vec2{25.1f, 25.0f};   // already inside the goal radius
    buffers.spawn(p);

    ChaffSystem sys;
    sys.set_tuning(flat_tuning(0.0f, 5.0f, 0.0f, 0.0f, 0.0f));
    sys.set_world_bounds(bounds);
    sys.set_goal(goal, 1.0f);

    Rng rng(8);
    rebuild(hash, buffers);
    const ChaffUpdateStats stats = sys.update(buffers, flow, sdf, TissueMask{}, hash, no_squads(), rng, kFixedDt, nullptr);

    REQUIRE(stats.despawned_at_goal == 1);
    REQUIRE((buffers.flags[0] & chaff_flags::kPendingKill) != 0);
}

TEST_CASE("same seed reproduces identical results across repeated serial runs",
          "[sim][chaff][determinism]") {
    const Rect bounds{Vec2{0.0f, 0.0f}, Vec2{80.0f, 60.0f}};
    FlowField flow = make_radial_flow(bounds, Vec2{70.0f, 30.0f}, 2.0f);
    DistanceField sdf;

    auto run = [&]() {
        SpatialHash hash = make_hash(bounds, 4.0f);
        ChaffBuffers buffers;
        buffers.reserve(600);
        Rng seed_rng(999);
        for (int i = 0; i < 400; ++i) {
            ChaffSpawnParams p;
            p.position = Vec2{seed_rng.range_f(0.0f, 80.0f), seed_rng.range_f(0.0f, 60.0f)};
            p.family = static_cast<PathogenFamily>(i % kFamilyCount);
            buffers.spawn(p);
        }

        ChaffSystem sys;
        ChaffTuning tuning = flat_tuning(20.0f, 6.0f, 1.2f, 8.0f, 0.4f, 0.3f);
        tuning.max_replications_per_tick = 16;
        sys.set_tuning(tuning);
        sys.set_world_bounds(bounds);
        sys.set_goal(Vec2{70.0f, 30.0f}, 2.0f);

        JobSystem jobs(0);   // explicitly serial — see JobSystem.h
        Rng rng(4242);
        for (int t = 0; t < 60; ++t) {
            hash.rebuild(buffers.pos_x.data(), buffers.pos_y.data(), buffers.count(), &jobs);
            sys.update(buffers, flow, sdf, TissueMask{}, hash, no_squads(), rng, kFixedDt, &jobs);
            buffers.compact();
        }
        return buffers;
    };

    const ChaffBuffers a = run();
    const ChaffBuffers b = run();

    REQUIRE(a.count() == b.count());
    for (usize i = 0; i < a.count(); ++i) {
        REQUIRE(a.pos_x[i] == b.pos_x[i]);
        REQUIRE(a.pos_y[i] == b.pos_y[i]);
        REQUIRE(a.vel_x[i] == b.vel_x[i]);
        REQUIRE(a.vel_y[i] == b.vel_y[i]);
        REQUIRE(a.family[i] == b.family[i]);
    }
}

TEST_CASE("below the parallel_for grain, serial and multi-worker results match exactly",
          "[sim][chaff][determinism]") {
    // JobSystem::parallel_for collapses to a single range whenever
    // ceil(count/grain) <= 1, regardless of worker count (core/JobSystem.cpp:
    // ranges = min(max_ranges, thread_count())). Below the default grain (256)
    // this makes the range partition — and therefore every per-range Rng fork —
    // identical between a serial run and a multi-worker run, so this is the
    // scope over which the frozen header's "does not depend on thread count"
    // is actually exact. At full 10k scale JobSystem coalesces agents into
    // however many ranges the worker count implies, so the per-range fork
    // stream boundaries (and therefore jitter/replication draws) legitimately
    // differ between thread counts; what stays true at any scale is
    // reproducibility for a *fixed* thread count, checked separately above.
    const usize kAgents = 120;   // well under the 256 default grain
    const Rect bounds{Vec2{0.0f, 0.0f}, Vec2{80.0f, 60.0f}};
    FlowField flow = make_radial_flow(bounds, Vec2{70.0f, 30.0f}, 2.0f);
    DistanceField sdf;

    auto run = [&](JobSystem* jobs) {
        SpatialHash hash = make_hash(bounds, 4.0f);
        ChaffBuffers buffers;
        buffers.reserve(kAgents + 64);
        Rng seed_rng(1234);
        for (usize i = 0; i < kAgents; ++i) {
            ChaffSpawnParams p;
            p.position = Vec2{seed_rng.range_f(0.0f, 80.0f), seed_rng.range_f(0.0f, 60.0f)};
            p.family = static_cast<PathogenFamily>(i % kFamilyCount);
            buffers.spawn(p);
        }

        ChaffSystem sys;
        ChaffTuning tuning = flat_tuning(20.0f, 6.0f, 1.2f, 8.0f, 0.4f, 0.2f);
        tuning.max_replications_per_tick = 8;
        sys.set_tuning(tuning);
        sys.set_world_bounds(bounds);
        sys.set_goal(Vec2{70.0f, 30.0f}, 2.0f);

        Rng rng(555);
        for (int t = 0; t < 40; ++t) {
            hash.rebuild(buffers.pos_x.data(), buffers.pos_y.data(), buffers.count(), jobs);
            sys.update(buffers, flow, sdf, TissueMask{}, hash, no_squads(), rng, kFixedDt, jobs);
            buffers.compact();
        }
        return buffers;
    };

    JobSystem serial(0);
    JobSystem parallel(4);
    const ChaffBuffers a = run(&serial);
    const ChaffBuffers b = run(&parallel);

    REQUIRE(a.count() == b.count());
    for (usize i = 0; i < a.count(); ++i) {
        REQUIRE(a.pos_x[i] == b.pos_x[i]);
        REQUIRE(a.pos_y[i] == b.pos_y[i]);
        REQUIRE(a.vel_x[i] == b.vel_x[i]);
        REQUIRE(a.vel_y[i] == b.vel_y[i]);
    }
}

TEST_CASE("a dense pack stops overlapping instead of stacking",
          "[sim][chaff][movement][crowd]") {
    // The movement overhaul's headline requirement, as a measurement rather
    // than a vibe: pack far more agents into a space than comfortably fit and
    // assert that they end up beside each other, not inside each other.
    //
    // This is what a velocity-only separation impulse could NOT deliver, and
    // why the positional contact pass exists. Separation is a force, forces are
    // rationed by max_speed, and that ration is already spoken for by the flow
    // field -- so however deep an overlap got, agents could only unstack at
    // walking pace, which at high density means never. The contact pass moves
    // positions directly and is not subject to that budget.
    const Rect bounds{Vec2{0.0f, 0.0f}, Vec2{60.0f, 60.0f}};
    FlowField flow = make_radial_flow(bounds, Vec2{55.0f, 30.0f});
    DistanceField sdf;   // unbaked: no walls, isolate agent-agent behaviour
    SpatialHash hash = make_hash(bounds, 4.0f);

    ChaffTuning tuning = flat_tuning(/*accel*/ 12.0f, /*max_speed*/ 6.0f,
                                     /*sep_radius*/ 1.2f, /*sep_strength*/ 8.0f,
                                     /*jitter*/ 0.0f);
    const f32 contact_radius =
        tuning.family[0].radius * tuning.family[0].contact_spacing;

    // 400 agents seeded inside a 4-unit disc. At contact_radius 1.0 that is
    // several times denser than can physically resolve, so the pass has to do
    // real work rather than getting a trivially satisfiable start.
    ChaffBuffers buffers;
    buffers.reserve(1024);
    Rng seed_rng(90210);
    for (u32 i = 0; i < 400; ++i) {
        ChaffSpawnParams p;
        p.position = Vec2{20.0f, 30.0f} + seed_rng.unit_disc() * 4.0f;
        buffers.spawn(p);
    }

    auto worst_overlap = [&](const ChaffBuffers& b) {
        f32 worst = 0.0f;   // how far inside contact_radius the closest pair is
        for (usize a = 0; a < b.count(); ++a) {
            for (usize c = a + 1; c < b.count(); ++c) {
                const f32 dx = b.pos_x[a] - b.pos_x[c];
                const f32 dy = b.pos_y[a] - b.pos_y[c];
                const f32 d = std::sqrt(dx * dx + dy * dy);
                worst = math::max(worst, contact_radius - d);
            }
        }
        return worst;
    };

    const f32 before = worst_overlap(buffers);
    INFO("initial worst overlap: " << before);
    REQUIRE(before > 0.5f);   // the seeding really is badly overlapped

    ChaffSystem sys;
    sys.set_tuning(tuning);
    sys.set_world_bounds(bounds);
    sys.set_goal(Vec2{55.0f, 30.0f}, 0.0f);   // no goal despawn

    Rng rng(7);
    for (int t = 0; t < 240; ++t) {
        rebuild(hash, buffers);
        sys.update(buffers, flow, sdf, TissueMask{}, hash, no_squads(), rng, kFixedDt, nullptr);
        buffers.compact();
    }

    REQUIRE(buffers.count() == 400);   // nothing was lost resolving the jam
    const f32 after = worst_overlap(buffers);
    INFO("worst overlap after settling: " << after << " (was " << before << ")");

    // Relaxation is iterative and the crowd is still being driven together by
    // the flow field, so this is "essentially touching, not interpenetrating"
    // rather than a hard geometric guarantee -- a quarter of a radius.
    REQUIRE(after < contact_radius * 0.25f);
    REQUIRE(after < before * 0.5f);
}

TEST_CASE("spawn_burst never places two agents inside each other",
          "[sim][chaff][spawn]") {
    // Uniform-random placement in a disc (what this used to do) overlaps by
    // construction -- it is the birthday problem, so a burst of any size
    // reliably produced interpenetrating agents that the contact pass then had
    // to unpick over the following ticks. The phyllotaxis packing places them
    // evenly instead, so a burst is clean the instant it appears.
    //
    // Measured at spawn time, with no ticks run: this is about the spawn
    // pattern itself, not about relaxation rescuing it afterwards.
    ChaffTuning tuning = flat_tuning(/*accel*/ 0.0f, /*max_speed*/ 6.0f,
                                     /*sep_radius*/ 1.2f, /*sep_strength*/ 8.0f,
                                     /*jitter*/ 0.0f);
    ChaffSystem sys;
    sys.set_tuning(tuning);

    const f32 contact_d =
        tuning.family[0].radius * tuning.family[0].contact_spacing;

    for (const u32 count : {8u, 60u, 250u}) {
        ChaffBuffers buffers;
        buffers.reserve(512);
        Rng rng(4242);
        // A spawn point far too small to hold the burst, to prove the packing grows
        // the disc rather than stacking agents inside the requested radius.
        const u32 spawned =
            sys.spawn_burst(buffers, PathogenFamily::Virus, Vec2{50.0f, 50.0f},
                            /*spawn_point_radius*/ 1.5f, count, rng);
        REQUIRE(spawned == count);

        f32 worst = 0.0f;
        for (usize a = 0; a < buffers.count(); ++a) {
            for (usize b = a + 1; b < buffers.count(); ++b) {
                const f32 dx = buffers.pos_x[a] - buffers.pos_x[b];
                const f32 dy = buffers.pos_y[a] - buffers.pos_y[b];
                worst = math::max(worst, contact_d - std::sqrt(dx * dx + dy * dy));
            }
        }
        INFO("count=" << count << " worst penetration=" << worst
                      << " (contact diameter " << contact_d << ")");
        REQUIRE(worst <= 0.0f);
    }
}

TEST_CASE("spawn_burst stays deterministic and varies between bursts",
          "[sim][chaff][spawn]") {
    ChaffTuning tuning = flat_tuning(0.0f, 6.0f, 1.2f, 8.0f, 0.0f);
    ChaffSystem sys;
    sys.set_tuning(tuning);

    auto burst = [&](u64 seed) {
        ChaffBuffers b;
        b.reserve(64);
        Rng rng(seed);
        sys.spawn_burst(b, PathogenFamily::Bacteria, Vec2{10.0f, 10.0f}, 4.0f, 24, rng);
        std::vector<f32> out;
        for (usize i = 0; i < b.count(); ++i) { out.push_back(b.pos_x[i]); out.push_back(b.pos_y[i]); }
        return out;
    };

    // Same seed -> identical placement (the pattern is a pure function of the
    // one phase draw), different seed -> a rotated pattern, so repeated waves
    // out of one spawn point are not stamped on top of each other.
    REQUIRE(burst(7) == burst(7));
    REQUIRE(burst(7) != burst(8));
}

TEST_CASE("a crushing crowd cannot be squeezed out through a lane wall",
          "[sim][chaff][walls]") {
    // Reported from play: past a certain local density, agents jammed against
    // tissue start appearing on the wrong side of it.
    //
    // Shaped like a real level rather than like a physics demo, because that is
    // where it happens: a narrow lane with solid tissue either side, a flow
    // field running along it, and far more horde in it than it comfortably
    // holds. The pressure that squeezes agents out is the crowd's own, not a
    // contrived head-on impact.
    constexpr f32 kCell = 1.0f;          // as coarse as the widened levels use
    constexpr f32 kLaneHalf = 6.0f;      // narrow: pressure has nowhere to go
    const Rect bounds{Vec2{0.0f, 0.0f}, Vec2{120.0f, 60.0f}};
    const f32 mid_y = 30.0f;

    TissueMask mask;
    const i32 w = static_cast<i32>(bounds.size().x / kCell);
    const i32 h = static_cast<i32>(bounds.size().y / kCell);
    mask.resize(w, h, kCell, bounds.min);
    for (i32 y = 0; y < h; ++y) {
        const f32 wy = (static_cast<f32>(y) + 0.5f) * kCell;
        const bool inside = std::fabs(wy - mid_y) <= kLaneHalf;
        for (i32 x = 0; x < w; ++x) mask.set_walkable(x, y, inside);
    }
    DistanceField sdf;
    sdf.bake(mask);

    FlowField flow;
    FlowFieldBakeDesc fd;
    fd.goal_cells = {mask.world_to_cell(Vec2{116.0f, mid_y})};
    flow.bake(mask, fd);

    SpatialHash hash = make_hash(bounds);
    ChaffSystem sys;
    ChaffTuning t = flat_tuning(/*accel*/ 40.0f, /*max_speed*/ 14.0f, /*sep_radius*/ 1.6f,
                                /*sep_strength*/ 8.0f, /*jitter*/ 0.3f);
    for (u32 f = 0; f < kFamilyCount; ++f) {
        t.family[f].radius = 0.5f;
        t.family[f].contact_spacing = 2.0f;
        t.family[f].contact_stiffness = 1.0f;
        t.family[f].pressure_gain = 0.5f;
        t.family[f].pressure_max = 8.0f;
    }
    sys.set_tuning(t);
    sys.set_world_bounds(bounds);
    sys.set_goal(Vec2{116.0f, mid_y}, 0.0f);

    ChaffBuffers buffers;
    buffers.reserve(4000);
    Rng rng(6502);
    // 1600 agents into a lane 12 units wide: several times what the width can
    // carry, so the crowd is permanently over-packed against both walls.
    for (u32 i = 0; i < 1600; ++i) {
        ChaffSpawnParams p;
        p.position = Vec2{6.0f + static_cast<f32>(i % 60) * 0.55f,
                          mid_y - 5.0f + static_cast<f32>((i / 60) % 20) * 0.5f};
        p.density = 1.0f;
        buffers.spawn(p);
    }

    u32 worst_outside = 0;
    f32 deepest = 0.0f;
    for (u32 tick = 0; tick < 900; ++tick) {
        rebuild(hash, buffers);
        sys.update(buffers, flow, sdf, mask, hash, no_squads(), rng, kFixedDt, nullptr);

        u32 outside = 0;
        for (usize i = 0; i < buffers.count(); ++i) {
            const f32 dy = std::fabs(buffers.pos_y[i] - mid_y);
            if (dy > kLaneHalf) {
                ++outside;
                deepest = math::max(deepest, dy - kLaneHalf);
            }
        }
        worst_outside = math::max(worst_outside, outside);
    }

    INFO("worst outside the lane " << worst_outside << " of " << buffers.count()
                                   << ", deepest " << deepest << " units into tissue");
    // Agents may touch the wall -- their centre can sit a hair outside while
    // their body overlaps it -- but nothing may end up meaningfully embedded in
    // solid tissue, and nothing at all may end up beyond it.
    REQUIRE(deepest < 1.0f);
}

TEST_CASE("a crowd cannot be pushed through a tower footprint", "[sim][chaff][walls][tower]") {
    // Placing a tower calls sim::block_rect() to mark its footprint
    // non-walkable and FlowField::mark_dirty() to reroute the horde around it.
    // The DistanceField is NOT rebaked -- so the wall-contact resolver, which
    // reads only the SDF, has never heard of the tower. The flow field steers
    // agents around it, but nothing physically stops them entering it, and once
    // the crowd is dense enough the pressure behind simply pushes them through.
    //
    // This reproduces that directly: bake the field, THEN block a rectangle the
    // way a tower does, then drive a dense crowd at it.
    constexpr f32 kCell = 0.5f;
    const Rect bounds{Vec2{0.0f, 0.0f}, Vec2{80.0f, 40.0f}};
    const f32 mid_y = 20.0f;

    TissueMask mask;
    const i32 w = static_cast<i32>(bounds.size().x / kCell);
    const i32 h = static_cast<i32>(bounds.size().y / kCell);
    mask.resize(w, h, kCell, bounds.min);
    for (i32 y = 0; y < h; ++y) {
        const f32 wy = (static_cast<f32>(y) + 0.5f) * kCell;
        for (i32 x = 0; x < w; ++x) mask.set_walkable(x, y, std::fabs(wy - mid_y) <= 9.0f);
    }
    DistanceField sdf;
    sdf.bake(mask);   // baked BEFORE the tower, exactly as the game does

    // The tower: a solid block mid-lane, leaving gaps above and below so the
    // flow field still has a route and the horde is not simply dammed.
    const Rect footprint{Vec2{38.0f, mid_y - 4.0f}, Vec2{44.0f, mid_y + 4.0f}};
    sim::block_rect(mask, footprint);

    FlowField flow;
    FlowFieldBakeDesc fd;
    fd.goal_cells = {mask.world_to_cell(Vec2{76.0f, mid_y})};
    flow.bake(mask, fd);

    SpatialHash hash = make_hash(bounds);
    ChaffSystem sys;
    ChaffTuning t = flat_tuning(/*accel*/ 40.0f, /*max_speed*/ 14.0f, /*sep_radius*/ 1.6f,
                                /*sep_strength*/ 8.0f, /*jitter*/ 0.3f);
    for (u32 f = 0; f < kFamilyCount; ++f) {
        t.family[f].radius = 0.5f;
        t.family[f].contact_spacing = 2.0f;
        t.family[f].contact_stiffness = 1.0f;
        t.family[f].pressure_gain = 0.5f;
        t.family[f].pressure_max = 8.0f;
    }
    sys.set_tuning(t);
    sys.set_world_bounds(bounds);
    sys.set_goal(Vec2{76.0f, mid_y}, 0.0f);

    ChaffBuffers buffers;
    buffers.reserve(3000);
    Rng rng(1234);
    for (u32 i = 0; i < 1200; ++i) {
        ChaffSpawnParams p;
        p.position = Vec2{4.0f + static_cast<f32>(i % 40) * 0.6f,
                          mid_y - 7.0f + static_cast<f32>((i / 40) % 30) * 0.5f};
        p.density = 1.0f;
        buffers.spawn(p);
    }

    u32 worst_in_tower = 0;
    for (u32 tick = 0; tick < 900; ++tick) {
        rebuild(hash, buffers);
        sys.update(buffers, flow, sdf, mask, hash, no_squads(), rng, kFixedDt, nullptr);
        u32 in_tower = 0;
        for (usize i = 0; i < buffers.count(); ++i) {
            if (footprint.contains(Vec2{buffers.pos_x[i], buffers.pos_y[i]})) ++in_tower;
        }
        worst_in_tower = math::max(worst_in_tower, in_tower);
    }
    INFO("worst agents inside the tower footprint: " << worst_in_tower << " of "
                                                     << buffers.count());
    REQUIRE(worst_in_tower == 0);
}
