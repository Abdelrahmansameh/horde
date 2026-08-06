#version 450 core
// Live projectile pass — vertex stage. Owner: Wave 6D.
//
// One instanced quad per live round in sim::ProjectileBuffers
// (src/sim/projectile/Projectiles.h). This is NOT a frozen contract —
// render::ProjectileGpuInstance is a Renderer.cpp implementation detail
// mirrored only here, the same arrangement field.vert has.
//
// These are the Gunner's REAL simulated rounds, and they must not read as just
// another particle. The cosmetic Tracer particles that trail them are long,
// soft, additive and fading; a round is short, hard-edged, alpha-blended and
// constant-brightness — matter, not glow. See projectile.frag.

layout(location = 0) in vec2 a_corner;      // shared unit quad, [-0.5, 0.5]

layout(location = 1) in vec2  i_position;
layout(location = 2) in vec2  i_velocity;
layout(location = 3) in float i_radius;
layout(location = 4) in float i_phase;      // per-round animation offset
layout(location = 5) in vec4  i_tint;
layout(location = 6) in uint  i_visual_id;

layout(location = 0) uniform mat4 u_view_projection;
layout(location = 1) uniform float u_time;

out vec2 v_local;
flat out vec4  v_tint;
flat out float v_aspect;
flat out float v_phase;
flat out uint  v_visual_id;

const float kPad = 1.5;
// Much tighter exposure than a Tracer particle's (0.035s): the round itself is
// a compact slug, the long soft smear behind it is the particle layer's job.
const float kStreakSeconds = 0.010;

void main() {
    float speed = length(i_velocity);
    vec2  dir   = speed > 1e-5 ? i_velocity / speed : vec2(1.0, 0.0);

    float streak = min(speed * kStreakSeconds, i_radius * 6.0);
    float half_w = i_radius;
    float half_l = i_radius + 0.5 * streak;
    vec2  center = i_position - dir * (0.5 * streak);

    float aspect = half_l / max(half_w, 1e-6);

    vec2 corner = a_corner * kPad;
    vec2 perp   = vec2(-dir.y, dir.x);
    vec2 offset = dir * (corner.x * 2.0 * half_l) + perp * (corner.y * 2.0 * half_w);

    v_local     = vec2(corner.x * aspect, corner.y);
    v_tint      = i_tint;
    v_aspect    = aspect;
    v_phase     = i_phase + u_time * 18.0;
    v_visual_id = i_visual_id;

    gl_Position = u_view_projection * vec4(center + offset, 0.0, 1.0);
}
