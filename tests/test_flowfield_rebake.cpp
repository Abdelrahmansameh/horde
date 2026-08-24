// The load-bearing test (docs/AGENT_BRIEF.md / FlowField.h rationale):
// FlowField::rebake_pending on a dirty region must produce EXACTLY the field a
// full FlowField::bake would produce on the same (edited) mask — just far
// cheaper. This file builds a scene with obstacles, edits it three different
// ways (block, unblock, cost-only), and diffs the incremental result against
// an independent from-scratch bake, cell by cell, over the whole grid — not
// just inside the dirty rect, since a bug in the boundary-support logic would
// most likely show up as a stale cost *outside* the edited region.
//
// Also exercises pump_rebake's frame-budget draining and reports the actual
// timings/cells-visited so the "cheaper than a full bake" claim is verified,
// not assumed.
#include "sim/flowfield/FlowField.h"
#include "sim/flowfield/TissueRaster.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdio>

using namespace immune;
using namespace immune::sim;

namespace {

constexpr i32 kW = 50;
constexpr i32 kH = 35;

/// Open field with a regular grid of 3x3 pillars, spaced so a 3-cell block
/// carved anywhere between pillars can't accidentally wall off a pocket.
TissueMask make_scene() {
    TissueMask m;
    m.resize(kW, kH, 1.0f, Vec2{0.0f, 0.0f});
    for (i32 y = 0; y < kH; ++y)
        for (i32 x = 0; x < kW; ++x) m.set_walkable(x, y, true);
    for (i32 px = 6; px <= 38; px += 8) {
        for (i32 py = 6; py <= 22; py += 8) {
            for (i32 y = py; y < py + 3; ++y)
                for (i32 x = px; x < px + 3; ++x) m.set_walkable(x, y, false);
        }
    }
    return m;
}

FlowFieldBakeDesc make_desc(i32 margin) {
    FlowFieldBakeDesc desc;
    desc.goal_cells = {IVec2{kW - 1, kH / 2}};
    desc.rebake_margin_cells = margin;
    return desc;
}

struct DiffResult {
    u32 cost_mismatches = 0;
    u32 reachability_mismatches = 0;
    u32 compared_reachable = 0;
    f32 max_cost_diff = 0.0f;
    f32 dot_sum = 0.0f;
    u32 dot_count = 0;
};

/// Compares two fields baked over the same WxH grid cell by cell. `cost_tol`
/// is an absolute tolerance covering floating-point summation-order noise
/// between the two independently-ordered sweeps, not a correctness fudge.
DiffResult diff_fields(const FlowField& a, const FlowField& b, i32 w, i32 h, f32 cost_tol) {
    DiffResult r;
    const f32* ca = a.costs();
    const f32* cb = b.costs();
    const Vec2* da = a.directions();
    const Vec2* db = b.directions();
    for (i32 i = 0; i < w * h; ++i) {
        const bool fa = std::isfinite(ca[i]);
        const bool fb = std::isfinite(cb[i]);
        if (fa != fb) {
            ++r.reachability_mismatches;
            continue;
        }
        if (!fa) continue; // both unreachable: agree, nothing more to compare
        ++r.compared_reachable;
        const f32 d = std::abs(ca[i] - cb[i]);
        if (d > cost_tol) ++r.cost_mismatches;
        r.max_cost_diff = math::max(r.max_cost_diff, d);

        const Vec2 va = da[i];
        const Vec2 vb = db[i];
        const bool za = (va.x == 0.0f && va.y == 0.0f);
        const bool zb = (vb.x == 0.0f && vb.y == 0.0f);
        if (!za && !zb) {
            r.dot_sum += va.x * vb.x + va.y * vb.y;
            ++r.dot_count;
        }
    }
    return r;
}

} // namespace

TEST_CASE("incremental rebake equals a full rebake after blocking a region",
          "[flowfield][rebake][equivalence]") {
    TissueMask mask = make_scene();
    const FlowFieldBakeDesc desc = make_desc(/*margin=*/4);

    FlowField incremental;
    incremental.bake(mask, desc);
    const f64 full_ms_before = incremental.stats().last_bake_ms;
    const u32 full_cells_before = incremental.stats().cells_visited;

    // Edit: place a "tower" in open space between pillars.
    const Rect edit = block_rect(mask, Rect{Vec2{17.0f, 17.0f}, Vec2{19.0f, 19.0f}});
    incremental.mark_dirty(edit);
    REQUIRE(incremental.has_pending_rebake());
    incremental.rebake_pending(mask);
    REQUIRE_FALSE(incremental.has_pending_rebake());
    REQUIRE_FALSE(incremental.stats().full_bake);

    FlowField full;
    full.bake(mask, desc); // independent from-scratch bake of the SAME edited mask

    const DiffResult d = diff_fields(incremental, full, kW, kH, /*cost_tol=*/0.05f);
    WARN("block edit: full_bake=" << full.stats().last_bake_ms << "ms/"
         << full.stats().cells_visited << " cells, incremental_rebake="
         << incremental.stats().last_bake_ms << "ms/" << incremental.stats().cells_visited
         << " cells, max_cost_diff=" << d.max_cost_diff
         << ", avg_dir_dot=" << (d.dot_count ? d.dot_sum / static_cast<f32>(d.dot_count) : 1.0f));

    REQUIRE(d.reachability_mismatches == 0);
    REQUIRE(d.cost_mismatches == 0);
    REQUIRE(d.compared_reachable > 0);
    REQUIRE((d.dot_sum / static_cast<f32>(d.dot_count)) > 0.98f);

    // The whole point: incremental work is a small fraction of a full bake on
    // a grid this size.
    REQUIRE(incremental.stats().cells_visited < full_cells_before / 2);
    (void)full_ms_before;
}

TEST_CASE("incremental rebake equals a full rebake after unblocking a region",
          "[flowfield][rebake][equivalence]") {
    TissueMask mask = make_scene();
    const Rect edit_world{Vec2{17.0f, 17.0f}, Vec2{19.0f, 19.0f}};
    block_rect(mask, edit_world); // baked-in obstacle from the start

    const FlowFieldBakeDesc desc = make_desc(/*margin=*/4);
    FlowField incremental;
    incremental.bake(mask, desc);

    // Remove the obstacle (tower sold) — costs nearby should DROP.
    for (i32 y = 17; y <= 20; ++y)
        for (i32 x = 17; x <= 20; ++x) mask.set_walkable(x, y, true);
    incremental.mark_dirty(edit_world);
    incremental.rebake_pending(mask);

    FlowField full;
    full.bake(mask, desc);

    const DiffResult d = diff_fields(incremental, full, kW, kH, 0.05f);
    WARN("unblock edit: max_cost_diff=" << d.max_cost_diff << ", avg_dir_dot="
         << (d.dot_count ? d.dot_sum / static_cast<f32>(d.dot_count) : 1.0f)
         << ", incremental cells_visited=" << incremental.stats().cells_visited
         << " vs full=" << full.stats().cells_visited);

    REQUIRE(d.reachability_mismatches == 0);
    REQUIRE(d.cost_mismatches == 0);
    REQUIRE((d.dot_sum / static_cast<f32>(d.dot_count)) > 0.98f);
}

TEST_CASE("incremental rebake equals a full rebake after a cost-only edit (no walkability change)",
          "[flowfield][rebake][equivalence]") {
    TissueMask mask = make_scene();
    const FlowFieldBakeDesc desc = make_desc(/*margin=*/4);

    FlowField incremental;
    incremental.bake(mask, desc);

    // Sludge: raise the cost multiplier of an open patch without blocking it.
    for (i32 y = 17; y <= 19; ++y)
        for (i32 x = 17; x <= 19; ++x) mask.set_cost(x, y, 6.0f);
    const Rect edit{Vec2{17.0f, 17.0f}, Vec2{20.0f, 20.0f}};
    incremental.mark_dirty(edit);
    incremental.rebake_pending(mask);

    FlowField full;
    full.bake(mask, desc);

    const DiffResult d = diff_fields(incremental, full, kW, kH, 0.05f);
    WARN("cost-only edit: max_cost_diff=" << d.max_cost_diff);
    REQUIRE(d.reachability_mismatches == 0);
    REQUIRE(d.cost_mismatches == 0);
}

TEST_CASE("incremental rebake equals a full rebake with direction smoothing on",
          "[flowfield][rebake][equivalence][smoothing]") {
    // Smoothing widens a cell's dependency cone from its four neighbours to
    // everything within `smoothing_radius`, so a rebake that only refreshed the
    // usual one-cell halo would leave a ring of cells holding directions
    // smoothed against costs that no longer exist -- a seam around every tower,
    // exactly the kind of artifact the smoothing was added to remove. This
    // pins the padding that prevents it.
    TissueMask mask = make_scene();
    FlowFieldBakeDesc desc = make_desc(/*margin=*/4);
    desc.smoothing_radius = 5.0f; // cell_size is 1.0, so five iterations
    desc.goal_radius = 3.0f;

    FlowField incremental;
    incremental.bake(mask, desc);

    const Rect edit = block_rect(mask, Rect{Vec2{17.0f, 17.0f}, Vec2{19.0f, 19.0f}});
    incremental.mark_dirty(edit);
    incremental.rebake_pending(mask);

    FlowField full;
    full.bake(mask, desc);

    const DiffResult d = diff_fields(incremental, full, kW, kH, /*cost_tol=*/0.05f);
    REQUIRE(d.reachability_mismatches == 0);
    REQUIRE(d.cost_mismatches == 0);
    REQUIRE(d.compared_reachable > 0);
    // Directions, not just costs: the smoothing halo is the thing under test,
    // and an under-padded rebake shows up here and nowhere else.
    REQUIRE((d.dot_sum / static_cast<f32>(d.dot_count)) > 0.9999f);

    // And the smoothed field still leads everywhere it is defined. This is the
    // property smoothing could plausibly break and that nothing else here would
    // catch: averaging directions can, in principle, manufacture a swirl or a
    // standing point that a shortest-path field can never contain, and an agent
    // that wanders into one stops arriving.
    //
    // The walk is cell-wise rather than a free-flying point, because a point
    // integrating a direction field with no collision response will eventually
    // clip a corner into a pillar and stall there -- which says something about
    // the probe, not about the field. Stepping to the walkable neighbour the
    // direction actually points at asks the question that matters: does
    // following this field always make progress toward the goal?
    u32 walked = 0;
    for (i32 sy = 1; sy < kH - 1; sy += 3) {
        for (i32 sx = 1; sx < kW - 1; sx += 3) {
            if (!mask.walkable(sx, sy)) continue;
            if (!full.reachable(mask.cell_to_world(sx, sy))) continue;

            const f32* costs = full.costs();
            i32 x = sx;
            i32 y = sy;
            bool arrived = false;
            // Cost falls strictly every step, so a walk longer than the grid
            // has cells means the field is circling.
            for (i32 step = 0; step < kW * kH && !arrived; ++step) {
                const f32 here = costs[y * kW + x];
                if (here <= 0.0f) {
                    arrived = true;
                    break;
                }
                const Vec2 dir = full.sample_nearest(mask.cell_to_world(x, y));
                if (dir.x == 0.0f && dir.y == 0.0f) break;

                // Best-aligned walkable neighbour that actually descends.
                i32 bx = x, by = y;
                f32 best_align = 0.0f;
                for (i32 oy = -1; oy <= 1; ++oy) {
                    for (i32 ox = -1; ox <= 1; ++ox) {
                        if (ox == 0 && oy == 0) continue;
                        const i32 nx = x + ox;
                        const i32 ny = y + oy;
                        if (!mask.walkable(nx, ny)) continue;
                        if (!(costs[ny * kW + nx] < here)) continue;
                        const f32 inv_len = (ox != 0 && oy != 0) ? 0.70710678f : 1.0f;
                        const f32 align =
                            (static_cast<f32>(ox) * dir.x + static_cast<f32>(oy) * dir.y) * inv_len;
                        if (align > best_align) {
                            best_align = align;
                            bx = nx;
                            by = ny;
                        }
                    }
                }
                if (bx == x && by == y) break; // nowhere the field points goes downhill
                x = bx;
                y = by;
            }
            INFO("field walk stalled starting from cell " << sx << "," << sy);
            REQUIRE(arrived);
            ++walked;
        }
    }
    REQUIRE(walked > 100);
}

TEST_CASE("pump_rebake drains the dirty queue over several budgeted calls and matches rebake_pending",
          "[flowfield][rebake][budget]") {
    TissueMask mask_budgeted = make_scene();
    TissueMask mask_immediate = make_scene();
    const FlowFieldBakeDesc desc = make_desc(/*margin=*/4);

    FlowField budgeted;
    budgeted.bake(mask_budgeted, desc);
    FlowField immediate;
    immediate.bake(mask_immediate, desc);

    // Three separate edits in three separate places, all marked dirty at once.
    const Rect edits[3] = {
        block_rect(mask_budgeted, Rect{Vec2{17.0f, 3.0f}, Vec2{19.0f, 5.0f}}),
        block_rect(mask_budgeted, Rect{Vec2{17.0f, 17.0f}, Vec2{19.0f, 19.0f}}),
        block_rect(mask_budgeted, Rect{Vec2{17.0f, 27.0f}, Vec2{19.0f, 29.0f}}),
    };
    for (const Rect& r : edits) {
        block_rect(mask_immediate, r);
        budgeted.mark_dirty(r);
        immediate.mark_dirty(r);
    }
    immediate.rebake_pending(mask_immediate);

    // Drain with a deliberately tiny budget so it takes multiple pumps.
    int pumps = 0;
    bool done = false;
    while (!done) {
        done = budgeted.pump_rebake(mask_budgeted, /*budget_ms=*/0.001);
        ++pumps;
        REQUIRE(pumps < 1000); // guard against an infinite loop on a real bug
        // Between pumps the field must stay internally consistent: no NaNs,
        // and cells already resolved keep valid (finite-or-inf) costs.
        const f32* c = budgeted.costs();
        for (i32 i = 0; i < kW * kH; ++i) REQUIRE_FALSE(std::isnan(c[i]));
    }
    REQUIRE(pumps > 1); // actually exercised multi-frame draining
    REQUIRE_FALSE(budgeted.has_pending_rebake());

    const DiffResult d = diff_fields(budgeted, immediate, kW, kH, 0.05f);
    WARN("pump_rebake drained over " << pumps << " calls; max_cost_diff=" << d.max_cost_diff);
    REQUIRE(d.reachability_mismatches == 0);
    REQUIRE(d.cost_mismatches == 0);
}

TEST_CASE("pump_rebake with a generous budget drains everything in one call",
          "[flowfield][rebake][budget]") {
    TissueMask mask = make_scene();
    const FlowFieldBakeDesc desc = make_desc(4);
    FlowField field;
    field.bake(mask, desc);

    block_rect(mask, Rect{Vec2{17.0f, 17.0f}, Vec2{19.0f, 19.0f}});
    field.mark_dirty(Rect{Vec2{17.0f, 17.0f}, Vec2{19.0f, 19.0f}});
    const bool drained = field.pump_rebake(mask, /*budget_ms=*/1000.0);
    REQUIRE(drained);
    REQUIRE_FALSE(field.has_pending_rebake());
}
