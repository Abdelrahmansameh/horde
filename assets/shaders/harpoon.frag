#version 450 core
// Spore harpoon pass — fragment stage.
//
// A HOSE, NOT A BEAM, and not a separate object either: it is the spore's
// own membrane drawn out of its spout. Same colour, same ramp (bright at the
// edge, lit inside), same white rim and the same soft edge chaff.frag gives
// the body, measured in the same sprite-local units -- so where the two
// passes meet there is nothing for the eye to catch on. The root is as wide
// as the spout and sits on its mouth (Renderer.cpp's spore_mouth() follows
// the body through every squash and recoil), then necks down to the hose's
// own gauge. The toxin shows through down the middle.
//
// THE MOTION is all the liquid feel there is, so it carries the design:
//   FLYING    the free end lashes: a travelling wave runs from the nozzle out
//             to the tip, growing toward it, and the tip carries a bead.
//   ANCHORED  both ends are held, so the lash rings down onto a pinned
//             standing wave (a damped spring), leaving a slow sway; the tip
//             flares into a sucker on the target's membrane.
//   SLUGS     each pulse is a swelling of the tube filled with bright toxin,
//             teardrop-shaped (steep at the front, pushing), travelling the
//             hose at the sim's own speed -- the sim deals the damage the
//             moment one reaches the end, so the eye can follow the hit in.
//   RELEASE   reeling in (the sim shrinks `reach`) the hose goes limp; a
//             severed one fades where it lies.
//
// Distances are world units throughout (v_st), so thickness and wavelength
// mean the same thing at every zoom.

in vec2 v_st;
flat in vec2 v_shadow;   // the drop shadow's offset, in this frame
flat in float v_length;
flat in vec4 v_body;     // reach, thickness, age, seed
flat in vec4 v_state;    // attached seconds (<0 flying), release 0..1, severed, slug length
flat in vec4 v_pulses;   // slug positions 0..1, <0 = none
flat in vec4 v_shape;    // whip, sway, wavelength, bulge
flat in vec4 v_wall;
flat in vec4 v_fluid;
flat in vec2 v_root;     // spout half-width at the nozzle (0 = none), sprite scale

layout(location = 1) uniform float u_shadows;   // render::kShadowsEnabled

out vec4 o_color;

const float kPi = 3.14159265;
// The spore sprite's membrane, sprite-local units (chaff.frag's FAM_SPORE
// branch): its soft edge, 0.06 * rim_scale 0.5, and its rim_band.
const float kSporeSoftEdge = 0.03;
const float kSporeRimBand = 0.06;

struct Hose {
    float y;      // centreline offset across the segment
    float slope;  // dy/ds, for a perpendicular distance
    float rad;    // half-width
    float slug;   // 0..1 how much of a slug is here (swells the tube)
    float lit;    // 0..1 how much bright toxin shows here (slugs and the flying tip)
    float neck;   // 0 at the spout, 1 once the hose is its own gauge
};

Hose hose_at(float s) {
    float reach = max(v_body.x, 1e-3);
    float thick = v_body.y;
    float age = v_body.z;
    float seed = v_body.w;
    float attached = v_state.x;
    float release = v_state.y;
    float slug_len = max(v_state.w, 0.1);
    float whip = v_shape.x;
    float sway = v_shape.y;
    float k = 2.0 * kPi / max(v_shape.z, 0.5);

    float u = clamp(s / reach, 0.0, 1.0);           // 0 at the nozzle, 1 at the tip
    // How far through the neck (below), which is part of the body and does
    // not swing: the wave starts past it, or the spout pokes out beside it.
    float x = v_root.x > 0.0 ? s / (2.2 * v_root.x) : 9.0;
    float rooted = smoothstep(1.0, 2.2, x);
    // Rooted at the nozzle, swinging hardest at the free end: a lash.
    float env_free = smoothstep(0.0, 0.3, u) * (0.35 + 0.65 * u) * rooted;
    // Both ends held: a standing wave between them.
    float env_held = sin(kPi * u) * rooted;

    float amp;
    if (attached < 0.0) {
        amp = whip * env_free;
    } else {
        // The lash rings down onto the pinned wave like a plucked string.
        float ring = exp(-attached * 6.5) * cos(attached * 21.0);
        amp = (whip * 0.75 * ring + sway) * env_held;
    }
    // Let go: the hose goes limp as it is reeled in.
    amp += release * whip * 0.7 * env_free;

    // Crests travel nozzle -> tip (phase k s - w t): the lash is thrown, and
    // once anchored the same direction reads as liquid flowing into the cell.
    float w = attached < 0.0 ? 16.0 : 5.0;
    float a1 = k * s - w * age + seed;
    float a2 = 2.3 * k * s - 1.6 * w * age + seed * 1.7;
    Hose h;
    h.y = amp * (sin(a1) + 0.3 * sin(a2));
    h.slope = amp * k * (cos(a1) + 0.69 * cos(a2));

    // Slugs: a teardrop swelling, steep at the leading edge.
    float slug = 0.0;
    for (int i = 0; i < 4; ++i) {
        float p = v_pulses[i];
        if (p < 0.0) continue;
        float d = (s - p * v_length) / slug_len;
        d *= d > 0.0 ? 1.6 : 0.75;
        slug = max(slug, exp(-d * d * 2.2));
    }
    h.slug = slug;
    // The flying tip carries the toxin's leading drop, lit like a slug but
    // not swollen like one, so the eye can follow the throw to the cell.
    h.lit = attached < 0.0 ? max(slug, exp(-pow((s - reach) / (0.5 * slug_len), 2.0))) : slug;

    // A little thicker at the nozzle, finer toward the tip, and never
    // perfectly even: a peristaltic shimmer runs down it with the flow.
    // At the root it is the spout's width, necking down in a fillet --
    // steepest where it leaves the body, level by the time it is hose -- and
    // the neck holds still: it is part of the body.
    float gauge = thick * mix(1.18, 0.82, u);
    float xn = clamp(x, 0.0, 1.0);
    h.neck = 1.0 - (1.0 - xn) * (1.0 - xn);
    float rad = v_root.x > 0.0 ? mix(v_root.x, gauge, h.neck) : gauge;
    rad *= 1.0 + 0.07 * h.neck * sin(k * 2.7 * s - age * 13.0 + seed);
    // A slug swells the hose by its own gauge. Not through the neck, which
    // is already wide: swelling past the spout there shows the root's cut.
    rad += gauge * v_shape.w * slug * h.neck;
    if (attached < 0.0) {
        // The bead on the flying tip: the toxin's leading drop.
        rad *= 1.0 + 0.4 * smoothstep(0.85, 0.98, u);
    } else {
        // The sucker: the tip flares onto the membrane as it bites.
        rad *= 1.0 + 0.85 * smoothstep(0.9, 1.0, u) * clamp(attached * 9.0, 0.0, 1.0);
    }
    h.rad = rad;
    return h;
}

// Signed distance to the tube, with round caps past either end.
float hose_d(vec2 st, out Hose h, out float lat) {
    float reach = max(v_body.x, 1e-3);
    float sc = clamp(st.x, 0.0, reach);
    h = hose_at(sc);
    float perp = (st.y - h.y) * inversesqrt(1.0 + h.slope * h.slope);
    lat = perp / max(h.rad, 1e-4);
    return length(vec2(st.x - sc, perp)) - h.rad;
}

void main() {
    if (v_body.x <= 0.0) discard;
    float px = max(fwidth(v_st.y), 1e-4);

    Hose h;
    float lat;
    float d = hose_d(v_st, h, lat);

    // Drop shadow: the same tube moved along the world's shadow direction,
    // under the same global switch as every other shadow.
    float shadow_a = 0.0;
    if (u_shadows > 0.0) {
        Hose hs;
        float lat_s;
        float ds = hose_d(v_st - v_shadow, hs, lat_s);
        shadow_a = (1.0 - smoothstep(-hs.rad * 0.5, hs.rad * 0.45, ds)) * 0.34 * u_shadows;
    }

    // Distances in the spore sprite's own units, so every band below is the
    // one chaff.frag draws the body with. Where the hose is thinner than
    // those bands they shrink with it, or it would be all rim and no tube.
    float scale = max(v_root.y, 1e-3);
    float d_l = d / scale;
    float rad_l = h.rad / scale;
    // The sprite's soft edge at the spout, firming up along the neck so the
    // hose keeps a solid core.
    float soft = mix(kSporeSoftEdge * scale, 0.35 * h.rad, h.neck);
    float body_a = 1.0 - smoothstep(-max(soft, px), 0.0, d);
    // The root is already inside the body's opaque edge, and behind it is
    // more body: it fades in there instead of capping, so no outline of the
    // hose's own is drawn over the cell.
    if (v_root.x > 0.0) body_a *= smoothstep(-kSporeSoftEdge * scale, 0.0, v_st.x);

    float release = v_state.y;
    float fade = v_state.z > 0.5 ? 1.0 - smoothstep(0.0, 1.0, release)
                                 : 1.0 - smoothstep(0.75, 1.0, release);
    body_a *= fade;
    shadow_a *= fade;
    if (body_a <= 0.001 && shadow_a <= 0.001) discard;

    // ---- the tube ---------------------------------------------------------
    // The body's membrane (chaff.frag, "Cheap interior shading"): bright at
    // the edge, lit inside, a white rim. Then what it carries: the toxin
    // drifting down the middle, and the slugs. A slug reads by its SHAPE --
    // the hose swelling round a lump of liquid -- and only faintly by
    // colour: a bright core racing down a line reads as a current, not a
    // fluid.
    float al = clamp(abs(lat), 0.0, 1.0);
    float cyl = sqrt(max(0.0, 1.0 - al * al));     // 1 on the centre line
    float age = v_body.z;
    float seed = v_body.w;
    float depth = clamp(-d_l * 3.4, 0.0, 1.0);
    vec3 col = v_wall.rgb * mix(1.30, 1.08, depth);
    float rim = 1.0 - smoothstep(0.0, min(kSporeRimBand, 1.3 * rad_l), abs(d_l));
    col = mix(col, mix(col, vec3(1.0), 0.72), rim * 0.80);

    vec3 toxin = mix(v_fluid.rgb, vec3(1.0), 0.35);
    // Filaments drifting toward the target, so the hose never looks still.
    // Slow and faint: fast bright streaks were most of what read as
    // electricity. Not through the neck, which is still the body.
    float channel = (1.0 - smoothstep(0.1, 0.55, al)) * h.neck;
    float stream = smoothstep(0.5, 1.0, sin(v_st.x * 2.6 - age * 9.0 + seed + lat * 1.3));
    col = mix(col, toxin, channel * (0.1 + 0.12 * stream));

    // A slug: the swollen hose already shades as a fuller, deeper tube (the
    // ramp above); the toxin shows through its middle only a little, with a
    // small wet gleam on the lit side.
    float slug = h.lit;
    col = mix(col, toxin, slug * 0.25 * cyl);
    float spec = exp(-pow((lat - 0.45) / 0.17, 2.0));
    col = mix(col, vec3(1.0, 0.99, 0.94), spec * 0.18 * slug);

    // Severed: the hose dies with its spore.
    if (v_state.z > 0.5) col = mix(col, col * 0.55, release);

    const vec3 kShadowRgb = vec3(0.03, 0.02, 0.03);
    float out_a = body_a + shadow_a * (1.0 - body_a);
    if (out_a <= 0.001) discard;
    vec3 out_rgb = (col * body_a + kShadowRgb * shadow_a * (1.0 - body_a)) / out_a;
    o_color = vec4(out_rgb, out_a);
}
