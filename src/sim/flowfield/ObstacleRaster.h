// sim/flowfield/ObstacleRaster.h — authored obstacles -> TissueMask carving.
// Owner: Wave 7. Header-only for the same reason TissueRaster.h is: src's
// CMakeLists lists sim sources explicitly and is orchestrator-owned, so this
// module must not add a .cpp.
//
// THE INVERSE OF TissueRaster.h. That file stamps lumen INTO rock (walkable =
// true); this one puts rock BACK inside a lumen (walkable = false), so a level
// can author an island of tissue standing in the middle of a lane: a plaque, a
// septum stub, a calcified nodule. There is no separate "obstacle" concept
// anywhere downstream and deliberately so -- a carved cell is indistinguishable
// from a cell the vessel spline never covered, which is exactly what makes the
// feature nearly free:
//
//   - VISUALS. tissue.frag reconstructs interstitium, vessel wall and lumen
//     from the SDF alone; it owns no per-vessel geometry. Carve before
//     DistanceField::bake() and an island grows the same warped edge, the same
//     concentric laminae and the same wall lighting as the lane's outer
//     boundary, because it IS the same boundary.
//   - PATHING. FlowField bakes from the mask, so the horde routes around an
//     island the way it routes around a bend.
//   - COLLISION. ChaffSystem's resolve_wall_contact (SDF gradient) and
//     contain_to_tissue (mask bisection) never ask which side of the lumen a
//     wall belongs to.
//   - TOWERS. TowerSystem::validate rejects anything whose SDF clearance is
//     under the footprint radius, so an island is unbuildable ground for free.
//
// ORDERING IS THE ONE RULE. Carve after rasterize_vessels() and before
// DistanceField::bake(); everything above follows from the SDF having seen the
// island. Carving after the bake leaves a hole the mask knows about and the
// renderer, the steering and placement validation do not.
//
// CELL CONVENTION. A cell is carved when its CENTER is inside the shape, the
// same test detail::stamp_disc uses to fill one, so a shape and a vessel of
// identical geometry agree on every cell and an island never leaves a
// half-carved fringe against the lumen it sits in.
//
// Level-load-time work. Flat-array writes, no per-cell allocation, never
// called from a tick.
#pragma once

#include "core/Math.h"
#include "sim/flowfield/FlowField.h"
#include "sim/flowfield/TissueRaster.h"   // VesselSpline/eval_spline, for carve_ridge.

#include <cmath>
#include <vector>

namespace immune::sim {

namespace detail {

/// Marks every cell in the world-space box [lo, hi] whose center satisfies
/// `inside` as non-walkable. The box is padded by one cell on each side so a
/// shape whose bound falls mid-cell still tests the cells it grazes.
template <typename Fn>
inline void carve_region(TissueMask& mask, Vec2 lo, Vec2 hi, Fn&& inside) {
    const IVec2 c0 = mask.world_to_cell(lo);
    const IVec2 c1 = mask.world_to_cell(hi);
    for (i32 y = c0.y - 1; y <= c1.y + 1; ++y) {
        for (i32 x = c0.x - 1; x <= c1.x + 1; ++x) {
            if (!mask.in_range(x, y)) continue;
            if (!inside(mask.cell_to_world(x, y))) continue;
            mask.set_walkable(x, y, false);
        }
    }
}

/// Squared distance from `p` to segment [a, b]. A degenerate segment (a == b)
/// falls through to the point case, which is what lets carve_capsule stand in
/// for a disc without a special case.
inline f32 dist_sq_to_segment(Vec2 p, Vec2 a, Vec2 b) {
    const Vec2 ab = b - a;
    const f32 len_sq = math::length_sq(ab);
    if (len_sq <= 1e-12f) return math::length_sq(p - a);
    const f32 t = math::saturate(((p.x - a.x) * ab.x + (p.y - a.y) * ab.y) / len_sq);
    return math::length_sq(p - (a + ab * t));
}

/// Even-odd crossing test. Even-odd rather than nonzero winding is the
/// authoring-friendly choice: a polygon that accidentally crosses itself carves
/// a visible hole instead of quietly filling solid, so the mistake shows up in
/// the first preview rather than shipping.
inline bool point_in_polygon(Vec2 p, const std::vector<Vec2>& poly) {
    bool in = false;
    const usize n = poly.size();
    for (usize i = 0, j = n - 1; i < n; j = i++) {
        const Vec2 a = poly[i];
        const Vec2 b = poly[j];
        if ((a.y > p.y) != (b.y > p.y)) {
            const f32 x_cross = a.x + (p.y - a.y) / (b.y - a.y) * (b.x - a.x);
            if (p.x < x_cross) in = !in;
        }
    }
    return in;
}

} // namespace detail

/// Solid disc of radius `r` at `c`.
inline void carve_disc(TissueMask& mask, Vec2 c, f32 r) {
    if (r <= 0.0f) return;
    const f32 r2 = r * r;
    detail::carve_region(mask, c - Vec2{r, r}, c + Vec2{r, r},
                         [&](Vec2 p) { return math::length_sq(p - c) <= r2; });
}

/// Stadium: everything within `r` of the segment [a, b]. The workhorse shape --
/// a strut, a bar across a lane, a wall stub -- and chained end to end it draws
/// any polyline barrier.
inline void carve_capsule(TissueMask& mask, Vec2 a, Vec2 b, f32 r) {
    if (r <= 0.0f) return;
    const Vec2 lo{math::min(a.x, b.x) - r, math::min(a.y, b.y) - r};
    const Vec2 hi{math::max(a.x, b.x) + r, math::max(a.y, b.y) + r};
    const f32 r2 = r * r;
    detail::carve_region(mask, lo, hi,
                         [&](Vec2 p) { return detail::dist_sq_to_segment(p, a, b) <= r2; });
}

/// Oriented box: `half_extents` about `center`, rotated `rotation` radians CCW.
/// Rotation is why this is not just Rect + block_rect: an axis-aligned bar is
/// the one thing a curving vessel almost never wants.
inline void carve_box(TissueMask& mask, Vec2 center, Vec2 half_extents, f32 rotation) {
    const f32 hx = math::max(half_extents.x, 0.0f);
    const f32 hy = math::max(half_extents.y, 0.0f);
    if (hx <= 0.0f || hy <= 0.0f) return;
    const f32 cs = std::cos(rotation);
    const f32 sn = std::sin(rotation);
    // Bound of the rotated box: the support along each world axis.
    const f32 bx = std::fabs(cs) * hx + std::fabs(sn) * hy;
    const f32 by = std::fabs(sn) * hx + std::fabs(cs) * hy;
    detail::carve_region(mask, center - Vec2{bx, by}, center + Vec2{bx, by}, [&](Vec2 p) {
        const Vec2 d = p - center;
        // Into the box's own frame (inverse rotation).
        const f32 lx = d.x * cs + d.y * sn;
        const f32 ly = -d.x * sn + d.y * cs;
        return std::fabs(lx) <= hx && std::fabs(ly) <= hy;
    });
}

/// Arbitrary polygon, convex or concave, optionally grown outward by `inflate`
/// world units (which also rounds its corners -- the offset is a disc sum, not
/// a mitre). `inflate` is what lets a two-point "polygon" degenerate into a
/// thick line, and what keeps a slim polygon from slipping between cell
/// centers.
inline void carve_polygon(TissueMask& mask, const std::vector<Vec2>& points, f32 inflate = 0.0f) {
    if (points.size() < 2) return;
    const f32 grow = math::max(inflate, 0.0f);
    Vec2 lo = points[0];
    Vec2 hi = points[0];
    for (const Vec2& p : points) {
        lo = Vec2{math::min(lo.x, p.x), math::min(lo.y, p.y)};
        hi = Vec2{math::max(hi.x, p.x), math::max(hi.y, p.y)};
    }
    const f32 grow_sq = grow * grow;
    const bool fillable = points.size() >= 3;
    detail::carve_region(mask, lo - Vec2{grow, grow}, hi + Vec2{grow, grow}, [&](Vec2 p) {
        if (fillable && detail::point_in_polygon(p, points)) return true;
        if (grow <= 0.0f) return false;
        for (usize i = 0, j = points.size() - 1; i < points.size(); j = i++) {
            if (detail::dist_sq_to_segment(p, points[j], points[i]) <= grow_sq) return true;
        }
        return false;
    });
}

/// Curved, variable-thickness barrier: the same Catmull-Rom-with-per-point-
/// width curve a vessel is authored from, stamped solid instead of hollow. The
/// natural shape for a septum that has to follow the lane it divides.
inline void carve_ridge(TissueMask& mask, const VesselSpline& spline) {
    const i32 n = static_cast<i32>(spline.points.size());
    if (n == 0 || mask.width() <= 0) return;
    if (n == 1) {
        carve_disc(mask, spline.points[0].pos, spline.points[0].width * 0.5f);
        return;
    }
    // Step count picked exactly as rasterize_vessel picks it, so a ridge and a
    // vessel of the same control points are watertight to the same standard.
    const f32 cs = mask.cell_size();
    f32 poly_len = 0.0f;
    for (i32 i = 0; i + 1 < n; ++i) {
        poly_len += math::length(spline.points[static_cast<usize>(i + 1)].pos -
                                 spline.points[static_cast<usize>(i)].pos);
    }
    const f32 span = static_cast<f32>(n - 1);
    i32 steps = static_cast<i32>(poly_len / math::max(cs, 1e-3f) * 4.0f);
    steps = math::max(steps, 8 * (n - 1));
    steps = math::min(steps, 1 << 20);
    for (i32 k = 0; k <= steps; ++k) {
        const f32 u = span * static_cast<f32>(k) / static_cast<f32>(steps);
        const VesselPoint vp = eval_spline(spline, u);
        carve_disc(mask, vp.pos, vp.width * 0.5f);
    }
}

} // namespace immune::sim
