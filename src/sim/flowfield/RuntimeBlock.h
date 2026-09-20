// sim/flowfield/RuntimeBlock.h — an oriented bar carved out of the tissue at
// runtime, and handed back later. Header-only for the same reason
// ObstacleRaster.h and LaneConnectivity.h are.
//
// WHAT A RUNTIME BLOCK IS. The Fibrin Clot (game/abilities) was the first: a
// bar the player drops mid-lane that the horde has to squeeze past. The
// Fibroblast's scars (sim/scar) are the second: the same bar, laid by a
// swarmer instead of a click, and torn down by damage instead of a clock.
// Both do exactly this:
//
//   1. mark every WALKABLE cell whose centre is inside the bar non-walkable,
//      remembering which ones those were;
//   2. mark the flow field dirty over the bar so the horde reroutes;
//   3. later, give exactly those cells back and mark dirty again.
//
// Only cells that were walkable at carve time are recorded and only those
// are restored, so a bar laid across authored rock, or across another block
// placed earlier, never un-blocks ground it did not block. That is the rule
// the clot always followed; it is stated once here so the scar cannot follow
// a different one.
//
// The SDF is deliberately left alone by both, for the reason ChaffSystem.cpp
// gives at contain_to_tissue(): the SDF is what tower placement reads for
// clearance, and folding runtime blocks into it would make every block
// unbuildable ground. Chaff collide with a block through the MASK (the
// containment pass), and swarmers -- which only ever read the SDF -- walk
// straight through it. That asymmetry is what makes a scar a wall for the
// horde and not for the player's own cells.
//
// ENEMIES ONLY, stated as a rule. A runtime block is a wall to the horde --
// chaff and named agents alike, both contained by contain_to_walkable()
// below -- and to nothing on the player's side: swarmers read the SDF, and
// rounds test TissueMask::authored_wall(), which every carved cell is
// flagged as NOT being (set_runtime_block). Anything new that wants to stop
// at tissue has to choose which of the two questions it is asking.
//
// CELL CONVENTION. Centre membership, padded by one cell, exactly as
// ObstacleRaster.h stamps: a bar and an authored obstacle of the same
// geometry cover the same cells.
#pragma once

#include "core/Math.h"
#include "core/Types.h"
#include "sim/flowfield/FlowField.h"

#include <cmath>
#include <vector>

namespace immune::sim {

/// The cells one block displaced, and the rectangle to hand
/// FlowField::mark_dirty when they come back.
struct CarvedFootprint {
    std::vector<u32> carved_cells;   ///< TissueMask::index() of each cell.
    Rect dirty_bounds{};
    bool empty() const { return carved_cells.empty(); }
};

/// An oriented bar: `half_extents.x` along `rotation`, `.y` across it.
struct Bar {
    Vec2 center{0.0f, 0.0f};
    Vec2 half_extents{1.0f, 1.0f};
    f32 rotation = 0.0f;   ///< Radians, CCW.

    f32 cs() const { return std::cos(rotation); }
    f32 sn() const { return std::sin(rotation); }

    /// Point-in-bar, in the bar's own frame.
    bool contains(Vec2 p) const {
        const Vec2 d = p - center;
        const f32 c = cs();
        const f32 s = sn();
        const f32 lx = d.x * c + d.y * s;
        const f32 ly = -d.x * s + d.y * c;
        return std::fabs(lx) <= half_extents.x && std::fabs(ly) <= half_extents.y;
    }

    /// World-space AABB: the support along each axis.
    Rect bounds() const {
        const f32 c = std::fabs(cs());
        const f32 s = std::fabs(sn());
        const f32 bx = c * half_extents.x + s * half_extents.y;
        const f32 by = s * half_extents.x + c * half_extents.y;
        return Rect{center - Vec2{bx, by}, center + Vec2{bx, by}};
    }

    /// Squared distance from `p` to the bar's solid (0 inside).
    f32 distance_sq(Vec2 p) const {
        const Vec2 d = p - center;
        const f32 c = cs();
        const f32 s = sn();
        const f32 lx = d.x * c + d.y * s;
        const f32 ly = -d.x * s + d.y * c;
        const f32 qx = math::max(std::fabs(lx) - half_extents.x, 0.0f);
        const f32 qy = math::max(std::fabs(ly) - half_extents.y, 0.0f);
        return qx * qx + qy * qy;
    }
};

/// Do two oriented bars overlap? Separating-axis test over the four face
/// normals (two per bar) -- exact for rectangles, and center-distance alone
/// is not a substitute: a long bar's far end can sit well past `spacing`
/// from its center while still physically under a second bar laid there.
inline bool bars_overlap(const Bar& a, const Bar& b) {
    const Vec2 ax{a.cs(), a.sn()};
    const Vec2 ay{-a.sn(), a.cs()};
    const Vec2 bx{b.cs(), b.sn()};
    const Vec2 by{-b.sn(), b.cs()};
    const Vec2 d = b.center - a.center;
    const Vec2 axes[4] = {ax, ay, bx, by};
    for (const Vec2& n : axes) {
        const f32 ra = std::fabs(ax.x * n.x + ax.y * n.y) * a.half_extents.x +
                       std::fabs(ay.x * n.x + ay.y * n.y) * a.half_extents.y;
        const f32 rb = std::fabs(bx.x * n.x + bx.y * n.y) * b.half_extents.x +
                       std::fabs(by.x * n.x + by.y * n.y) * b.half_extents.y;
        const f32 dist = std::fabs(d.x * n.x + d.y * n.y);
        if (dist > ra + rb) return false;
    }
    return true;
}

/// The bar that lies ACROSS the local flow at `p`: one drop makes a dam, not
/// a divider. A point with no flow (off the baked field, an unreachable
/// pocket) gets a horizontal bar; callers that care reject such points
/// before asking.
inline f32 across_flow_rotation(const FlowField& flow, Vec2 p) {
    const Vec2 flow_dir = math::normalize_safe(flow.sample(p));
    Vec2 along{1.0f, 0.0f};
    if (flow_dir.x != 0.0f || flow_dir.y != 0.0f) along = Vec2{-flow_dir.y, flow_dir.x};
    return std::atan2(along.y, along.x);
}

/// Carves `bar` out of `mask` and marks `flow` dirty over it. The returned
/// footprint is what restore_block() needs later. An empty footprint means
/// the bar covered no walkable cell at all (it was laid on rock), and in
/// that case nothing was marked dirty either.
inline CarvedFootprint carve_block(TissueMask& mask, FlowField& flow, const Bar& bar) {
    CarvedFootprint fp;
    if (mask.width() <= 0 || mask.height() <= 0) return fp;
    const Rect bounds = bar.bounds();
    const IVec2 c0 = mask.world_to_cell(bounds.min);
    const IVec2 c1 = mask.world_to_cell(bounds.max);
    for (i32 y = c0.y - 1; y <= c1.y + 1; ++y) {
        for (i32 x = c0.x - 1; x <= c1.x + 1; ++x) {
            if (!mask.walkable(x, y)) continue;
            if (!bar.contains(mask.cell_to_world(x, y))) continue;
            mask.set_walkable(x, y, false);
            mask.set_runtime_block(x, y, true);
            fp.carved_cells.push_back(static_cast<u32>(mask.index(x, y)));
        }
    }
    fp.dirty_bounds = bounds;
    if (!fp.carved_cells.empty()) flow.mark_dirty(bounds);
    return fp;
}

/// Hands a block's cells back to the mask and marks the flow dirty over
/// them, exactly as carve_block() took them. Safe on an empty footprint.
inline void restore_block(TissueMask& mask, FlowField& flow, const CarvedFootprint& fp) {
    if (fp.carved_cells.empty() || mask.width() <= 0) return;
    const u32 w = static_cast<u32>(mask.width());
    for (u32 idx : fp.carved_cells) {
        const i32 x = static_cast<i32>(idx % w);
        const i32 y = static_cast<i32>(idx / w);
        mask.set_walkable(x, y, true);
        mask.set_runtime_block(x, y, false);
    }
    flow.mark_dirty(fp.dirty_bounds);
}

/// Keeps a walker that STARTED the tick on walkable ground on walkable
/// ground at its end: bisects the step to the last walkable point, then
/// re-applies what is left of it one axis at a time so motion along a face
/// survives and motion into it does not. The chaff kernel's containment
/// pass (ChaffSystem.cpp, contain_to_tissue, where the full argument for
/// this shape lives) and the named-agent movement system both run exactly
/// this, so a wall the horde cannot press through is one an elite cannot
/// either. A walker that started off walkable ground is left alone: the
/// SDF response owns recovering it.
inline void contain_to_walkable(const TissueMask& mask, Vec2& p, Vec2 old) {
    if (mask.width() <= 0 || mask.height() <= 0) return;
    const auto walkable = [&mask](Vec2 q) {
        const IVec2 c = mask.world_to_cell(q);
        return mask.walkable(c.x, c.y);
    };
    if (walkable(p)) return;
    if (!walkable(old)) return;

    const Vec2 d = p - old;
    f32 good = 0.0f;
    f32 bad = 1.0f;
    for (u32 k = 0; k < 6; ++k) {
        const f32 mid = 0.5f * (good + bad);
        if (walkable(old + d * mid)) good = mid;
        else bad = mid;
    }
    Vec2 c = old + d * good;
    const f32 rest = 1.0f - good;
    const Vec2 r = d * rest;
    if (r.x != 0.0f && walkable(Vec2{c.x + r.x, c.y})) c.x += r.x;
    if (r.y != 0.0f && walkable(Vec2{c.x, c.y + r.y})) c.y += r.y;
    p = c;
}

} // namespace immune::sim
