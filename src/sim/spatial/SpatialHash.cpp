// Uniform-grid broadphase. Owner: Wave 1B.
//
// Build is a two-pass counting sort (CSR): pass 1 counts per-cell occupancy and
// caches each agent's cell id; a prefix sum turns counts into offsets; pass 2
// scatters agent indices into `indices_`. No per-cell vector, no allocation
// after the buffers have reached their working size.
//
// The build is deliberately SERIAL. At 10k agents both passes are ~10k linear
// touches (tens of microseconds — the §8.6 budget is 1 ms), and a serial scatter
// leaves each cell's index run in ascending agent order. That ordering is what
// makes the separation sum in ChaffSystem bit-identical run to run: floating
// point addition is not associative, so a scheduling-dependent neighbour order
// would be a determinism bug. Parallelising the build would buy microseconds and
// cost the project's verification substrate.
#include "sim/spatial/SpatialHash.h"

#include "core/Math.h"

#include <algorithm>

namespace immune::sim {

void SpatialHash::configure(const SpatialHashDesc& desc) {
    bounds_ = desc.bounds;
    cell_size_ = desc.cell_size > 0.0f ? desc.cell_size : 1.0f;
    inv_cell_size_ = 1.0f / cell_size_;
    const Vec2 extent = bounds_.size();
    dims_ = IVec2{math::max(1, static_cast<i32>(extent.x * inv_cell_size_) + 1),
                  math::max(1, static_cast<i32>(extent.y * inv_cell_size_) + 1)};
    const usize cells = static_cast<usize>(dims_.x) * static_cast<usize>(dims_.y);
    cell_start_.assign(cells + 1, 0u);
    occupancy_.assign(cells, 0u);
    cursor_.assign(cells, 0u);
    indices_.clear();
    cell_of_.clear();
    max_occupancy_ = 0;
}

void SpatialHash::rebuild(const f32* pos_x, const f32* pos_y, usize count, JobSystem* jobs) {
    // `jobs` is accepted for interface symmetry with the rest of the sim; see the
    // file header for why this build stays serial.
    (void)jobs;

    const usize cells = occupancy_.size();
    if (cells == 0 || count == 0 || pos_x == nullptr || pos_y == nullptr) {
        clear();
        return;
    }

    // resize() only touches the allocator the first time a given count is seen;
    // the sim reserves to max_chaff and count never exceeds it afterwards.
    if (indices_.size() != count) indices_.resize(count);
    if (cell_of_.size() != count) cell_of_.resize(count);

    // Pass 1 — count, caching the cell id so pass 2 does no float maths.
    std::fill(occupancy_.begin(), occupancy_.end(), 0u);
    const f32 min_x = bounds_.min.x;
    const f32 min_y = bounds_.min.y;
    const i32 dim_x = dims_.x;
    const i32 dim_y = dims_.y;
    for (usize i = 0; i < count; ++i) {
        const i32 cx = math::clamp(static_cast<i32>((pos_x[i] - min_x) * inv_cell_size_), 0, dim_x - 1);
        const i32 cy = math::clamp(static_cast<i32>((pos_y[i] - min_y) * inv_cell_size_), 0, dim_y - 1);
        const u32 c = static_cast<u32>(cy) * static_cast<u32>(dim_x) + static_cast<u32>(cx);
        cell_of_[i] = c;
        ++occupancy_[c];
    }

    // Prefix sum -> CSR offsets. cell_start_[c] is also the write cursor seed.
    u32 running = 0;
    u32 max_occ = 0;
    for (usize c = 0; c < cells; ++c) {
        cell_start_[c] = running;
        cursor_[c] = running;
        const u32 n = occupancy_[c];
        if (n > max_occ) max_occ = n;
        running += n;
    }
    cell_start_[cells] = running;
    max_occupancy_ = max_occ;

    // Pass 2 — scatter in ascending agent order (see file header).
    for (usize i = 0; i < count; ++i) {
        indices_[cursor_[cell_of_[i]]++] = static_cast<u32>(i);
    }
}

void SpatialHash::clear() {
    std::fill(cell_start_.begin(), cell_start_.end(), 0u);
    std::fill(occupancy_.begin(), occupancy_.end(), 0u);
    indices_.clear();
    max_occupancy_ = 0;
}

IVec2 SpatialHash::cell_coord(Vec2 p) const {
    const Vec2 local = (p - bounds_.min) * inv_cell_size_;
    return IVec2{math::clamp(static_cast<i32>(local.x), 0, dims_.x - 1),
                 math::clamp(static_cast<i32>(local.y), 0, dims_.y - 1)};
}

u32 SpatialHash::cell_index(IVec2 c) const {
    return static_cast<u32>(c.y) * static_cast<u32>(dims_.x) + static_cast<u32>(c.x);
}

void SpatialHash::cell_range(u32 cell, u32& out_begin, u32& out_end) const {
    if (static_cast<usize>(cell) + 1 >= cell_start_.size()) {
        out_begin = out_end = 0;
        return;
    }
    out_begin = cell_start_[cell];
    out_end = cell_start_[cell + 1];
}

// ---------------------------------------------------------------------------
// Queries. All are conservative at cell granularity: they return every agent in
// every overlapped cell and the caller applies the exact test. That is the whole
// point of the grid — the exact test is cheap, the candidate search is not.
// ---------------------------------------------------------------------------

void SpatialHash::gather_cells(IVec2 lo, IVec2 hi, std::vector<u32>& out) const {
    if (indices_.empty()) return;
    lo.x = math::clamp(lo.x, 0, dims_.x - 1);
    lo.y = math::clamp(lo.y, 0, dims_.y - 1);
    hi.x = math::clamp(hi.x, 0, dims_.x - 1);
    hi.y = math::clamp(hi.y, 0, dims_.y - 1);
    const u32* idx = indices_.data();
    for (i32 cy = lo.y; cy <= hi.y; ++cy) {
        const u32 row = static_cast<u32>(cy) * static_cast<u32>(dims_.x);
        // Cells in a row are contiguous in the CSR array, so a whole row-span
        // copies in one go rather than cell by cell.
        const u32 begin = cell_start_[row + static_cast<u32>(lo.x)];
        const u32 end = cell_start_[row + static_cast<u32>(hi.x) + 1u];
        out.insert(out.end(), idx + begin, idx + end);
    }
}

void SpatialHash::query_circle(Vec2 center, f32 radius, std::vector<u32>& out) const {
    out.clear();
    if (radius < 0.0f) return;
    gather_cells(cell_coord(center - Vec2{radius, radius}),
                 cell_coord(center + Vec2{radius, radius}), out);
}

void SpatialHash::query_rect(const Rect& rect, std::vector<u32>& out) const {
    out.clear();
    gather_cells(cell_coord(rect.min), cell_coord(rect.max), out);
}

void SpatialHash::query_cone(Vec2 origin, Vec2 direction, f32 radius, f32 half_angle,
                             std::vector<u32>& out) const {
    out.clear();
    if (radius < 0.0f) return;
    // A cone is bounded by the circle of its own radius. For a wide arc that is
    // the tightest cheap bound anyway; for a narrow arc we additionally clip to
    // the bounding box of {origin, the two arc edge endpoints, and the arc
    // midpoint}, which is a large win for the Cytotoxic-T style thin cones.
    const Vec2 d = math::normalize_safe(direction);
    if (d.x == 0.0f && d.y == 0.0f || half_angle >= math::kPi * 0.5f) {
        gather_cells(cell_coord(origin - Vec2{radius, radius}),
                     cell_coord(origin + Vec2{radius, radius}), out);
        return;
    }
    const f32 c = std::cos(half_angle);
    const f32 s = std::sin(half_angle);
    const Vec2 e0{d.x * c - d.y * s, d.x * s + d.y * c};
    const Vec2 e1{d.x * c + d.y * s, -d.x * s + d.y * c};
    Vec2 lo = origin, hi = origin;
    const Vec2 pts[3] = {origin + e0 * radius, origin + e1 * radius, origin + d * radius};
    for (const Vec2& p : pts) {
        lo.x = math::min(lo.x, p.x); lo.y = math::min(lo.y, p.y);
        hi.x = math::max(hi.x, p.x); hi.y = math::max(hi.y, p.y);
    }
    gather_cells(cell_coord(lo), cell_coord(hi), out);
}

void SpatialHash::query_neighbourhood(Vec2 p, std::vector<u32>& out) const {
    out.clear();
    const IVec2 c = cell_coord(p);
    gather_cells(IVec2{c.x - 1, c.y - 1}, IVec2{c.x + 1, c.y + 1}, out);
}

} // namespace immune::sim
