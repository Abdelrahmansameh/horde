// Tests for aggregate damage evaluation. Owner: Wave 2A.
//
// Correctness is checked the same way test_spatial_hash.cpp checks its own
// conservative queries: a shape's exact-test result must exclude candidates
// that are in the same spatial-hash cell superset but geometrically outside
// the shape.
#include "sim/damage/DamageField.h"

#include "core/Rng.h"
#include "sim/chaff/ChaffBuffers.h"
#include "sim/spatial/SpatialHash.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>

using namespace immune;
using namespace immune::sim;

namespace {

SpatialHash make_hash(Rect bounds, f32 cell_size) {
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

u32 spawn_at(ChaffBuffers& b, Vec2 pos, PathogenFamily fam = PathogenFamily::Virus,
            f32 density = 1.0f, u8 flags = 0) {
    ChaffSpawnParams p;
    p.position = pos;
    p.family = fam;
    p.density = density;
    p.flags = flags;
    b.spawn(p);
    return static_cast<u32>(b.count() - 1);
}

} // namespace

TEST_CASE("circle field only damages agents inside the exact radius", "[damage][shape]") {
    const Rect bounds{Vec2{0.0f, 0.0f}, Vec2{50.0f, 50.0f}};
    // Coarse cell so the conservative query returns a wide superset around a
    // small circle -- exercises the exact-test, not just the cell filter.
    SpatialHash hash = make_hash(bounds, 8.0f);

    ChaffBuffers chaff;
    chaff.reserve(8);
    const u32 inside = spawn_at(chaff, Vec2{25.0f, 25.0f});        // dead centre
    const u32 near_edge_in = spawn_at(chaff, Vec2{27.9f, 25.0f});  // radius 3 -> inside
    const u32 outside = spawn_at(chaff, Vec2{29.5f, 25.0f});       // same cell, outside radius
    const u32 far_outside = spawn_at(chaff, Vec2{40.0f, 40.0f});   // different cell entirely

    rebuild(hash, chaff);

    DamageField field;
    field.shape = FieldShape::Circle;
    field.origin = Vec2{25.0f, 25.0f};
    field.radius = 3.0f;
    field.kill_rate = 10.0f;   // large: guarantees measurable loss in one tick

    DamageSystem sys;
    sys.set_mode(ThinningMode::DensityThinning);
    sys.submit(field);

    Rng rng(1);
    const DamageStats stats = sys.apply(chaff, hash, rng, kFixedDt);

    REQUIRE(chaff.density[inside] < 1.0f);
    REQUIRE(chaff.density[near_edge_in] < 1.0f);
    REQUIRE(chaff.density[outside] == Catch::Approx(1.0f));
    REQUIRE(chaff.density[far_outside] == Catch::Approx(1.0f));
    REQUIRE(stats.fields_evaluated == 1);
    REQUIRE(stats.density_removed > 0.0f);
}

TEST_CASE("rect field only damages agents inside the exact rectangle", "[damage][shape]") {
    const Rect bounds{Vec2{0.0f, 0.0f}, Vec2{50.0f, 50.0f}};
    SpatialHash hash = make_hash(bounds, 8.0f);

    ChaffBuffers chaff;
    chaff.reserve(8);
    const u32 inside = spawn_at(chaff, Vec2{12.0f, 12.0f});
    const u32 outside_same_cell = spawn_at(chaff, Vec2{19.0f, 19.0f});   // same 8x8 cell, outside rect
    const u32 outside_far = spawn_at(chaff, Vec2{45.0f, 45.0f});

    rebuild(hash, chaff);

    DamageField field;
    field.shape = FieldShape::Rect;
    field.rect = Rect{Vec2{10.0f, 10.0f}, Vec2{15.0f, 15.0f}};
    field.kill_rate = 10.0f;

    DamageSystem sys;
    sys.set_mode(ThinningMode::DensityThinning);
    sys.submit(field);

    Rng rng(1);
    sys.apply(chaff, hash, rng, kFixedDt);

    REQUIRE(chaff.density[inside] < 1.0f);
    REQUIRE(chaff.density[outside_same_cell] == Catch::Approx(1.0f));
    REQUIRE(chaff.density[outside_far] == Catch::Approx(1.0f));
}

TEST_CASE("cone field respects both the angle and the distance test", "[damage][shape]") {
    const Rect bounds{Vec2{0.0f, 0.0f}, Vec2{100.0f, 100.0f}};
    SpatialHash hash = make_hash(bounds, 4.0f);

    ChaffBuffers chaff;
    chaff.reserve(8);
    const Vec2 origin{50.0f, 50.0f};
    const u32 in_arc = spawn_at(chaff, origin + Vec2{10.0f, 0.0f});        // straight ahead
    const u32 behind = spawn_at(chaff, origin + Vec2{-10.0f, 0.0f});       // same radius, opposite side
    const u32 too_wide = spawn_at(chaff, origin + Vec2{5.0f, 9.0f});       // within radius, outside half-angle
    const u32 too_far = spawn_at(chaff, origin + Vec2{25.0f, 0.0f});       // in arc direction, past radius

    rebuild(hash, chaff);

    DamageField field;
    field.shape = FieldShape::Cone;
    field.origin = origin;
    field.direction = Vec2{1.0f, 0.0f};
    field.radius = 15.0f;
    field.arc_radians = 0.3f;   // ~17 degrees half angle
    field.kill_rate = 10.0f;

    DamageSystem sys;
    sys.set_mode(ThinningMode::DensityThinning);
    sys.submit(field);

    Rng rng(1);
    sys.apply(chaff, hash, rng, kFixedDt);

    REQUIRE(chaff.density[in_arc] < 1.0f);
    REQUIRE(chaff.density[behind] == Catch::Approx(1.0f));
    REQUIRE(chaff.density[too_wide] == Catch::Approx(1.0f));
    REQUIRE(chaff.density[too_far] == Catch::Approx(1.0f));
}

TEST_CASE("chain field jumps to successive nearest agents, not everything in range",
          "[damage][shape][chain]") {
    const Rect bounds{Vec2{0.0f, 0.0f}, Vec2{100.0f, 20.0f}};
    SpatialHash hash = make_hash(bounds, 4.0f);

    ChaffBuffers chaff;
    chaff.reserve(16);
    // A line of agents spaced 2 apart from x=10 to x=20 (6 agents), reachable
    // by 2-unit chain hops; one agent far off to the side must never be hit.
    std::vector<u32> line;
    for (int i = 0; i < 6; ++i) {
        line.push_back(spawn_at(chaff, Vec2{10.0f + 2.0f * static_cast<f32>(i), 10.0f}));
    }
    const u32 off_to_side = spawn_at(chaff, Vec2{10.0f, 19.0f});   // far from the chain path

    rebuild(hash, chaff);

    DamageField field;
    field.shape = FieldShape::Chain;
    field.origin = Vec2{10.0f, 10.0f};
    field.radius = 2.5f;   // covers each 2-unit hop, not the off-path agent
    field.kill_rate = 10.0f;

    DamageSystem sys;
    sys.set_mode(ThinningMode::DensityThinning);
    sys.submit(field);

    Rng rng(1);
    sys.apply(chaff, hash, rng, kFixedDt);

    for (u32 idx : line) {
        REQUIRE(chaff.density[idx] < 1.0f);
    }
    REQUIRE(chaff.density[off_to_side] == Catch::Approx(1.0f));
}

TEST_CASE("family_mask excludes non-matching families", "[damage][family]") {
    const Rect bounds{Vec2{0.0f, 0.0f}, Vec2{50.0f, 50.0f}};
    SpatialHash hash = make_hash(bounds, 4.0f);

    ChaffBuffers chaff;
    chaff.reserve(8);
    const u32 virus = spawn_at(chaff, Vec2{25.0f, 25.0f}, PathogenFamily::Virus);
    const u32 bacteria = spawn_at(chaff, Vec2{25.1f, 25.0f}, PathogenFamily::Bacteria);

    rebuild(hash, chaff);

    DamageField field;
    field.shape = FieldShape::Circle;
    field.origin = Vec2{25.0f, 25.0f};
    field.radius = 3.0f;
    field.kill_rate = 10.0f;
    field.family_mask = static_cast<u8>(1u << static_cast<u8>(PathogenFamily::Virus));

    DamageSystem sys;
    sys.submit(field);

    Rng rng(1);
    const DamageStats stats = sys.apply(chaff, hash, rng, kFixedDt);

    REQUIRE(chaff.density[virus] < 1.0f);
    REQUIRE(chaff.density[bacteria] == Catch::Approx(1.0f));
    REQUIRE(stats.density_removed_by_family[static_cast<u32>(PathogenFamily::Virus)] > 0.0f);
    REQUIRE(stats.density_removed_by_family[static_cast<u32>(PathogenFamily::Bacteria)] == 0.0f);
}

TEST_CASE("DensityThinning measurably reduces a horde over several ticks", "[damage][thinning]") {
    const Rect bounds{Vec2{0.0f, 0.0f}, Vec2{50.0f, 50.0f}};
    SpatialHash hash = make_hash(bounds, 4.0f);

    ChaffBuffers chaff;
    chaff.reserve(64);
    Rng seed_rng(2);
    for (int i = 0; i < 40; ++i) {
        spawn_at(chaff, Vec2{25.0f + seed_rng.range_f(-4.0f, 4.0f), 25.0f + seed_rng.range_f(-4.0f, 4.0f)});
    }
    const f32 before = chaff.total_density();

    DamageField field;
    field.shape = FieldShape::Circle;
    field.origin = Vec2{25.0f, 25.0f};
    field.radius = 6.0f;
    field.kill_rate = 0.5f;

    DamageSystem sys;
    sys.set_mode(ThinningMode::DensityThinning);

    Rng rng(3);
    for (int t = 0; t < 30; ++t) {
        rebuild(hash, chaff);
        sys.submit(field);   // persistent field: owner re-submits every tick
        sys.apply(chaff, hash, rng, kFixedDt);
        chaff.compact();
        sys.clear_transient(kFixedDt);
    }

    REQUIRE(chaff.total_density() < before);
}

TEST_CASE("ProbabilisticRemoval measurably reduces a horde over several ticks", "[damage][thinning]") {
    const Rect bounds{Vec2{0.0f, 0.0f}, Vec2{50.0f, 50.0f}};
    SpatialHash hash = make_hash(bounds, 4.0f);

    ChaffBuffers chaff;
    chaff.reserve(64);
    Rng seed_rng(2);
    for (int i = 0; i < 40; ++i) {
        spawn_at(chaff, Vec2{25.0f + seed_rng.range_f(-4.0f, 4.0f), 25.0f + seed_rng.range_f(-4.0f, 4.0f)});
    }
    const usize count_before = chaff.count();

    DamageField field;
    field.shape = FieldShape::Circle;
    field.origin = Vec2{25.0f, 25.0f};
    field.radius = 6.0f;
    field.kill_rate = 3.0f;   // probability = kill_rate * dt per tick

    DamageSystem sys;
    sys.set_mode(ThinningMode::ProbabilisticRemoval);

    Rng rng(3);
    for (int t = 0; t < 30; ++t) {
        rebuild(hash, chaff);
        sys.submit(field);
        sys.apply(chaff, hash, rng, kFixedDt);
        chaff.compact();
        sys.clear_transient(kFixedDt);
    }

    REQUIRE(chaff.count() < count_before);
}

TEST_CASE("ProbabilisticRemoval is deterministic for a fixed seed", "[damage][thinning][determinism]") {
    const Rect bounds{Vec2{0.0f, 0.0f}, Vec2{50.0f, 50.0f}};

    auto run = [&]() {
        SpatialHash hash = make_hash(bounds, 4.0f);
        ChaffBuffers chaff;
        chaff.reserve(64);
        Rng seed_rng(2);
        for (int i = 0; i < 40; ++i) {
            spawn_at(chaff, Vec2{25.0f + seed_rng.range_f(-4.0f, 4.0f), 25.0f + seed_rng.range_f(-4.0f, 4.0f)});
        }

        DamageField field;
        field.shape = FieldShape::Circle;
        field.origin = Vec2{25.0f, 25.0f};
        field.radius = 6.0f;
        field.kill_rate = 3.0f;

        DamageSystem sys;
        sys.set_mode(ThinningMode::ProbabilisticRemoval);

        Rng rng(3);
        for (int t = 0; t < 10; ++t) {
            rebuild(hash, chaff);
            sys.submit(field);
            sys.apply(chaff, hash, rng, kFixedDt);
            // Deliberately no compact(): keep indices meaningful across the two
            // runs so the density snapshot below compares apples to apples.
            sys.clear_transient(kFixedDt);
        }
        return chaff;
    };

    const ChaffBuffers a = run();
    const ChaffBuffers b = run();

    REQUIRE(a.count() == b.count());
    for (usize i = 0; i < a.count(); ++i) {
        REQUIRE(a.density[i] == b.density[i]);
        REQUIRE(a.flags[i] == b.flags[i]);
    }
}

TEST_CASE("ProbabilisticRemoval does not depend on the spatial hash's candidate traversal order",
          "[damage][thinning][determinism]") {
    // Same ChaffBuffers array (same index <-> agent mapping) queried through
    // two differently-sized grids, so query_circle enumerates the identical
    // candidate SET in a different order. The per-agent fork is keyed on the
    // agent's array index, not loop position, so the removal outcome must be
    // identical either way.
    const Rect bounds{Vec2{0.0f, 0.0f}, Vec2{50.0f, 50.0f}};

    ChaffBuffers chaff;
    chaff.reserve(64);
    Rng seed_rng(9);
    for (int i = 0; i < 50; ++i) {
        spawn_at(chaff, Vec2{25.0f + seed_rng.range_f(-8.0f, 8.0f), 25.0f + seed_rng.range_f(-8.0f, 8.0f)});
    }

    DamageField field;
    field.shape = FieldShape::Circle;
    field.origin = Vec2{25.0f, 25.0f};
    field.radius = 8.0f;
    field.kill_rate = 5.0f;

    auto run_with_cell_size = [&](f32 cell_size) {
        ChaffBuffers copy = chaff;   // identical array, same indices
        SpatialHash hash = make_hash(bounds, cell_size);
        hash.rebuild(copy.pos_x.data(), copy.pos_y.data(), copy.count(), nullptr);

        DamageSystem sys;
        sys.set_mode(ThinningMode::ProbabilisticRemoval);
        sys.submit(field);

        Rng rng(3);
        sys.apply(copy, hash, rng, kFixedDt);
        return copy;
    };

    const ChaffBuffers a = run_with_cell_size(1.5f);
    const ChaffBuffers b = run_with_cell_size(11.0f);   // coarse: whole query is ~1 cell

    REQUIRE(a.count() == b.count());
    for (usize i = 0; i < a.count(); ++i) {
        REQUIRE(a.density[i] == b.density[i]);
        REQUIRE(a.flags[i] == b.flags[i]);
    }
}

TEST_CASE("kMarked agents take the marked_multiplier bonus", "[damage][marked]") {
    const Rect bounds{Vec2{0.0f, 0.0f}, Vec2{50.0f, 50.0f}};
    SpatialHash hash = make_hash(bounds, 4.0f);

    ChaffBuffers chaff;
    chaff.reserve(8);
    const u32 plain = spawn_at(chaff, Vec2{24.0f, 25.0f}, PathogenFamily::Virus, 10.0f);
    const u32 marked = spawn_at(chaff, Vec2{26.0f, 25.0f}, PathogenFamily::Virus, 10.0f,
                                chaff_flags::kMarked);

    rebuild(hash, chaff);

    DamageField field;
    field.shape = FieldShape::Circle;
    field.origin = Vec2{25.0f, 25.0f};
    field.radius = 5.0f;
    field.kill_rate = 1.0f;
    field.marked_multiplier = 3.0f;

    DamageSystem sys;
    sys.set_mode(ThinningMode::DensityThinning);
    sys.submit(field);

    Rng rng(1);
    sys.apply(chaff, hash, rng, kFixedDt);

    const f32 plain_loss = 10.0f - chaff.density[plain];
    const f32 marked_loss = 10.0f - chaff.density[marked];
    REQUIRE(plain_loss > 0.0f);
    REQUIRE(marked_loss == Catch::Approx(plain_loss * 3.0f).margin(1e-4f));
}

TEST_CASE("measure_density does not mutate any agent state", "[damage][measure]") {
    const Rect bounds{Vec2{0.0f, 0.0f}, Vec2{50.0f, 50.0f}};
    SpatialHash hash = make_hash(bounds, 4.0f);

    ChaffBuffers chaff;
    chaff.reserve(16);
    Rng seed_rng(5);
    for (int i = 0; i < 12; ++i) {
        spawn_at(chaff, Vec2{25.0f + seed_rng.range_f(-6.0f, 6.0f), 25.0f + seed_rng.range_f(-6.0f, 6.0f)});
    }
    rebuild(hash, chaff);

    const std::vector<f32> density_before = chaff.density;
    const std::vector<u8> flags_before = chaff.flags;
    const f32 total_before = chaff.total_density();

    DamageField region;
    region.shape = FieldShape::Circle;
    region.origin = Vec2{25.0f, 25.0f};
    region.radius = 6.0f;

    DamageSystem sys;
    const f32 measured = sys.measure_density(chaff, hash, region);

    // Cross-check against a brute-force exact sum.
    f32 expected = 0.0f;
    for (usize i = 0; i < chaff.count(); ++i) {
        const f32 dx = chaff.pos_x[i] - region.origin.x, dy = chaff.pos_y[i] - region.origin.y;
        if (dx * dx + dy * dy <= region.radius * region.radius) expected += chaff.density[i];
    }
    REQUIRE(measured == Catch::Approx(expected));

    for (usize i = 0; i < chaff.count(); ++i) {
        REQUIRE(chaff.density[i] == density_before[i]);
        REQUIRE(chaff.flags[i] == flags_before[i]);
    }
    REQUIRE(chaff.total_density() == Catch::Approx(total_before));
}

TEST_CASE("agents_killed counts only agents that die during this call", "[damage][stats]") {
    const Rect bounds{Vec2{0.0f, 0.0f}, Vec2{50.0f, 50.0f}};
    SpatialHash hash = make_hash(bounds, 4.0f);

    ChaffBuffers chaff;
    chaff.reserve(8);
    const u32 low_density = spawn_at(chaff, Vec2{25.0f, 25.0f}, PathogenFamily::Virus, 0.5f);
    const u32 high_density = spawn_at(chaff, Vec2{25.1f, 25.0f}, PathogenFamily::Virus, 100.0f);

    rebuild(hash, chaff);

    DamageField field;
    field.shape = FieldShape::Circle;
    field.origin = Vec2{25.0f, 25.0f};
    field.radius = 5.0f;
    field.kill_rate = 60.0f;   // 60 * dt(1/60) == 1.0 removed this tick

    DamageSystem sys;
    sys.set_mode(ThinningMode::DensityThinning);
    sys.submit(field);

    Rng rng(1);
    const DamageStats stats = sys.apply(chaff, hash, rng, kFixedDt);

    REQUIRE((chaff.flags[low_density] & chaff_flags::kPendingKill) != 0);
    REQUIRE((chaff.flags[high_density] & chaff_flags::kPendingKill) == 0);
    REQUIRE(stats.agents_killed == 1);
}

TEST_CASE("regression: a dense chaff cluster measurably erodes under one field over many ticks",
          "[damage][regression]") {
    // The pattern other waves' --sim-test scripts will match: spawn a cluster,
    // submit one persistent field, run ticks, total density must drop.
    const Rect bounds{Vec2{0.0f, 0.0f}, Vec2{60.0f, 60.0f}};
    SpatialHash hash = make_hash(bounds, 4.0f);

    ChaffBuffers chaff;
    chaff.reserve(2048);
    Rng seed_rng(42);
    for (int i = 0; i < 800; ++i) {
        spawn_at(chaff, Vec2{30.0f + seed_rng.range_f(-10.0f, 10.0f), 30.0f + seed_rng.range_f(-10.0f, 10.0f)});
    }
    const f32 before = chaff.total_density();
    REQUIRE(before > 0.0f);

    DamageField field;
    field.shape = FieldShape::Circle;
    field.origin = Vec2{30.0f, 30.0f};
    field.radius = 12.0f;
    field.kill_rate = 0.8f;

    DamageSystem sys;
    sys.set_mode(ThinningMode::DensityThinning);

    Rng rng(7);
    for (int t = 0; t < 120; ++t) {
        rebuild(hash, chaff);
        sys.submit(field);
        sys.apply(chaff, hash, rng, kFixedDt);
        chaff.compact();
        sys.clear_transient(kFixedDt);
    }

    REQUIRE(chaff.total_density() < before * 0.9f);
}

TEST_CASE("apply() stays well under budget at 10k agents with multiple active fields",
          "[damage][perf]") {
    const Rect bounds{Vec2{0.0f, 0.0f}, Vec2{256.0f, 144.0f}};
    SpatialHash hash = make_hash(bounds, 4.0f);

    ChaffBuffers chaff;
    chaff.reserve(10000);
    Rng seed_rng(123);
    for (int i = 0; i < 10000; ++i) {
        spawn_at(chaff, Vec2{seed_rng.range_f(0.0f, 256.0f), seed_rng.range_f(0.0f, 144.0f)},
                static_cast<PathogenFamily>(i % kFamilyCount));
    }
    rebuild(hash, chaff);

    DamageSystem sys;
    sys.set_mode(ThinningMode::DensityThinning);
    sys.reserve(32);
    for (int i = 0; i < 16; ++i) {
        DamageField field;
        field.shape = (i % 3 == 0) ? FieldShape::Circle : (i % 3 == 1) ? FieldShape::Rect : FieldShape::Cone;
        field.origin = Vec2{seed_rng.range_f(0.0f, 256.0f), seed_rng.range_f(0.0f, 144.0f)};
        field.radius = 10.0f;
        field.rect = Rect{field.origin - Vec2{8.0f, 8.0f}, field.origin + Vec2{8.0f, 8.0f}};
        field.direction = Vec2{1.0f, 0.0f};
        field.arc_radians = 0.5f;
        field.kill_rate = 0.2f;
        sys.submit(field);
    }

    Rng rng(1);
    // Warm up the reused scratch buffer's capacity before timing.
    sys.apply(chaff, hash, rng, kFixedDt);

    const int kSamples = 20;
    f64 worst_ms = 0.0;
    for (int i = 0; i < kSamples; ++i) {
        const auto start = std::chrono::steady_clock::now();
        sys.apply(chaff, hash, rng, kFixedDt);
        const auto end = std::chrono::steady_clock::now();
        const f64 ms = std::chrono::duration<f64, std::milli>(end - start).count();
        if (ms > worst_ms) worst_ms = ms;
    }

    WARN("worst DamageSystem::apply() over " << kSamples << " samples @ 10k agents / 16 fields: "
                                              << worst_ms << "ms");
    REQUIRE(worst_ms < 2.0);   // field-count-bound, not agent-count-bound; generous headroom over the ~1ms target
}
