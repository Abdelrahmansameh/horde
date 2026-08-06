#version 450 core
// Live projectile pass — fragment stage. Owner: Wave 6D.
//
// A round must read as MATTER, not glow, so it stays legible against the
// Gunner's own additive tracer storm. The cosmetic Tracer particles trailing it
// are long, soft, additive and fading; this is short, hard-edged, and holds a
// near-constant brightness for its whole flight. If the two ever start looking
// alike, harden this one — the round is the thing the player is actually
// tracking, the smear behind it is atmosphere.
//
// v_local is the capsule frame from projectile.vert: |y| = 0.5 is the true edge
// across the round's width, |x| = 0.5 * v_aspect the edge along its travel.

in vec2 v_local;
flat in vec4  v_tint;
flat in float v_aspect;
flat in float v_phase;
flat in uint  v_visual_id;

out vec4 frag_color;

/// Distance to a capsule's surface in the normalized local frame. Degenerates
/// to a circle at aspect == 1, so one expression covers a stationary round and
/// a fast-moving streaked one.
float capsule_sd(vec2 p, float aspect) {
    float half_span = max(aspect * 0.5 - 0.5, 0.0);
    p.x -= clamp(p.x, -half_span, half_span);
    return length(p);
}

void main() {
    float d = capsule_sd(v_local, v_aspect);

    // Hard edge. Deliberately a much tighter falloff than the particle pass
    // uses -- this is the silhouette of a physical slug.
    float body = 1.0 - smoothstep(0.42, 0.5, d);
    if (body <= 0.001) discard;

    // Hot core -> tinted rim. The core stays near-white so a round is visible
    // even on top of a saturated additive bloom.
    float core = 1.0 - smoothstep(0.0, 0.30, d);
    vec3 rgb = mix(v_tint.rgb, vec3(1.0), core * 0.85);

    // Tier escalation: higher-tier rounds carry a faint travelling shimmer
    // along their length so a maxed Gunner's stream reads as hotter without
    // changing the silhouette.
    if (v_visual_id >= 3u) {
        float shimmer = 0.5 + 0.5 * sin(v_phase + v_local.x * 6.0);
        rgb += v_tint.rgb * shimmer * 0.25;
    }

    frag_color = vec4(rgb, v_tint.a * body);
}
