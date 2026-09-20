// Wave 0: the camera transform is real — tower placement (Wave 2B) and the
// screenshot path both need an exact screen<->world mapping from day one.
#include "render/Camera.h"

#include "core/Math.h"

#include <glm/gtc/matrix_transform.hpp>

namespace immune::render {

void Camera::set_tilt_degrees(f32 degrees) {
    tilt_degrees_ = math::clamp(degrees, 0.0f, 45.0f);
}

namespace {
/// World-space half-width/half-height visible at `view_height`, independent
/// of center -- the shared piece of visible_bounds() and the pan clamps.
Vec2 half_extent_at(f32 view_height, IVec2 viewport, f32 tilt_degrees) {
    const f32 cos_tilt = std::cos(glm::radians(tilt_degrees));
    const f32 half_h = view_height * 0.5f;
    const f32 aspect = viewport.y > 0
        ? static_cast<f32>(viewport.x) / static_cast<f32>(viewport.y)
        : 1.0f;
    const f32 half_w = half_h * aspect;
    const f32 world_half_h = cos_tilt > math::kEpsilon ? half_h / cos_tilt : half_h;
    return Vec2{half_w, world_half_h};
}
} // namespace

Vec2 Camera::clamp_center_at(Vec2 center, f32 view_height) const {
    const Vec2 extent = bounds_.size();
    if (extent.x <= 0.0f || extent.y <= 0.0f) return center;
    const Vec2 half = half_extent_at(view_height, viewport_, tilt_degrees_);
    Vec2 c = center;
    if (half.x * 2.0f >= extent.x) c.x = bounds_.center().x;
    else c.x = math::clamp(c.x, bounds_.min.x + half.x, bounds_.max.x - half.x);
    if (half.y * 2.0f >= extent.y) c.y = bounds_.center().y;
    else c.y = math::clamp(c.y, bounds_.min.y + half.y, bounds_.max.y - half.y);
    return c;
}

void Camera::clamp_to_bounds() {
    center_ = clamp_center_at(center_, view_height_);
}

Vec2 Camera::clamp_center_to_reference(Vec2 center, f32 view_height,
                                        f32 reference_view_height) const {
    const Vec2 extent = bounds_.size();
    if (extent.x <= 0.0f || extent.y <= 0.0f) return center;
    const Vec2 half = half_extent_at(view_height, viewport_, tilt_degrees_);
    const Vec2 ref_half = half_extent_at(reference_view_height, viewport_, tilt_degrees_);
    // The edge the reference zoom reaches: bounds() itself, unless the
    // reference is already wide enough to spill past it, in which case the
    // reference's own (center-locked) visible edge is as far out as any zoom
    // is allowed to go.
    const f32 lo_x = ref_half.x * 2.0f >= extent.x ? bounds_.center().x - ref_half.x : bounds_.min.x;
    const f32 hi_x = ref_half.x * 2.0f >= extent.x ? bounds_.center().x + ref_half.x : bounds_.max.x;
    const f32 lo_y = ref_half.y * 2.0f >= extent.y ? bounds_.center().y - ref_half.y : bounds_.min.y;
    const f32 hi_y = ref_half.y * 2.0f >= extent.y ? bounds_.center().y + ref_half.y : bounds_.max.y;
    Vec2 c = center;
    c.x = (hi_x - lo_x <= half.x * 2.0f) ? (lo_x + hi_x) * 0.5f
                                          : math::clamp(c.x, lo_x + half.x, hi_x - half.x);
    c.y = (hi_y - lo_y <= half.y * 2.0f) ? (lo_y + hi_y) * 0.5f
                                          : math::clamp(c.y, lo_y + half.y, hi_y - half.y);
    return c;
}

glm::mat4 Camera::view_projection() const {
    const f32 aspect = viewport_.y > 0
        ? static_cast<f32>(viewport_.x) / static_cast<f32>(viewport_.y)
        : 1.0f;
    const f32 half_h = view_height_ * 0.5f;
    const f32 half_w = half_h * aspect;
    const glm::mat4 proj = glm::ortho(-half_w, half_w, -half_h, half_h, -1000.0f, 1000.0f);

    // Fixed tilt: world Y is foreshortened by cos(tilt). No rotation about Z.
    const f32 cos_tilt = std::cos(glm::radians(tilt_degrees_));
    glm::mat4 view(1.0f);
    view = glm::scale(view, glm::vec3(1.0f, cos_tilt, 1.0f));
    view = glm::translate(view, glm::vec3(-center_.x, -center_.y, 0.0f));
    return proj * view;
}

Vec2 Camera::world_to_screen(Vec2 world) const {
    const f32 cos_tilt = std::cos(glm::radians(tilt_degrees_));
    const f32 half_h = view_height_ * 0.5f;
    const f32 aspect = viewport_.y > 0
        ? static_cast<f32>(viewport_.x) / static_cast<f32>(viewport_.y)
        : 1.0f;
    const f32 half_w = half_h * aspect;

    const f32 ndc_x = (world.x - center_.x) / half_w;
    const f32 ndc_y = ((world.y - center_.y) * cos_tilt) / half_h;
    return Vec2{(ndc_x * 0.5f + 0.5f) * static_cast<f32>(viewport_.x),
                (1.0f - (ndc_y * 0.5f + 0.5f)) * static_cast<f32>(viewport_.y)};
}

Vec2 Camera::screen_to_world(Vec2 screen_px) const {
    const f32 cos_tilt = std::cos(glm::radians(tilt_degrees_));
    const f32 half_h = view_height_ * 0.5f;
    const f32 aspect = viewport_.y > 0
        ? static_cast<f32>(viewport_.x) / static_cast<f32>(viewport_.y)
        : 1.0f;
    const f32 half_w = half_h * aspect;

    const f32 ndc_x = (screen_px.x / static_cast<f32>(viewport_.x)) * 2.0f - 1.0f;
    const f32 ndc_y = (1.0f - screen_px.y / static_cast<f32>(viewport_.y)) * 2.0f - 1.0f;
    const f32 wy = cos_tilt > math::kEpsilon ? (ndc_y * half_h) / cos_tilt : ndc_y * half_h;
    return Vec2{center_.x + ndc_x * half_w, center_.y + wy};
}

Rect Camera::visible_bounds() const {
    const f32 cos_tilt = std::cos(glm::radians(tilt_degrees_));
    const f32 half_h = view_height_ * 0.5f;
    const f32 aspect = viewport_.y > 0
        ? static_cast<f32>(viewport_.x) / static_cast<f32>(viewport_.y)
        : 1.0f;
    const f32 half_w = half_h * aspect;
    const f32 world_half_h = cos_tilt > math::kEpsilon ? half_h / cos_tilt : half_h;
    return Rect{center_ - Vec2{half_w, world_half_h}, center_ + Vec2{half_w, world_half_h}};
}

} // namespace immune::render
