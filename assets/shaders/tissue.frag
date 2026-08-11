#version 450 core
// Tissue substrate — fragment stage. Procedural only; this project ships no
// binary art.
//
// ---------------------------------------------------------------------------
// WHAT THIS LAYER IS
// ---------------------------------------------------------------------------
// Everything behind the action: the interstitial flesh the level is carved out
// of, the vessel wall, and the plasma inside the lumen that the horde swims
// through. It is drawn as ONE fullscreen quad over the camera's visible extent
// and reconstructs all of that from three small textures:
//
//   u_sdf   R32F   signed distance to the vessel wall, world units, + inside.
//   u_flow  RGB16F rg = unit flow direction toward the objective,
//                  b  = normalised cost-to-goal in [0,1] (1 = far, 0 = at it).
//   u_lane  RGBA8  rgb = this point's lane hue, a = lane tempo / 2
//                  (DESIGN.md §9.2's per-vessel-type identity, flooded and
//                  blurred CPU-side so it varies smoothly).
//
// ---------------------------------------------------------------------------
// THE THREE IDEAS THAT DO THE WORK
// ---------------------------------------------------------------------------
// 1. THE SDF IS A MATERIAL COORDINATE, NOT JUST A MASK. `d` is a real distance
//    in world units, so banding on it gives layers *parallel to the wall* for
//    free — that is how the wall gets concentric elastic laminae without any
//    per-vessel geometry. Its gradient is the wall normal, which is what the
//    fake directional light shades against; that single term is what turns a
//    flat cut-out silhouette into a channel carved into flesh.
//
// 2. THE EDGE IS WARPED BEFORE ANYTHING READS IT. The mask is rasterized by
//    stamping discs along a spline (sim/flowfield/TissueRaster.h), which leaves
//    a faintly polygonal, stair-stepped boundary that a hard smoothstep shows
//    off badly. Adding a two-octave noise offset to `d` up front both hides
//    that and — far more importantly — makes the vessel read as something grown
//    rather than something stamped. Every later band inherits the irregularity
//    because they all key off the same warped `d`.
//
// 3. THE LUMEN FLOWS ALONG THE ACTUAL FLOW FIELD. Plasma striations are value
//    noise smeared with a 5-tap line integral along `u_flow`, so they elongate
//    into streamlines that follow the exact field the horde is steering on, and
//    advecting the sample point makes them travel. The cost-to-goal channel
//    then gives a genuine along-vessel coordinate, which is what lets a systolic
//    pressure wave propagate *up the vessel toward the objective* instead of
//    just pulsing everywhere at once (DESIGN.md §9.2: "arterial lanes pulse
//    faster", felt before it is understood).
//
// ---------------------------------------------------------------------------
// BUDGET / READABILITY
// ---------------------------------------------------------------------------
// This is one fullscreen quad per frame, so it can afford real fbm and Worley
// where chaff.frag cannot — but it is also the layer DESIGN.md §9.3 says "must
// never compete with foreground". So the rule here is: lots of *structure*,
// very little *contrast*. Detail earns its place by making the substrate read
// as living tissue at a glance; the moment a term starts pulling the eye away
// from the horde or a tower, its amplitude is wrong, not its idea. The two
// halves are branched (interstitium vs. lumen) and the branch is spatially
// coherent over large screen regions, so the divergence cost is negligible.

in vec2 v_uv;
in vec2 v_world;
in vec2 v_screen;

out vec4 o_color;

layout(binding = 0) uniform sampler2D u_sdf;
layout(binding = 1) uniform sampler2D u_flow;
layout(binding = 2) uniform sampler2D u_lane;

layout(location = 5) uniform float u_heartbeat_phase;
layout(location = 6) uniform float u_time;
layout(location = 7) uniform float u_have_flow;
layout(location = 8) uniform float u_have_lane;

// ===========================================================================
// Noise toolkit. Deliberately the same hash/vnoise/fbm as entity.frag's
// organic-shading section: the towers and the tissue they sit in should be
// built out of the same grain, or the towers read as decals on a backdrop.
// ===========================================================================

float hash21(vec2 p) {
    p = fract(p * vec2(127.1, 311.7));
    p += dot(p, p + 34.23);
    return fract(p.x * p.y);
}

float vnoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    return mix(mix(hash21(i + vec2(0, 0)), hash21(i + vec2(1, 0)), u.x),
               mix(hash21(i + vec2(0, 1)), hash21(i + vec2(1, 1)), u.x), u.y);
}

float fbm(vec2 p) {
    float v = 0.0, a = 0.5;
    for (int k = 0; k < 4; ++k) {
        v += a * vnoise(p);
        p *= 2.02;
        a *= 0.5;
    }
    return v;
}

/// Worley / cellular noise. Returns (F1, F2, cell hash).
///
/// This is THE primitive for the interstitium: tissue is literally a packing of
/// cells, and F2-F1 is the distance to the seam between two of them, which is
/// exactly the membrane. F1 alone doubles as the distance to that cell's
/// centre, so the nucleus comes out of the same nine-cell loop for free.
vec3 worley(vec2 p) {
    vec2 ip = floor(p);
    vec2 fp = fract(p);
    float f1 = 8.0, f2 = 8.0, id = 0.0;
    for (int j = -1; j <= 1; ++j) {
        for (int i = -1; i <= 1; ++i) {
            vec2 g = vec2(float(i), float(j));
            float h = hash21(ip + g);
            vec2 feature = g + vec2(h, fract(h * 41.73));
            float dd = length(feature - fp);
            if (dd < f1) { f2 = f1; f1 = dd; id = h; }
            else if (dd < f2) { f2 = dd; }
        }
    }
    return vec3(f1, f2, id);
}

// ---------------------------------------------------------------------------
// INTERSTITIUM — the flesh the vessel is carved out of.
//
// Three scales stacked, because that is what stops any procedural surface from
// reading as wallpaper: lobules (organ substructure, ~13 world units), cells
// (~3 world units), and ridged collagen fibre bundles threading between them.
// The whole thing is domain-warped first so the packing is irregular rather
// than a tidy Voronoi diagram.
//
// Kept very dark and very low contrast on purpose — this is the majority of the
// screen on a narrow-vessel level and it is the one region that must never
// attract the eye.
// ---------------------------------------------------------------------------
vec3 interstitium(vec2 p, vec3 lane_hue) {
    vec2 wp = p + vec2(fbm(p * 0.14), fbm(p * 0.14 + 31.4)) * 3.8;

    vec3 cells = worley(wp * 0.22);
    vec3 lobes = worley(wp * 0.055);

    // 0 exactly on the seam between two cells, 1 well inside one. The contrast
    // here is deliberately weak: strong seams turn the whole field into cracked
    // stone and start reading as level geometry, which is the one thing the
    // back layer must never do.
    float membrane = smoothstep(0.015, 0.20, cells.y - cells.x);
    float nucleus  = 1.0 - smoothstep(0.06, 0.15, cells.x);

    vec3 col = vec3(0.083, 0.048, 0.058);
    // Per-cell brightness jitter: cytoplasm is never uniform, and this is what
    // makes the packing legible without drawing an outline around every cell.
    col *= 0.90 + 0.20 * cells.z;
    col *= mix(0.84, 1.03, membrane);
    col = mix(col, vec3(0.108, 0.060, 0.086), nucleus * 0.28);

    // Lobule-scale shading: a slow swell across many cells at once, so the
    // texture has a silhouette at screen scale and not only at pixel scale, and
    // a cool/warm shift with it so the field is not one flat maroon.
    float lobe = smoothstep(0.04, 0.60, lobes.y - lobes.x);
    col *= 0.88 + 0.22 * lobe;
    col = mix(col, col * vec3(0.86, 0.94, 1.14), (1.0 - lobe) * 0.45);

    // Collagen. Ridged noise (1 - |2n-1|) gives creases instead of blobs, and
    // the anisotropic frequency stretches them into fibre bundles.
    float fibre = 1.0 - abs(2.0 * fbm(p * vec2(0.40, 0.90) + 7.0) - 1.0);
    col += vec3(0.020, 0.019, 0.028) * pow(fibre, 4.0);

    // Micro-vasculature: the same ridged trick at a much higher exponent, which
    // narrows the crest into a thin branching filament rather than a band. Warm
    // and very faint — a capillary bed feeding the flesh, readable only when
    // the eye rests on it.
    float capillary = pow(1.0 - abs(2.0 * fbm(p * 0.17 + 19.0) - 1.0), 15.0);
    col += vec3(0.055, 0.017, 0.021) * capillary;

    // A whisper of the lane's identity, so the surrounding flesh belongs to the
    // vessel running through it rather than being a neutral backdrop.
    col = mix(col, col * (0.55 + lane_hue), 0.40);
    return col;
}

void main() {
    vec2 uv = clamp(v_uv, 0.0, 1.0);
    vec2 p = v_world;

    float d_raw = texture(u_sdf, uv).r;

    // ---- Wall normal, by central difference on the distance field ----------
    // Points toward increasing clearance, i.e. into the lumen. Only its
    // direction is used, so the texel-to-world scale never has to be known.
    vec2 texel = 1.0 / vec2(textureSize(u_sdf, 0));
    float gx = texture(u_sdf, uv + vec2(texel.x, 0.0)).r -
               texture(u_sdf, uv - vec2(texel.x, 0.0)).r;
    float gy = texture(u_sdf, uv + vec2(0.0, texel.y)).r -
               texture(u_sdf, uv - vec2(0.0, texel.y)).r;
    vec2 nrm = vec2(gx, gy);
    float nlen = length(nrm);
    nrm = nlen > 1e-6 ? nrm / nlen : vec2(0.0, 1.0);

    // ---- Organic edge warp (see idea 2 up top) -----------------------------
    // Two octaves and no more: the low one bends the vessel's course, the high
    // one roughens its wall. A third, finer octave was tried and read as a
    // stippled dissolve at gameplay zoom rather than as texture — the boundary
    // has to stay a *line*, because §9.4 makes the horde's leading edge legible
    // against it.
    float d = d_raw
            + (fbm(p * 0.20) - 0.5) * 1.20
            + (vnoise(p * 0.62) - 0.5) * 0.36;

    // ---- Lane identity -----------------------------------------------------
    vec3 lane_hue = vec3(0.76, 0.34, 0.30);  // arterial default
    float tempo = 1.0;
    if (u_have_lane > 0.5) {
        vec4 lane = texture(u_lane, uv);
        lane_hue = lane.rgb;
        tempo = max(lane.a * 2.0, 0.25);
    }

    // ---- Flow direction + along-vessel progress ----------------------------
    vec2 flow = vec2(1.0, 0.0);
    float to_goal = 0.5;
    if (u_have_flow > 0.5) {
        vec3 f = texture(u_flow, uv).rgb;
        float flen = length(f.xy);
        // FlowField::sample returns (0,0) for "no guidance" (outside the field
        // or an unreachable pocket) — never normalise that.
        if (flen > 0.08) flow = f.xy / flen;
        to_goal = f.b;
    }

    // =======================================================================
    // BANDS
    //
    // Three materials, resolved as explicit masks in world units rather than
    // as one blurry ramp, because each of them wants a different edge:
    //
    //   lumen   d >  0        crisp, ~0.4 units of transition. This edge is
    //                         gameplay-critical (§9.4) and gets no more
    //                         softness than antialiasing needs.
    //   wall    d in [-W, 0]  a solid ring of muscle with real thickness,
    //                         directionally lit so the vessel reads as a
    //                         channel and not a hole cut in paper.
    //   flesh   d < -W        soft blend outward, so the wall sits *in* the
    //                         tissue instead of on top of it.
    // =======================================================================
    const float kWallThick = 2.6;

    float inside = smoothstep(-0.12, 0.32, d);
    // 0 at the lumen boundary, 1 at the wall's outer surface.
    float wall_t = clamp(-d / kWallThick, 0.0, 1.0);
    float in_wall = (1.0 - inside) * (1.0 - smoothstep(0.55, 1.0, wall_t));

    // Fake directional light for the §9.1 tilted-camera read. `nrm` points into
    // the lumen, so the wall facing the light catches a highlight and the far
    // side falls away. This single term is what gives the vessel its relief;
    // the direction matches the drop-shadow offset the sprite passes use.
    const vec2 kLight = vec2(-0.42, 0.91);
    float lit = dot(nrm, kLight);

    // Skipped outright deep inside a lumen. On a floodplain level the lane is
    // most of the screen, and the interstitium is by far the most expensive
    // thing here (two Worley lookups plus four fbm); paying for it under a
    // fully opaque plasma is the one easy win this pass has.
    vec3 col = vec3(0.0);
    if (inside < 0.997) col = interstitium(p, lane_hue);

    // The flesh right outside the wall, treated as the lip of a groove: a
    // gentle occlusion all the way round to seat the vessel into the tissue,
    // plus a highlight on whichever side rolls over toward the light. Biasing
    // the *occlusion* by `lit` instead was tried and reads backwards — it puts
    // the heavy shadow on the lip the light is hitting.
    float lip = (1.0 - smoothstep(0.0, 4.5, -d - kWallThick * 0.5)) * (1.0 - inside);
    col *= 1.0 - 0.24 * lip;
    col *= 1.0 + 0.30 * lip * max(-lit, 0.0);

    if (inside > 0.004) {
        // Plasma. Warm and pale per DESIGN.md §9.3's "pinks/creams floor",
        // carrying only a minority fraction of the lane hue so lane identity
        // never eats the saturation budget the pathogen families need.
        vec3 plasma = mix(vec3(0.60, 0.47, 0.45), lane_hue, 0.42);

        // The lumen is a channel with depth: it lifts toward the middle and
        // falls into contact shadow against the wall. `d` is in world units, so
        // this is a physically consistent gradient at any vessel width.
        float depth = smoothstep(0.0, 8.0, d);
        plasma *= mix(0.58, 0.98, depth);
        // Contact shadow where the plasma meets the wall. Split into a constant
        // part and a directional part on purpose: the plasma touches the wall
        // on BOTH sides, so both get a contact shadow, and only its depth
        // follows the light. Driving the whole term off `lit` gave the lit side
        // essentially no contact shadow at all, which was half of why the two
        // edges of a lane read as different materials.
        plasma *= 1.0 - (1.0 - depth) * (0.09 + 0.13 * (0.5 - 0.5 * lit));

        // Flow-aligned striations. Seven taps of value noise along the flow
        // direction are a miniature line-integral convolution: it destroys
        // detail across the streamline and keeps it along the streamline, which
        // is what turns round noise blobs into filaments of moving fluid.
        // Advecting the sample point upstream makes those filaments travel
        // downstream at the lane's own tempo.
        //
        // Tap spacing MUST stay well under the noise's wavelength (1.0 in `sp`
        // space). Stepping by ~1.0 samples the same phase of the noise every
        // time, so the taps correlate instead of averaging out across the
        // streamline, and the result is just the original isotropic blobs —
        // the smear silently does nothing.
        vec2 sp = p * 0.80 - flow * (u_time * 1.00 * tempo);
        float lic = 0.0;
        for (int k = -3; k <= 3; ++k) lic += vnoise(sp + flow * (float(k) * 0.38));
        lic *= 1.0 / 7.0;
        // Centred on the mean so the streaks both brighten and darken; a purely
        // additive smear just washes the lane out. The gain is high because
        // averaging seven taps has already collapsed most of the variance.
        plasma *= 1.0 + 0.52 * (lic - 0.5);

        // A faster, finer filament layer over the top, strongest where the
        // plasma drags against the wall — the same place a real velocity
        // profile has its steepest gradient.
        vec2 sp2 = p * 2.10 - flow * (u_time * 1.90 * tempo);
        float fine = 0.0;
        for (int k = -2; k <= 2; ++k) fine += vnoise(sp2 + flow * (float(k) * 0.34));
        fine *= 0.2;
        plasma *= 1.0 + 0.26 * (fine - 0.5) * (1.0 - 0.55 * depth);

        // Large-scale density mottling. A floodplain lumen can be most of the
        // screen, and without a term at this scale the streamlines tile it with
        // one uniform grain; this gives the lane a shape of its own at the
        // distance the level is actually read from.
        plasma *= 0.90 + 0.20 * fbm(p * 0.075 + 4.0);

        // Systolic pressure wave, travelling along the vessel toward the
        // objective. `to_goal` is the only genuine along-vessel coordinate
        // available, which is exactly what this needs.
        plasma *= 1.0 + 0.055 * sin(to_goal * 26.0 + u_heartbeat_phase * tempo);

        // Corpuscles drifting downstream. Deliberately near the threshold of
        // visibility: they add life to an empty lane and must vanish under a
        // horde rather than dot it.
        vec2 cp = (p - flow * (u_time * 1.9 * tempo)) * 0.55;
        vec2 ci = floor(cp);
        float ch = hash21(ci);
        float cd = length(fract(cp) - vec2(ch, fract(ch * 17.0)));
        float corpuscle = (1.0 - smoothstep(0.10, 0.30, cd)) * step(0.88, ch);
        plasma = mix(plasma, plasma * vec3(1.25, 0.70, 0.68), corpuscle * 0.32 * depth);

        col = mix(col, plasma, inside);
    }

    // ---- Vessel wall -------------------------------------------------------
    if (in_wall > 0.003) {
        // Concentric elastic laminae. Banding on `d` alone would read as
        // contour lines on a map; phase-shifting it by low-frequency noise
        // makes the layers wander and pinch the way real ones do. Roughly two
        // laminae across kWallThick, which is as many as survive at the zoom
        // the game is actually played at.
        float lamina = sin(d * 4.4 + fbm(p * 0.50) * 8.0);
        float layered = smoothstep(-0.2, 1.0, lamina);

        // Distinctly lighter and greyer than the flesh behind it: the wall has
        // to read as a *structure containing* the lane, so it needs its own
        // value, not a slightly different shade of the surround.
        vec3 wall = mix(vec3(0.225, 0.132, 0.140), lane_hue * 0.40, 0.24);
        wall *= 0.68 + 0.62 * layered;
        // Ranged rather than centred on 1.0: the shadowed half of a groove wall
        // still has to hold a readable value, and a full +-60% swing crushed it
        // to black, which turned every vessel into a thick outline.
        //
        // The floor here is load-bearing and was tuned by measurement, not by
        // eye. `nrm` points into the lumen, so on a horizontal lane the two
        // walls sit at lit = -0.91 and +0.91 — the widest possible split. At
        // the old 0.80 +- 0.40 that put the shadowed wall at 0.44x, which
        // landed its mean luminance within ~2/255 of the surrounding flesh: the
        // wall did not read as a wall at all on that side, and the vessel
        // looked like it had an outline along one edge and nothing along the
        // other. Keep the swing narrow enough that BOTH walls stay clearly
        // separated from the flesh behind them.
        wall *= 0.86 + 0.30 * lit;
        // Falls off into the flesh across its outer half, so there is no seam
        // where the ring ends.
        wall *= mix(1.0, 0.55, smoothstep(0.15, 1.0, wall_t));

        // Bounce light. The lumen is the brightest thing on screen and it sits
        // directly against this surface, so the wall face turned away from the
        // key light is still lit — by the plasma. Adding it back as a warm,
        // lane-tinted fill is what lets the swing above stay narrow without
        // flattening the relief: the shadowed wall keeps its own light source
        // rather than just being less dark.
        //
        // Spread across the whole wall, only *leaning* toward the lumen. It
        // must not be concentrated at wall_t = 0: `in_wall` below is
        // (1 - inside) * ..., which goes to zero exactly at the lumen boundary,
        // so anything piled up there gets multiplied away and the term silently
        // does nothing at all.
        float bounce = max(-lit, 0.0) * (1.0 - 0.55 * wall_t);
        wall += mix(vec3(1.0), lane_hue, 0.65) * (0.10 * bounce);

        col = mix(col, wall, in_wall);

        // Adventitia: a thin dark sheath on the wall's outer surface. Without
        // it the lit side of the wall bleeds into the flesh and the vessel
        // loses its outline exactly where the light is strongest.
        float sheath = 1.0 - smoothstep(0.0, 0.9, abs(-d - kWallThick * 0.92));
        col *= 1.0 - 0.20 * sheath * (1.0 - inside);
    }

    // NOTE: there is deliberately no bright endothelial hairline on the lumen
    // boundary. An earlier version drew one as a literal wet lining, on the
    // theory that DESIGN.md §9.4's "leading-edge silhouette is the whole story"
    // wanted a guaranteed contrast partner behind the horde. It read as a white
    // outline traced around every lane — a graphic-design stroke rather than
    // anatomy — and the edge does not need it: `inside` is a tight ~0.4-unit
    // smoothstep and the wall is far darker than the plasma, so the boundary
    // already resolves on value contrast alone.

    // ---- Ambient particulate (DESIGN.md §9.1's third layer) ----------------
    // Screen-locked rather than world-locked ON PURPOSE. This is meant to read
    // as the layer nearest the camera, so it has to lag the world as the camera
    // pans; anchoring it to the frame is the cheapest possible parallax and
    // costs no camera uniform. Additive only, so it can never occlude anything.
    vec2 dust = v_screen * vec2(90.0, 52.0) + vec2(u_time * 0.35, u_time * 0.17);
    float dust_h = hash21(floor(dust));
    float dust_d = length(fract(dust) - vec2(dust_h, fract(dust_h * 13.0)));
    float mote = (1.0 - smoothstep(0.06, 0.22, dust_d)) * step(0.955, dust_h);
    col += mote * 0.05;

    // ---- Frame finish ------------------------------------------------------
    // Screen-space vignette. v_screen is the visible extent's own [0,1], so
    // this sits on the frame and not on the world — it darkens the periphery
    // and pushes the eye to the middle of the action.
    vec2 vig = v_screen - 0.5;
    col *= 1.0 - 0.36 * dot(vig, vig);

    // Static (not time-varying) film grain: it breaks up the smooth gradients
    // that would otherwise band in 8-bit, without adding a crawling dither.
    col += (hash21(gl_FragCoord.xy) - 0.5) * 0.012;

    o_color = vec4(max(col, vec3(0.0)), 1.0);
}
