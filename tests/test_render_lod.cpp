// tests/test_render_lod.cpp — density-LOD crossfade math. Owner: Wave 1C.
//
// The crossfade band is the subtlest part of the renderer's job
// (docs/ARCHITECTURE.md §5.2): as occupancy climbs from lod_blob_threshold to
// lod_blob_full, an agent's contribution must move smoothly from "instance
// alpha" to "blob density" while the sum stays exactly 1. These tests assert
// that identity numerically across the whole band, plus the two saturated
// ends, with no GL context involved at all.
#include "render/ChaffBatcher.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace immune;
using namespace immune::render;
using Catch::Approx;

TEST_CASE("lod_split conserves mass at and below the threshold", "[render][lod]") {
    for (u32 occ = 0; occ <= 24; ++occ) {
        const LodSplit s = lod_split(occ, 24, 48);
        REQUIRE(s.instance_alpha == Approx(1.0f));
        REQUIRE(s.blob_weight == Approx(0.0f));
        REQUIRE(s.instance_alpha + s.blob_weight == Approx(1.0f));
    }
}

TEST_CASE("lod_split conserves mass at and above full", "[render][lod]") {
    for (u32 occ = 48; occ <= 96; ++occ) {
        const LodSplit s = lod_split(occ, 24, 48);
        REQUIRE(s.instance_alpha == Approx(0.0f));
        REQUIRE(s.blob_weight == Approx(1.0f));
        REQUIRE(s.instance_alpha + s.blob_weight == Approx(1.0f));
    }
}

TEST_CASE("lod_split conserves mass across every step of the crossfade band", "[render][lod]") {
    for (u32 occ = 24; occ <= 48; ++occ) {
        const LodSplit s = lod_split(occ, 24, 48);
        REQUIRE(s.instance_alpha >= 0.0f);
        REQUIRE(s.instance_alpha <= 1.0f);
        REQUIRE(s.blob_weight >= 0.0f);
        REQUIRE(s.blob_weight <= 1.0f);
        REQUIRE((s.instance_alpha + s.blob_weight) == Approx(1.0f).margin(1e-5f));
    }
}

TEST_CASE("lod_split is monotonic through the band", "[render][lod]") {
    f32 prev_alpha = 1.0f;
    for (u32 occ = 24; occ <= 48; ++occ) {
        const LodSplit s = lod_split(occ, 24, 48);
        REQUIRE(s.instance_alpha <= prev_alpha + 1e-6f);
        prev_alpha = s.instance_alpha;
    }
}

TEST_CASE("lod_split degrades to pure-instance when full <= threshold", "[render][lod]") {
    // Misconfigured thresholds (full <= threshold) must not divide by zero or
    // produce NaN; treat it as an immediate hard cut to blob at the threshold.
    const LodSplit below = lod_split(10, 24, 24);
    REQUIRE(below.instance_alpha == Approx(1.0f));
    // At and below `threshold` is always pure-instance (see the "at and below
    // the threshold" case above); the degenerate full<=threshold collapse only
    // matters strictly above it.
    const LodSplit above = lod_split(25, 24, 24);
    REQUIRE(above.blob_weight == Approx(1.0f));
}

// ---------------------------------------------------------------------------
// DensityGrid: bilinear splat conserves the deposited mass exactly.
// ---------------------------------------------------------------------------

TEST_CASE("DensityGrid::splat conserves total mass via bilinear weights", "[render][lod][density]") {
    DensityGrid grid;
    grid.configure(16, 16);
    grid.set_extent(Rect{Vec2{0.0f, 0.0f}, Vec2{160.0f, 160.0f}});
    grid.clear();

    grid.splat(Vec2{55.0f, 77.0f}, Vec4{1.0f, 0.0f, 0.0f, 1.0f}, 3.5f);
    grid.splat(Vec2{12.3f, 140.0f}, Vec4{0.0f, 1.0f, 0.0f, 1.0f}, 1.25f);

    REQUIRE(grid.total_mass() == Approx(4.75f).margin(1e-4f));
    REQUIRE(grid.deposited_mass() == Approx(4.75f).margin(1e-4f));
}

TEST_CASE("DensityGrid::clear resets accumulated mass", "[render][lod][density]") {
    DensityGrid grid;
    grid.configure(8, 8);
    grid.set_extent(Rect{Vec2{0.0f, 0.0f}, Vec2{80.0f, 80.0f}});
    grid.splat(Vec2{40.0f, 40.0f}, Vec4{1.0f}, 2.0f);
    REQUIRE(grid.total_mass() > 0.0f);
    grid.clear();
    REQUIRE(grid.total_mass() == Approx(0.0f));
    REQUIRE(grid.deposited_mass() == Approx(0.0f));
}
