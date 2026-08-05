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
constexpr f32 kSqrt2 = 1.41421356237309504880f;

/// Fixed neighbour order: 4 orthogonal then 4 diagonal. Frozen for determinism.
constexpr i32 kOffX[8] = {-1, 1, 0, 0, -1, 1, -1, 1};
constexpr i32 kOffY[8] = {0, 0, -1, 1, -1, -1, 1, 1};
constexpr f32 kStep[8] = {1.0f, 1.0f, 1.0f, 1.0f, kSqrt2, kSqrt2, kSqrt2, kSqrt2};

/// Tolerance used when deciding whether a neighbour could have been a cell's
/// shortest-path parent. Costs are O(grid diagonal * cell size); 1e-3 world
/// units is far below one cell yet comfortably above f32 sweep noise.
constexpr f32 kSupportEps = 1e-3f;

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

/// Edge weight from cell `a` to adjacent cell `b`, in world units. The per-cell
/// multipliers are averaged so a cost boundary is symmetric (traversing it
/// costs the same in either direction), which the incremental parent-support
/// test relies on.
f32 FlowField::edge_cost(const TissueMask& mask, i32 ax, i32 ay, i32 bx, i32 by, i32 dir) const {
    const f32 mul = 0.5f * (mask.cost(ax, ay) + mask.cost(bx, by));
    return cell_size_ * kStep[dir] * math::max(mul, 0.0f);
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
    const i32 dir_count = desc_.allow_diagonals ? 8 : 4;
    while (!heap_.empty()) {
        const HeapNode node = heap_pop();
        if (node.key > cost_[node.cell]) continue; // stale entry
        ++stats_.cells_visited;

        const i32 x = static_cast<i32>(node.cell % static_cast<u32>(width_));
        const i32 y = static_cast<i32>(node.cell / static_cast<u32>(width_));

        for (i32 d = 0; d < dir_count; ++d) {
            const i32 nx = x + kOffX[d];
            const i32 ny = y + kOffY[d];
            if (!mask.walkable(nx, ny)) continue;
            const u32 ni = static_cast<u32>(mask.index(nx, ny));
            if (restricted && !in_dirty(ni)) continue;
            if (!diagonal_ok(mask, x, y, d)) continue;
            const f32 nc = node.key + edge_cost(mask, x, y, nx, ny, d);
            if (nc < cost_[ni]) {
                cost_[ni] = nc;
                touch(nx, ny);
                heap_push(ni, nc);
            }
        }
    }
}

/// Negative normalized gradient of the cost field over a cell rect.
void FlowField::compute_directions(const TissueMask& mask, i32 x0, i32 y0, i32 x1, i32 y1) {
    x0 = math::max(x0, 0);
    y0 = math::max(y0, 0);
    x1 = math::min(x1, width_ - 1);
    y1 = math::min(y1, height_ - 1);
    const i32 dir_count = desc_.allow_diagonals ? 8 : 4;

    for (i32 y = y0; y <= y1; ++y) {
        for (i32 x = x0; x <= x1; ++x) {
            const usize i = mask.index(x, y);
            const f32 c = cost_[i];
            if (!mask.walkable(x, y) || !std::isfinite(c) || c <= 0.0f) {
                direction_[i] = Vec2{0.0f, 0.0f};
                continue;
            }

            // One-sided where a neighbour is a wall or unreachable: substitute
            // this cell's own cost and divide by the number of real samples, so
            // a cell hugging a wall still gets an accurate downhill direction.
            f32 acc[2] = {0.0f, 0.0f};
            i32 cnt[2] = {0, 0};
            for (i32 d = 0; d < 4; ++d) {
                const i32 nx = x + kOffX[d];
                const i32 ny = y + kOffY[d];
                if (!mask.walkable(nx, ny)) continue;
                const f32 nc = cost_[mask.index(nx, ny)];
                if (!std::isfinite(nc)) continue;
                const i32 axis = (d < 2) ? 0 : 1;
                const f32 sign = (d == 0 || d == 2) ? -1.0f : 1.0f; // -x, +x, -y, +y
                acc[axis] += sign * (nc - c);
                cnt[axis] += 1;
            }
            const f32 gx = cnt[0] > 0 ? acc[0] / static_cast<f32>(cnt[0]) : 0.0f;
            const f32 gy = cnt[1] > 0 ? acc[1] / static_cast<f32>(cnt[1]) : 0.0f;
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
            direction_[i] = dir;
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

    for (const IVec2& g : desc_.goal_cells) {
        if (!mask.walkable(g.x, g.y)) continue;
        const u32 gi = static_cast<u32>(mask.index(g.x, g.y));
        if (cost_[gi] != 0.0f) {
            cost_[gi] = 0.0f;
            heap_push(gi, 0.0f);
        }
    }

    sweep(mask, /*restricted=*/false);
    compute_directions(mask, 0, 0, width_ - 1, height_ - 1);

    stats_.regions_processed = 1;
    stats_.last_bake_ms = clock.elapsed_ms();
}

// ---- incremental rebake ----------------------------------------------------

void FlowField::mark_dirty(const Rect& region) { dirty_.push_back(region); }

bool FlowField::is_goal_cell(i32 x, i32 y) const {
    for (const IVec2& g : desc_.goal_cells) {
        if (g.x == x && g.y == y) return true;
    }
    return false;
}

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
    const i32 dir_count = desc_.allow_diagonals ? 8 : 4;
    for (usize head = 0; head < scan_.size(); ++head) {
        const u32 ci = scan_[head];
        const i32 cx = static_cast<i32>(ci % static_cast<u32>(width_));
        const i32 cy = static_cast<i32>(ci / static_cast<u32>(width_));
        for (i32 d = 0; d < dir_count; ++d) {
            const i32 nx = cx + kOffX[d];
            const i32 ny = cy + kOffY[d];
            if (!mask.walkable(nx, ny)) continue;
            const u32 ni = static_cast<u32>(mask.index(nx, ny));
            if (dirty_mark_[ni] == dirty_gen_) continue;
            if (is_goal_cell(nx, ny)) continue; // goals are self-supporting

            // Does `ni` still have at least one valid parent?
            bool supported = false;
            for (i32 e = 0; e < dir_count && !supported; ++e) {
                const i32 px = nx + kOffX[e];
                const i32 py = ny + kOffY[e];
                if (!mask.walkable(px, py)) continue;
                const u32 pi = static_cast<u32>(mask.index(px, py));
                if (dirty_mark_[pi] == dirty_gen_) continue;
                if (!diagonal_ok(mask, nx, ny, e)) continue;
                if (cost_[pi] + edge_cost(mask, px, py, nx, ny, e) <= cost_[ni] + kSupportEps) {
                    supported = true;
                }
            }
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
        f32 best = kInf;
        for (i32 d = 0; d < dir_count; ++d) {
            const i32 nx = cx + kOffX[d];
            const i32 ny = cy + kOffY[d];
            if (!mask.walkable(nx, ny)) continue;
            const u32 ni = static_cast<u32>(mask.index(nx, ny));
            if (dirty_mark_[ni] == dirty_gen_) continue; // also invalid
            if (!std::isfinite(cost_[ni])) continue;
            if (!diagonal_ok(mask, cx, cy, d)) continue;
            best = math::min(best, cost_[ni] + edge_cost(mask, nx, ny, cx, cy, d));
        }
        cost_[ci] = best;
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
        for (i32 d = 0; d < dir_count; ++d) {
            const i32 nx = cx + kOffX[d];
            const i32 ny = cy + kOffY[d];
            if (!mask.walkable(nx, ny)) continue;
            const u32 ni = static_cast<u32>(mask.index(nx, ny));
            if (dirty_mark_[ni] == dirty_gen_) continue;
            if (!diagonal_ok(mask, cx, cy, d)) continue;
            const f32 nc = cost_[ci] + edge_cost(mask, cx, cy, nx, ny, d);
            if (nc < cost_[ni]) {
                cost_[ni] = nc;
                touch(nx, ny);
                heap_push(ni, nc);
            }
        }
    }
    if (!heap_.empty()) sweep(mask, /*restricted=*/false);

    // --- 4. directions over everything that moved, plus a one-cell halo ------
    compute_directions(mask, touch_min_.x - 1, touch_min_.y - 1, touch_max_.x + 1, touch_max_.y + 1);
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
