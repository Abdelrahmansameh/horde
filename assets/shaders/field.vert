#version 450 core
// Damage-field VFX pass — vertex stage. Owner: Wave 4G.
//
// One instanced quad per active sim::DamageField (src/sim/damage/DamageField.h).
// This is NOT a frozen contract — render::FieldGpuInstance is a Renderer.cpp
// implementation detail mirrored only here, since no other consumer reads it.
//
// Shape-specific meaning of i_scale/i_rotation/i_arc_cos:
//   Circle      (shape 0/4): i_scale.xy = diameter (2*radius), i_rotation = 0.
//                              0 is a timed burst, 4 a persistent disc — see
//                              Renderer::submit_fields for why they split.
//   Rect         (shape 1):   i_scale.xy = full rect width/height, rotation = 0
//                              (sim::Rect is axis-aligned, never rotated), and
//                              i_arc_cos carries width/height so the fragment
//                              stage can tell which axis the beam runs along.
//   Cone         (shape 2):   i_scale.xy = diameter (2*radius), i_rotation =
//                              atan2(direction), i_arc_cos = cos(arc_radians).
//   Chain        (shape 3):   i_scale.xy = diameter (2*radius), i_rotation = 0.
//
// v_local is passed as the corner *before* the per-instance scale/rotate, so
// the fragment stage always does its SDF math in a fixed normalized frame
// (true edge at length/abs == 0.5) regardless of the instance's world size —
// same trick chaff.vert/entity.vert use for their SDFs.

layout(location = 0) in vec2 a_corner; // shared unit quad, [-0.5, 0.5]

layout(location = 1) in vec2  i_position;
layout(location = 2) in vec2  i_scale;
layout(location = 3) in float i_rotation;
layout(location = 4) in float i_arc_cos;
layout(location = 5) in float i_falloff;
layout(location = 6) in float i_intensity;
layout(location = 7) in vec4  i_tint;
layout(location = 8) in uint  i_shape_id;

layout(location = 0) uniform mat4 u_view_projection;

out vec2  v_local;
out vec4  v_tint;
flat out uint  v_shape_id;
flat out float v_arc_cos;
flat out float v_falloff;
flat out float v_intensity;

// Soft glow bleeds a little past the field's true edge instead of cutting off
// hard at the simulation boundary — DESIGN.md §9.5 wants this to read as
// atmosphere, not a rasterized mask.
const float kPad = 1.3;

void main() {
    vec2 corner = a_corner * kPad;

    // Scale first (in the shape's own axis-aligned frame), then rotate — the
    // only shape with non-uniform scale (Rect) never rotates, and the only
    // shape that rotates (Cone) always has uniform scale, so this order is
    // safe for every case actually produced by DamageField.
    vec2 scaled = corner * i_scale;
    float s = sin(i_rotation);
    float c = cos(i_rotation);
    vec2 local = vec2(scaled.x * c - scaled.y * s, scaled.x * s + scaled.y * c);

    v_local = corner;
    v_tint = i_tint;
    v_shape_id = i_shape_id;
    v_arc_cos = i_arc_cos;
    v_falloff = i_falloff;
    v_intensity = i_intensity;

    gl_Position = u_view_projection * vec4(i_position + local, 0.0, 1.0);
}
