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

#include <algorithm>
#include <cmath>

using namespace immune;
using namespace immune::render;
using Catch::Approx;

TEST_CASE("lod_split conserves mass at and below the threshold", "[render][lod]") {
    for (u32 occ = 0; occ <= 24; ++occ) {
        const LodSplit s = lod_split(static_cast<f32>(occ), 24, 48);
        REQUIRE(s.instance_alpha == Approx(1.0f));
        REQUIRE(s.blob_weight == Approx(0.0f));
        REQUIRE(s.instance_alpha + s.blob_weight == Approx(1.0f));
    }
}

TEST_CASE("lod_split conserves mass at and above full", "[render][lod]") {
    for (u32 occ = 48; occ <= 96; ++occ) {
        const LodSplit s = lod_split(static_cast<f32>(occ), 24, 48);
        REQUIRE(s.instance_alpha == Approx(0.0f));
        REQUIRE(s.blob_weight == Approx(1.0f));
        REQUIRE(s.instance_alpha + s.blob_weight == Approx(1.0f));
    }
}

TEST_CASE("lod_split conserves mass across every step of the crossfade band", "[render][lod]") {
    for (u32 occ = 24; occ <= 48; ++occ) {
        const LodSplit s = lod_split(static_cast<f32>(occ), 24, 48);
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
        const LodSplit s = lod_split(static_cast<f32>(occ), 24, 48);
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
// OccupancyGrid: the crossfade's INPUT has to be continuous too.
// ---------------------------------------------------------------------------

TEST_CASE("at_smooth reads a cell's own value at its centre", "[render][lod]") {
    // The interpolation must be centred on cell CENTRES, not corners. Get this
    // wrong and the whole field shifts half a cell, which is a silent bug: it
    // still looks smooth, it is just in the wrong place.
    const u32 occ[9] = {0, 0, 0,
                        0, 40, 0,
                        0, 0, 0};
    OccupancyGrid g;
    g.bounds = Rect{Vec2{0.0f, 0.0f}, Vec2{30.0f, 30.0f}};
    g.cell_size = 10.0f;
    g.dims = IVec2{3, 3};
    g.occupancy = occ;

    REQUIRE(g.at_smooth(Vec2{15.0f, 15.0f}) == Approx(40.0f));
    REQUIRE(g.at_smooth(Vec2{5.0f, 5.0f}) == Approx(0.0f));
    // Halfway between the hot cell's centre and its neighbour's: half the value.
    REQUIRE(g.at_smooth(Vec2{10.0f, 15.0f}) == Approx(20.0f));
}

TEST_CASE("at_smooth never steps across a cell boundary", "[render][lod]") {
    // The dark-squares regression, stated as the property that was missing.
    // `at()` is a step function of position, so two agents a millimetre apart
    // either side of a cell edge got completely different LOD treatment and the
    // horde tiled into squares. Walking the smooth field across the same edge
    // must produce no jump larger than the ramp itself.
    const u32 occ[4] = {0, 60,
                        0, 60};
    OccupancyGrid g;
    g.bounds = Rect{Vec2{0.0f, 0.0f}, Vec2{20.0f, 20.0f}};
    g.cell_size = 10.0f;
    g.dims = IVec2{2, 2};
    g.occupancy = occ;

    f32 prev = g.at_smooth(Vec2{5.0f, 10.0f});
    f32 worst_step = 0.0f;
    for (int i = 1; i <= 100; ++i) {
        const f32 x = 5.0f + 10.0f * (static_cast<f32>(i) / 100.0f);   // centre to centre
        const f32 v = g.at_smooth(Vec2{x, 10.0f});
        worst_step = std::max(worst_step, std::abs(v - prev));
        REQUIRE(v >= prev - 1e-4f);   // monotonic: no ripple at the seam
        prev = v;
    }
    // 60 occupancy spread over 100 samples of one cell width. `at()` would step
    // the whole 60 at once, right at x = 10.
    REQUIRE(worst_step < 1.0f);
    REQUIRE(prev == Approx(60.0f));
}

TEST_CASE("at_smooth clamps at the grid border instead of fading to zero",
          "[render][lod]") {
    // Outside the grid the nearest edge cell is the honest answer. Fading to
    // zero would pop the agents at the rim back to full sprites.
    const u32 occ[4] = {30, 30,
                        30, 30};
    OccupancyGrid g;
    g.bounds = Rect{Vec2{0.0f, 0.0f}, Vec2{20.0f, 20.0f}};
    g.cell_size = 10.0f;
    g.dims = IVec2{2, 2};
    g.occupancy = occ;

    REQUIRE(g.at_smooth(Vec2{0.0f, 0.0f}) == Approx(30.0f));
    REQUIRE(g.at_smooth(Vec2{-50.0f, 30.0f}) == Approx(30.0f));
}

TEST_CASE("an unavailable occupancy grid reads as empty", "[render][lod]") {
    // The correct degradation is "every agent draws as an instance".
    OccupancyGrid g;
    REQUIRE(g.at_smooth(Vec2{1.0f, 1.0f}) == Approx(0.0f));
    REQUIRE(lod_split(g.at_smooth(Vec2{1.0f, 1.0f}), 24, 48).instance_alpha == Approx(1.0f));
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
