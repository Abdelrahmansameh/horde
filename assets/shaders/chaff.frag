#version 450 core
// Chaff instanced sprite pass — fragment stage.
// Procedural only: the pathogen silhouette is an SDF, never a texture
// (no binary assets in this project). Wave 1C owns the body.

in vec2  v_local;
in vec4  v_tint;
flat in uint v_flags;
in float v_anim_phase;

out vec4 o_color;

// Mirrors sim/chaff/ChaffBuffers.h chaff_flags.
const uint FLAG_MARKED  = 1u << 1;
const uint FLAG_SLOWED  = 1u << 2;
const uint FLAG_CLUMPED = 1u << 3;
const uint FLAG_HIDDEN  = 1u << 4;

void main() {
    float d = length(v_local) - 0.5;
    float alpha = 1.0 - smoothstep(-0.06, 0.0, d);
    if (alpha <= 0.0) discard;

    vec3 rgb = v_tint.rgb;
    if ((v_flags & FLAG_MARKED) != 0u) rgb = mix(rgb, vec3(1.0), 0.25);
    if ((v_flags & FLAG_HIDDEN) != 0u) alpha *= 0.35;

    o_color = vec4(rgb, alpha * v_tint.a);
}
