#version 450 core
// Cytotoxic T swarmer pass — fragment stage.
//
// One lytic granule. There will be dozens to a couple of hundred of these on
// screen at once, so every decision here is really a decision about what a
// CLOUD of them looks like:
//
//   - The silhouette wobbles. A perfect circle repeated eighty times reads as
//     bubbles or as UI; a membrane that bulges differently per granule and per
//     moment reads as a lot of small living things.
//   - Alpha-blended and near-constant brightness, like the projectile pass and
//     unlike the particle pass. These are matter. If they glowed additively
//     they would stack into one bright smear exactly where the swarm is
//     densest, which is the one place the player needs to read individuals.
//   - A granule that has LATCHED onto a host is visibly hotter and grows a
//     small feeding halo. That is the only state change the player has to be
//     able to see at a glance, so it is the only thing given a hard contrast
//     step rather than a subtle one.

in vec2 v_local;
flat in vec4  v_tint;
flat in float v_phase;
flat in uint  v_flags;
flat in vec2  v_heading;

out vec4 frag_color;

const uint kAttached = 1u;

void main() {
    float r = length(v_local);
    float ang = atan(v_local.y, v_local.x);

    // Membrane wobble: two out-of-step lobed terms so the outline never
    // resolves into a clean polygon the way a single cos(n*ang) does.
    float wobble = 0.055 * sin(ang * 3.0 + v_phase)
                 + 0.035 * sin(ang * 5.0 - v_phase * 0.7 + 1.7);

    // Slight forward taper along the heading, so a swimming granule has a
    // recognisable front and the cloud shows its direction of travel without
    // anyone stretching the quad.
    float along = dot(normalize(v_local + vec2(1e-5)), v_heading);
    float taper = 0.030 * along;

    float edge = 0.42 + wobble + taper;
    float body = 1.0 - smoothstep(edge - 0.055, edge, r);
    if (body <= 0.003) discard;

    bool attached = (v_flags & kAttached) != 0u;

    // Dense core, softer rind — the same nucleus-in-cytoplasm read the tower
    // that released it has, at a fortieth of the size.
    float core = 1.0 - smoothstep(0.0, 0.20, r);
    vec3 rgb = mix(v_tint.rgb, vec3(1.0), core * 0.75);

    float alpha = v_tint.a * body;

    if (attached) {
        // Feeding: hotter core, and a faint halo just outside the membrane so a
        // clump of latched granules on one pathogen glows as a group.
        rgb = mix(rgb, vec3(1.0), 0.35 + 0.25 * (0.5 + 0.5 * sin(v_phase * 2.3)));
        float halo = (1.0 - smoothstep(edge, edge + 0.10, r)) * (1.0 - body);
        alpha = max(alpha, halo * 0.45 * v_tint.a);
    }

    frag_color = vec4(rgb, alpha);
}
