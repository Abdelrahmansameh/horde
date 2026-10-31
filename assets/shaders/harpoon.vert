#version 450 core
// Spore harpoon pass — vertex stage.
//
// One instanced quad per live hose in sim::HostileSystem::harpoons()
// (src/sim/hostile/Harpoon.h). render::HarpoonGpuInstance is a Renderer.cpp
// implementation detail mirrored only here, the arrangement projectile.vert
// has.
//
// The quad is laid along the nozzle -> anchor segment and padded sideways by
// the hose's widest swing, so harpoon.frag can draw the whole wavy tube, and
// its drop shadow, in one frame: v_s runs along the segment in world units
// (0 at the nozzle), v_t across it.

layout(location = 0) in vec2 a_corner;      // shared unit quad, [-0.5, 0.5]

layout(location = 1) in vec4 i_ends;        // nozzle.xy, anchor.xy
layout(location = 2) in vec4 i_body;        // reach, thickness, age, seed
layout(location = 3) in vec4 i_state;       // attached seconds (<0 flying), release 0..1, severed, slug length
layout(location = 4) in vec4 i_pulses;      // slug positions 0..1 along the hose, <0 = none
layout(location = 5) in vec4 i_shape;       // whip, sway, wavelength, bulge
layout(location = 6) in vec4 i_wall;        // family colour
layout(location = 7) in vec4 i_fluid;       // toxin colour
layout(location = 8) in vec2 i_root;        // spout half-width at the nozzle (0 = none), sprite scale

layout(location = 0) uniform mat4 u_view_projection;

out vec2 v_st;
flat out vec2 v_shadow;
flat out float v_length;
flat out vec4 v_body;
flat out vec4 v_state;
flat out vec4 v_pulses;
flat out vec4 v_shape;
flat out vec4 v_wall;
flat out vec4 v_fluid;
flat out vec2 v_root;

void main() {
    vec2 a = i_ends.xy;
    vec2 b = i_ends.zw;
    vec2 ab = b - a;
    float len = length(ab);
    vec2 dir = len > 1e-4 ? ab / len : vec2(1.0, 0.0);
    vec2 perp = vec2(-dir.y, dir.x);

    // Widest the drawn tube can get: the swing, a slug's swelling, and the
    // drop shadow's offset, with a little slack for the anti-aliased edge.
    float thick = i_body.y;
    // harpoon.frag's swing peaks at 1.3x its amplitude (two harmonics), and
    // its amplitude at 1.45x whip (a ring-down or a limp release) plus sway.
    // The root swells to the spout it grows out of, edged as softly as the
    // sprite is (harpoon.frag).
    float root = i_root.x + 0.09 * i_root.y;
    float half_w = max(1.3 * (1.5 * i_shape.x + i_shape.y) + thick * (1.0 + i_shape.w) * 1.6, root) + 0.9;
    float cap = max(thick * 3.0, root) + 0.6;

    float s = mix(-cap, len + cap, a_corner.x + 0.5);
    float t = a_corner.y * 2.0 * half_w;

    v_st = vec2(s, t);
    // The drop shadow falls a fixed way in the WORLD (down and a little
    // right, like the chaff shadow), so hand it over in the hose's frame.
    const vec2 kShadowWorld = vec2(0.25, -0.7);
    v_shadow = vec2(dot(kShadowWorld, dir), dot(kShadowWorld, perp));
    v_length = len;
    v_body = i_body;
    v_state = i_state;
    v_pulses = i_pulses;
    v_shape = i_shape;
    v_wall = i_wall;
    v_fluid = i_fluid;
    v_root = i_root;

    gl_Position = u_view_projection * vec4(a + dir * s + perp * t, 0.0, 1.0);
}
