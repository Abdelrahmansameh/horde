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

    /// Per-cell traversal cost multiplier (>= 1). Sludge, NETs, and biofilm
    /// raise it; the flow field routes around expensive cells automatically.
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
    /// Diagonal moves cost sqrt(2) when true (smoother fields, slightly slower).
    bool allow_diagonals = true;
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
    /// sold, NET dropped, biofilm formed). Cheap: only records the rect.
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

    // ---- Introspection -----------------------------------------------------
    i32 width() const { return width_; }
    i32 height() const { return height_; }
    f32 cell_size() const { return cell_size_; }
    /// Raw per-cell unit vectors, row-major. Debug PNG dumps read this.
    const Vec2* directions() const { return direction_.data(); }
    const f32* costs() const { return cost_.data(); }
    const RebakeStats& stats() const { return stats_; }

private:
    i32 width_ = 0, height_ = 0;
    f32 cell_size_ = 1.0f;
    Vec2 origin_{0.0f, 0.0f};
    std::vector<f32> cost_;
    std::vector<Vec2> direction_;
    std::vector<Rect> dirty_;
    FlowFieldBakeDesc desc_{};
    RebakeStats stats_{};
};

} // namespace immune::sim
