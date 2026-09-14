#version 450 core
// Tissue substrate — back layer (DESIGN.md §9.1). Vertex stage: a single quad
// drawn over the camera's visible world extent (so it always fills the
// viewport regardless of pan), sampling a distance-field texture that covers
// the *level's* extent, which is generally larger — hence the separate
// rectangles below.
//
// Two rectangles besides the screen's: the distance field's (which, when it is
// the smooth field from game/level/RenderSdf.h, is padded well past the level)
// and the sim mask's (which the flow and lane textures are on).

layout(location = 0) in vec2 a_corner; // shared unit quad, [-0.5, 0.5]

layout(location = 0) uniform mat4 u_view_projection;
layout(location = 1) uniform vec2 u_screen_min;   // camera visible_bounds().min
layout(location = 2) uniform vec2 u_screen_size;  // camera visible_bounds().size()
layout(location = 3) uniform vec2 u_sdf_min;      // distance field rect
layout(location = 4) uniform vec2 u_sdf_size;
layout(location = 9) uniform vec2 u_mask_min;     // TissueMask::world_bounds()
layout(location = 10) uniform vec2 u_mask_size;

out vec2 v_uv;        // into u_sdf
out vec2 v_uv_mask;   // into u_flow / u_lane
// World position. The fragment stage evaluates every pattern in WORLD space,
// not in v_uv: uv is normalised by the level's extent, so a pattern keyed off
// it would stretch differently on a wide level than on a tall one, and would
// swim under the camera. World space keeps cell size physically constant,
// which is what makes the substrate read as a place the camera moves over
// rather than a texture stuck to the screen.
out vec2 v_world;
// Screen-space [0,1] across the visible extent. The quad is exactly the
// visible rect, so the unit-quad corner already is this.
out vec2 v_screen;

void main() {
    vec2 corner01 = a_corner + 0.5;
    vec2 world = u_screen_min + corner01 * u_screen_size;
    v_uv = (world - u_sdf_min) / max(u_sdf_size, vec2(1e-4));
    v_uv_mask = (world - u_mask_min) / max(u_mask_size, vec2(1e-4));
    v_world = world;
    v_screen = corner01;
    gl_Position = u_view_projection * vec4(world, 0.0, 1.0);
}
