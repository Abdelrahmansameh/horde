#version 450 core
// Density-LOD blob pass — fragment stage.
//
// DESIGN.md §8.5 / docs/ARCHITECTURE.md §5: where local occupancy exceeds
// lod_blob_threshold, agents stop being drawn as instances and instead
// deposit density into a low-res texture (rgb premultiplied by mass, a =
// mass). This shader recovers the mass-weighted colour and turns accumulated
// mass into a soft, shader-driven "mass" look — deliberately not crisp, so
// the crossfade against the sprite pass (which fades OUT over the same band)
// reads as one continuous material rather than two competing techniques.

in vec2 v_uv;
out vec4 o_color;

layout(binding = 0) uniform sampler2D u_density;
layout(location = 3) uniform float u_mass_scale;

void main() {
    vec4 texel = texture(u_density, v_uv);
    float mass = texel.a;
    if (mass <= 0.0005) discard;

    vec3 rgb = texel.rgb / max(mass, 1e-4);
    // Exponential falloff: never fully opaque, so a very dense pocket still
    // reads as "a lot of mass" rather than clipping to a flat colour disc.
    float alpha = 1.0 - exp(-mass * u_mass_scale);
    alpha = clamp(alpha, 0.0, 0.92);

    o_color = vec4(rgb, alpha);
}
