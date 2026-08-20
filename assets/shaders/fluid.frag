#version 450 core
// Goblet Cell fluid — pass 1 of 2, fragment stage.
//
// One soft radial blob, summed additively. Nothing here is "the look"; the look
// is entirely fluid_composite.frag's job. What this stage owes that one is a
// smooth, strictly non-negative thickness field plus the two extra quantities
// it cannot recover from thickness alone.
//
// CHANNEL CONTRACT (RGBA16F, additive, all pre-weighted by thickness):
//   R = thickness            summed blob weight
//   G = motion.x * weight    so the composite can average a flow direction
//   B = motion.y * weight
//   A = foam * weight        churn, for the white broken-up look of a splash
//
// G/B/A are MASS-WEIGHTED and must be divided by R on read. Storing them raw
// would let a hundred slow particles in a puddle be outvoted by one fast
// droplet passing over it.

in vec2 v_local;
flat in float v_weight;
flat in float v_foam;
flat in vec2  v_motion;

out vec4 frag_color;

void main() {
    float r = length(v_local);
    if (r >= 1.0) discard;

    // Smooth, compactly-supported falloff — the 2D Wendland-ish (1-r^2)^3.
    // The choice matters: a Gaussian never reaches zero, so its tails would
    // leave a faint haze past the surface that the composite's threshold turns
    // into a permanent grey fringe around every droplet. This one is exactly
    // zero at the rim, and its flat top keeps the field from being lumpy in the
    // interior where the normals are computed.
    float t = 1.0 - r * r;
    float w = t * t * t * v_weight;

    frag_color = vec4(w, v_motion.x * w, v_motion.y * w, v_foam * w);
}
