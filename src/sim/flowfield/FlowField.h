// sim/flowfield/FlowField.h — tissue mask -> SDF -> flow field. FROZEN CONTRACT.
// Owner: Wave 1A.
//
// RATIONALE (DESIGN.md §8.3)
// 10,000 agents cannot each run A*. Instead the level bakes, once, a vector
// field giving every walkable point a direction toward the objective. An agent's
// entire "pathfinding" cost becomes one bilinear sample. Vessels get organic
// width and branching for free because the field is baked from a rasterized
// tissue mask, not from a corridor graph.
//
// THREE-STAGE PIPELINE
//   1. TissueMask   — rasterized from level splines: which cells are walkable,
//                     plus a per-cell width/cost multiplier.
//   2. SDF          — distance from each walkable cell to the nearest wall.
//                     Drives vessel-hugging steering and valid tower placement.
//   3. FlowField    — cost-to-goal via a Dijkstra/eikonal sweep, then the
//                     negative gradient, normalized, per cell.
//
// INCREMENTAL REBAKE (the load-bearing requirement)
// Placing a tower blocks part of a lane and must reroute the horde *visibly and
// immediately* — but a full-level bake is far too slow to do inside a frame.
// `rebake_region` re-runs the sweep over a dirty rectangle plus a margin,
// seeded from the *existing* cost values on the region boundary. Correct as long
// as the true shortest path from any changed cell leaves and re-enters the
// region at most once; the margin is what buys that. Rebakes may be budgeted
// across frames (`pump_rebake`), with agents flowing on the stale field for a
// tick or two — which reads as momentum, not as a bug.
#pragma once

#include "core/Types.h"

#include <vector>

namespace immune::sim {

/// Rasterized walkability + local cost. Produced by the level loader (Wave 2D)
/// from spline geometry, consumed by the flow-field baker.
class TissueMask {
public:
    void resize(i32 width, i32 height, f32 cell_size, Vec2 world_origin);

    i32 width() const { return width_; }
    i32 height() const { return height_; }
    f32 cell_size() const { return cell_size_; }
    Vec2 world_origin() const { return origin_; }
    Rect world_bounds() const;

    bool in_range(i32 x, i32 y) const { return x >= 0 && y >= 0 && x < width_ && y < height_; }
    usize index(i32 x, i32 y) const { return static_cast<usize>(y) * static_cast<usize>(width_) + static_cast<usize>(x); }

    /// True if chaff may occupy the cell (inside a vessel and not blocked).
    bool walkable(i32 x, i32 y) const { return in_range(x, y) && walkable_[index(x, y)] != 0; }
    void set_walkable(i32 x, i32 y, bool v) { if (in_range(x, y)) walkable_[index(x, y)] = v ? u8{1} : u8{0}; }

    /// Per-cell traversal cost multiplier (>= 1). Sludge and NETs raise it;
    /// the flow field routes around expensive cells automatically.
    f32 cost(i32 x, i32 y) const { return in_range(x, y) ? cost_[index(x, y)] : 1.0f; }
    void set_cost(i32 x, i32 y, f32 c) { if (in_range(x, y)) cost_[index(x, y)] = c; }

    Vec2 cell_to_world(i32 x, i32 y) const;
    IVec2 world_to_cell(Vec2 p) const;

    const u8* walkable_data() const { return walkable_.data(); }
    const f32* cost_data() const { return cost_.data(); }

private:
    i32 width_ = 0, height_ = 0;
    f32 cell_size_ = 1.0f;
    Vec2 origin_{0.0f, 0.0f};
    std::vector<u8> walkable_;
    std::vector<f32> cost_;
};

/// Signed distance to the nearest non-walkable cell, in world units.
/// Positive inside tissue. Used for vessel-hugging steering, placement
/// validation ("tower must sit on tissue with clearance >= r"), and the
/// blob-LOD shader's vessel silhouette.
class DistanceField {
public:
    void bake(const TissueMask& mask);
    f32 sample(Vec2 world_pos) const;      ///< bilinear
    Vec2 gradient(Vec2 world_pos) const;   ///< direction of increasing clearance
    const f32* data() const { return distance_.data(); }
    i32 width() const { return width_; }
    i32 height() const { return height_; }

private:
    i32 width_ = 0, height_ = 0;
    f32 cell_size_ = 1.0f;
    Vec2 origin_{0.0f, 0.0f};
    std::vector<f32> distance_;
};

struct FlowFieldBakeDesc {
    /// Goal cells (objective/organ). Multi-goal is supported: the sweep is
    /// seeded with every goal at cost 0, so agents head for the nearest one.
    std::vector<IVec2> goal_cells;
    /// Extra cells added to the dirty margin on every incremental rebake.
    i32 rebake_margin_cells = 16;
    /// Whether travel may cut across a cell diagonally. Named for the metric it
    /// produces, not for a step set: the sweep is 4-connected either way, and
    /// what this selects is whether the eikonal update may combine both upwind
    /// axes (true -> a round, isotropic distance) or must take them one at a
    /// time (false -> a Manhattan field whose contours are diamonds).
    bool allow_diagonals = true;

    /// World-space radius around every goal cell that also counts as "arrived",
    /// seeded at cost 0. Zero keeps the historical single-cell sink.
    ///
    /// WHY. Chaff despawns the moment it enters the objective's radius, so the
    /// rim *is* the goal — but a one-cell sink makes the field aim every agent
    /// at the exact centre from halfway across the level, which reads as the
    /// organ sucking the horde into a point instead of the horde arriving at
    /// it. Seeding the disc removes the singularity and, with it, most of the
    /// long-range lateral pull inside a wide vessel.
    f32 goal_radius = 0.0f;

    /// World-space radius of the direction-field smoothing pass (0 = off).
    ///
    /// WHY. A shortest-path field is exact but not *natural*: where two
    /// wavefronts meet, the descent direction flips across a one-cell crease,
    /// so agents either side of it are steered hard into the seam and the debug
    /// arrows show a visible scar. Averaging the unit directions over a
    /// neighbourhood turns each crease into a gradual turn — the field stops
    /// being a set of funnels and becomes a general direction that follows the
    /// shape of the lane. Every smoothed vector is still checked against the
    /// true descent direction (see kMinDescentAlign) so no local minimum, and
    /// therefore no agent trap, can ever be introduced.
    f32 smoothing_radius = 0.0f;
};

struct RebakeStats {
    u32 cells_visited = 0;
    u32 regions_processed = 0;
    f64 last_bake_ms = 0.0;
    bool full_bake = false;
};

class FlowField {
public:
    /// Full bake from scratch. Level load only — too slow for a frame.
    void bake(const TissueMask& mask, const FlowFieldBakeDesc& desc);

    /// Marks a world-space rectangle dirty after a mask edit (tower placed or
    /// sold, NET dropped). Cheap: only records the rect.
    void mark_dirty(const Rect& world_region);

    /// Immediately re-solves every pending dirty region. Correctness-first path,
    /// used by tests and by --sim-test.
    void rebake_pending(const TissueMask& mask);

    /// Frame-budgeted rebake: processes dirty regions until `budget_ms` is
    /// exhausted, leaving the rest for next frame. The gameplay path.
    /// Returns true when the dirty queue is fully drained.
    bool pump_rebake(const TissueMask& mask, f64 budget_ms);

    bool has_pending_rebake() const { return !dirty_.empty(); }

    // ---- Sampling (hot path) ----------------------------------------------

    /// Unit direction toward the goal at `world_pos`, bilinearly interpolated
    /// across the four surrounding cells. Returns (0,0) outside the field or in
    /// an unreachable pocket — callers must treat zero as "no guidance", NOT as
    /// "move nowhere is fine".
    Vec2 sample(Vec2 world_pos) const;

    /// Cost-to-goal at `world_pos`. Infinity for unreachable cells. Used by the
    /// wave director for lane threat estimates and by UI threat overlays.
    f32 sample_cost(Vec2 world_pos) const;

    /// True if a path to any goal exists from this point. Tower placement uses
    /// this to reject a placement that would fully wall off a lane.
    bool reachable(Vec2 world_pos) const;

    /// Nearest-cell direction lookup — no interpolation, cheapest possible.
    Vec2 sample_nearest(Vec2 world_pos) const;

    /// sample(), plus how much of the 2x2 stencil actually carried guidance.
    /// `support` is 1 where the sample sits wholly inside the baked field and
    /// falls toward 0 as it straddles a vessel wall.
    ///
    /// Overlays need this. sample() renormalizes over whatever cells it found,
    /// which is right for steering — an agent brushing a wall still gets a
    /// usable direction — but it means a point mostly outside the lumen still
    /// returns a full-length unit vector, and drawing those at full strength is
    /// what fringes a vessel's edge with arrows that answer to nothing.
    Vec2 sample_with_support(Vec2 world_pos, f32& support) const;

    // ---- Introspection -----------------------------------------------------
    i32 width() const { return width_; }
    i32 height() const { return height_; }
    f32 cell_size() const { return cell_size_; }
    /// Raw per-cell unit vectors, row-major. Debug PNG dumps read this.
    const Vec2* directions() const { return direction_.data(); }
    const f32* costs() const { return cost_.data(); }
    const RebakeStats& stats() const { return stats_; }

private:
    // --- Wave 1A implementation detail. Private only; no public signature or
    // documented layout above this line was changed. ------------------------
    struct HeapNode {
        f32 key = 0.0f;
        u32 cell = 0;
    };

    static bool heap_less(const HeapNode& a, const HeapNode& b);
    void heap_push(u32 cell, f32 key);
    HeapNode heap_pop();

    /// Godunov upwind solution of |grad T| = f at one cell, from whichever of
    /// the four orthogonal neighbours currently hold a usable cost. This is the
    /// single place the metric is defined: the sweep, the incremental
    /// invalidation's parent-support test, and the boundary reseed all call it,
    /// so they cannot disagree about what a correct cost is.
    f32 eikonal(const TissueMask& mask, i32 x, i32 y, bool exclude_dirty) const;
    static bool diagonal_ok(const TissueMask& mask, i32 x, i32 y, i32 dir);
    bool is_goal_cell(i32 x, i32 y) const { return !goal_mark_.empty() && goal_mark_[static_cast<usize>(y) * static_cast<usize>(width_) + static_cast<usize>(x)] != 0u; }
    void mark_goals(const TissueMask& mask);
    i32 smoothing_iterations() const;
    bool in_dirty(u32 cell) const { return dirty_mark_[cell] == dirty_gen_; }
    void touch(i32 x, i32 y);

    void sweep(const TissueMask& mask, bool restricted);
    void compute_directions(const TissueMask& mask, i32 x0, i32 y0, i32 x1, i32 y1);
    void smooth_directions(const TissueMask& mask, i32 x0, i32 y0, i32 x1, i32 y1);
    void rebake_region(const TissueMask& mask, const Rect& world_region);

    i32 width_ = 0, height_ = 0;
    f32 cell_size_ = 1.0f;
    Vec2 origin_{0.0f, 0.0f};
    std::vector<f32> cost_;
    std::vector<Vec2> direction_;
    /// Unsmoothed negative gradient, kept alongside the published field.
    /// Smoothing is a fixed function of the *raw* neighbourhood, so an
    /// incremental rebake can reproduce a full bake's smoothed result exactly
    /// by re-running the pass over a halo instead of iterating on values that
    /// have already been smoothed a different number of times.
    std::vector<Vec2> raw_direction_;
    std::vector<Vec2> smooth_front_, smooth_back_;
    /// Per-cell goal flag. O(1) where a scan of goal_cells would be O(goals),
    /// which matters now that a goal is a disc rather than a single cell.
    std::vector<u8> goal_mark_;
    std::vector<Rect> dirty_;
    FlowFieldBakeDesc desc_{};
    RebakeStats stats_{};

    // Sweep scratch, owned so a rebake performs no allocation once warm.
    std::vector<HeapNode> heap_;
    std::vector<u32> scan_;
    std::vector<u32> dirty_mark_;
    u32 dirty_gen_ = 0;
    IVec2 touch_min_{0, 0};
    IVec2 touch_max_{0, 0};
    /// Bounding box of cells whose *raw direction* actually moved, tracked
    /// separately from the cost-change box because the two are wildly
    /// different sizes: a reroute shifts cost-to-goal across everything
    /// downstream of the edit, but far downstream that shift is very nearly
    /// uniform, so the gradient -- and therefore the direction -- does not
    /// move at all. Smoothing only has to revisit the small box.
    IVec2 dir_touch_min_{0, 0};
    IVec2 dir_touch_max_{-1, -1};
};

} // namespace immune::sim
