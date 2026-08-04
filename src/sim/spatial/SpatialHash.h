// sim/spatial/SpatialHash.h — uniform-grid broadphase. FROZEN CONTRACT.
// Owner: Wave 1B.
//
// RATIONALE (DESIGN.md §8.4)
// The only spatial queries the game makes are: (a) local separation neighbours
// for chaff, (b) "which chaff overlap this damage field / AoE", (c) tower
// targeting "nearest or strongest in radius". All three are short-range queries
// over a roughly uniform density of points in a bounded 2D world. A uniform grid
// beats a tree here: build is O(n) with two linear passes, lookup is O(1)
// arithmetic, and the whole structure is two flat integer arrays with no
// pointers and no per-frame allocation.
//
// LAYOUT
// Counting sort (CSR-style): `cell_start` has cell_count+1 entries; the agent
// indices in cell c are `indices[cell_start[c] .. cell_start[c+1])`. No
// per-cell std::vector — that would allocate thousands of times a frame.
//
// LIFETIME
// Rebuilt from the chaff SoA once per tick, before movement. It stores *indices
// into the chaff SoA*, so it is invalidated by any compaction/despawn pass.
// Rebuild before use; never cache indices across ticks.
#pragma once

#include "core/Types.h"

#include <vector>

namespace immune { class JobSystem; }

namespace immune::sim {

/// Cell size should be ~2x the separation radius (DESIGN.md §8.3: "agent radius x k"),
/// so a 3x3 cell neighbourhood covers every possible separation partner.
struct SpatialHashDesc {
    Rect bounds{};              ///< World-space extent covered by the grid.
    f32 cell_size = 4.0f;       ///< World units per cell.
};

class SpatialHash {
public:
    /// Allocates the grid. Safe to call again on level change; not per tick.
    void configure(const SpatialHashDesc& desc);

    /// Rebuilds from a positions array of `count` points (the chaff SoA `pos_x`
    /// / `pos_y` streams). Reuses its buffers: no allocation after the first
    /// build at a given capacity. `jobs` may be null for a serial build.
    void rebuild(const f32* pos_x, const f32* pos_y, usize count, JobSystem* jobs = nullptr);

    /// Clears occupancy without releasing memory.
    void clear();

    // ---- Queries -----------------------------------------------------------
    // All queries append agent indices to `out_indices` (caller-owned, cleared
    // by the callee) and never allocate if the caller has reserved capacity.
    // Queries are *conservative at cell granularity*: they return every agent in
    // the overlapping cells. Callers must do the exact distance/point test.

    /// Agents in cells overlapping the circle (radius test NOT applied).
    void query_circle(Vec2 center, f32 radius, std::vector<u32>& out_indices) const;

    /// Agents in cells overlapping the rectangle (exact test NOT applied).
    void query_rect(const Rect& rect, std::vector<u32>& out_indices) const;

    /// Agents in the 3x3 cell block around `p` — the separation query. This is
    /// the hottest path in the game; prefer the raw cell accessors below inside
    /// a per-agent loop to avoid touching a std::vector 10,000 times a tick.
    void query_neighbourhood(Vec2 p, std::vector<u32>& out_indices) const;

    // ---- Raw cell access (hot path) ---------------------------------------
    IVec2 cell_coord(Vec2 p) const;
    u32 cell_index(IVec2 c) const;
    IVec2 grid_dims() const { return dims_; }
    f32 cell_size() const { return cell_size_; }
    const Rect& bounds() const { return bounds_; }

    /// CSR span of a cell: indices()[begin .. end).
    void cell_range(u32 cell, u32& out_begin, u32& out_end) const;
    const u32* indices() const { return indices_.data(); }
    usize indexed_count() const { return indices_.size(); }

    /// Number of agents in the fullest cell — a density signal the renderer's
    /// LOD pass and the Mast Cell trigger both consume.
    u32 max_cell_occupancy() const { return max_occupancy_; }

    /// Occupancy of every cell, row-major, dims().x * dims().y entries.
    /// Used to build the density-LOD blob field without a second pass.
    const u32* occupancy() const { return occupancy_.data(); }

private:
    Rect bounds_{};
    f32 cell_size_ = 4.0f;
    f32 inv_cell_size_ = 0.25f;
    IVec2 dims_{0, 0};
    std::vector<u32> cell_start_;  ///< size = cell_count + 1
    std::vector<u32> occupancy_;   ///< size = cell_count
    std::vector<u32> indices_;     ///< size = agent count
    u32 max_occupancy_ = 0;
};

} // namespace immune::sim
