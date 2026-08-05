// sim/flowfield/TissueRaster.h — vessel splines -> TissueMask rasterization.
// Owner: Wave 1A. Header-only by design: src/CMakeLists.txt lists sim sources
// explicitly and is orchestrator-owned, so this module must not add a .cpp.
//
// A level (DESIGN.md §4, ARCHITECTURE.md §6 `game/level`) is authored as
// centerline splines with a per-point width, never as a painted mask. This file
// is the geometry->grid step: it evaluates a centripetal-ish Catmull-Rom through
// the control points, and stamps a capsule of the interpolated width along the
// curve into the mask's walkable + cost planes.
//
// Everything here is level-load / test-time work. It is not hot-path code, but
// it still writes into flat arrays and never allocates per cell.
#pragma once

#include "core/Math.h"
#include "sim/flowfield/FlowField.h"

#include <vector>

namespace immune::sim {

/// One authored control point on a vessel centerline.
struct VesselPoint {
    Vec2 pos{0.0f, 0.0f};
    /// Full lumen width in world units (the stamped radius is width * 0.5).
    f32 width = 4.0f;
    /// Traversal cost multiplier applied to cells this point covers (>= 1).
    f32 cost_mul = 1.0f;
};

/// A vessel centerline. Two points give a straight segment; three or more are
/// interpolated with Catmull-Rom. Splines are additive: rasterizing several into
/// the same mask unions their lumens, which is how bifurcations are authored
/// (share a control point, then diverge).
struct VesselSpline {
    std::vector<VesselPoint> points;
};

namespace detail {

/// Uniform Catmull-Rom basis. Returns the interpolated value at `t` in [0,1]
/// between p1 and p2, with p0/p3 as the surrounding tangent references.
inline f32 catmull_rom(f32 p0, f32 p1, f32 p2, f32 p3, f32 t) {
    const f32 t2 = t * t;
    const f32 t3 = t2 * t;
    return 0.5f * ((2.0f * p1) + (-p0 + p2) * t + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 +
                   (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
}

inline Vec2 catmull_rom(Vec2 p0, Vec2 p1, Vec2 p2, Vec2 p3, f32 t) {
    return Vec2{catmull_rom(p0.x, p1.x, p2.x, p3.x, t), catmull_rom(p0.y, p1.y, p2.y, p3.y, t)};
}

/// Clamped control-point fetch, so the endpoints get a sane tangent reference
/// without the caller duplicating points.
inline const VesselPoint& at(const VesselSpline& s, i32 i) {
    const i32 n = static_cast<i32>(s.points.size());
    return s.points[static_cast<usize>(math::clamp(i, 0, n - 1))];
}

/// Stamps a filled disc of radius `r` (world units) centered at `c`.
inline void stamp_disc(TissueMask& mask, Vec2 c, f32 r, f32 cost_mul) {
    if (r <= 0.0f) return;
    const Vec2 lo = c - Vec2{r, r};
    const Vec2 hi = c + Vec2{r, r};
    const IVec2 c0 = mask.world_to_cell(lo);
    const IVec2 c1 = mask.world_to_cell(hi);
    const f32 r2 = r * r;
    for (i32 y = c0.y - 1; y <= c1.y + 1; ++y) {
        for (i32 x = c0.x - 1; x <= c1.x + 1; ++x) {
            if (!mask.in_range(x, y)) continue;
            const Vec2 p = mask.cell_to_world(x, y);
            if (math::length_sq(p - c) > r2) continue;
            // Overlapping vessels take the cheaper (more permeable) lumen.
            const bool was_walkable = mask.walkable(x, y);
            const f32 prev = mask.cost(x, y);
            mask.set_walkable(x, y, true);
            mask.set_cost(x, y, was_walkable ? math::min(prev, cost_mul) : cost_mul);
        }
    }
}

} // namespace detail

/// Evaluates the spline at arc parameter `u` in [0, segment_count]. Returns the
/// interpolated centerline point, width, and cost multiplier.
inline VesselPoint eval_spline(const VesselSpline& s, f32 u) {
    const i32 n = static_cast<i32>(s.points.size());
    if (n == 0) return VesselPoint{};
    if (n == 1) return s.points[0];
    const f32 max_u = static_cast<f32>(n - 1);
    u = math::clamp(u, 0.0f, max_u);
    i32 i = static_cast<i32>(u);
    if (i >= n - 1) i = n - 2;
    const f32 t = u - static_cast<f32>(i);

    const VesselPoint& p0 = detail::at(s, i - 1);
    const VesselPoint& p1 = detail::at(s, i);
    const VesselPoint& p2 = detail::at(s, i + 1);
    const VesselPoint& p3 = detail::at(s, i + 2);

    VesselPoint out;
    out.pos = detail::catmull_rom(p0.pos, p1.pos, p2.pos, p3.pos, t);
    out.width = math::max(0.0f, detail::catmull_rom(p0.width, p1.width, p2.width, p3.width, t));
    out.cost_mul = math::max(1.0f, math::lerp(p1.cost_mul, p2.cost_mul, t));
    return out;
}

/// Rasterizes one vessel into `mask`, unioning with whatever is already there.
/// The curve is sampled finely enough that consecutive disc stamps overlap by
/// at least half a cell, so the lumen is watertight at any width.
inline void rasterize_vessel(TissueMask& mask, const VesselSpline& spline) {
    const i32 n = static_cast<i32>(spline.points.size());
    if (n == 0 || mask.width() <= 0) return;
    if (n == 1) {
        detail::stamp_disc(mask, spline.points[0].pos, spline.points[0].width * 0.5f,
                           math::max(1.0f, spline.points[0].cost_mul));
        return;
    }

    const f32 cs = mask.cell_size();
    // Conservative arc-length estimate from the control polygon; Catmull-Rom
    // overshoot is bounded well below 2x for sane authoring, and we oversample.
    f32 poly_len = 0.0f;
    for (i32 i = 0; i + 1 < n; ++i) {
        poly_len += math::length(spline.points[static_cast<usize>(i + 1)].pos -
                                 spline.points[static_cast<usize>(i)].pos);
    }
    const f32 span = static_cast<f32>(n - 1);
    // Two samples per cell of estimated length, floor of 8 per segment.
    i32 steps = static_cast<i32>(poly_len / math::max(cs, 1e-3f) * 4.0f);
    steps = math::max(steps, 8 * (n - 1));
    steps = math::min(steps, 1 << 20);

    for (i32 k = 0; k <= steps; ++k) {
        const f32 u = span * static_cast<f32>(k) / static_cast<f32>(steps);
        const VesselPoint vp = eval_spline(spline, u);
        detail::stamp_disc(mask, vp.pos, vp.width * 0.5f, vp.cost_mul);
    }
}

inline void rasterize_vessels(TissueMask& mask, const std::vector<VesselSpline>& splines) {
    for (const VesselSpline& s : splines) rasterize_vessel(mask, s);
}

/// Carves a solid axis-aligned world rectangle out of the mask (tower footprint,
/// NET drop, scripted collapse). Returns the affected world rect, ready to hand
/// straight to FlowField::mark_dirty.
inline Rect block_rect(TissueMask& mask, const Rect& r) {
    const IVec2 c0 = mask.world_to_cell(r.min);
    const IVec2 c1 = mask.world_to_cell(r.max);
    for (i32 y = c0.y; y <= c1.y; ++y) {
        for (i32 x = c0.x; x <= c1.x; ++x) {
            mask.set_walkable(x, y, false);
        }
    }
    return r;
}

} // namespace immune::sim
