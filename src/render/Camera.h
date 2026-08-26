// render/Camera.h — fixed tilted-topdown camera. FROZEN CONTRACT.
// Owner: Wave 1C.
//
// RATIONALE (DESIGN.md §7)
// The camera tilts ~15-25 degrees and never rotates. That constraint is what
// lets the whole game be 2D sprites with a fake-3D read: world Y maps to screen
// Y scaled by cos(tilt), and a sprite's height above the plane becomes a
// constant vertical offset plus a drop shadow. No 3D geometry anywhere.
//
// Because the tilt is fixed, world<->screen is an affine transform, so
// screen_to_world is exact — which is what grid-free continuous tower placement
// (DESIGN.md §4) needs.
#pragma once

#include "core/Types.h"

#include <glm/mat4x4.hpp>

namespace immune::render {

class Camera {
public:
    /// Tilt in degrees from straight-down. Clamped to [0, 45].
    void set_tilt_degrees(f32 degrees);
    f32 tilt_degrees() const { return tilt_degrees_; }

    /// World-space point at the centre of the view.
    void set_center(Vec2 center) { center_ = center; }
    Vec2 center() const { return center_; }

    /// World units visible across the viewport height (before tilt foreshortening).
    void set_view_height(f32 units) { view_height_ = units; }
    f32 view_height() const { return view_height_; }

    void set_viewport(i32 width, i32 height) { viewport_ = IVec2{width, height}; }
    IVec2 viewport() const { return viewport_; }

    /// Clamps the centre so the view stays inside the level bounds.
    void set_bounds(const Rect& world_bounds) { bounds_ = world_bounds; }
    /// The clamp rect. Read by the level editor, which drives its own free
    /// pan/zoom and needs to know the limits it is navigating within (and
    /// deliberately widens them, so you can see outside the level while
    /// dragging the world rectangle itself).
    const Rect& bounds() const { return bounds_; }
    void clamp_to_bounds();

    /// Combined view-projection for the sprite shaders.
    glm::mat4 view_projection() const;

    Vec2 world_to_screen(Vec2 world) const;
    Vec2 screen_to_world(Vec2 screen_px) const;

    /// World-space rect currently visible. Culling and the blob density
    /// texture's extent both derive from this.
    Rect visible_bounds() const;

private:
    Vec2 center_{0.0f, 0.0f};
    f32 view_height_ = 90.0f;
    f32 tilt_degrees_ = 20.0f;
    IVec2 viewport_{1600, 900};
    Rect bounds_{};
};

} // namespace immune::render
