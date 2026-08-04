// Wave 0: the camera transform is real — tower placement (Wave 2B) and the
// screenshot path both need an exact screen<->world mapping from day one.
#include "render/Camera.h"

#include "core/Math.h"

#include <glm/gtc/matrix_transform.hpp>

namespace immune::render {

void Camera::set_tilt_degrees(f32 degrees) {
    tilt_degrees_ = math::clamp(degrees, 0.0f, 45.0f);
}

void Camera::clamp_to_bounds() {
    const Vec2 extent = bounds_.size();
    if (extent.x <= 0.0f || extent.y <= 0.0f) return;
    const Rect vis = visible_bounds();
    const Vec2 half = vis.size() * 0.5f;
    Vec2 c = center_;
    if (half.x * 2.0f >= extent.x) c.x = bounds_.center().x;
    else c.x = math::clamp(c.x, bounds_.min.x + half.x, bounds_.max.x - half.x);
    if (half.y * 2.0f >= extent.y) c.y = bounds_.center().y;
    else c.y = math::clamp(c.y, bounds_.min.y + half.y, bounds_.max.y - half.y);
    center_ = c;
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
