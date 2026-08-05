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
layout(location = 7) in float i_pad;        // offset 28 : per-family wobble amount

layout(location = 0) uniform mat4 u_view_projection;
layout(location = 1) uniform float u_time;

out vec2  v_local;
out vec4  v_tint;
flat out uint v_flags;
out float v_anim_phase;
out float v_wobble;
flat out vec2 v_shadow_offset;

// DESIGN.md §7: "soft directional drop-shadow + slight vertical sprite offset
// sell height without real 3D geometry." Both are folded into this one pass so
// the per-family instance count stays at one draw call: the quad is padded
// wide enough to hold an off-centre shadow blob, and the fragment stage tests
// two SDF centres against the same local space.
const float kHeightFrac = 0.16;         // body lift, fraction of i_scale
const vec2  kShadowDirFrac = vec2(0.10, -0.08); // directional shadow drift
const float kPad = 1.9;                 // quad padding so the shadow isn't clipped

void main() {
    float s = sin(i_rotation);
    float c = cos(i_rotation);

    vec2 corner = a_corner * kPad;
    vec2 local = vec2(corner.x * c - corner.y * s,
                      corner.x * s + corner.y * c) * i_scale;

    vec2 center_world = i_position + vec2(0.0, kHeightFrac * i_scale);

    v_local = corner;
    v_tint = i_tint;
    v_flags = i_flags;
    v_anim_phase = i_anim_phase;
    v_wobble = i_pad;

    // The shadow sits at the un-lifted ground position, offset slightly along
    // a fixed world direction. Object space is unrotated (SDFs are isotropic),
    // so the world-space delta is rotated by -i_rotation to land in the same
    // frame v_local is evaluated in.
    vec2 delta = kShadowDirFrac - vec2(0.0, kHeightFrac);
    v_shadow_offset = vec2(delta.x * c + delta.y * s, -delta.x * s + delta.y * c);

    gl_Position = u_view_projection * vec4(center_world + local, 0.0, 1.0);
}
