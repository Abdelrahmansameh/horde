#version 450 core
// Tissue substrate — back layer (DESIGN.md §7). Vertex stage: a single quad
// drawn over the camera's visible world extent (so it always fills the
// viewport regardless of pan), sampling a distance-field texture that covers
// the *level's* extent, which is generally larger — hence the two separate
// rectangles below.

layout(location = 0) in vec2 a_corner; // shared unit quad, [-0.5, 0.5]

layout(location = 0) uniform mat4 u_view_projection;
layout(location = 1) uniform vec2 u_screen_min;   // camera visible_bounds().min
layout(location = 2) uniform vec2 u_screen_size;  // camera visible_bounds().size()
layout(location = 3) uniform vec2 u_sdf_min;      // TissueMask::world_bounds().min
layout(location = 4) uniform vec2 u_sdf_size;     // TissueMask::world_bounds().size()

out vec2 v_uv;

void main() {
    vec2 corner01 = a_corner + 0.5;
    vec2 world = u_screen_min + corner01 * u_screen_size;
    v_uv = (world - u_sdf_min) / max(u_sdf_size, vec2(1e-4));
    gl_Position = u_view_projection * vec4(world, 0.0, 1.0);
}
