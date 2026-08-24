// sim/flowfield/FlowField.cpp — Wave 1A implementation of the three-stage
// tissue -> SDF -> flow-field pipeline declared in FlowField.h.
//
// Everything here is flat-array, index-based, and free of virtual dispatch
// (DESIGN.md §8.2). The sweep is an explicit binary heap over u32 cell indices;
// there is no node allocation per cell and no pointer chasing.
//
// DETERMINISM. No RNG, no wall-clock reads feed any value that ends up in the
// field. The one wall-clock use is RebakeStats::last_bake_ms / pump_rebake's
// budget, which the frozen header mandates and which never influences the
// baked result: pump_rebake only decides *how many* whole regions to process
// this call, and every region is solved identically regardless of when it runs.
// Heap ties break on cell index, so the pop order is fixed across machines and
// the result does not depend on floating-point comparison order.
#include "sim/flowfield/FlowField.h"

#include "core/Clock.h"
#include "core/Math.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace immune::sim {
namespace {

constexpr f32 kInf = std::numeric_limits<f32>::infinity();

/// Fixed neighbour order: 4 orthogonal then 4 diagonal. Frozen for determinism.
constexpr i32 kOffX[8] = {-1, 1, 0, 0, -1, 1, -1, 1};
constexpr i32 kOffY[8] = {0, 0, -1, 1, -1, -1, 1, 1};

/// Weight a diagonal carries in the direction-smoothing kernel, relative to an
/// orthogonal one. It is further away, so it counts for less -- with a flat
/// square kernel the smoothed field grows its own faint 45-degree bias, which
/// is exactly the artifact the eikonal solve was adopted to remove.
constexpr f32 kDiagonalWeight = 0.70710678f;

/// Tolerance used when deciding whether a neighbour could have been a cell's
/// shortest-path parent. Costs are O(grid diagonal * cell size); 1e-3 world
/// units is far below one cell yet comfortably above f32 sweep noise.
constexpr f32 kSupportEps = 1e-3f;

/// A smoothed direction must keep at least this much of the true descent
/// direction; below it the cell reverts to its raw gradient.
///
/// This is the guarantee that smoothing cannot trap an agent: every published
/// vector still has a strictly positive component down the cost gradient, so
/// following the field always decreases cost-to-goal and no local minimum can
/// be introduced. 0.25 is inside a 75-degree cone -- loose enough for a crease
/// to round off over several cells, tight enough that nothing ever ends up
/// travelling along an iso-cost line.
constexpr f32 kMinDescentAlign = 0.25f;

/// Relative weight a cell's own direction keeps in the smoothing kernel. Above
/// 1 so a single pass is a gentle nudge and the radius (iteration count), not
/// the kernel shape, controls how far smoothing reaches.
constexpr f32 kSmoothCenterWeight = 2.0f;

/// Cap on smoothing iterations, so a level authored with an absurd radius (or a
/// very small cell size) cannot turn a bake into a multi-second stall.
constexpr i32 kMaxSmoothIterations = 64;

/// Floor on a cell's slowness multiplier. A zero or negative authored cost
/// would make the eikonal update degenerate (everything reachable at cost 0).
constexpr f32 kMinSlowness = 1e-3f;

} // namespace

// ===========================================================================
// TissueMask
// ===========================================================================

void TissueMask::resize(i32 width, i32 height, f32 cell_size, Vec2 world_origin) {
    width_ = math::max(0, width);
    height_ = math::max(0, height);
    cell_size_ = cell_size > 0.0f ? cell_size : 1.0f;
    origin_ = world_origin;
    const usize n = static_cast<usize>(width_) * static_cast<usize>(height_);
    walkable_.assign(n, 0u);
    cost_.assign(n, 1.0f);
}

Rect TissueMask::world_bounds() const {
    return Rect{origin_, origin_ + Vec2{static_cast<f32>(width_) * cell_size_,
                                        static_cast<f32>(height_) * cell_size_}};
}

Vec2 TissueMask::cell_to_world(i32 x, i32 y) const {
    return origin_ + Vec2{(static_cast<f32>(x) + 0.5f) * cell_size_,
                          (static_cast<f32>(y) + 0.5f) * cell_size_};
}

IVec2 TissueMask::world_to_cell(Vec2 p) const {
    const Vec2 local = (p - origin_) / cell_size_;
    return IVec2{static_cast<i32>(std::floor(local.x)), static_cast<i32>(std::floor(local.y))};
}

// ===========================================================================
// DistanceField — 8SSEDT (Danielsson two-pass vector propagation)
// ===========================================================================
//
// A naive "scan every wall from every cell" transform is O(n^2) and unusable at
// 256x256+. 8SSEDT propagates, per cell, the *offset vector* to the nearest seed
// in two raster sweeps (down-right, then up-left) with an 8-neighbour stencil.
// It is O(n), exact for the overwhelming majority of cells and off by at most a
// fraction of a cell in rare concave configurations — well inside what
// vessel-hugging steering and placement clearance care about.
//
// The grid is padded by one cell of solid wall on every side, so the field is
// correct at the mask border without a special case in the inner loop.

namespace {

struct EdtGrid {
    i32 w = 0, h = 0;
    std::vector<i16> dx, dy;

    void init(i32 width, i32 height) {
        w = width;
        h = height;
        const usize n = static_cast<usize>(w) * static_cast<usize>(h);
        dx.assign(n, 0);
        dy.assign(n, 0);
    }
    usize idx(i32 x, i32 y) const { return static_cast<usize>(y) * static_cast<usize>(w) + static_cast<usize>(x); }
    f32 dist_sq(usize i) const {
        const f32 fx = static_cast<f32>(dx[i]);
        const f32 fy = static_cast<f32>(dy[i]);
        return fx * fx + fy * fy;
    }
};

/// Far marker: large enough to lose every comparison, small enough that
/// squaring it stays comfortably inside f32.
constexpr i16 kEdtFar = 8192;

inline void edt_compare(EdtGrid& g, usize i, i32 x, i32 y, i32 ox, i32 oy) {
    const i32 nx = x + ox;
    const i32 ny = y + oy;
    if (nx < 0 || ny < 0 || nx >= g.w || ny >= g.h) return;
    const usize j = g.idx(nx, ny);
    const i32 cdx = static_cast<i32>(g.dx[j]) - ox;
    const i32 cdy = static_cast<i32>(g.dy[j]) - oy;
    const f32 cand = static_cast<f32>(cdx) * static_cast<f32>(cdx) +
                     static_cast<f32>(cdy) * static_cast<f32>(cdy);
    if (cand < g.dist_sq(i)) {
        constexpr i32 kFar32 = static_cast<i32>(kEdtFar);
        g.dx[i] = static_cast<i16>(math::clamp(cdx, -kFar32, kFar32));
        g.dy[i] = static_cast<i16>(math::clamp(cdy, -kFar32, kFar32));
    }
}

void edt_propagate(EdtGrid& g) {
    for (i32 y = 0; y < g.h; ++y) {
        for (i32 x = 0; x < g.w; ++x) {
            const usize i = g.idx(x, y);
            edt_compare(g, i, x, y, -1, 0);
            edt_compare(g, i, x, y, 0, -1);
            edt_compare(g, i, x, y, -1, -1);
            edt_compare(g, i, x, y, 1, -1);
        }
        for (i32 x = g.w - 1; x >= 0; --x) {
            const usize i = g.idx(x, y);
            edt_compare(g, i, x, y, 1, 0);
        }
    }
    for (i32 y = g.h - 1; y >= 0; --y) {
        for (i32 x = g.w - 1; x >= 0; --x) {
            const usize i = g.idx(x, y);
            edt_compare(g, i, x, y, 1, 0);
            edt_compare(g, i, x, y, 0, 1);
            edt_compare(g, i, x, y, -1, 1);
            edt_compare(g, i, x, y, 1, 1);
        }
        for (i32 x = 0; x < g.w; ++x) {
            const usize i = g.idx(x, y);
            edt_compare(g, i, x, y, -1, 0);
        }
    }
}

} // namespace

void DistanceField::bake(const TissueMask& mask) {
    width_ = mask.width();
    height_ = mask.height();
    cell_size_ = mask.cell_size();
    origin_ = mask.world_origin();
    const usize n = static_cast<usize>(width_) * static_cast<usize>(height_);
    distance_.assign(n, 0.0f);
    if (n == 0) return;

    // Padded grids: index (x+1, y+1) in EDT space is mask cell (x, y).
    const i32 pw = width_ + 2;
    const i32 ph = height_ + 2;
    EdtGrid inside;   // seeds = walls   -> distance from a cell to nearest wall
    EdtGrid outside;  // seeds = tissue  -> distance from a wall to nearest tissue
    inside.init(pw, ph);
    outside.init(pw, ph);

    for (i32 y = 0; y < ph; ++y) {
        for (i32 x = 0; x < pw; ++x) {
            const usize i = inside.idx(x, y);
            const bool walk = mask.walkable(x - 1, y - 1); // out-of-range -> false (padding)
            if (walk) {
                inside.dx[i] = kEdtFar;
                inside.dy[i] = kEdtFar;
                outside.dx[i] = 0;
                outside.dy[i] = 0;
            } else {
                inside.dx[i] = 0;
                inside.dy[i] = 0;
                outside.dx[i] = kEdtFar;
                outside.dy[i] = kEdtFar;
            }
        }
    }
    edt_propagate(inside);
    edt_propagate(outside);

    // Convention: distance is measured to the *face* of the nearest opposite
    // cell, so a tissue cell touching a wall has clearance 0.5 * cell_size and
    // the zero crossing sits on the wall surface, not on the wall's center.
    for (i32 y = 0; y < height_; ++y) {
        for (i32 x = 0; x < width_; ++x) {
            const usize pi = inside.idx(x + 1, y + 1);
            const usize mi = static_cast<usize>(y) * static_cast<usize>(width_) + static_cast<usize>(x);
            if (mask.walkable(x, y)) {
                distance_[mi] = (std::sqrt(inside.dist_sq(pi)) - 0.5f) * cell_size_;
            } else {
                distance_[mi] = -(std::sqrt(outside.dist_sq(pi)) - 0.5f) * cell_size_;
            }
        }
    }
}

f32 DistanceField::sample(Vec2 world_pos) const {
    if (width_ <= 0 || height_ <= 0) return 0.0f;
    const Vec2 g = (world_pos - origin_) / cell_size_ - Vec2{0.5f, 0.5f};
    const f32 fx = std::floor(g.x);
    const f32 fy = std::floor(g.y);
    const i32 x0 = static_cast<i32>(fx);
    const i32 y0 = static_cast<i32>(fy);
    const f32 tx = g.x - fx;
    const f32 ty = g.y - fy;
    auto at = [&](i32 x, i32 y) -> f32 {
        x = math::clamp(x, 0, width_ - 1);
        y = math::clamp(y, 0, height_ - 1);
        return distance_[static_cast<usize>(y) * static_cast<usize>(width_) + static_cast<usize>(x)];
    };
    return math::bilerp(at(x0, y0), at(x0 + 1, y0), at(x0, y0 + 1), at(x0 + 1, y0 + 1), tx, ty);
}

Vec2 DistanceField::gradient(Vec2 world_pos) const {
    if (width_ <= 0 || height_ <= 0) return Vec2{0.0f, 0.0f};
    // Central difference on the bilinear reconstruction, one cell apart.
    const f32 h = cell_size_;
    const f32 gx = sample(world_pos + Vec2{h, 0.0f}) - sample(world_pos - Vec2{h, 0.0f});
    const f32 gy = sample(world_pos + Vec2{0.0f, h}) - sample(world_pos - Vec2{0.0f, h});
    return math::normalize_safe(Vec2{gx, gy});
}

// ===========================================================================
// FlowField
// ===========================================================================

// ---- internal helpers ------------------------------------------------------

void FlowField::heap_push(u32 cell, f32 key) {
    heap_.push_back(HeapNode{key, cell});
    usize i = heap_.size() - 1;
    while (i > 0) {
        const usize parent = (i - 1) / 2;
        if (heap_less(heap_[i], heap_[parent])) {
            std::swap(heap_[i], heap_[parent]);
            i = parent;
        } else {
            break;
        }
    }
}

FlowField::HeapNode FlowField::heap_pop() {
    const HeapNode top = heap_.front();
    heap_.front() = heap_.back();
    heap_.pop_back();
    const usize n = heap_.size();
    usize i = 0;
    for (;;) {
        const usize l = 2 * i + 1;
        const usize r = l + 1;
        usize best = i;
        if (l < n && heap_less(heap_[l], heap_[best])) best = l;
        if (r < n && heap_less(heap_[r], heap_[best])) best = r;
        if (best == i) break;
        std::swap(heap_[i], heap_[best]);
        i = best;
    }
    return top;
}

bool FlowField::heap_less(const HeapNode& a, const HeapNode& b) {
    if (a.key != b.key) return a.key < b.key;
    return a.cell < b.cell; // deterministic tie-break
}

/// Godunov upwind eikonal update -- the metric this field is solved under.
///
/// WHY NOT A GRAPH. The previous solver relaxed eight graph edges of length 1
/// and sqrt(2). That is exact for the graph and wrong for the plane: octile
/// distance carries ~8% directional error, so its iso-cost contours are
/// octagons and the gradient of the result snaps toward the eight step
/// directions. Agents inherited those bands as visible 45-degree kinks and the
/// debug arrows drew them as a herringbone. Solving |grad T| = f instead gives
/// a field whose contours are round to within a fraction of a cell, so its
/// gradient is a genuine continuous direction rather than a quantized one.
///
/// The update never consumes a neighbour whose cost exceeds the value it
/// returns: the two-sided branch is only taken while the two upwind values are
/// within one cell-cost of each other, and the one-sided branch is capped by
/// construction. That is what keeps it monotone, which is in turn what lets a
/// plain Dijkstra-ordered heap drain it correctly and finally -- a popped cell
/// is final, exactly as before.
///
/// `exclude_dirty` restricts the stencil to cells an incremental rebake still
/// trusts; everything else reads the live field.
f32 FlowField::eikonal(const TissueMask& mask, i32 x, i32 y, bool exclude_dirty) const {
    const auto side = [&](i32 nx, i32 ny) -> f32 {
        if (!mask.walkable(nx, ny)) return kInf;
        const usize ni = mask.index(nx, ny);
        if (exclude_dirty && dirty_mark_[ni] == dirty_gen_) return kInf;
        return cost_[ni];
    };

    f32 a = math::min(side(x - 1, y), side(x + 1, y));
    f32 b = math::min(side(x, y - 1), side(x, y + 1));
    if (a > b) std::swap(a, b);
    if (!std::isfinite(a)) return kInf;

    const f32 f = cell_size_ * math::max(mask.cost(x, y), kMinSlowness);
    // One-sided: the cross-axis wavefront is too far behind to contribute, or
    // the caller asked for the axis-aligned (Manhattan) metric.
    if (!desc_.allow_diagonals || !std::isfinite(b) || b - a >= f) return a + f;
    const f32 diff = b - a;
    return 0.5f * (a + b + std::sqrt(math::max(2.0f * f * f - diff * diff, 0.0f)));
}

/// Stamps the per-cell goal flag from `goal_cells` plus `goal_radius`.
void FlowField::mark_goals(const TissueMask& mask) {
    const usize n = static_cast<usize>(width_) * static_cast<usize>(height_);
    goal_mark_.assign(n, 0u);
    const f32 r = math::max(desc_.goal_radius, 0.0f);
    const i32 span = static_cast<i32>(std::floor(r / math::max(cell_size_, 1e-6f)));
    const f32 r2 = r * r;
    for (const IVec2& g : desc_.goal_cells) {
        for (i32 dy = -span; dy <= span; ++dy) {
            for (i32 dx = -span; dx <= span; ++dx) {
                const i32 x = g.x + dx;
                const i32 y = g.y + dy;
                if (!mask.walkable(x, y)) continue;
                // The centre cell is a goal even when goal_radius is 0.
                if (dx != 0 || dy != 0) {
                    const Vec2 d = mask.cell_to_world(x, y) - mask.cell_to_world(g.x, g.y);
                    if (math::length_sq(d) > r2) continue;
                }
                goal_mark_[mask.index(x, y)] = 1u;
            }
        }
    }
}

i32 FlowField::smoothing_iterations() const {
    if (desc_.smoothing_radius <= 0.0f || cell_size_ <= 0.0f) return 0;
    return math::clamp(static_cast<i32>(desc_.smoothing_radius / cell_size_ + 0.5f), 0,
                       kMaxSmoothIterations);
}

/// Diagonal steps may not squeeze between two blocking cells.
bool FlowField::diagonal_ok(const TissueMask& mask, i32 x, i32 y, i32 dir) {
    if (dir < 4) return true;
    return mask.walkable(x + kOffX[dir], y) && mask.walkable(x, y + kOffY[dir]);
}

/// Drains the heap. When `restricted`, only cells flagged in `active_` may be
/// written; that is what confines an incremental rebake to its dirty set.
/// Grows [touch_min_, touch_max_] to cover every cell whose cost changed.
void FlowField::sweep(const TissueMask& mask, bool restricted) {
    while (!heap_.empty()) {
        const HeapNode node = heap_pop();
        if (node.key > cost_[node.cell]) continue; // stale entry
        ++stats_.cells_visited;

        const i32 x = static_cast<i32>(node.cell % static_cast<u32>(width_));
        const i32 y = static_cast<i32>(node.cell / static_cast<u32>(width_));

        // Propagation is 4-connected: the eikonal update reads the whole
        // orthogonal stencil at once, so a diagonal neighbour is reached
        // through the two cells between it and here rather than over a
        // sqrt(2) edge. That is what removes the octile banding.
        for (i32 d = 0; d < 4; ++d) {
            const i32 nx = x + kOffX[d];
            const i32 ny = y + kOffY[d];
            if (!mask.walkable(nx, ny)) continue;
            const u32 ni = static_cast<u32>(mask.index(nx, ny));
            if (restricted && !in_dirty(ni)) continue;
            const f32 nc = eikonal(mask, nx, ny, /*exclude_dirty=*/false);
            if (nc < cost_[ni]) {
                cost_[ni] = nc;
                touch(nx, ny);
                heap_push(ni, nc);
            }
        }
    }
}

/// Negative normalized gradient of the cost field over a cell rect, written to
/// `raw_direction_` (and mirrored into the published field, which a later
/// smoothing pass may then overwrite).
///
/// WALL HANDLING IS NEUMANN. A neighbour that is a wall or unreachable
/// contributes this cell's own cost rather than being dropped from the stencil.
/// Reflecting instead of dropping zeroes the wall-normal component of the
/// gradient at the boundary, so the field runs *along* a vessel wall instead of
/// being biased away from it -- which is most of what makes a lane read as a
/// channel the horde flows down rather than a corridor it bounces inside.
void FlowField::compute_directions(const TissueMask& mask, i32 x0, i32 y0, i32 x1, i32 y1) {
    x0 = math::max(x0, 0);
    y0 = math::max(y0, 0);
    x1 = math::min(x1, width_ - 1);
    y1 = math::min(y1, height_ - 1);
    const i32 dir_count = desc_.allow_diagonals ? 8 : 4;
    // With smoothing on, `direction_` is the smoothed field and only
    // smooth_directions() may write it -- except where the raw direction moved,
    // which is exactly the set smooth_directions() is about to revisit anyway.
    // Writing raw everywhere would leave every unchanged cell in the rebake
    // halo holding an unsmoothed vector: a visible seam around each tower.
    const bool publish_raw = smoothing_iterations() <= 0;

    // Records that a cell's raw direction moved, so smoothing knows how far it
    // has to reach. Returns whether the published field may take the raw value.
    const auto note = [&](usize i, i32 x, i32 y, Vec2 dir) {
        const Vec2 prev = raw_direction_[i];
        const bool changed = prev.x != dir.x || prev.y != dir.y;
        if (changed) {
            dir_touch_min_.x = math::min(dir_touch_min_.x, x);
            dir_touch_min_.y = math::min(dir_touch_min_.y, y);
            dir_touch_max_.x = math::max(dir_touch_max_.x, x);
            dir_touch_max_.y = math::max(dir_touch_max_.y, y);
        }
        raw_direction_[i] = dir;
        if (changed || publish_raw) direction_[i] = dir;
    };

    for (i32 y = y0; y <= y1; ++y) {
        for (i32 x = x0; x <= x1; ++x) {
            const usize i = mask.index(x, y);
            const f32 c = cost_[i];
            if (!mask.walkable(x, y) || !std::isfinite(c) || c <= 0.0f) {
                note(i, x, y, Vec2{0.0f, 0.0f});
                continue;
            }

            const auto neighbour = [&](i32 nx, i32 ny) -> f32 {
                if (!mask.walkable(nx, ny)) return c;
                const f32 nc = cost_[mask.index(nx, ny)];
                return std::isfinite(nc) ? nc : c;
            };
            const f32 gx = 0.5f * (neighbour(x + 1, y) - neighbour(x - 1, y));
            const f32 gy = 0.5f * (neighbour(x, y + 1) - neighbour(x, y - 1));
            Vec2 dir = math::normalize_safe(Vec2{-gx, -gy});

            if (dir.x == 0.0f && dir.y == 0.0f) {
                // Degenerate gradient (plateau, or a one-cell notch). Fall back
                // to the steepest-descent neighbour, which always exists for a
                // reachable non-goal cell.
                f32 best = c;
                i32 best_d = -1;
                for (i32 d = 0; d < dir_count; ++d) {
                    const i32 nx = x + kOffX[d];
                    const i32 ny = y + kOffY[d];
                    if (!mask.walkable(nx, ny)) continue;
                    if (!diagonal_ok(mask, x, y, d)) continue;
                    const f32 nc = cost_[mask.index(nx, ny)];
                    if (nc < best) {
                        best = nc;
                        best_d = d;
                    }
                }
                if (best_d >= 0) {
                    dir = math::normalize_safe(Vec2{static_cast<f32>(kOffX[best_d]),
                                                    static_cast<f32>(kOffY[best_d])});
                }
            }
            note(i, x, y, dir);
        }
    }
}

/// Iterated neighbourhood averaging of the unit direction field. `direction_`
/// is rewritten over [x0,y0]..[x1,y1]; see FlowFieldBakeDesc::smoothing_radius
/// for why this exists at all.
///
/// EXACTNESS UNDER INCREMENTAL REBAKE. A smoothed vector is a pure function of
/// the *raw* directions within N cells of it (N = iteration count), so it can
/// always be rebuilt from scratch rather than iterated further -- which is what
/// stops a cell that has been rebaked twice from drifting away from the same
/// cell in a from-scratch bake. To get that, the pass runs over the requested
/// rect expanded by N and publishes only the requested rect: values within N of
/// the expanded rect's edge are contaminated by reads from outside it, and
/// those are precisely the ones thrown away.
void FlowField::smooth_directions(const TissueMask& mask, i32 x0, i32 y0, i32 x1, i32 y1) {
    const i32 iterations = smoothing_iterations();
    if (iterations <= 0) return;

    x0 = math::max(x0, 0);
    y0 = math::max(y0, 0);
    x1 = math::min(x1, width_ - 1);
    y1 = math::min(y1, height_ - 1);
    if (x1 < x0 || y1 < y0) return;

    // Working rect: the published rect plus the smoothing reach.
    const i32 ex0 = math::max(x0 - iterations, 0);
    const i32 ey0 = math::max(y0 - iterations, 0);
    const i32 ex1 = math::min(x1 + iterations, width_ - 1);
    const i32 ey1 = math::min(y1 + iterations, height_ - 1);

    // Sized to the grid for a flat index, but only the working rect is ever
    // touched: a rebake pays for its own region, not for a memset of the level.
    // (assign() here cost more than the smoothing itself on a real level.)
    const usize n = direction_.size();
    if (smooth_front_.size() != n) {
        smooth_front_.resize(n);
        smooth_back_.resize(n);
    }
    for (i32 y = ey0; y <= ey1; ++y) {
        for (i32 x = ex0; x <= ex1; ++x) {
            const usize i = mask.index(x, y);
            smooth_front_[i] = raw_direction_[i];
        }
    }

    for (i32 it = 0; it < iterations; ++it) {
        for (i32 y = ey0; y <= ey1; ++y) {
            for (i32 x = ex0; x <= ex1; ++x) {
                const usize i = mask.index(x, y);
                const Vec2 raw = raw_direction_[i];
                if (raw.x == 0.0f && raw.y == 0.0f) {
                    smooth_back_[i] = Vec2{0.0f, 0.0f};
                    continue;
                }

                Vec2 acc = smooth_front_[i] * kSmoothCenterWeight;
                for (i32 d = 0; d < 8; ++d) {
                    const i32 nx = x + kOffX[d];
                    const i32 ny = y + kOffY[d];
                    if (!mask.walkable(nx, ny)) continue;
                    const usize ni = mask.index(nx, ny);
                    // Outside the working rect there is no iterated value to
                    // read, so the raw one stands in. It only perturbs the
                    // outer ring, which is discarded rather than published.
                    const bool inside =
                        nx >= ex0 && nx <= ex1 && ny >= ey0 && ny <= ey1;
                    const Vec2 nd = inside ? smooth_front_[ni] : raw_direction_[ni];
                    if (nd.x == 0.0f && nd.y == 0.0f) continue;
                    acc += nd * (d < 4 ? 1.0f : kDiagonalWeight);
                }

                const Vec2 sm = math::normalize_safe(acc);
                // The descent guarantee. A cell whose neighbourhood disagrees
                // with it this badly sits on a genuine watershed rather than on
                // a seam worth rounding off, and it keeps the exact gradient.
                const f32 align = sm.x * raw.x + sm.y * raw.y;
                smooth_back_[i] = align >= kMinDescentAlign ? sm : raw;
            }
        }
        smooth_front_.swap(smooth_back_);
    }

    for (i32 y = y0; y <= y1; ++y) {
        for (i32 x = x0; x <= x1; ++x) {
            const usize i = mask.index(x, y);
            direction_[i] = smooth_front_[i];
        }
    }
}

// ---- full bake -------------------------------------------------------------

void FlowField::bake(const TissueMask& mask, const FlowFieldBakeDesc& desc) {
    WallClock clock;

    desc_ = desc;
    width_ = mask.width();
    height_ = mask.height();
    cell_size_ = mask.cell_size();
    origin_ = mask.world_origin();
    const usize n = static_cast<usize>(width_) * static_cast<usize>(height_);
    cost_.assign(n, kInf);
    direction_.assign(n, Vec2{0.0f, 0.0f});
    raw_direction_.assign(n, Vec2{0.0f, 0.0f});
    smooth_front_.clear();
    smooth_back_.clear();
    smooth_front_.shrink_to_fit();
    smooth_back_.shrink_to_fit();
    goal_mark_.clear();
    dirty_.clear();
    dirty_mark_.assign(n, 0u);
    dirty_gen_ = 0u;
    scan_.clear();
    heap_.clear();
    stats_ = RebakeStats{};
    stats_.full_bake = true;
    if (n == 0) return;

    touch_min_ = IVec2{0, 0};
    touch_max_ = IVec2{width_ - 1, height_ - 1};
    dir_touch_min_ = IVec2{0, 0};
    dir_touch_max_ = IVec2{width_ - 1, height_ - 1};

    // Every cell inside goal_radius is a sink, not just the authored centre.
    mark_goals(mask);
    for (usize i = 0; i < n; ++i) {
        if (goal_mark_[i] == 0u) continue;
        cost_[i] = 0.0f;
        heap_push(static_cast<u32>(i), 0.0f);
    }

    sweep(mask, /*restricted=*/false);
    compute_directions(mask, 0, 0, width_ - 1, height_ - 1);
    smooth_directions(mask, 0, 0, width_ - 1, height_ - 1);

    stats_.regions_processed = 1;
    stats_.last_bake_ms = clock.elapsed_ms();
}

// ---- incremental rebake ----------------------------------------------------

void FlowField::mark_dirty(const Rect& region) { dirty_.push_back(region); }

void FlowField::touch(i32 x, i32 y) {
    touch_min_.x = math::min(touch_min_.x, x);
    touch_min_.y = math::min(touch_min_.y, y);
    touch_max_.x = math::max(touch_max_.x, x);
    touch_max_.y = math::max(touch_max_.y, y);
}

/// Re-solves one dirty rectangle.
///
/// Step 1 (invalidate) is what makes this *equal* to a full bake rather than
/// merely close to it. Resetting only the dirty rect is not enough: a cell
/// outside the rect whose shortest path ran through the rect now holds a stale
/// cost. So invalidation floods outward from the rect, taking any cell whose
/// every possible shortest-path parent is itself invalid. A cell that keeps one
/// valid parent keeps its (still correct) cost and becomes a seed. Because a
/// path that left the invalid set and re-entered it would have to pass through
/// a cell that this rule invalidates, the seeded sweep reproduces the full-bake
/// answer exactly.
///
/// Step 3 handles the opposite direction: an *unblocking* edit (tower sold) can
/// lower costs outside the invalid set, which no amount of invalidation would
/// have caught. Relaxing outward from the boundary and running an unrestricted
/// sweep costs work proportional to the area that genuinely changed.
void FlowField::rebake_region(const TissueMask& mask, const Rect& world_region) {
    if (width_ <= 0 || height_ <= 0) return;

    const i32 margin = math::max(0, desc_.rebake_margin_cells);
    const IVec2 lo_c = mask.world_to_cell(world_region.min);
    const IVec2 hi_c = mask.world_to_cell(world_region.max);
    const i32 x0 = math::clamp(lo_c.x - margin, 0, width_ - 1);
    const i32 y0 = math::clamp(lo_c.y - margin, 0, height_ - 1);
    const i32 x1 = math::clamp(hi_c.x + margin, 0, width_ - 1);
    const i32 y1 = math::clamp(hi_c.y + margin, 0, height_ - 1);
    if (x1 < x0 || y1 < y0) return;

    ++dirty_gen_;
    if (dirty_gen_ == 0u) { // wrapped: reset the stamp array
        std::fill(dirty_mark_.begin(), dirty_mark_.end(), 0u);
        dirty_gen_ = 1u;
    }

    touch_min_ = IVec2{x0, y0};
    touch_max_ = IVec2{x1, y1};
    // Empty until compute_directions() below finds a direction that moved.
    dir_touch_min_ = IVec2{width_, height_};
    dir_touch_max_ = IVec2{-1, -1};

    // --- 1a. seed the invalid set with the margin-expanded rect --------------
    scan_.clear();
    for (i32 y = y0; y <= y1; ++y) {
        for (i32 x = x0; x <= x1; ++x) {
            const u32 i = static_cast<u32>(mask.index(x, y));
            dirty_mark_[i] = dirty_gen_;
            scan_.push_back(i);
        }
    }

    // --- 1b. flood invalidation outward using the *old* costs ---------------
    // 4-connected, because that is the shape of an eikonal dependency: a cell's
    // cost is a function of its four orthogonal neighbours and nothing else, so
    // the set of cells that can lose support is exactly the 4-connected closure.
    // Walking diagonals as well only re-tests cells the orthogonal walk already
    // reaches, at four extra support solves apiece.
    for (usize head = 0; head < scan_.size(); ++head) {
        const u32 ci = scan_[head];
        const i32 cx = static_cast<i32>(ci % static_cast<u32>(width_));
        const i32 cy = static_cast<i32>(ci / static_cast<u32>(width_));
        for (i32 d = 0; d < 4; ++d) {
            const i32 nx = cx + kOffX[d];
            const i32 ny = cy + kOffY[d];
            if (!mask.walkable(nx, ny)) continue;
            const u32 ni = static_cast<u32>(mask.index(nx, ny));
            if (dirty_mark_[ni] == dirty_gen_) continue;
            if (is_goal_cell(nx, ny)) continue; // goals are self-supporting

            // Is `ni`'s cost still reproducible from cells we still trust?
            // Asking the solver itself, rather than re-deriving the rule here,
            // is what keeps invalidation and the sweep in agreement about what
            // a correct cost is -- and it is the whole rule under the eikonal
            // metric, where a value comes from an upwind *pair*, not one edge.
            const bool supported =
                eikonal(mask, nx, ny, /*exclude_dirty=*/true) <= cost_[ni] + kSupportEps;
            if (!supported) {
                dirty_mark_[ni] = dirty_gen_;
                scan_.push_back(ni);
                touch(nx, ny);
            }
        }
    }

    // --- 2. reset the invalid set, then seed from valid boundary costs -------
    for (const u32 i : scan_) cost_[i] = kInf;

    heap_.clear();
    for (const u32 ci : scan_) {
        const i32 cx = static_cast<i32>(ci % static_cast<u32>(width_));
        const i32 cy = static_cast<i32>(ci / static_cast<u32>(width_));
        if (!mask.walkable(cx, cy)) continue;
        if (is_goal_cell(cx, cy)) {
            cost_[ci] = 0.0f;
            continue;
        }
        cost_[ci] = eikonal(mask, cx, cy, /*exclude_dirty=*/true);
    }
    for (const u32 ci : scan_) {
        const i32 cx = static_cast<i32>(ci % static_cast<u32>(width_));
        if (!mask.walkable(cx, static_cast<i32>(ci / static_cast<u32>(width_)))) continue;
        if (std::isfinite(cost_[ci])) heap_push(ci, cost_[ci]);
    }

    sweep(mask, /*restricted=*/true);

    // --- 3. propagate any cost *decrease* back out of the region ------------
    heap_.clear();
    for (const u32 ci : scan_) {
        if (!std::isfinite(cost_[ci])) continue;
        const i32 cx = static_cast<i32>(ci % static_cast<u32>(width_));
        const i32 cy = static_cast<i32>(ci / static_cast<u32>(width_));
        if (!mask.walkable(cx, cy)) continue;
        for (i32 d = 0; d < 4; ++d) {
            const i32 nx = cx + kOffX[d];
            const i32 ny = cy + kOffY[d];
            if (!mask.walkable(nx, ny)) continue;
            const u32 ni = static_cast<u32>(mask.index(nx, ny));
            if (dirty_mark_[ni] == dirty_gen_) continue;
            const f32 nc = eikonal(mask, nx, ny, /*exclude_dirty=*/false);
            if (nc < cost_[ni]) {
                cost_[ni] = nc;
                touch(nx, ny);
                heap_push(ni, nc);
            }
        }
    }
    if (!heap_.empty()) sweep(mask, /*restricted=*/false);

    // --- 4. directions over everything that moved, plus a one-cell halo ------
    // A raw direction reads its cell's four neighbours, so one cell of halo is
    // exactly what a changed cost can reach.
    compute_directions(mask, touch_min_.x - 1, touch_min_.y - 1, touch_max_.x + 1, touch_max_.y + 1);

    // Smoothing reaches one cell further per iteration, so a cell up to N away
    // from a raw direction that just moved needs its smoothed value rebuilt
    // too. smooth_directions() expands what it is given by a further N
    // internally, which is what makes the published values here identical to a
    // full bake's rather than merely close.
    //
    // Driven off the direction box, not the cost box. Those differ by an order
    // of magnitude on a real reroute, and smoothing the cost box would spend
    // most of its time rewriting cells with the values they already had.
    if (const i32 pad = smoothing_iterations();
        pad > 0 && dir_touch_max_.x >= dir_touch_min_.x) {
        smooth_directions(mask, dir_touch_min_.x - pad, dir_touch_min_.y - pad,
                          dir_touch_max_.x + pad, dir_touch_max_.y + pad);
    }
    ++stats_.regions_processed;
}

void FlowField::rebake_pending(const TissueMask& mask) {
    WallClock clock;
    stats_.cells_visited = 0;
    stats_.regions_processed = 0;
    stats_.full_bake = false;
    for (const Rect& r : dirty_) rebake_region(mask, r);
    dirty_.clear();
    stats_.last_bake_ms = clock.elapsed_ms();
}

bool FlowField::pump_rebake(const TissueMask& mask, f64 budget_ms) {
    if (dirty_.empty()) return true;
    WallClock clock;
    stats_.cells_visited = 0;
    stats_.regions_processed = 0;
    stats_.full_bake = false;

    // Regions are atomic: a region half-solved would leave the field
    // inconsistent, which is worse than overrunning the budget. So the budget
    // is checked between regions and at least one region always runs.
    usize done = 0;
    while (done < dirty_.size()) {
        rebake_region(mask, dirty_[done]);
        ++done;
        if (clock.elapsed_ms() >= budget_ms) break;
    }
    dirty_.erase(dirty_.begin(), dirty_.begin() + static_cast<std::ptrdiff_t>(done));
    stats_.last_bake_ms = clock.elapsed_ms();
    return dirty_.empty();
}

// ---- sampling (hot path) ---------------------------------------------------

Vec2 FlowField::sample(Vec2 world_pos) const {
    f32 support = 0.0f;
    return sample_with_support(world_pos, support);
}

Vec2 FlowField::sample_with_support(Vec2 world_pos, f32& support) const {
    support = 0.0f;
    if (width_ <= 0 || height_ <= 0) return Vec2{0.0f, 0.0f};
    const Vec2 g = (world_pos - origin_) / cell_size_ - Vec2{0.5f, 0.5f};
    const f32 fx = std::floor(g.x);
    const f32 fy = std::floor(g.y);
    const i32 x0 = static_cast<i32>(fx);
    const i32 y0 = static_cast<i32>(fy);
    const f32 tx = g.x - fx;
    const f32 ty = g.y - fy;

    // Wall / unreachable cells hold (0,0). Including them in the blend would
    // drag the direction toward zero next to every wall, so they are dropped
    // from the weighted sum and the weights renormalized.
    const f32 wx[2] = {1.0f - tx, tx};
    const f32 wy[2] = {1.0f - ty, ty};
    Vec2 acc{0.0f, 0.0f};
    f32 wsum = 0.0f;
    for (i32 j = 0; j < 2; ++j) {
        for (i32 i = 0; i < 2; ++i) {
            const i32 x = x0 + i;
            const i32 y = y0 + j;
            if (x < 0 || y < 0 || x >= width_ || y >= height_) continue;
            const Vec2 d = direction_[static_cast<usize>(y) * static_cast<usize>(width_) +
                                      static_cast<usize>(x)];
            if (d.x == 0.0f && d.y == 0.0f) continue;
            const f32 w = wx[i] * wy[j];
            acc += d * w;
            wsum += w;
        }
    }
    if (wsum <= 0.0f) return Vec2{0.0f, 0.0f};
    support = math::saturate(wsum);
    return math::normalize_safe(acc);
}

Vec2 FlowField::sample_nearest(Vec2 world_pos) const {
    if (width_ <= 0 || height_ <= 0) return Vec2{0.0f, 0.0f};
    const Vec2 g = (world_pos - origin_) / cell_size_;
    const i32 x = static_cast<i32>(std::floor(g.x));
    const i32 y = static_cast<i32>(std::floor(g.y));
    if (x < 0 || y < 0 || x >= width_ || y >= height_) return Vec2{0.0f, 0.0f};
    return direction_[static_cast<usize>(y) * static_cast<usize>(width_) + static_cast<usize>(x)];
}

f32 FlowField::sample_cost(Vec2 world_pos) const {
    if (width_ <= 0 || height_ <= 0) return kInf;
    const Vec2 g = (world_pos - origin_) / cell_size_ - Vec2{0.5f, 0.5f};
    const f32 fx = std::floor(g.x);
    const f32 fy = std::floor(g.y);
    const i32 x0 = static_cast<i32>(fx);
    const i32 y0 = static_cast<i32>(fy);
    const f32 tx = g.x - fx;
    const f32 ty = g.y - fy;
    const f32 wx[2] = {1.0f - tx, tx};
    const f32 wy[2] = {1.0f - ty, ty};

    f32 acc = 0.0f;
    f32 wsum = 0.0f;
    for (i32 j = 0; j < 2; ++j) {
        for (i32 i = 0; i < 2; ++i) {
            const i32 x = x0 + i;
            const i32 y = y0 + j;
            if (x < 0 || y < 0 || x >= width_ || y >= height_) continue;
            const f32 c = cost_[static_cast<usize>(y) * static_cast<usize>(width_) +
                                static_cast<usize>(x)];
            if (!std::isfinite(c)) continue;
            const f32 w = wx[i] * wy[j];
            acc += c * w;
            wsum += w;
        }
    }
    if (wsum <= 0.0f) return kInf;
    return acc / wsum;
}

bool FlowField::reachable(Vec2 world_pos) const {
    if (width_ <= 0 || height_ <= 0) return false;
    const Vec2 g = (world_pos - origin_) / cell_size_;
    const i32 x = static_cast<i32>(std::floor(g.x));
    const i32 y = static_cast<i32>(std::floor(g.y));
    if (x < 0 || y < 0 || x >= width_ || y >= height_) return false;
    return std::isfinite(cost_[static_cast<usize>(y) * static_cast<usize>(width_) +
                               static_cast<usize>(x)]);
}

} // namespace immune::sim
