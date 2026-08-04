// Wave 0 stub. Implementation is owned by Wave 1B.
#include "sim/spatial/SpatialHash.h"

#include "core/Math.h"

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
    indices_.clear();
    max_occupancy_ = 0;
}

void SpatialHash::rebuild(const f32*, const f32*, usize, JobSystem*) {
    // Wave 1B: two-pass counting sort into cell_start_/indices_.
    clear();
}

void SpatialHash::clear() {
    for (auto& v : cell_start_) v = 0u;
    for (auto& v : occupancy_) v = 0u;
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
    if (cell + 1 >= cell_start_.size()) {
        out_begin = out_end = 0;
        return;
    }
    out_begin = cell_start_[cell];
    out_end = cell_start_[cell + 1];
}

void SpatialHash::query_circle(Vec2, f32, std::vector<u32>& out) const { out.clear(); }
void SpatialHash::query_rect(const Rect&, std::vector<u32>& out) const { out.clear(); }
void SpatialHash::query_neighbourhood(Vec2, std::vector<u32>& out) const { out.clear(); }

} // namespace immune::sim
