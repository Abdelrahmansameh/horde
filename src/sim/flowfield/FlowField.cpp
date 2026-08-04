// Wave 0 stub. Implementation is owned by Wave 1A.
#include "sim/flowfield/FlowField.h"

#include "core/Math.h"

#include <limits>

namespace immune::sim {

// ---- TissueMask ------------------------------------------------------------

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
    return IVec2{static_cast<i32>(local.x), static_cast<i32>(local.y)};
}

// ---- DistanceField ---------------------------------------------------------

void DistanceField::bake(const TissueMask& mask) {
    width_ = mask.width();
    height_ = mask.height();
    cell_size_ = mask.cell_size();
    origin_ = mask.world_origin();
    distance_.assign(static_cast<usize>(width_) * static_cast<usize>(height_), 0.0f);
    // Wave 1A: two-pass chamfer / jump-flood SDF.
}

f32 DistanceField::sample(Vec2) const { return 0.0f; }
Vec2 DistanceField::gradient(Vec2) const { return Vec2{0.0f, 0.0f}; }

// ---- FlowField -------------------------------------------------------------

void FlowField::bake(const TissueMask& mask, const FlowFieldBakeDesc& desc) {
    desc_ = desc;
    width_ = mask.width();
    height_ = mask.height();
    cell_size_ = mask.cell_size();
    origin_ = mask.world_origin();
    const usize n = static_cast<usize>(width_) * static_cast<usize>(height_);
    cost_.assign(n, std::numeric_limits<f32>::infinity());
    direction_.assign(n, Vec2{0.0f, 0.0f});
    dirty_.clear();
    stats_ = RebakeStats{};
    stats_.full_bake = true;
    // Wave 1A: seed goals at cost 0, Dijkstra/eikonal sweep, then gradient.
}

void FlowField::mark_dirty(const Rect& region) { dirty_.push_back(region); }

void FlowField::rebake_pending(const TissueMask&) {
    // Wave 1A: re-solve each dirty rect expanded by desc_.rebake_margin_cells,
    // seeded from existing boundary costs.
    stats_.regions_processed = static_cast<u32>(dirty_.size());
    stats_.full_bake = false;
    dirty_.clear();
}

bool FlowField::pump_rebake(const TissueMask& mask, f64) {
    rebake_pending(mask);
    return true;
}

Vec2 FlowField::sample(Vec2) const { return Vec2{0.0f, 0.0f}; }
Vec2 FlowField::sample_nearest(Vec2) const { return Vec2{0.0f, 0.0f}; }
f32 FlowField::sample_cost(Vec2) const { return std::numeric_limits<f32>::infinity(); }
bool FlowField::reachable(Vec2) const { return false; }

} // namespace immune::sim
