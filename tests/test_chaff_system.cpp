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
#include "sim/spatial/SpatialHash.h"

#include "core/JobSystem.h"
#include "core/Math.h"
#include "core/Rng.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace immune;
using namespace immune::sim;

namespace {

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
        sys.update(buffers, flow, hash, rng, kFixedDt, nullptr);
    }

    REQUIRE(buffers.count() == 1);
    REQUIRE(buffers.pos_x[0] > 5.0f);   // moved toward the goal, i.e. +x
}

TEST_CASE("kDrifting agents ignore the flow field and follow ambient drift",
          "[sim][chaff][movement][flags]") {
    const Rect bounds{Vec2{0.0f, 0.0f}, Vec2{50.0f, 50.0f}};
    const Vec2 goal{45.0f, 25.0f};   // strongly to the +x
    FlowField flow = make_radial_flow(bounds, goal);
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
        sys.update(buffers, flow, hash, rng, kFixedDt, nullptr);
    }

    REQUIRE(buffers.count() == 1);
    REQUIRE(buffers.pos_y[0] < 25.0f);            // drifted down
    REQUIRE(buffers.pos_x[0] == Catch::Approx(25.0f).margin(0.01f)); // NOT pulled toward goal
}

TEST_CASE("kClumped disables separation", "[sim][chaff][movement][flags]") {
    const Rect bounds{Vec2{0.0f, 0.0f}, Vec2{50.0f, 50.0f}};
    FlowField flow = make_radial_flow(bounds, Vec2{25.0f, 25.0f});
    SpatialHash hash = make_hash(bounds, 4.0f);

    auto dist = [](const ChaffBuffers& b) {
        const f32 dx = b.pos_x[0] - b.pos_x[1];
        const f32 dy = b.pos_y[0] - b.pos_y[1];
        return std::sqrt(dx * dx + dy * dy);
    };

    ChaffTuning tuning = flat_tuning(/*accel*/ 0.0f, /*max_speed*/ 50.0f,
                                     /*sep_radius*/ 1.0f, /*sep_strength*/ 200.0f,
                                     /*jitter*/ 0.0f);

    ChaffBuffers clumped;
    clumped.reserve(4);
    {
        ChaffSpawnParams p; p.position = Vec2{10.0f, 10.0f}; p.flags = chaff_flags::kClumped;
        clumped.spawn(p);
    }
    {
        ChaffSpawnParams p; p.position = Vec2{10.05f, 10.0f}; p.flags = chaff_flags::kClumped;
        clumped.spawn(p);
    }
    const f32 before_clumped = dist(clumped);

    ChaffBuffers free_pair;
    free_pair.reserve(4);
    {
        ChaffSpawnParams p; p.position = Vec2{10.0f, 10.0f};
        free_pair.spawn(p);
    }
    {
        ChaffSpawnParams p; p.position = Vec2{10.05f, 10.0f};
        free_pair.spawn(p);
    }
    const f32 before_free = dist(free_pair);
    REQUIRE(before_clumped == Catch::Approx(before_free));

    ChaffSystem sys;
    sys.set_tuning(tuning);
    sys.set_world_bounds(bounds);
    sys.set_goal(Vec2{25.0f, 25.0f}, 0.0f);

    Rng rng_a(3), rng_b(3);
    rebuild(hash, clumped);
    sys.update(clumped, flow, hash, rng_a, kFixedDt, nullptr);
    rebuild(hash, free_pair);
    sys.update(free_pair, flow, hash, rng_b, kFixedDt, nullptr);

    REQUIRE(dist(clumped) == Catch::Approx(before_clumped).margin(1e-5f));  // unchanged
    REQUIRE(dist(free_pair) > before_free + 1e-4f);                          // pushed apart
}

TEST_CASE("kSlowed lowers the effective max speed", "[sim][chaff][movement][flags]") {
    const Rect bounds{Vec2{0.0f, 0.0f}, Vec2{200.0f, 200.0f}};
    FlowField flow = make_radial_flow(bounds, Vec2{190.0f, 100.0f}, 2.0f);
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
    sys.update(buffers, flow, hash, rng, kFixedDt, nullptr);

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
        sys.update(buffers, flow, hash, rng, kFixedDt, nullptr);
        REQUIRE(buffers.pos_x[0] == Catch::Approx(25.0f));
        REQUIRE(buffers.pos_y[0] == Catch::Approx(25.0f));
        REQUIRE(buffers.vel_x[0] == 0.0f);
        REQUIRE(buffers.vel_y[0] == 0.0f);
    }
}

TEST_CASE("replication respects the per-tick global cap", "[sim][chaff][replication]") {
    const Rect bounds{Vec2{0.0f, 0.0f}, Vec2{50.0f, 50.0f}};
    FlowField flow = make_radial_flow(bounds, Vec2{25.0f, 25.0f});
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
    const ChaffUpdateStats stats = sys.update(buffers, flow, hash, rng, kFixedDt, nullptr);

    REQUIRE(stats.replicated == 3);
    REQUIRE(buffers.count() == 53);
}

TEST_CASE("agents leaving the world bounds are flagged for despawn", "[sim][chaff][despawn]") {
    const Rect bounds{Vec2{0.0f, 0.0f}, Vec2{20.0f, 20.0f}};
    FlowField flow = make_radial_flow(bounds, Vec2{10.0f, 10.0f});
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
    const ChaffUpdateStats stats = sys.update(buffers, flow, hash, rng, kFixedDt, nullptr);

    REQUIRE(stats.despawned_out_of_bounds == 1);
    REQUIRE((buffers.flags[0] & chaff_flags::kPendingKill) != 0);
    REQUIRE(buffers.compact() == 1);
    REQUIRE(buffers.count() == 0);
}

TEST_CASE("agents reaching the goal are flagged for despawn", "[sim][chaff][despawn]") {
    const Rect bounds{Vec2{0.0f, 0.0f}, Vec2{50.0f, 50.0f}};
    const Vec2 goal{25.0f, 25.0f};
    FlowField flow = make_radial_flow(bounds, goal);
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
    const ChaffUpdateStats stats = sys.update(buffers, flow, hash, rng, kFixedDt, nullptr);

    REQUIRE(stats.despawned_at_goal == 1);
    REQUIRE((buffers.flags[0] & chaff_flags::kPendingKill) != 0);
}

TEST_CASE("same seed reproduces identical results across repeated serial runs",
          "[sim][chaff][determinism]") {
    const Rect bounds{Vec2{0.0f, 0.0f}, Vec2{80.0f, 60.0f}};
    FlowField flow = make_radial_flow(bounds, Vec2{70.0f, 30.0f}, 2.0f);

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
            sys.update(buffers, flow, hash, rng, kFixedDt, &jobs);
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
            sys.update(buffers, flow, hash, rng, kFixedDt, jobs);
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
