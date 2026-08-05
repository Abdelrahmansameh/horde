#version 450 core
// Chaff instanced sprite pass — fragment stage.
// Procedural only: the pathogen silhouette is an SDF, never a texture
// (no binary assets in this project). Wave 1C owns the body.
//
// DESIGN.md §6 readability rule: colour = family (v_tint, set by the CPU
// batcher from render::family_color), silhouette size = threat tier (baked
// into i_scale before this stage runs), animation tempo = speed tier (baked
// into v_anim_phase's rate of advance). This shader only adds the *within*
// family texture: a small tempo-driven pulse plus the drop shadow.

in vec2  v_local;
in vec4  v_tint;
flat in uint v_flags;
in float v_anim_phase;
in float v_wobble;
flat in vec2 v_shadow_offset;

out vec4 o_color;

// Mirrors sim/chaff/ChaffBuffers.h chaff_flags.
const uint FLAG_MARKED  = 1u << 1;
const uint FLAG_SLOWED  = 1u << 2;
const uint FLAG_CLUMPED = 1u << 3;
const uint FLAG_HIDDEN  = 1u << 4;

void main() {
    // Tempo pulse: small so 10k instances read as "alive", not "flickering".
    float pulse = sin(v_anim_phase) * 0.05 * v_wobble;
    float body_d = length(v_local) - (0.5 + pulse);
    float body_alpha = 1.0 - smoothstep(-0.06, 0.0, body_d);

    float shadow_d = length(v_local - v_shadow_offset) - 0.46;
    float shadow_alpha = (1.0 - smoothstep(-0.14, 0.0, shadow_d)) * 0.28;

    if (body_alpha <= 0.0 && shadow_alpha <= 0.0) discard;

    vec3 rgb = v_tint.rgb;
    if ((v_flags & FLAG_MARKED) != 0u) rgb = mix(rgb, vec3(1.0), 0.25);
    if ((v_flags & FLAG_SLOWED) != 0u) rgb = mix(rgb, vec3(0.55, 0.75, 1.0), 0.35);

    float body_a = body_alpha * v_tint.a;
    if ((v_flags & FLAG_HIDDEN) != 0u) body_a *= 0.35;

    // Standard "body over shadow" compositing so the whole sprite + shadow
    // resolves to one straight-alpha output for the destination blend.
    const vec3 kShadowRgb = vec3(0.03, 0.02, 0.03);
    float out_a = body_a + shadow_alpha * (1.0 - body_a);
    if (out_a <= 0.001) discard;
    vec3 out_rgb = (rgb * body_a + kShadowRgb * shadow_alpha * (1.0 - body_a)) / out_a;

    o_color = vec4(out_rgb, out_a);
}
