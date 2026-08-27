// Hand-built topologies exercising FlowField::bake end to end: a straight
// corridor, a bifurcation, a dead-end pocket, and a blocked lane forcing a
// reroute. Owner: Wave 1A.
//
// The bifurcation and reroute cases also dump .imff artifacts (see
// FlowFieldDebug.h) to the system temp directory so tools/preview_level.py
// can render them for visual sign-off. The exact path is always printed via
// WARN so it shows up in normal test output.
#include "sim/flowfield/FlowField.h"
#include "sim/flowfield/FlowFieldDebug.h"
#include "sim/flowfield/TissueRaster.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

using namespace immune;
using namespace immune::sim;

namespace {

std::filesystem::path debug_dump_dir() {
    const auto dir = std::filesystem::temp_directory_path() / "immune_flowfield_debug";
    std::filesystem::create_directories(dir);
    return dir;
}

void dump(const char* name, const TissueMask& mask, const FlowField& field) {
    const auto path = debug_dump_dir() / name;
    const bool ok = dump_flow_field(path.string(), mask, field);
    REQUIRE(ok);
    WARN("wrote debug dump: " << path.string());
}

} // namespace

// ---- 1. Straight corridor ------------------------------------------------

TEST_CASE("straight corridor: field points end to end and cost grows with distance",
          "[flowfield][topology][corridor]") {
    TissueMask m;
    m.resize(30, 3, 1.0f, Vec2{0.0f, 0.0f});
    for (i32 y = 0; y < 3; ++y)
        for (i32 x = 0; x < 30; ++x) m.set_walkable(x, y, true);

    FlowFieldBakeDesc desc;
    desc.goals = {sim::FlowGoal{IVec2{29, 1}}};
    FlowField field;
    field.bake(m, desc);

    REQUIRE(field.stats().full_bake);

    // Every cell in the corridor reaches the goal.
    for (i32 x = 0; x < 30; ++x) REQUIRE(field.reachable(m.cell_to_world(x, 1)));

    // Direction at the start points toward the goal (+x dominant).
    const Vec2 start_dir = field.sample(m.cell_to_world(1, 1));
    REQUIRE(start_dir.x > 0.9f);
    REQUIRE(std::abs(start_dir.y) < 0.3f);

    // At the goal itself cost is zero; monotonically decreasing as x increases.
    const f32 c0 = field.sample_cost(m.cell_to_world(0, 1));
    const f32 c15 = field.sample_cost(m.cell_to_world(15, 1));
    const f32 c29 = field.sample_cost(m.cell_to_world(29, 1));
    REQUIRE(c0 > c15);
    REQUIRE(c15 > c29);
    REQUIRE(c29 == 0.0f);

    // Outside the mask entirely: no guidance.
    const Vec2 outside = field.sample(Vec2{-100.0f, -100.0f});
    REQUIRE(outside.x == 0.0f);
    REQUIRE(outside.y == 0.0f);
}

// ---- 2. Bifurcation --------------------------------------------------------

namespace {
/// Builds a Y-shaped mask: a horizontal trunk that forks into an upper and a
/// lower vertical-then-horizontal branch, each terminating at its own goal.
/// All arms are 3 cells wide.
///           .............. (24,1) goal A
///           .
/// (0,10)----+
///           .
///           .............. (24,21) goal B
TissueMask make_bifurcation(i32 w = 25, i32 h = 23) {
    TissueMask m;
    m.resize(w, h, 1.0f, Vec2{0.0f, 0.0f});
    auto fill = [&](i32 x0, i32 x1, i32 y0, i32 y1) {
        for (i32 y = y0; y <= y1; ++y)
            for (i32 x = x0; x <= x1; ++x) m.set_walkable(x, y, true);
    };
    fill(0, 9, 9, 11);    // trunk
    fill(9, 11, 0, 11);   // branch A vertical
    fill(9, 24, 0, 2);    // branch A horizontal
    fill(9, 11, 9, 22);   // branch B vertical
    fill(9, 24, 20, 22);  // branch B horizontal
    return m;
}
} // namespace

TEST_CASE("bifurcation: trunk heads into the fork and each arm heads to its own goal",
          "[flowfield][topology][bifurcation]") {
    TissueMask m = make_bifurcation();

    FlowFieldBakeDesc desc;
    desc.goals = {sim::FlowGoal{IVec2{24, 1}}, sim::FlowGoal{IVec2{24, 21}}};
    FlowField field;
    field.bake(m, desc);

    // Trunk: heading into the fork, +x dominant.
    const Vec2 trunk_dir = field.sample(m.cell_to_world(2, 10));
    REQUIRE(trunk_dir.x > 0.8f);

    // Deep in branch A (top arm), direction should aim at goal A: +x, and
    // slightly -y if not yet on the goal row.
    const Vec2 a_dir = field.sample(m.cell_to_world(20, 1));
    REQUIRE(a_dir.x > 0.9f);

    // Deep in branch B (bottom arm): +x, on the goal row.
    const Vec2 b_dir = field.sample(m.cell_to_world(20, 21));
    REQUIRE(b_dir.x > 0.9f);

    // A point already up in branch A's vertical leg should head further up
    // (toward y=1) rather than back down into the trunk.
    const Vec2 a_vertical_dir = field.sample(m.cell_to_world(10, 6));
    REQUIRE(a_vertical_dir.y < -0.3f);

    const Vec2 b_vertical_dir = field.sample(m.cell_to_world(10, 16));
    REQUIRE(b_vertical_dir.y > 0.3f);

    // Everything in the Y is reachable; a cell never rasterized is not.
    REQUIRE(field.reachable(m.cell_to_world(2, 10)));
    REQUIRE_FALSE(field.reachable(m.cell_to_world(2, 20)));

    dump("bifurcation.imff", m, field);
}

// ---- 3. Dead-end pocket ----------------------------------------------------

TEST_CASE("dead-end pocket: unreachable, sample() is zero, cost is infinite",
          "[flowfield][topology][deadend]") {
    TissueMask m;
    m.resize(12, 12, 1.0f, Vec2{0.0f, 0.0f});
    for (i32 x = 0; x < 12; ++x) m.set_walkable(x, 0, true); // main lane along y=0

    // A walkable 3x3 pocket, fully surrounded by non-walkable cells: no shared
    // border with the main lane.
    for (i32 y = 5; y <= 7; ++y)
        for (i32 x = 4; x <= 6; ++x) m.set_walkable(x, y, true);

    FlowFieldBakeDesc desc;
    desc.goals = {sim::FlowGoal{IVec2{11, 0}}};
    FlowField field;
    field.bake(m, desc);

    REQUIRE(field.reachable(m.cell_to_world(0, 0)));
    REQUIRE_FALSE(field.reachable(m.cell_to_world(5, 6)));

    const Vec2 dir = field.sample(m.cell_to_world(5, 6));
    REQUIRE(dir.x == 0.0f);
    REQUIRE(dir.y == 0.0f);

    const f32 cost = field.sample_cost(m.cell_to_world(5, 6));
    REQUIRE_FALSE(std::isfinite(cost));
}

// ---- 4. Blocked lane forcing a reroute -------------------------------------

namespace {
/// A single main lane (y=4) with a parallel bypass lane (y=0) joined by two
/// vertical connectors at x=5 and x=14. Undamaged, the direct main lane is
/// always cheaper than detouring via the bypass.
TissueMask make_ladder(i32 w = 20, i32 h = 5) {
    TissueMask m;
    m.resize(w, h, 1.0f, Vec2{0.0f, 0.0f});
    for (i32 x = 0; x < w; ++x) {
        m.set_walkable(x, 0, true); // bypass
        m.set_walkable(x, 4, true); // main
    }
    for (i32 y = 0; y <= 4; ++y) {
        m.set_walkable(5, y, true);  // connector A
        m.set_walkable(14, y, true); // connector B
    }
    return m;
}
} // namespace

TEST_CASE("blocking the main lane forces a reroute through the bypass",
          "[flowfield][topology][reroute]") {
    TissueMask m = make_ladder();
    FlowFieldBakeDesc desc;
    desc.goals = {sim::FlowGoal{IVec2{19, 4}}};

    FlowField before;
    before.bake(m, desc);

    // Direct path: cell (7,4) heads straight along the main lane toward the goal.
    const Vec2 dir_before = before.sample(m.cell_to_world(7, 4));
    REQUIRE(dir_before.x > 0.9f);
    const f32 cost_before = before.sample_cost(m.cell_to_world(3, 4));

    dump("reroute_before.imff", m, before);

    // Block the main lane strictly between the two connectors.
    for (i32 x = 8; x <= 11; ++x) m.set_walkable(x, 4, false);

    FlowField after;
    after.bake(m, desc); // full rebake here; the incremental path is covered separately

    // Still reachable via the bypass.
    REQUIRE(after.reachable(m.cell_to_world(3, 4)));

    // (7,4)'s only walkable neighbour is now (6,4) — the field must point
    // backward, toward connector A, not forward into the wall.
    const Vec2 dir_after = after.sample_nearest(m.cell_to_world(7, 4));
    REQUIRE(dir_after.x < -0.9f);

    // Rerouting is strictly more expensive than the lost direct path.
    const f32 cost_after = after.sample_cost(m.cell_to_world(3, 4));
    REQUIRE(cost_after > cost_before);

    dump("reroute_after.imff", m, after);
}
