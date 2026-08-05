#version 450 core
// Flow-field debug overlay (F1). Off in release play; a plain coloured line
// list rebuilt on the CPU each call — never in the sim hot path.

layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec4 a_color;

layout(location = 0) uniform mat4 u_view_projection;

out vec4 v_color;

void main() {
    v_color = a_color;
    gl_Position = u_view_projection * vec4(a_pos, 0.0, 1.0);
}
