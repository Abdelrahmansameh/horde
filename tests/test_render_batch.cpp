// tests/test_render_batch.cpp — CPU-side instance-batch construction. Owner:
// Wave 1C. No GL context: build_chaff_batches is pure data-in/data-out, which
// is exactly what makes the crossfade and batching logic testable without
// standing up a headless window (see also test_render_lod.cpp).
#include "render/ChaffBatcher.h"
#include "sim/chaff/ChaffBuffers.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace immune;
using namespace immune::render;
using namespace immune::sim;
using Catch::Approx;

namespace {

ChaffBuffers make_chaff(usize capacity) {
    ChaffBuffers c;
    c.reserve(capacity);
    return c;
}

/// A grid with no cells above the crossfade threshold: everything renders as
/// a plain instance. Good enough for tests that aren't about the LOD band.
OccupancyGrid flat_grid(const Rect& bounds) {
    OccupancyGrid g;
    g.bounds = bounds;
    g.cell_size = 1000.0f; // one giant cell covering the whole world
    g.dims = IVec2{1, 1};
    static const u32 kZero = 0;
    g.occupancy = &kZero;
    return g;
}

} // namespace

TEST_CASE("build_chaff_batches issues one contiguous range per family", "[render][batch]") {
    ChaffBuffers chaff = make_chaff(64);
    const Rect bounds{Vec2{0.0f, 0.0f}, Vec2{100.0f, 100.0f}};

    // Interleave families on purpose: the batcher must sort into per-family
    // ranges regardless of spawn order (this is what makes "one draw call per
    // family" possible at all).
    for (u32 i = 0; i < 12; ++i) {
        ChaffSpawnParams p;
        p.family = static_cast<PathogenFamily>(i % kFamilyCount);
        p.position = Vec2{10.0f + static_cast<f32>(i), 10.0f};
        p.density = 1.0f;
        chaff.spawn(p);
    }

    OccupancyGrid occ = flat_grid(bounds);
    ChaffBatchParams params;
    params.per_family_capacity = 16;
    std::vector<ChaffInstance> dest(kFamilyCount * params.per_family_capacity);

    const ChaffBatchResult result = build_chaff_batches(chaff, occ, params, dest.data(), nullptr);

    // 12 agents / 6 families, round-robin -> exactly 2 per family.
    for (u32 f = 0; f < kFamilyCount; ++f) {
        REQUIRE(result.family_counts[f] == 2u);
    }
    REQUIRE(result.instances_total == 12u);
    REQUIRE(result.instances_dropped == 0u);

    // Every instance in family f's range actually carries family f's colour.
    for (u32 f = 0; f < kFamilyCount; ++f) {
        const Vec4 expected = family_color(static_cast<PathogenFamily>(f));
        const u32 expected_rgb = pack_rgba8(Vec4{expected.r, expected.g, expected.b, 1.0f}) & 0x00FFFFFFu;
        for (u32 i = 0; i < result.family_counts[f]; ++i) {
            const ChaffInstance& inst = dest[f * params.per_family_capacity + i];
            REQUIRE((inst.tint_rgba8 & 0x00FFFFFFu) == expected_rgb);
        }
    }
}

TEST_CASE("build_chaff_batches never writes past per_family_capacity", "[render][batch]") {
    ChaffBuffers chaff = make_chaff(32);
    const Rect bounds{Vec2{0.0f, 0.0f}, Vec2{100.0f, 100.0f}};
    for (u32 i = 0; i < 10; ++i) {
        ChaffSpawnParams p;
        p.family = PathogenFamily::Virus; // all one family, all overflow the same range
        p.position = Vec2{5.0f, 5.0f};
        p.density = 1.0f;
        chaff.spawn(p);
    }

    OccupancyGrid occ = flat_grid(bounds);
    ChaffBatchParams params;
    params.per_family_capacity = 4; // deliberately smaller than the agent count

    // Sentinel-fill the destination so an out-of-range write is detectable.
    std::vector<ChaffInstance> dest(kFamilyCount * params.per_family_capacity);
    for (auto& inst : dest) inst.pad = -1.0f;

    const ChaffBatchResult result = build_chaff_batches(chaff, occ, params, dest.data(), nullptr);

    REQUIRE(result.family_counts[static_cast<u32>(PathogenFamily::Virus)] == 4u);
    REQUIRE(result.instances_dropped == 6u);
    REQUIRE(result.instances_total == 4u);

    // No family after Virus in the fixed-stride layout should have been
    // touched by the overflow.
    for (u32 f = 1; f < kFamilyCount; ++f) {
        for (u32 i = 0; i < params.per_family_capacity; ++i) {
            REQUIRE(dest[f * params.per_family_capacity + i].pad == Approx(-1.0f));
        }
    }
}

TEST_CASE("build_chaff_batches conserves total density with no LOD active", "[render][batch]") {
    ChaffBuffers chaff = make_chaff(200);
    const Rect bounds{Vec2{0.0f, 0.0f}, Vec2{200.0f, 200.0f}};
    f32 total = 0.0f;
    for (u32 i = 0; i < 150; ++i) {
        ChaffSpawnParams p;
        p.family = static_cast<PathogenFamily>(i % kFamilyCount);
        p.position = Vec2{static_cast<f32>(i % 40) * 4.0f, static_cast<f32>(i / 40) * 4.0f};
        p.density = 0.4f + 0.01f * static_cast<f32>(i);
        chaff.spawn(p);
        total += p.density;
    }

    OccupancyGrid occ = flat_grid(bounds);
    ChaffBatchParams params;
    params.per_family_capacity = 64;
    std::vector<ChaffInstance> dest(kFamilyCount * params.per_family_capacity);

    const ChaffBatchResult result = build_chaff_batches(chaff, occ, params, dest.data(), nullptr);

    REQUIRE(result.blob_mass == Approx(0.0f));
    REQUIRE(result.instance_mass == Approx(total).margin(1e-3f));
    REQUIRE(result.total_mass() == Approx(total).margin(1e-3f));
}

TEST_CASE("build_chaff_batches culls agents outside the cull rect", "[render][batch]") {
    ChaffBuffers chaff = make_chaff(16);
    const Rect bounds{Vec2{0.0f, 0.0f}, Vec2{200.0f, 200.0f}};
    for (u32 i = 0; i < 4; ++i) {
        ChaffSpawnParams p;
        p.family = PathogenFamily::Bacteria;
        p.position = Vec2{static_cast<f32>(i) * 100.0f, 0.0f}; // 0, 100, 200, 300
        p.density = 1.0f;
        chaff.spawn(p);
    }

    OccupancyGrid occ = flat_grid(bounds);
    ChaffBatchParams params;
    params.per_family_capacity = 16;
    params.cull_enabled = true;
    params.cull = Rect{Vec2{-1.0f, -1.0f}, Vec2{50.0f, 50.0f}}; // only the first agent survives
    std::vector<ChaffInstance> dest(kFamilyCount * params.per_family_capacity);

    const ChaffBatchResult result = build_chaff_batches(chaff, occ, params, dest.data(), nullptr);
    REQUIRE(result.instances_total == 1u);
    REQUIRE(result.agents_culled == 3u);
}

TEST_CASE("family_visual assigns distinct silhouette/tempo per family", "[render][batch][readability]") {
    // DESIGN.md §6: colour = family, silhouette = threat tier, tempo = speed
    // tier. Enforce structurally: no two families should accidentally share
    // both the batching colour AND (silhouette, tempo) — that would erase the
    // second and third readability channels for that pair.
    for (u32 a = 0; a < kFamilyCount; ++a) {
        for (u32 b = a + 1; b < kFamilyCount; ++b) {
            const FamilyVisual va = family_visual(static_cast<PathogenFamily>(a));
            const FamilyVisual vb = family_visual(static_cast<PathogenFamily>(b));
            const bool same_silhouette = va.silhouette == Approx(vb.silhouette).margin(1e-4f);
            const bool same_tempo = va.tempo == Approx(vb.tempo).margin(1e-4f);
            REQUIRE_FALSE((same_silhouette && same_tempo));
        }
    }
}
