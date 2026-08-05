#version 450 core
// Tissue substrate — fragment stage.
//
// DESIGN.md §7: "host tissue = warm, low-saturation pinks/creams (the
// 'floor'), must never compete with foreground." This layer only ever reads
// the DistanceField (positive inside walkable tissue) and a slow heartbeat
// phase; contrast and saturation are kept deliberately low so 10k saturated
// chaff sprites always win the eye.

in vec2 v_uv;
out vec4 o_color;

layout(binding = 0) uniform sampler2D u_sdf; // r = signed distance, world units
layout(location = 5) uniform float u_heartbeat_phase;

void main() {
    vec2 uv = clamp(v_uv, 0.0, 1.0);
    float d = texture(u_sdf, uv).r;

    // Soft edge at the vessel wall boundary rather than a hard line — the
    // "vessel" reads as an organic channel, not a rasterized mask.
    float inside = smoothstep(-0.75, 0.75, d);

    const vec3 kOutside = vec3(0.129, 0.086, 0.106); // matches the old flat clear colour
    const vec3 kInside  = vec3(0.58, 0.42, 0.40);    // warm low-saturation pink/cream

    vec3 base = mix(kOutside, kInside, inside);

    // A very low amplitude pulse so it reads as "alive" without competing
    // with any foreground element.
    float pulse = 1.0 + 0.025 * sin(u_heartbeat_phase);
    base *= pulse;

    // Faint vessel-wall rim: a slightly darker band right at the boundary.
    float rim = (1.0 - smoothstep(0.0, 1.1, abs(d))) * 0.06;
    base -= rim;

    // DESIGN.md §9.1: "ambient particulate (drifting cytokines/dust,
    // parallax-scrolls for depth, purely decorative, never occludes gameplay-
    // critical info)". Cheap hash-noise specks, confined to walkable tissue,
    // drifting slowly by reusing u_heartbeat_phase as a general time driver.
    // Amplitude is tiny and additive-only so it can never darken or occlude —
    // worst case it's invisible, never in the way.
    vec2 drift = v_uv * 46.0 + vec2(u_heartbeat_phase * 0.06, u_heartbeat_phase * 0.04);
    vec2 cell = floor(drift);
    vec2 f = fract(drift);
    float h = fract(sin(dot(cell, vec2(127.1, 311.7))) * 43758.5453);
    float speck_dist = length(f - vec2(h, fract(h * 7.0)));
    float speck = (1.0 - smoothstep(0.05, 0.14, speck_dist)) * step(0.93, h);
    base += speck * 0.05 * inside;

    o_color = vec4(max(base, vec3(0.0)), 1.0);
}
