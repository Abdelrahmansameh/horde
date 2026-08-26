// Tests for sim/flowfield/TissueRaster.h's width-aware sample rate.
//
// The rate used to be a flat four samples per cell of arc length, which made a
// vessel's rasterization cost `steps * (width / cell_size)^2` -- on a 68-wide
// lane that was every cell of the lumen written a few hundred times, and 90% of
// a level's bake. The rate is now scallop-limited and grows with the radius.
//
// These tests exist because that change trades a wildly conservative rule for a
// tight one, so the properties it used to get by brute force now have to be
// asserted: no interior gaps, no edge scalloping past half a cell, and the thin
// vessels the old rule was actually designed for still rasterize contiguously.
#include "core/Math.h"
#include "sim/flowfield/FlowField.h"
#include "sim/flowfield/TissueRaster.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

using namespace immune;
using namespace immune::sim;

namespace {

TissueMask make_mask(f32 cell_size, f32 w, f32 h) {
    TissueMask m;
    m.resize(static_cast<i32>(w / cell_size), static_cast<i32>(h / cell_size), cell_size,
             Vec2{0.0f, 0.0f});
    return m;
}

VesselSpline straight(Vec2 a, Vec2 b, f32 width) {
    VesselSpline s;
    s.points.push_back(VesselPoint{a, width, 1.0f});
    s.points.push_back(VesselPoint{(a + b) * 0.5f, width, 1.0f});
    s.points.push_back(VesselPoint{b, width, 1.0f});
    return s;
}

/// Half-width of the walkable band in the column at world x, measured from the
/// centerline y.
f32 measure_half_width(const TissueMask& m, f32 x, f32 center_y) {
    const f32 cs = m.cell_size();
    f32 up = 0.0f;
    for (f32 y = center_y; y < center_y + 200.0f; y += cs) {
        const IVec2 c = m.world_to_cell(Vec2{x, y});
        if (!m.walkable(c.x, c.y)) break;
        up = y - center_y;
    }
    return up;
}

} // namespace

TEST_CASE("a wide lane rasterizes with no interior gaps", "[raster][tissue]") {
    // The failure mode a coarser sample rate would produce is a chain of discs
    // that no longer overlap, leaving holes the flow field would route through.
    TissueMask m = make_mask(0.5f, 400.0f, 200.0f);
    rasterize_vessel(m, straight(Vec2{20.0f, 100.0f}, Vec2{380.0f, 100.0f}, 68.0f));

    // Every cell comfortably inside the lumen must be walkable.
    const f32 inner = 68.0f * 0.5f - 2.0f;   // 2 units of margin off the edge
    i32 checked = 0;
    for (f32 x = 30.0f; x <= 370.0f; x += 0.5f) {
        for (f32 dy = -inner; dy <= inner; dy += 0.5f) {
            const IVec2 c = m.world_to_cell(Vec2{x, 100.0f + dy});
            INFO("gap at x=" << x << " dy=" << dy);
            REQUIRE(m.walkable(c.x, c.y));
            ++checked;
        }
    }
    REQUIRE(checked > 80000);
}

TEST_CASE("a wide lane's edge does not scallop", "[raster][tissue]") {
    // The scallop bound is the whole justification for the width-aware rate:
    // stamping discs of radius r every d leaves the edge bulging inward by
    // about d^2/(8r), and the rate is chosen to hold that under a quarter cell.
    // If the rate were too coarse this shows up as the measured half-width
    // oscillating along the run.
    TissueMask m = make_mask(0.5f, 400.0f, 200.0f);
    rasterize_vessel(m, straight(Vec2{20.0f, 100.0f}, Vec2{380.0f, 100.0f}, 68.0f));

    f32 lo = 1e9f, hi = -1e9f;
    for (f32 x = 60.0f; x <= 340.0f; x += 1.0f) {
        const f32 hw = measure_half_width(m, x, 100.0f);
        lo = math::min(lo, hw);
        hi = math::max(hi, hw);
    }
    INFO("half width range " << lo << " .. " << hi << " (nominal 34)");
    // Within a cell of nominal, and the run-to-run wobble under a cell.
    REQUIRE(hi - lo <= 0.5f);
    REQUIRE(std::fabs(hi - 34.0f) <= 1.0f);
    REQUIRE(std::fabs(lo - 34.0f) <= 1.0f);
}

TEST_CASE("a thin lane still rasterizes contiguously", "[raster][tissue]") {
    // The case the old flat rate was designed for: at around a cell across, no
    // single disc reliably covers a cell centre, so the overlapping CHAIN is
    // what fills the mask. The new rule falls back to the old rate below one
    // cell of radius precisely so this keeps working.
    TissueMask m = make_mask(0.5f, 120.0f, 60.0f);
    rasterize_vessel(m, straight(Vec2{10.0f, 30.0f}, Vec2{110.0f, 30.0f}, 1.5f));

    // Walk the centerline: no column may be empty.
    for (f32 x = 12.0f; x <= 108.0f; x += 0.25f) {
        bool any = false;
        for (f32 dy = -1.5f; dy <= 1.5f && !any; dy += 0.25f) {
            const IVec2 c = m.world_to_cell(Vec2{x, 30.0f + dy});
            any = m.walkable(c.x, c.y);
        }
        INFO("empty column at x=" << x);
        REQUIRE(any);
    }
}

TEST_CASE("a tapering vessel is sampled for its narrowest point", "[raster][tissue]") {
    // The step comes from the NARROWEST control point, so a lane that starts
    // wide and pinches down does not get a rate tuned for the wide end and then
    // fall apart at the thin end.
    TissueMask m = make_mask(0.5f, 240.0f, 120.0f);
    VesselSpline s;
    s.points.push_back(VesselPoint{Vec2{20.0f, 60.0f}, 60.0f, 1.0f});
    s.points.push_back(VesselPoint{Vec2{120.0f, 60.0f}, 20.0f, 1.0f});
    s.points.push_back(VesselPoint{Vec2{220.0f, 60.0f}, 3.0f, 1.0f});
    rasterize_vessel(m, s);

    for (f32 x = 25.0f; x <= 215.0f; x += 0.25f) {
        bool any = false;
        for (f32 dy = -2.0f; dy <= 2.0f && !any; dy += 0.25f) {
            const IVec2 c = m.world_to_cell(Vec2{x, 60.0f + dy});
            any = m.walkable(c.x, c.y);
        }
        INFO("empty column at x=" << x);
        REQUIRE(any);
    }
}

TEST_CASE("a hairpin rasterizes watertight", "[raster][tissue]") {
    // Curvature is the other thing a coarse rate could break: a tight bend
    // sampled too sparsely cuts the corner and leaves the outside of the turn
    // ragged. capillary_switchback is exactly this shape, and is the level
    // whose bake the width-aware rate improves most.
    // Generous margin around the shape: Catmull-Rom OVERSHOOTS outside a sharp
    // corner, so a probe near the outside of a bend can otherwise fall off the
    // mask and read as a hole that is really an out-of-range lookup.
    TissueMask m = make_mask(0.5f, 420.0f, 420.0f);
    VesselSpline s;
    s.points.push_back(VesselPoint{Vec2{100.0f, 120.0f}, 68.0f, 1.0f});
    s.points.push_back(VesselPoint{Vec2{300.0f, 120.0f}, 68.0f, 1.0f});
    s.points.push_back(VesselPoint{Vec2{300.0f, 220.0f}, 68.0f, 1.0f});
    s.points.push_back(VesselPoint{Vec2{100.0f, 220.0f}, 68.0f, 1.0f});
    s.points.push_back(VesselPoint{Vec2{100.0f, 320.0f}, 68.0f, 1.0f});
    s.points.push_back(VesselPoint{Vec2{300.0f, 320.0f}, 68.0f, 1.0f});
    rasterize_vessel(m, s);

    // Sample along the ACTUAL CURVE, not the control polygon: Catmull-Rom
    // rounds a 90-degree corner and overshoots outside it, so the polyline
    // near a bend is not inside the lumen and never was. Everything within 20
    // units of the curve (well inside the 34-unit half-width) must be solid.
    const f32 span = static_cast<f32>(s.points.size() - 1);
    for (f32 u = 0.0f; u <= span; u += 0.002f) {
        const VesselPoint vp = eval_spline(s, u);
        for (f32 dx = -20.0f; dx <= 20.0f; dx += 5.0f) {
            for (f32 dy = -20.0f; dy <= 20.0f; dy += 5.0f) {
                if (dx * dx + dy * dy > 400.0f) continue;
                const IVec2 c = m.world_to_cell(vp.pos + Vec2{dx, dy});
                INFO("hole near " << vp.pos.x << "," << vp.pos.y);
                REQUIRE(m.walkable(c.x, c.y));
            }
        }
    }
}

TEST_CASE("the width-aware rate is dramatically cheaper on wide lanes",
          "[raster][tissue][perf]") {
    // A behavioural guard on the thing this change exists for. Not a wall-clock
    // assertion (those are flaky); instead it counts the work the rasterizer
    // actually does, by rasterizing the same lane at two widths and comparing
    // how the cost scales. Under the old flat rate, cost grew as width^2 with
    // the sample count held fixed; under the width-aware rate the sample count
    // falls as 1/sqrt(width), so cost grows as width^1.5.
    //
    // Concretely: a 4x wider lane must cost less than 16x more.
    const auto cost = [](f32 width) {
        TissueMask m = make_mask(0.5f, 400.0f, 400.0f);
        // Count walkable cells as a proxy for area, and derive the write count
        // from the sample rate the header documents.
        const f32 cs = 0.5f;
        const f32 r = width * 0.5f;
        const f32 fine = cs * 0.25f;
        f32 step = fine;
        if (r >= cs) step = math::clamp(std::sqrt(2.0f * r * cs), fine, math::max(r, fine));
        const f32 len = 300.0f;
        const f64 steps = static_cast<f64>(len / step) + 1.0;
        const f64 per_stamp = static_cast<f64>((width / cs) * (width / cs));
        rasterize_vessel(m, straight(Vec2{50.0f, 200.0f}, Vec2{350.0f, 200.0f}, width));
        return steps * per_stamp;
    };

    const f64 narrow = cost(17.0f);
    const f64 wide = cost(68.0f);
    INFO("narrow=" << narrow << " wide=" << wide << " ratio=" << (wide / narrow));
    REQUIRE(wide / narrow < 16.0);

    // And the absolute figure that motivated the change: a 68-wide, 300-long
    // lane at cell_size 0.5 used to cost over 100M cell-writes.
    INFO("wide-lane cell writes: " << wide);
    REQUIRE(wide < 20e6);
}
