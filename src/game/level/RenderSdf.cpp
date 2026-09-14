// game/level/RenderSdf.cpp — analytic, rounded, padded distance field for the
// tissue pass. See the header for what it is and why it exists next to the
// sim's DistanceField.
#include "game/level/RenderSdf.h"

#include "core/Math.h"
#include "game/level/Level.h"
#include "sim/flowfield/TissueRaster.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace immune::game {

namespace {

constexpr f32 kInf = std::numeric_limits<f32>::infinity();

/// One swept capsule: the lumen between two consecutive samples of a vessel
/// centreline, with the radius interpolated along it.
struct Segment {
    Vec2 a, b;
    f32 ra, rb;
};

/// (t, distance) of the closest point on [a,b] to p.
inline void closest_on_segment(Vec2 p, Vec2 a, Vec2 b, f32& t, f32& dist) {
    const Vec2 ab = b - a;
    const f32 len_sq = math::length_sq(ab);
    t = len_sq > 1e-12f ? math::saturate(((p.x - a.x) * ab.x + (p.y - a.y) * ab.y) / len_sq) : 0.0f;
    dist = math::length(p - (a + ab * t));
}

/// Row-major float grid with world placement; the working representation for
/// every stage below.
struct Grid {
    i32 w = 0, h = 0;
    f32 cell = 1.0f;
    Vec2 origin{0.0f, 0.0f};
    std::vector<f32> v;

    Vec2 center(i32 x, i32 y) const {
        return Vec2{origin.x + (static_cast<f32>(x) + 0.5f) * cell,
                    origin.y + (static_cast<f32>(y) + 0.5f) * cell};
    }
    IVec2 to_cell(Vec2 p) const {
        return IVec2{static_cast<i32>(std::floor((p.x - origin.x) / cell)),
                     static_cast<i32>(std::floor((p.y - origin.y) / cell))};
    }
    f32& at(i32 x, i32 y) { return v[static_cast<usize>(y) * static_cast<usize>(w) + static_cast<usize>(x)]; }
    f32 at(i32 x, i32 y) const { return v[static_cast<usize>(y) * static_cast<usize>(w) + static_cast<usize>(x)]; }

    /// Visits every cell whose centre lies in the world rect [lo, hi], grown by
    /// `pad`, clipped to the grid.
    template <typename Fn>
    void for_each_in(Vec2 lo, Vec2 hi, f32 pad, Fn&& fn) {
        const IVec2 c0 = to_cell(lo - Vec2{pad, pad});
        const IVec2 c1 = to_cell(hi + Vec2{pad, pad});
        const i32 x0 = math::max(c0.x, 0), x1 = math::min(c1.x + 1, w - 1);
        const i32 y0 = math::max(c0.y, 0), y1 = math::min(c1.y + 1, h - 1);
        for (i32 y = y0; y <= y1; ++y) {
            for (i32 x = x0; x <= x1; ++x) fn(x, y, center(x, y));
        }
    }
};

/// Unions one swept capsule into the field: d = max(d, r(t) - dist).
void stamp_segment_max(Grid& g, const Segment& s, f32 band) {
    const f32 rmax = math::max(s.ra, s.rb);
    const Vec2 lo{math::min(s.a.x, s.b.x), math::min(s.a.y, s.b.y)};
    const Vec2 hi{math::max(s.a.x, s.b.x), math::max(s.a.y, s.b.y)};
    g.for_each_in(lo, hi, rmax + band, [&](i32 x, i32 y, Vec2 p) {
        f32 t, dist;
        closest_on_segment(p, s.a, s.b, t, dist);
        const f32 r = math::lerp(s.ra, s.rb, t);
        f32& d = g.at(x, y);
        d = math::max(d, r - dist);
    });
}

/// Ramer-Douglas-Peucker over (x, y, r): drops every sample that lies within
/// `tol` of the chord between its surviving neighbours, radius included. A
/// straight vessel collapses to one capsule; a bend keeps only as many as its
/// curvature needs. This is what makes the stamp affordable -- every capsule
/// visits its whole bounding box, so a thousand overlapping capsules along a
/// straight lane cost a thousand times what one does.
void simplify_polyline(std::vector<Vec2>& pts, std::vector<f32>& radii, f32 tol) {
    const usize n = pts.size();
    if (n < 3) return;
    std::vector<u8> keep(n, 0);
    keep[0] = 1;
    keep[n - 1] = 1;
    std::vector<std::pair<usize, usize>> stack;
    stack.emplace_back(0, n - 1);
    const f32 tol2 = tol * tol;
    while (!stack.empty()) {
        const auto [a, b] = stack.back();
        stack.pop_back();
        if (b <= a + 1) continue;
        // Farthest interior point from the 3D chord (a -> b).
        const f32 ax = pts[a].x, ay = pts[a].y, ar = radii[a];
        const f32 dx = pts[b].x - ax, dy = pts[b].y - ay, dr = radii[b] - ar;
        const f32 len2 = dx * dx + dy * dy + dr * dr;
        usize worst = a;
        f32 worst_d2 = -1.0f;
        for (usize i = a + 1; i < b; ++i) {
            const f32 px = pts[i].x - ax, py = pts[i].y - ay, pr = radii[i] - ar;
            f32 t = len2 > 1e-12f ? (px * dx + py * dy + pr * dr) / len2 : 0.0f;
            t = math::saturate(t);
            const f32 ex = px - dx * t, ey = py - dy * t, er = pr - dr * t;
            const f32 d2 = ex * ex + ey * ey + er * er;
            if (d2 > worst_d2) { worst_d2 = d2; worst = i; }
        }
        if (worst_d2 > tol2) {
            keep[worst] = 1;
            stack.emplace_back(a, worst);
            stack.emplace_back(worst, b);
        }
    }
    usize w = 0;
    for (usize i = 0; i < n; ++i) {
        if (!keep[i]) continue;
        pts[w] = pts[i];
        radii[w] = radii[i];
        ++w;
    }
    pts.resize(w);
    radii.resize(w);
}

/// Samples a Catmull-Rom vessel into a chain of capsules: densely along the
/// curve first (far finer than any authored curvature), then simplified to
/// within `tol` so straight runs are single capsules.
void sample_spline(const sim::VesselSpline& spline, f32 step, f32 tol, std::vector<Vec2>& pts,
                   std::vector<f32>& radii) {
    pts.clear();
    radii.clear();
    const i32 n = static_cast<i32>(spline.points.size());
    if (n == 0) return;
    if (n == 1) {
        pts.push_back(spline.points[0].pos);
        radii.push_back(spline.points[0].width * 0.5f);
        return;
    }
    f32 poly_len = 0.0f;
    for (i32 i = 0; i + 1 < n; ++i) {
        poly_len += math::length(spline.points[static_cast<usize>(i + 1)].pos -
                                 spline.points[static_cast<usize>(i)].pos);
    }
    i32 steps = static_cast<i32>(poly_len / math::max(step, 1e-3f)) + 1;
    steps = math::clamp(steps, 8 * (n - 1), 1 << 16);
    const f32 span = static_cast<f32>(n - 1);
    pts.reserve(static_cast<usize>(steps) + 1);
    radii.reserve(static_cast<usize>(steps) + 1);
    for (i32 k = 0; k <= steps; ++k) {
        const f32 u = span * static_cast<f32>(k) / static_cast<f32>(steps);
        const sim::VesselPoint vp = sim::eval_spline(spline, u);
        pts.push_back(vp.pos);
        radii.push_back(math::max(vp.width * 0.5f, 0.0f));
    }
    simplify_polyline(pts, radii, tol);
}

/// If `end` sits at (or past) the edge of the world and its tangent points out
/// of it, returns true and writes the far point of a straight run-off segment
/// long enough to leave `grid_bounds`.
bool run_off_point(Vec2 end, Vec2 tangent, f32 width, const Rect& world, const Rect& grid_bounds,
                   f32 extend_widths, Vec2& far) {
    if (extend_widths <= 0.0f) return false;
    const f32 tl = math::length(tangent);
    if (tl < 1e-6f) return false;
    const Vec2 t = tangent / tl;
    const f32 reach = math::max(width, 1e-3f) * extend_widths;
    // Nearest world edge and its outward normal.
    const f32 dl = end.x - world.min.x, dr = world.max.x - end.x;
    const f32 db = end.y - world.min.y, dt = world.max.y - end.y;
    f32 best = dl;
    Vec2 normal{-1.0f, 0.0f};
    if (dr < best) { best = dr; normal = Vec2{1.0f, 0.0f}; }
    if (db < best) { best = db; normal = Vec2{0.0f, -1.0f}; }
    if (dt < best) { best = dt; normal = Vec2{0.0f, 1.0f}; }
    if (best > reach) return false;
    if (t.x * normal.x + t.y * normal.y < 0.35f) return false;
    const Vec2 gs = grid_bounds.size();
    const f32 len = std::sqrt(gs.x * gs.x + gs.y * gs.y) + width;
    far = end + t * len;
    return true;
}

/// Signed distance to a polygon, NEGATIVE inside, even-odd parity (matching
/// sim::detail::point_in_polygon). Two points degenerate to an unsigned
/// segment distance, matching carve_polygon.
f32 sd_polygon(Vec2 p, const std::vector<Vec2>& v) {
    const usize n = v.size();
    if (n == 0) return kInf;
    if (n == 1) return math::length(p - v[0]);
    f32 d = math::length_sq(p - v[0]);
    f32 s = 1.0f;
    for (usize i = 0, j = n - 1; i < n; j = i++) {
        const Vec2 e = v[j] - v[i];
        const Vec2 w = p - v[i];
        const f32 el = math::length_sq(e);
        const f32 tt = el > 1e-12f ? math::saturate((w.x * e.x + w.y * e.y) / el) : 0.0f;
        const Vec2 b = w - e * tt;
        d = math::min(d, math::length_sq(b));
        if (n >= 3) {
            const bool c0 = p.y >= v[i].y;
            const bool c1 = p.y < v[j].y;
            const bool c2 = e.x * w.y > e.y * w.x;
            if ((c0 && c1 && c2) || (!c0 && !c1 && !c2)) s = -s;
        }
    }
    return s * std::sqrt(d);
}

/// Carves one obstacle: d = min(d, -(distance inside the solid)).
void stamp_obstacle(Grid& g, const ObstacleDef& o, f32 band, f32 step) {
    switch (o.shape) {
        case ObstacleShape::Disc: {
            if (o.radius <= 0.0f) return;
            g.for_each_in(o.position, o.position, o.radius + band, [&](i32 x, i32 y, Vec2 p) {
                f32& d = g.at(x, y);
                d = math::min(d, math::length(p - o.position) - o.radius);
            });
            break;
        }
        case ObstacleShape::Capsule: {
            if (o.radius <= 0.0f || o.points.size() < 2) return;
            const Vec2 a = o.points[0].position, b = o.points[1].position;
            const Vec2 lo{math::min(a.x, b.x), math::min(a.y, b.y)};
            const Vec2 hi{math::max(a.x, b.x), math::max(a.y, b.y)};
            g.for_each_in(lo, hi, o.radius + band, [&](i32 x, i32 y, Vec2 p) {
                f32 t, dist;
                closest_on_segment(p, a, b, t, dist);
                f32& d = g.at(x, y);
                d = math::min(d, dist - o.radius);
            });
            break;
        }
        case ObstacleShape::Box: {
            const f32 hx = math::max(o.half_extents.x, 0.0f);
            const f32 hy = math::max(o.half_extents.y, 0.0f);
            if (hx <= 0.0f || hy <= 0.0f) return;
            const f32 cs = std::cos(o.rotation), sn = std::sin(o.rotation);
            const f32 bx = std::fabs(cs) * hx + std::fabs(sn) * hy;
            const f32 by = std::fabs(sn) * hx + std::fabs(cs) * hy;
            g.for_each_in(o.position - Vec2{bx, by}, o.position + Vec2{bx, by}, band,
                          [&](i32 x, i32 y, Vec2 p) {
                const Vec2 dd = p - o.position;
                const f32 lx = std::fabs(dd.x * cs + dd.y * sn) - hx;
                const f32 ly = std::fabs(-dd.x * sn + dd.y * cs) - hy;
                const f32 ox = math::max(lx, 0.0f), oy = math::max(ly, 0.0f);
                const f32 sd = std::sqrt(ox * ox + oy * oy) + math::min(math::max(lx, ly), 0.0f);
                f32& d = g.at(x, y);
                d = math::min(d, sd);
            });
            break;
        }
        case ObstacleShape::Polygon: {
            if (o.points.size() < 2) return;
            std::vector<Vec2> verts;
            verts.reserve(o.points.size());
            Vec2 lo = o.points[0].position, hi = lo;
            for (const VesselPoint& vp : o.points) {
                verts.push_back(vp.position);
                lo = Vec2{math::min(lo.x, vp.position.x), math::min(lo.y, vp.position.y)};
                hi = Vec2{math::max(hi.x, vp.position.x), math::max(hi.y, vp.position.y)};
            }
            const f32 grow = math::max(o.radius, 0.0f);
            g.for_each_in(lo, hi, grow + band, [&](i32 x, i32 y, Vec2 p) {
                f32& d = g.at(x, y);
                d = math::min(d, sd_polygon(p, verts) - grow);
            });
            break;
        }
        case ObstacleShape::Ridge: {
            sim::VesselSpline spline;
            for (const VesselPoint& vp : o.points) {
                spline.points.push_back(sim::VesselPoint{vp.position, vp.width, 1.0f});
            }
            std::vector<Vec2> pts;
            std::vector<f32> radii;
            sample_spline(spline, step, math::max(g.cell * 0.15f, 0.02f), pts, radii);
            if (pts.empty()) return;
            // A solid ridge is the vessel stamp with the sign flipped: build it
            // in a scratch field as a lumen, then subtract.
            Grid scratch;
            scratch.w = g.w; scratch.h = g.h; scratch.cell = g.cell; scratch.origin = g.origin;
            scratch.v.assign(g.v.size(), -kInf);
            if (pts.size() == 1) {
                stamp_segment_max(scratch, Segment{pts[0], pts[0], radii[0], radii[0]}, band);
            }
            for (usize i = 0; i + 1 < pts.size(); ++i) {
                stamp_segment_max(scratch, Segment{pts[i], pts[i + 1], radii[i], radii[i + 1]}, band);
            }
            for (usize i = 0; i < g.v.size(); ++i) {
                if (scratch.v[i] > -kInf) g.v[i] = math::min(g.v[i], -scratch.v[i]);
            }
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Sub-texel contour redistancing.
//
// A binary EDT places every boundary on a cell centre, so the distance it
// returns is wrong by up to half a cell, in a pattern that repeats along the
// wall -- and that pattern is exactly what the eye reads as a jagged edge once
// a shader draws a crisp line at d = 0. Here every cell next to the zero
// crossing first locates the crossing precisely (one Newton step along the
// gradient: p - f * grad f / |grad f|^2), and those sub-texel points are the
// SEEDS of the transform. The transform itself is Felzenszwalb & Huttenlocher's
// separable lower-envelope-of-parabolas, which does not actually need integer
// parabola vertices -- only sorted ones -- so it takes the real-valued seed
// coordinates unchanged. The result is the distance to a piecewise-linear
// contour sampled every texel, which is smooth to a small fraction of a texel
// everywhere a distance is drawn.
// ---------------------------------------------------------------------------

struct Seed {
    f32 x, y;   ///< texel coordinates (cell centre of texel i is i + 0.5)
};

/// Lower envelope of parabolas y = (q - v_i)^2 + h_i, evaluated at q = 0.5,
/// 1.5, ..., n-0.5. `items` must be sorted by v ascending. Writes the minimum
/// into out[q] and the winning item index into who[q]. Items are (v, h, id).
struct Parabola {
    f32 v, h;
    i32 id;
};

void lower_envelope(const std::vector<Parabola>& items, i32 n, std::vector<f32>& out,
                    std::vector<i32>& who, std::vector<i32>& hull, std::vector<f32>& z) {
    out.assign(static_cast<usize>(n), kInf);
    who.assign(static_cast<usize>(n), -1);
    if (items.empty()) return;
    hull.clear();
    z.clear();
    hull.push_back(0);
    z.push_back(-kInf);
    z.push_back(kInf);
    for (i32 q = 1; q < static_cast<i32>(items.size()); ++q) {
        const Parabola& pq = items[static_cast<usize>(q)];
        for (;;) {
            if (hull.empty()) {
                hull.push_back(q);
                z.assign({-kInf, kInf});
                break;
            }
            const Parabola& pk = items[static_cast<usize>(hull.back())];
            const f32 dv = pq.v - pk.v;
            if (dv < 1e-6f) {
                // Coincident vertices: keep whichever is lower.
                if (pq.h >= pk.h) break;
                hull.pop_back();
                z.pop_back();
                continue;
            }
            const f32 s = ((pq.h + pq.v * pq.v) - (pk.h + pk.v * pk.v)) / (2.0f * dv);
            if (s <= z[hull.size() - 1]) {
                hull.pop_back();
                z.pop_back();
                continue;
            }
            hull.push_back(q);
            z.back() = s;
            z.push_back(kInf);
            break;
        }
    }
    usize k = 0;
    for (i32 q = 0; q < n; ++q) {
        const f32 x = static_cast<f32>(q) + 0.5f;
        while (z[k + 1] < x) ++k;
        const Parabola& p = items[static_cast<usize>(hull[k])];
        const f32 dx = x - p.v;
        out[static_cast<usize>(q)] = dx * dx + p.h;
        who[static_cast<usize>(q)] = hull[k];
    }
}

} // namespace

void resample_to_contour(std::vector<f32>& field, i32 w, i32 h, f32 cell) {
    if (w <= 0 || h <= 0 || cell <= 0.0f) return;
    const usize n = static_cast<usize>(w) * static_cast<usize>(h);
    if (field.size() != n) return;
    const auto at = [&](i32 x, i32 y) -> f32 {
        x = math::clamp(x, 0, w - 1);
        y = math::clamp(y, 0, h - 1);
        return field[static_cast<usize>(y) * static_cast<usize>(w) + static_cast<usize>(x)];
    };
    const auto inside = [](f32 f) { return f >= 0.0f; };

    // ---- Seeds: one per cell adjacent to the zero crossing ---------------
    std::vector<std::vector<Seed>> column_seeds(static_cast<usize>(w));
    const f32 inv_cell = 1.0f / cell;
    for (i32 y = 0; y < h; ++y) {
        for (i32 x = 0; x < w; ++x) {
            const f32 f = at(x, y);
            if (!std::isfinite(f)) continue;
            const bool in = inside(f);
            const bool crossing = inside(at(x - 1, y)) != in || inside(at(x + 1, y)) != in ||
                                  inside(at(x, y - 1)) != in || inside(at(x, y + 1)) != in;
            if (!crossing) continue;
            // Gradient in texel units (field is in world units per texel of
            // `cell`), central differences.
            f32 gx = (at(x + 1, y) - at(x - 1, y)) * 0.5f;
            f32 gy = (at(x, y + 1) - at(x, y - 1)) * 0.5f;
            if (!std::isfinite(gx)) gx = 0.0f;
            if (!std::isfinite(gy)) gy = 0.0f;
            const f32 g2 = gx * gx + gy * gy;
            f32 ox = 0.0f, oy = 0.0f;
            if (g2 > 1e-12f) {
                // Offset to the crossing, in texels: -f * g / |g|^2, with |g|
                // in world units per texel.
                const f32 k = -f / g2;
                ox = k * gx;
                oy = k * gy;
                // Never trust a step that leaves the cell's neighbourhood: a
                // crease in the field has a small gradient and a huge
                // extrapolation, and the crossing is still within a texel.
                const f32 ol = std::sqrt(ox * ox + oy * oy);
                const f32 cap = 1.0f;
                if (ol > cap) { ox *= cap / ol; oy *= cap / ol; }
            }
            column_seeds[static_cast<usize>(x)].push_back(
                Seed{static_cast<f32>(x) + 0.5f + ox, static_cast<f32>(y) + 0.5f + oy});
        }
    }
    (void)inv_cell;

    // ---- Pass 1: per column, nearest seed along y ------------------------
    // best_sq[x,y] = min over the column's seeds of the full squared distance
    // from the cell centre; best_seed[x,y] = which. Seeds are attributed to
    // the column of the cell that produced them, which is within a texel of
    // their true x -- the x mismatch is folded into the parabola height.
    std::vector<f32> col_sq(n, kInf);
    std::vector<Seed> col_seed(n, Seed{0.0f, 0.0f});
    {
        std::vector<Parabola> items;
        std::vector<f32> out, z;
        std::vector<i32> who, hull;
        for (i32 x = 0; x < w; ++x) {
            const std::vector<Seed>& seeds = column_seeds[static_cast<usize>(x)];
            if (seeds.empty()) continue;
            items.clear();
            const f32 cx = static_cast<f32>(x) + 0.5f;
            for (i32 i = 0; i < static_cast<i32>(seeds.size()); ++i) {
                const Seed& s = seeds[static_cast<usize>(i)];
                const f32 dx = cx - s.x;
                items.push_back(Parabola{s.y, dx * dx, i});
            }
            std::sort(items.begin(), items.end(),
                      [](const Parabola& a, const Parabola& b) { return a.v < b.v; });
            lower_envelope(items, h, out, who, hull, z);
            for (i32 y = 0; y < h; ++y) {
                const usize i = static_cast<usize>(y) * static_cast<usize>(w) + static_cast<usize>(x);
                col_sq[i] = out[static_cast<usize>(y)];
                col_seed[i] = seeds[static_cast<usize>(items[static_cast<usize>(who[static_cast<usize>(y)])].id)];
            }
        }
    }

    // ---- Pass 2: per row, the true 2D minimum over columns ---------------
    {
        std::vector<Parabola> items;
        std::vector<f32> out, z;
        std::vector<i32> who, hull;
        for (i32 y = 0; y < h; ++y) {
            items.clear();
            const f32 cy = static_cast<f32>(y) + 0.5f;
            for (i32 x = 0; x < w; ++x) {
                const usize i = static_cast<usize>(y) * static_cast<usize>(w) + static_cast<usize>(x);
                if (!(col_sq[i] < kInf)) continue;
                const Seed& s = col_seed[i];
                const f32 dy = cy - s.y;
                items.push_back(Parabola{s.x, dy * dy, x});
            }
            if (items.empty()) {
                for (i32 x = 0; x < w; ++x) {
                    const usize i = static_cast<usize>(y) * static_cast<usize>(w) + static_cast<usize>(x);
                    field[i] = inside(field[i]) ? kInf : -kInf;
                }
                continue;
            }
            std::sort(items.begin(), items.end(),
                      [](const Parabola& a, const Parabola& b) { return a.v < b.v; });
            lower_envelope(items, w, out, who, hull, z);
            for (i32 x = 0; x < w; ++x) {
                const usize i = static_cast<usize>(y) * static_cast<usize>(w) + static_cast<usize>(x);
                const f32 d = std::sqrt(math::max(out[static_cast<usize>(x)], 0.0f)) * cell;
                field[i] = inside(field[i]) ? d : -d;
            }
        }
    }
    // Rows with no seeds at all were left at +-inf; if the whole field had no
    // contour that is the honest answer, otherwise clamp them to something a
    // texture can hold.
    f32 far = 0.0f;
    for (const f32 f : field) if (std::isfinite(f)) far = math::max(far, std::fabs(f));
    far = math::max(far, cell);
    for (f32& f : field) {
        if (!std::isfinite(f)) f = f > 0.0f ? far : -far;
    }
}

f32 RenderSdf::sample(Vec2 world) const {
    if (!valid()) return 0.0f;
    const Vec2 g = (world - bounds.min) / cell_size - Vec2{0.5f, 0.5f};
    const f32 fx = std::floor(g.x), fy = std::floor(g.y);
    const i32 x0 = static_cast<i32>(fx), y0 = static_cast<i32>(fy);
    const f32 tx = g.x - fx, ty = g.y - fy;
    const auto at = [&](i32 x, i32 y) -> f32 {
        x = math::clamp(x, 0, width - 1);
        y = math::clamp(y, 0, height - 1);
        return distance[static_cast<usize>(y) * static_cast<usize>(width) + static_cast<usize>(x)];
    };
    return math::bilerp(at(x0, y0), at(x0 + 1, y0), at(x0, y0 + 1), at(x0 + 1, y0 + 1), tx, ty);
}

RenderSdf bake_render_sdf(const LevelDef& def, const RenderSdfDesc& desc) {
    RenderSdf out;
    const Vec2 extent = def.world_bounds.size();
    if (extent.x <= 0.0f || extent.y <= 0.0f) return out;

    // ---- Grid placement --------------------------------------------------
    // The margin past the world, and never less than the sim's own grid: the
    // mask is written from this field, so every sim cell has to sample it.
    const Vec2 margin{extent.x * desc.margin_frac, extent.y * desc.margin_frac};
    const Rect sim = level_sim_bounds(def);
    Rect bounds;
    bounds.min = Vec2{math::min(def.world_bounds.min.x - margin.x, sim.min.x),
                      math::min(def.world_bounds.min.y - margin.y, sim.min.y)};
    bounds.max = Vec2{math::max(def.world_bounds.max.x + margin.x, sim.max.x),
                      math::max(def.world_bounds.max.y + margin.y, sim.max.y)};
    const Vec2 gsize = bounds.size();
    const f32 level_cell = def.cell_size > 0.0f ? def.cell_size : 0.5f;
    const f32 budget_cell = std::sqrt((gsize.x * gsize.y) / static_cast<f32>(math::max<usize>(desc.texel_budget, 1024)));
    const f32 cell = math::max(level_cell * desc.min_cell_frac, budget_cell);

    Grid g;
    g.cell = cell;
    g.origin = bounds.min;
    g.w = math::max(static_cast<i32>(std::ceil(gsize.x / cell)), 1);
    g.h = math::max(static_cast<i32>(std::ceil(gsize.y / cell)), 1);
    bounds.max = Vec2{bounds.min.x + static_cast<f32>(g.w) * cell,
                      bounds.min.y + static_cast<f32>(g.h) * cell};

    // ---- Fillet radius ---------------------------------------------------
    f32 min_width = kInf;
    for (const Vessel& v : def.vessels) {
        for (const VesselPoint& p : v.points) min_width = math::min(min_width, p.width);
    }
    const f32 round_r = (min_width < kInf && min_width > 0.0f)
                            ? math::min(min_width * desc.round_frac, desc.round_max)
                            : 0.0f;
    const f32 band = math::max(round_r, 2.0f) + 3.0f * cell;

    // Everything starts as deep wall; the stamps below only ever raise it.
    g.v.assign(static_cast<usize>(g.w) * static_cast<usize>(g.h), -band);

    // ---- Vessels ---------------------------------------------------------
    // Dense pre-sample; simplify_polyline() throws away what the curvature
    // does not need, so this can stay fine regardless of level size.
    const f32 step = math::clamp(cell, 0.25f, 1.0f);
    std::vector<Vec2> pts;
    std::vector<f32> radii;
    for (const Vessel& v : def.vessels) {
        sim::VesselSpline spline;
        spline.points.reserve(v.points.size());
        for (const VesselPoint& p : v.points) {
            spline.points.push_back(sim::VesselPoint{p.position, p.width, 1.0f});
        }
        sample_spline(spline, step, math::max(cell * 0.15f, 0.02f), pts, radii);
        if (pts.empty()) continue;
        if (pts.size() == 1) {
            stamp_segment_max(g, Segment{pts[0], pts[0], radii[0], radii[0]}, band);
            continue;
        }
        for (usize i = 0; i + 1 < pts.size(); ++i) {
            stamp_segment_max(g, Segment{pts[i], pts[i + 1], radii[i], radii[i + 1]}, band);
        }
        // Run-off at either end that leaves the level.
        Vec2 far;
        const usize last = pts.size() - 1;
        if (run_off_point(pts[0], pts[0] - pts[1], radii[0] * 2.0f, def.world_bounds, bounds,
                          desc.edge_extend_widths, far)) {
            stamp_segment_max(g, Segment{pts[0], far, radii[0], radii[0]}, band);
        }
        if (run_off_point(pts[last], pts[last] - pts[last - 1], radii[last] * 2.0f,
                          def.world_bounds, bounds, desc.edge_extend_widths, far)) {
            stamp_segment_max(g, Segment{pts[last], far, radii[last], radii[last]}, band);
        }
    }

    // ---- Obstacles -------------------------------------------------------
    for (const ObstacleDef& o : def.obstacles) stamp_obstacle(g, o, band, step);

    // ---- Bounded closing -------------------------------------------------
    // d0: the exact field. d1: distance field of the lumen dilated by R.
    // d2: that eroded back by R -- the classical closing, which fillets every
    // convex wall corner with radius R but would also swallow any wall thinner
    // than 2R. Two bounds keep it honest:
    //
    //   DEPTH. The final lumen is the original plus only those closed-in cells
    //   that were within `depth` of the original lumen. A right-angle fillet
    //   reaches 0.41 R deep, so 0.5 R lets every ordinary corner round fully
    //   while a sharp wedge is merely blunted.
    //
    //   THIN STRUCTURES. Closing is the complement of OPENING the wall, so it
    //   removes every part of the wall that no R-disc fits inside -- which is
    //   not only corner fillets but the whole of any structure thinner than
    //   2R: a septum between two passes of a switchback, a capsule bar across
    //   a chamber, a plaque island. The depth bound alone does not save those;
    //   it just leaves their cores, shaved by `depth` on every side. So thin
    //   structures are found and protected outright, on the wall's medial
    //   axis: a cell at least as deep as all eight neighbours is a ridge
    //   point, and a ridge point shallower than R is the spine of a structure
    //   no R-disc fits in. Every cell within that ridge point's own depth of
    //   it (the disc it is the centre of) is protected, which is exactly the
    //   structure's own footprint. A corner's bisector is not a ridge -- depth
    //   keeps increasing away from the tip -- so fillets are unaffected,
    //   including at the root of a protected septum, where the wall outside
    //   the septum still rounds.
    std::vector<f32> d0 = g.v;
    if (round_r > 0.0f) {
        const f32 depth = round_r * desc.round_depth_frac;

        std::vector<u8> protect(d0.size(), 0);
        {
            const auto at = [&](i32 x, i32 y) { return d0[static_cast<usize>(y) * static_cast<usize>(g.w) + static_cast<usize>(x)]; };
            // Generous tie tolerance: a ridge that runs diagonally through
            // the grid only passes through the odd cell centre, and the
            // protection discs of a thin structure have to overlap along it.
            const f32 eps = cell * 0.35f;
            for (i32 y = 1; y + 1 < g.h; ++y) {
                for (i32 x = 1; x + 1 < g.w; ++x) {
                    const f32 t = -at(x, y);   // wall depth
                    if (t < cell * 0.75f || t >= round_r) continue;
                    bool ridge = true;
                    for (i32 j = -1; j <= 1 && ridge; ++j) {
                        for (i32 i = -1; i <= 1; ++i) {
                            if (i == 0 && j == 0) continue;
                            if (-at(x + i, y + j) > t + eps) { ridge = false; break; }
                        }
                    }
                    if (!ridge) continue;
                    const i32 rad = static_cast<i32>(std::ceil(t / cell)) + 1;
                    const f32 r2 = (t + cell) * (t + cell);
                    for (i32 j = -rad; j <= rad; ++j) {
                        for (i32 i = -rad; i <= rad; ++i) {
                            const i32 px = x + i, py = y + j;
                            if (px < 0 || py < 0 || px >= g.w || py >= g.h) continue;
                            const f32 dx = static_cast<f32>(i) * cell, dy = static_cast<f32>(j) * cell;
                            if (dx * dx + dy * dy > r2) continue;
                            protect[static_cast<usize>(py) * static_cast<usize>(g.w) + static_cast<usize>(px)] = 1;
                        }
                    }
                }
            }
        }

        std::vector<f32> d1 = d0;
        for (f32& f : d1) f += round_r;
        resample_to_contour(d1, g.w, g.h, cell);
        std::vector<f32>& d2 = d1;
        for (f32& f : d2) f -= round_r;
        resample_to_contour(d2, g.w, g.h, cell);
        for (usize i = 0; i < d0.size(); ++i) {
            if (protect[i] != 0) continue;
            d0[i] = math::max(d0[i], math::min(d2[i], d0[i] + depth));
        }
    }
    resample_to_contour(d0, g.w, g.h, cell);

    out.width = g.w;
    out.height = g.h;
    out.cell_size = cell;
    out.bounds = bounds;
    out.round_radius = round_r;
    out.distance = std::move(d0);
    return out;
}

} // namespace immune::game
