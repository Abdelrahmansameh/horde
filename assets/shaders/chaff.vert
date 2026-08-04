#version 450 core
// Chaff instanced sprite pass — vertex stage.
//
// CONTRACT: the instance attribute layout below mirrors render::ChaffInstance
// (src/render/Renderer.h) byte for byte. Changing one without the other is a
// contract break. 32 bytes per instance.
//
// Wave 1C owns the body of this shader. Wave 0 fixes only the interface.

// Per-vertex: a unit quad, [-0.5, 0.5] in both axes.
layout(location = 0) in vec2 a_corner;

// Per-instance (divisor 1), matching ChaffInstance:
layout(location = 1) in vec2  i_position;   // offset  0 : x, y
layout(location = 2) in float i_scale;      // offset  8
layout(location = 3) in float i_rotation;   // offset 12
layout(location = 4) in vec4  i_tint;       // offset 16 : tint_rgba8, normalized
layout(location = 5) in uint  i_flags;      // offset 20 : chaff_flags mirror
layout(location = 6) in float i_anim_phase; // offset 24
                                            // offset 28 : pad

layout(location = 0) uniform mat4 u_view_projection;
layout(location = 1) uniform float u_time;

out vec2  v_local;
out vec4  v_tint;
flat out uint v_flags;
out float v_anim_phase;

void main() {
    float s = sin(i_rotation);
    float c = cos(i_rotation);
    vec2 local = vec2(a_corner.x * c - a_corner.y * s,
                      a_corner.x * s + a_corner.y * c) * i_scale;

    v_local = a_corner;
    v_tint = i_tint;
    v_flags = i_flags;
    v_anim_phase = i_anim_phase;

    gl_Position = u_view_projection * vec4(i_position + local, 0.0, 1.0);
}
