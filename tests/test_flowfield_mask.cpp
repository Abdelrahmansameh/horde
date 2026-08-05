// Tests for TissueMask, DistanceField (EDT), and spline rasterization
// (TissueRaster.h). Owner: Wave 1A.
#include "sim/flowfield/FlowField.h"
#include "sim/flowfield/TissueRaster.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace immune;
using namespace immune::sim;
using Catch::Approx;

namespace {
TissueMask make_box(i32 w, i32 h, f32 cs = 1.0f) {
    TissueMask m;
    m.resize(w, h, cs, Vec2{0.0f, 0.0f});
    for (i32 y = 0; y < h; ++y)
        for (i32 x = 0; x < w; ++x) m.set_walkable(x, y, true);
    return m;
}
} // namespace

// ---- TissueMask -------------------------------------------------------

TEST_CASE("TissueMask coordinate round-trip and defaults", "[flowfield][mask]") {
    TissueMask m;
    m.resize(10, 8, 2.0f, Vec2{5.0f, -3.0f});
    REQUIRE(m.width() == 10);
    REQUIRE(m.height() == 8);
    REQUIRE(m.cell_size() == 2.0f);

    // Fresh mask: nothing walkable, cost multiplier defaults to 1.
    REQUIRE_FALSE(m.walkable(0, 0));
    REQUIRE(m.cost(3, 3) == 1.0f);

    const Vec2 w = m.cell_to_world(2, 3);
    const IVec2 c = m.world_to_cell(w);
    REQUIRE(c.x == 2);
    REQUIRE(c.y == 3);

    // Out of range reads are safe and return sane defaults.
    REQUIRE_FALSE(m.walkable(-1, 0));
    REQUIRE_FALSE(m.walkable(100, 100));
    REQUIRE(m.cost(-1, 0) == 1.0f);
}

TEST_CASE("TissueMask set_walkable/set_cost only affect in-range cells", "[flowfield][mask]") {
    TissueMask m = make_box(4, 4);
    m.set_walkable(1, 1, false);
    REQUIRE_FALSE(m.walkable(1, 1));
    m.set_cost(2, 2, 5.0f);
    REQUIRE(m.cost(2, 2) == 5.0f);

    // Writes outside range are no-ops, not crashes.
    m.set_walkable(-5, -5, true);
    m.set_cost(999, 999, 3.0f);
}

// ---- DistanceField ------------------------------------------------------

TEST_CASE("DistanceField is positive inside, negative outside, zero at the wall face",
          "[flowfield][sdf]") {
    // A 10x10 room of tissue, single-cell wall border implied by resize defaults
    // (everything starts non-walkable, so we carve out the interior).
    TissueMask m;
    m.resize(12, 12, 1.0f, Vec2{0.0f, 0.0f});
    for (i32 y = 1; y < 11; ++y)
        for (i32 x = 1; x < 11; ++x) m.set_walkable(x, y, true);

    DistanceField sdf;
    sdf.bake(m);

    // Center of the room: far from any wall, clearly positive.
    const f32 center = sdf.sample(m.cell_to_world(5, 5));
    REQUIRE(center > 3.0f);

    // A cell touching the wall has clearance ~0.5 cell (its own half-width).
    const f32 edge = sdf.sample(m.cell_to_world(1, 5));
    REQUIRE(edge == Approx(0.5f).margin(0.05f));

    // A wall cell is negative.
    const f32 wall = sdf.sample(m.cell_to_world(0, 5));
    REQUIRE(wall < 0.0f);

    // Center is farther from a wall than an edge cell.
    REQUIRE(center > edge);
}

TEST_CASE("DistanceField gradient points away from the nearest wall", "[flowfield][sdf]") {
    TissueMask m;
    m.resize(12, 12, 1.0f, Vec2{0.0f, 0.0f});
    for (i32 y = 1; y < 11; ++y)
        for (i32 x = 1; x < 11; ++x) m.set_walkable(x, y, true);

    DistanceField sdf;
    sdf.bake(m);

    // Near the left wall (x=1), clearance increases moving in +x, so the
    // gradient (direction of increasing clearance) should point mostly +x.
    const Vec2 g = sdf.gradient(m.cell_to_world(2, 5));
    REQUIRE(g.x > 0.5f);
}

TEST_CASE("DistanceField bilinear sample is continuous between cell centers", "[flowfield][sdf]") {
    TissueMask m = make_box(20, 20);
    DistanceField sdf;
    sdf.bake(m);
    const f32 a = sdf.sample(m.cell_to_world(10, 10));
    const f32 b = sdf.sample(Vec2{10.5f, 10.5f});
    const f32 c = sdf.sample(m.cell_to_world(11, 11));
    // Interpolated midpoint should sit between its neighbours, not jump.
    REQUIRE(b >= math::min(a, c) - 0.5f);
    REQUIRE(b <= math::max(a, c) + 0.5f);
}

// ---- TissueRaster (spline rasterization) ---------------------------------

TEST_CASE("rasterize_vessel stamps a straight two-point segment with the given width",
          "[flowfield][raster]") {
    TissueMask m;
    m.resize(30, 22, 1.0f, Vec2{0.0f, 0.0f});

    // Line runs through the center of cell row 10 (world y=10.5). Width 8 ->
    // radius 4. Test rows sit a full cell inside/outside that radius so the
    // assertions don't depend on exactly which curve sample lands nearest a
    // given column (a boundary-exact check would be one float epsilon from
    // flaking depending on discrete sampling along the spline).
    VesselSpline s;
    s.points.push_back(VesselPoint{Vec2{2.0f, 10.5f}, 8.0f, 1.0f});
    s.points.push_back(VesselPoint{Vec2{27.0f, 10.5f}, 8.0f, 1.0f});
    rasterize_vessel(m, s);

    // Centerline is walkable along its length.
    for (i32 x = 3; x < 27; ++x) REQUIRE(m.walkable(x, 10));

    // Comfortably inside radius 4 (distance 3, rows 7 and 13).
    REQUIRE(m.walkable(15, 7));
    REQUIRE(m.walkable(15, 13));
    // Comfortably outside radius 4 (distance 6, rows 4 and 16).
    REQUIRE_FALSE(m.walkable(15, 4));
    REQUIRE_FALSE(m.walkable(15, 16));

    // Nothing stamped far from the segment (a corner well outside even the
    // endpoint disc's radius).
    REQUIRE_FALSE(m.walkable(0, 0));
}

TEST_CASE("rasterize_vessel widens smoothly along a tapering spline", "[flowfield][raster]") {
    TissueMask m;
    m.resize(40, 20, 1.0f, Vec2{0.0f, 0.0f});

    VesselSpline s;
    s.points.push_back(VesselPoint{Vec2{2.0f, 10.0f}, 2.0f, 1.0f});
    s.points.push_back(VesselPoint{Vec2{20.0f, 10.0f}, 2.0f, 1.0f});
    s.points.push_back(VesselPoint{Vec2{38.0f, 10.0f}, 10.0f, 1.0f});
    rasterize_vessel(m, s);

    // Near the narrow end, a cell 4 away from centerline should be outside the
    // lumen; near the wide end (width 10 -> radius 5) it should be inside.
    REQUIRE_FALSE(m.walkable(3, 14));
    REQUIRE(m.walkable(37, 14));
}

TEST_CASE("rasterize_vessels unions two splines sharing a control point (bifurcation seam)",
          "[flowfield][raster]") {
    TissueMask m;
    m.resize(40, 40, 1.0f, Vec2{0.0f, 0.0f});

    VesselSpline trunk;
    trunk.points.push_back(VesselPoint{Vec2{2.0f, 20.0f}, 4.0f, 1.0f});
    trunk.points.push_back(VesselPoint{Vec2{20.0f, 20.0f}, 4.0f, 1.0f});

    VesselSpline branch_a;
    branch_a.points.push_back(VesselPoint{Vec2{20.0f, 20.0f}, 4.0f, 1.0f});
    branch_a.points.push_back(VesselPoint{Vec2{35.0f, 5.0f}, 4.0f, 1.0f});

    VesselSpline branch_b;
    branch_b.points.push_back(VesselPoint{Vec2{20.0f, 20.0f}, 4.0f, 1.0f});
    branch_b.points.push_back(VesselPoint{Vec2{35.0f, 35.0f}, 4.0f, 1.0f});

    rasterize_vessels(m, {trunk, branch_a, branch_b});

    REQUIRE(m.walkable(10, 20));  // trunk
    REQUIRE(m.walkable(20, 20));  // shared seam
    REQUIRE(m.walkable(34, 6));   // near branch A end
    REQUIRE(m.walkable(34, 34));  // near branch B end
}

TEST_CASE("block_rect carves a hole and returns the affected world rect", "[flowfield][raster]") {
    TissueMask m = make_box(20, 20);
    const Rect r{Vec2{5.0f, 5.0f}, Vec2{8.0f, 8.0f}};
    const Rect out = block_rect(m, r);
    REQUIRE(out.min.x == r.min.x);
    REQUIRE(out.max.x == r.max.x);

    for (i32 y = 5; y <= 8; ++y)
        for (i32 x = 5; x <= 8; ++x) REQUIRE_FALSE(m.walkable(x, y));

    // Outside the rect is untouched.
    REQUIRE(m.walkable(0, 0));
    REQUIRE(m.walkable(19, 19));
}
