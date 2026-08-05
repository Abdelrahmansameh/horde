#version 450 core
// Density-LOD blob pass — vertex stage.
// Draws a single quad covering the camera's visible world extent; the
// fragment stage samples the low-res density texture accumulated by the CPU
// batcher (render::DensityGrid) over that same extent.

layout(location = 0) in vec2 a_corner; // shared unit quad, [-0.5, 0.5]

layout(location = 0) uniform mat4 u_view_projection;
layout(location = 1) uniform vec2 u_extent_min;
layout(location = 2) uniform vec2 u_extent_size;

out vec2 v_uv;

void main() {
    vec2 uv = a_corner + 0.5;
    v_uv = uv;
    vec2 world = u_extent_min + uv * u_extent_size;
    gl_Position = u_view_projection * vec4(world, 0.0, 1.0);
}
