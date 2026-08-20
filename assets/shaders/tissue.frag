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
//   u_flow  RGBA16F rg = unit flow direction toward the objective,
//                   b  = normalised cost-to-goal in [0,1] (1 = far, 0 = at it),
//                   a  = coherence in [0,1] -- how well-defined that direction
//                        is. 1 in the body of a lane, falling to 0 over a tower
//                        footprint, at a dead-end cap, and at the objective
//                        where the field genuinely radiates.
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

// ===========================================================================
// SMOOTH TEXTURE RECONSTRUCTION
//
// Both data textures are coarse -- the SDF is the sim's own grid (half a world
// unit per texel, i.e. ~5 screen pixels at gameplay zoom) and the flow texture
// is coarser still. Hardware bilinear filtering reconstructs those as a field
// that is continuous but whose DERIVATIVE jumps at every texel boundary, and
// this shader reads them through terms that are exquisitely sensitive to
// exactly that: `sin(d * 4.4)` for the laminae, a 0.44-unit smoothstep for the
// lumen edge, and a central difference for the wall normal. A kinked field run
// through those comes out as a lattice of hard creases -- vessel walls that
// look torn out of paper along little straight facets, and lighting that goes
// blocky on the texel grid. That is the single most visible artifact this pass
// had, and it is present with or without a tower on the map.
//
// The fix is to reconstruct with a cubic B-spline instead. It is C2, so the
// gradient is continuous everywhere and there is no texel grid left to see. The
// standard four-bilinear-tap formulation is used, so it costs 4 samples rather
// than 16, and the B-spline's slight smoothing is a bonus here: the mask is
// rasterized by stamping discs (sim/flowfield/TissueRaster.h) and its stair
// steps are noise, not signal.
// ===========================================================================

/// Cubic B-spline weights, returned as the two offsets and the lerp factor the
/// four-tap trick needs. `f` is the fractional texel coordinate.
void bspline_taps(vec2 f, out vec2 off0, out vec2 off1, out vec2 mixf) {
    vec2 f2 = f * f;
    vec2 f3 = f2 * f;
    vec2 w0 = (1.0 - 3.0 * f + 3.0 * f2 - f3) / 6.0;
    vec2 w1 = (4.0 - 6.0 * f2 + 3.0 * f3) / 6.0;
    vec2 w2 = (1.0 + 3.0 * f + 3.0 * f2 - 3.0 * f3) / 6.0;
    vec2 w3 = f3 / 6.0;
    vec2 s0 = w0 + w1;
    vec2 s1 = w2 + w3;   // s0 + s1 == 1 exactly, so s1 *is* the lerp factor
    off0 = -1.0 + w1 / s0;
    off1 =  1.0 + w3 / s1;
    mixf = s1;
}

float sdf_smooth(vec2 uv) {
    vec2 ts = vec2(textureSize(u_sdf, 0));
    vec2 tc = uv * ts - 0.5;
    vec2 base = floor(tc);
    vec2 off0, off1, mixf;
    bspline_taps(tc - base, off0, off1, mixf);
    vec2 c = (base + 0.5) / ts;
    vec2 t0 = c + off0 / ts;
    vec2 t1 = c + off1 / ts;
    float a = texture(u_sdf, vec2(t0.x, t0.y)).r;
    float b = texture(u_sdf, vec2(t1.x, t0.y)).r;
    float e = texture(u_sdf, vec2(t0.x, t1.y)).r;
    float g = texture(u_sdf, vec2(t1.x, t1.y)).r;
    return mix(mix(a, b, mixf.x), mix(e, g, mixf.x), mixf.y);
}

vec4 flow_smooth(vec2 uv) {
    vec2 ts = vec2(textureSize(u_flow, 0));
    vec2 tc = uv * ts - 0.5;
    vec2 base = floor(tc);
    vec2 off0, off1, mixf;
    bspline_taps(tc - base, off0, off1, mixf);
    vec2 c = (base + 0.5) / ts;
    vec2 t0 = c + off0 / ts;
    vec2 t1 = c + off1 / ts;
    vec4 a = texture(u_flow, vec2(t0.x, t0.y));
    vec4 b = texture(u_flow, vec2(t1.x, t0.y));
    vec4 e = texture(u_flow, vec2(t0.x, t1.y));
    vec4 g = texture(u_flow, vec2(t1.x, t1.y));
    return mix(mix(a, b, mixf.x), mix(e, g, mixf.x), mixf.y);
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
// Deep saturated crimson, built from big epithelial plates with GLOWING seams.
// Two things here are the opposite of what a "recede into the background" layer
// normally wants, and both are deliberate:
//
//   BRIGHT SEAMS, NOT DARK ONES. Creasing the boundary between cells reads as
//   dried mud or cracked stone. Lighting it reads as wet tissue with fluid in
//   the intercellular space, which is what the medical-illustration look is
//   actually made of — the plates sit in shadow and the borders catch light.
//
//   BIG PLATES. ~10 world units across rather than ~3. At gameplay zoom small
//   cells collapse into uniform grain and the eye reads texture, not anatomy;
//   at this size you can see individual cells and the field reads as flesh.
//
// The saturation is high enough that this no longer "loses to the foreground"
// on value alone the way a near-black substrate did. What keeps the horde
// readable now is hue and edge: the pathogen families are yellow-green, teal
// and magenta against a red field, every agent carries a drop shadow, and the
// interstitium's own contrast stays low *within itself* even though its overall
// level is high.
// ---------------------------------------------------------------------------
vec3 interstitium(vec2 p, vec3 lane_hue) {
    vec2 wp = p + vec2(fbm(p * 0.14), fbm(p * 0.14 + 31.4)) * 4.6;

    vec3 cells = worley(wp * 0.105);
    vec3 lobes = worley(wp * 0.035);

    // 1 exactly on the seam between two plates, falling off into the plate.
    // Thin: the reference's borders are hairlines between broad soft plates,
    // not a wide glowing web.
    float seam = 1.0 - smoothstep(0.0, 0.13, cells.y - cells.x);
    // Each plate domed slightly: brightest at its own centre, so a plate reads
    // as a swollen cell rather than a flat tile.
    float dome = 1.0 - smoothstep(0.0, 0.55, cells.x);

    // Sampled straight off the reference: plates read ~#5A0A1C, borders only
    // modestly brighter at ~#7E1028. The whole field there spans barely 24
    // luminance levels from p05 to p95 — it is a FLAT, saturated red, and the
    // structure is carried by hue and soft doming rather than by contrast. An
    // earlier pass made the seams properly bright and it immediately read as
    // glowing lava cracks, which is the failure mode to watch for here.
    const vec3 kPlate = vec3(0.256, 0.054, 0.121); // cell interior, in shadow
    const vec3 kSeam  = vec3(0.403, 0.091, 0.184); // intercellular fluid, lit

    vec3 col = kPlate;
    // Per-plate jitter: no two cells hold quite the same amount of blood.
    col *= 0.88 + 0.22 * cells.z;
    col += kPlate * 0.32 * dome;
    col = mix(col, kSeam, seam * 0.48);

    // Lobule-scale swell across many plates at once, so the field has a
    // silhouette at screen scale and not only at pixel scale.
    float lobe = smoothstep(0.04, 0.60, lobes.y - lobes.x);
    col *= 0.88 + 0.20 * lobe;

    // Collagen. Ridged noise (1 - |2n-1|) gives creases instead of blobs, and
    // the anisotropic frequency stretches them into fibre bundles.
    float fibre = 1.0 - abs(2.0 * fbm(p * vec2(0.40, 0.90) + 7.0) - 1.0);
    col += vec3(0.033, 0.012, 0.016) * pow(fibre, 4.0);

    // Micro-vasculature: the same ridged trick at a much higher exponent, which
    // narrows the crest into a thin branching filament rather than a band — a
    // capillary bed feeding the flesh.
    float capillary = pow(1.0 - abs(2.0 * fbm(p * 0.17 + 19.0) - 1.0), 15.0);
    col += vec3(0.045, 0.014, 0.020) * capillary;

    // The lane's identity bleeds into the flesh around it, so a lymph channel
    // does not sit in arterial tissue.
    col = mix(col, col * (0.45 + lane_hue * 1.15), 0.45);
    return col;
}

void main() {
    vec2 uv = clamp(v_uv, 0.0, 1.0);
    vec2 p = v_world;

    float d_raw = sdf_smooth(uv);

    // ---- Wall normal, by central difference on the distance field ----------
    // Points toward increasing clearance, i.e. into the lumen. Only its
    // direction is used, so the texel-to-world scale never has to be known.
    // Differenced across the SMOOTH reconstruction, and over a full texel each
    // side: a central difference on the bilinear field is piecewise constant
    // within a texel, which quantised the wall's lighting into visible blocks.
    vec2 texel = 1.0 / vec2(textureSize(u_sdf, 0));
    float gx = sdf_smooth(uv + vec2(texel.x, 0.0)) -
               sdf_smooth(uv - vec2(texel.x, 0.0));
    float gy = sdf_smooth(uv + vec2(0.0, texel.y)) -
               sdf_smooth(uv - vec2(0.0, texel.y));
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
    // How much this point's flow direction can be believed. Everything driven
    // by `flow` is scaled by it, which is what makes the degenerate places --
    // a tower footprint, a dead-end cap, the objective the whole field points
    // at — fade to smooth fluid instead of drawing a starburst of streamlines
    // radiating out of a single texel.
    float coherence = 0.0;
    if (u_have_flow > 0.5) {
        vec4 f = flow_smooth(uv);
        float flen = length(f.xy);
        // The field is (0,0) wherever there is no guidance (outside it, or an
        // unreachable pocket) — never normalise that. There is deliberately no
        // hard cutoff any more: the old `flen > 0.08` test snapped the
        // direction to +x the instant the interpolation between a real vector
        // and an empty one crossed it, which drew a discontinuity ring just
        // inside every wall. Coherence below already fades this out smoothly.
        if (flen > 1e-4) flow = f.xy / flen;
        to_goal = f.b;
        // Already shaped CPU-side (Renderer.cpp's directional-agreement pass);
        // nothing to do here but clamp off interpolation overshoot.
        coherence = clamp(f.a, 0.0, 1.0);
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
    const float kWallThick = 3.9;

    float inside = smoothstep(-0.12, 0.32, d);
    // 0 at the lumen boundary, 1 at the wall's outer surface.
    float wall_t = clamp(-d / kWallThick, 0.0, 1.0);
    // NO `(1.0 - inside)` FACTOR. It used to be here, and it is what drew the
    // thin dark line that ringed every lumen. With it, the wall's own weight
    // was forced to zero exactly at the lumen boundary, so the fragments in the
    // ~0.44-unit transition composited PLASMA AGAINST INTERSTITIUM -- and the
    // interstitium is the darkest material in the frame. The result was a hard
    // inked outline traced round every vessel, precisely the thing the notes
    // further down say was deliberately removed.
    //
    // The fix is ordering, not weighting: the wall is laid over the flesh at
    // full strength across its whole thickness (it reaches into the lumen,
    // where `wall_t` is 0), and the plasma is then composited over the wall
    // with `inside`. Every fragment on the boundary is now a blend of the two
    // materials that actually meet there, and there is nothing dark left in
    // between for the eye to read as a stroke.
    float in_wall = 1.0 - smoothstep(0.78, 1.0, wall_t);

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
    // Softened hard, for the same reason the sheath went: with a bright wall
    // against dark flesh, an occlusion band just outside it reads as a second
    // outline rather than as seating.
    float lip = (1.0 - smoothstep(0.0, 4.5, -d - kWallThick * 0.5)) * (1.0 - inside);
    col *= 1.0 - 0.10 * lip;
    col *= 1.0 + 0.16 * lip * max(-lit, 0.0);

    // ---- Vessel wall -------------------------------------------------------
    // `inside < 0.997` is the same early-out the interstitium gets: under a
    // fully opaque plasma the wall is composited away to nothing, and it is
    // not worth an fbm to prove it.
    if (in_wall > 0.003 && inside < 0.997) {
        // Concentric elastic laminae. Banding on `d` alone would read as
        // contour lines on a map; phase-shifting it by low-frequency noise
        // makes the layers wander and pinch the way real ones do. Roughly two
        // laminae across kWallThick, which is as many as survive at the zoom
        // the game is actually played at.
        float lamina = sin(d * 4.4 + fbm(p * 0.50) * 8.0);
        float layered = smoothstep(-0.75, 0.95, lamina);

        // THE WALL IS BRIGHTER THAN THE FLESH, NOT DARKER. This is the single
        // biggest structural difference from the earlier version, and it came
        // out of profiling the reference rather than out of taste: a slice
        // crossing its lower wall runs 59 -> 97 -> 68 -> 49 -> 36 -> 20 in
        // luminance. The wall is a band of bright crimson ribbons sitting
        // between a bright lumen and DARK interstitium, with a hot specular
        // crest on one fold. There is no dark outline anywhere in that
        // transition. Rendering the wall as a shadow (which is what a
        // physically-lit groove wants) is what produced the black ring the old
        // version drew around every vessel.
        vec3 wall = mix(vec3(0.605, 0.172, 0.243), lane_hue * 0.85, 0.30);
        // Ribbon contrast, centred so the darkest fold still reads as red.
        // Narrow. The reference's folds move between luminance 59 and 89 —
        // a gentle swell. A wide range renders them as alternating bright and
        // near-black stripes, which reads as ribbing on a hose, not as tissue.
        wall *= 0.88 + 0.26 * layered;
        // Gentle directional term. Deliberately much weaker than a physical
        // groove would want: in the reference the relief is carried by the
        // ribbons themselves, not by a global light gradient across the vessel,
        // and a strong gradient here is what used to make the two edges of a
        // lane look like different materials.
        wall *= 0.94 + 0.16 * lit;
        // Specular crest on the outermost fold — the reference's hot #EC395E
        // hairline. A high power keeps it to the crest only.
        wall += vec3(0.026, 0.013, 0.015) * pow(layered, 3.0) * (0.55 + 0.45 * max(lit, 0.0));
        // Ramps down into the dark flesh across the outer half. This is where
        // the value drop belongs — after the wall, not between it and the lumen.
        wall *= mix(1.0, 0.46, smoothstep(0.45, 1.0, wall_t));

        // Bounce light. The lumen is the brightest thing on screen and it sits
        // directly against this surface, so the wall face turned away from the
        // key light is still lit — by the plasma. Adding it back as a warm,
        // lane-tinted fill is what lets the swing above stay narrow without
        // flattening the relief: the shadowed wall keeps its own light source
        // rather than just being less dark.
        //
        // Spread across the whole wall, only *leaning* toward the lumen: the
        // fragments at wall_t = 0 are the ones the plasma composites over
        // hardest, so a term piled up there is largely painted out anyway.
        float bounce = max(-lit, 0.0) * (1.0 - 0.55 * wall_t);
        wall += mix(vec3(1.0), lane_hue, 0.65) * (0.10 * bounce);

        col = mix(col, wall, in_wall);

        // NOTE: no adventitial dark sheath. There used to be one here, to stop
        // the lit wall bleeding into the flesh. Now that the wall is the bright
        // element and the flesh is the dark one, the value drop does that job
        // on its own, and a dark band on top of it reads as an inked outline —
        // the reference has nothing of the kind anywhere in the transition.
    }

    if (inside > 0.004) {
        // Plasma. Vivid and high-key: the lumen is the brightest thing on
        // screen and the lane's colour identity lives here, so it takes the
        // majority of the lane hue rather than a pale wash of it. Lifted toward
        // a hot crimson-white rather than toward neutral, which is what keeps
        // it reading as backlit fluid instead of as tinted fog.
        // Calibrated against the reference's lumen median (#CF314D, luminance
        // 84/255). The first attempt at "vivid" drove the red channel to 255
        // and clipped, which flattens all the flow detail into a single blown
        // pink — brighter is not more vibrant once a channel saturates.
        vec3 plasma = mix(lane_hue, vec3(0.828, 0.407, 0.485), 0.25) * 0.85;

        // The lumen is a channel with depth: it lifts toward the middle and
        // falls into contact shadow against the wall. `d` is in world units, so
        // this is a physically consistent gradient at any vessel width.
        float depth = smoothstep(0.0, 8.0, d);
        // Shallow on purpose. A profile across the reference shows its lumen
        // holding luminance 64-70 right up to the wall — there is no dark
        // trough between fluid and vessel. A steep ramp here put one there, and
        // it read as a bruise ringing every lane.
        plasma *= mix(0.86, 1.06, depth);
        // Contact shadow where the plasma meets the wall. Split into a constant
        // part and a directional part on purpose: the plasma touches the wall
        // on BOTH sides, so both get a contact shadow, and only its depth
        // follows the light. Driving the whole term off `lit` gave the lit side
        // essentially no contact shadow at all, which was half of why the two
        // edges of a lane read as different materials.
        plasma *= 1.0 - (1.0 - depth) * (0.03 + 0.05 * (0.5 - 0.5 * lit));

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
        //
        // Coherence scales the TAP SPACING, not the result. That is the whole
        // trick: at coherence 1 the taps spread along the streamline and smear
        // the noise into filaments as before, and as coherence falls they
        // collapse back onto the sample point, where seven identical taps are
        // just plain isotropic value noise. So the places with no meaningful
        // direction lose the streaks and keep the texture, instead of going
        // flat -- which is what happens if the smeared result is faded out
        // instead, and a lumen with a bald patch in it is not obviously an
        // improvement on one with a starburst in it. The plasma still advects
        // along `flow` at full rate there; it was the smear that read wrong at
        // a fan, never the motion.
        vec2 sp = p * 0.80 - flow * (u_time * 1.00 * tempo);
        float lic = 0.0;
        for (int k = -3; k <= 3; ++k) {
            lic += vnoise(sp + flow * (float(k) * 0.38 * coherence));
        }
        lic *= 1.0 / 7.0;
        // Centred on the mean so the streaks both brighten and darken; a purely
        // additive smear just washes the lane out. The gain is high because
        // averaging seven taps has already collapsed most of the variance.
        // The gain compensates the other half of that trick: averaging seven
        // spread taps collapses most of the variance, and seven collapsed ones
        // do not, so the same gain would render the isotropic fallback at
        // roughly twice the contrast of the striations it replaces.
        plasma *= 1.0 + 0.46 * (lic - 0.5) * mix(0.55, 1.0, coherence);
        // Silk sheen on the crests of those striations. A high power keeps it
        // to the few brightest filaments, which is what reads as light skating
        // off moving fluid rather than as the whole lane getting lighter.
        plasma += vec3(0.17, 0.105, 0.110) * pow(max(lic - 0.54, 0.0) * 2.2, 2.0) * depth *
                  coherence;

        // A faster, finer filament layer over the top, strongest where the
        // plasma drags against the wall — the same place a real velocity
        // profile has its steepest gradient.
        vec2 sp2 = p * 2.10 - flow * (u_time * 1.90 * tempo);
        float fine = 0.0;
        for (int k = -2; k <= 2; ++k) {
            fine += vnoise(sp2 + flow * (float(k) * 0.34 * coherence));
        }
        fine *= 0.2;
        plasma *= 1.0 + 0.26 * (fine - 0.5) * (1.0 - 0.55 * depth) *
                        mix(0.55, 1.0, coherence);

        // Large-scale density mottling. A floodplain lumen can be most of the
        // screen, and without a term at this scale the streamlines tile it with
        // one uniform grain; this gives the lane a shape of its own at the
        // distance the level is actually read from.
        plasma *= 0.90 + 0.20 * fbm(p * 0.075 + 4.0);

        // Systolic pressure wave, travelling along the vessel toward the
        // objective. `to_goal` is the only genuine along-vessel coordinate
        // available, which is exactly what this needs.
        // Damped by coherence for the same reason as the striations: `to_goal`
        // is the cost channel of the same field, and where the direction is
        // degenerate the cost has a kink in it too, which this band structure
        // renders as a hard contour across the lane.
        plasma *= 1.0 + 0.055 * sin(to_goal * 26.0 + u_heartbeat_phase * tempo) *
                        (0.35 + 0.65 * coherence);

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
    col *= 1.0 - 0.26 * dot(vig, vig);

    // Static (not time-varying) film grain: it breaks up the smooth gradients
    // that would otherwise band in 8-bit, without adding a crawling dither.
    col += (hash21(gl_FragCoord.xy) - 0.5) * 0.012;

    o_color = vec4(max(col, vec3(0.0)), 1.0);
}
