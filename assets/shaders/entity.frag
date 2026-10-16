#version 450 core
// Named-agent / tower instanced pass — fragment stage. Procedural SDF only.
// shape_id (from comp::Sprite::atlas_index, OR from a renderer-internal
// overlay instance appended in Renderer::submit_entities — see that function)
// picks the silhouette:
//   0 = filled blob (elites, chaff-like reuse)
//   1 = ring (range indicators / shielded)
//   2 = diamond (telegraphed / alert)
//   3 = telegraph countdown ring (Wave 4G overlay; v_anim_phase repurposed as
//       ActiveTelegraph::progress in [0,1], not a phase angle)
//   4 = elite death burst (Wave 4G overlay; v_anim_phase repurposed as a
//       burst-fade progress in [0,1])
//   5 = fibrin clot bar (game/abilities Fibrin Clot; v_shape_param is the
//       bar's aspect, half_length / half_width, and v_tint.a its dissolve)
//   6 = collagen scar bar (sim/scar, the Fibroblast's wall; v_shape_param is
//       the aspect as for 5, and the tint carries its STATE, not a colour:
//       r assembled, g repair freshness, b a per-wall seed, a the integrity
//       shown -- see Renderer::submit_entities)
// Tower shape ids start at 16 (kTowerShapeBase in TowerSystem.cpp) and run in
// TowerType declaration order, so id == 16 + TowerType:
//   16 = Neutrophil   17 = Macrophage (branching multi-grabber)
//   18 = Cytotoxic T  19 = Goblet Cell
//   20 = Fibroblast
// Any other id falls back to the filled blob.

in vec2  v_local;
in vec4  v_tint;
flat in uint v_shape_id;
in float v_anim_phase;
/// Extra per-shape parameter (EntityInstance::shape_param); meaning is defined
/// by v_shape_id, 0 for shapes that don't declare one. Every tower body (16-20)
/// reads it as the tower's TIER, 1-3, and spends it on a countable feature —
/// phagosomes, lytic granules, mucin granules, blades — so an
/// upgrade is legible from the silhouette instead of only from the stat panel.
in float v_shape_param;
/// Instance world rotation; see entity.vert. Used to hold a feature still while
/// the quad spins.
in float v_rotation;

// render::kShadowsEnabled as 0/1: a global kill switch for every drop shadow
// this pass draws. Applied once, in over_shadow(), which every shadowed
// shape composites through.
layout(location = 1) uniform float u_shadows;

out vec4 o_color;

float sdf_circle(vec2 p, float r) { return length(p) - r; }
float sdf_diamond(vec2 p, float r) {
    return abs(p.x) + abs(p.y) - r;
}

// ===========================================================================
// Organic-shading toolkit.
//
// Unlike chaff.frag (thousands of instances, ~8 pixels each, closed-form only)
// this pass draws dozens of towers at tens of pixels across, so it can afford
// real noise and multi-primitive composition. Everything here is procedural:
// this project ships no binary art (see DESIGN.md's asset rule).
// ===========================================================================

float hash21(vec2 p) {
    p = fract(p * vec2(127.1, 311.7));
    p += dot(p, p + 34.23);
    return fract(p.x * p.y);
}

/// Value noise with smooth (quintic) interpolation.
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

/// Smooth minimum. THE organic-shape primitive: a hard min() unions two blobs
/// with a visible crease, smin() fuses them into one grown body. This is what
/// makes pseudopods look like they belong to the cell instead of being glued on.
float smin(float a, float b, float k) {
    float h = clamp(0.5 + 0.5 * (b - a) / k, 0.0, 1.0);
    return mix(b, a, h) - k * h * (1.0 - h);
}

// ---------------------------------------------------------------------------
// GUNNER — Neutrophil.
//
// The defining feature of a real neutrophil is its nucleus: "polymorphonuclear"
// means it is split into 3-5 lobes joined by thin strands, which is why they
// are instantly identifiable down a microscope. So the nucleus here is three
// overlapping blobs fused with smin() rather than one disc — that single
// detail does more for "this is a white blood cell" than anything else on
// screen. Around it sits a granule-filled cytoplasm (neutrophils are packed
// with azurophilic granules) inside a domain-warped, slowly writhing membrane
// with a few pseudopods.
// ---------------------------------------------------------------------------
float sdf_neutrophil(vec2 p, float phase, out float nucleus_d, out float granule) {
    // Domain warp: the single highest-impact trick for making an SDF read as
    // organic. Evaluating a circle at a noise-perturbed position turns a
    // sterile disc into an irregular membrane, and animating the perturbation
    // makes it writhe. Warped on both axes independently (two fbm lookups at
    // different offsets) -- a single scalar warp only breathes in and out and
    // still leaves a recognisably round outline.
    float wx = fbm(p * 3.1 + vec2(phase * 0.13, 0.0)) - 0.5;
    float wy = fbm(p * 3.1 + vec2(5.2, -phase * 0.11)) - 0.5;
    vec2 wp = p + vec2(wx, wy) * 0.17;

    // Body, plus pseudopods fused in with smin so there are no seams.
    //
    // FIVE lobes, not four, and at deliberately uneven radii. An even count at
    // regular spacing produces exactly the polygon of that order -- four
    // pseudopods at 90 degrees rendered as a rounded SQUARE, which is the one
    // silhouette a cell must never have. An odd count has no opposing-pair
    // symmetry to line up, and jittering each lobe's distance and size breaks
    // what regularity is left.
    float body = length(wp) - 0.34;
    for (int k = 0; k < 5; ++k) {
        float fk = float(k);
        float a = phase * 0.17 + fk * 1.2566;              // 2*pi/5
        // Irregular, but a fixed function of k, so a given cell is stable.
        float dist = 0.28 + 0.10 * fract(sin(fk * 12.9898) * 43758.5453);
        float rad  = 0.10 + 0.045 * fract(sin(fk * 78.233) * 43758.5453);
        vec2 c = vec2(cos(a), sin(a)) * dist;
        float lobe = length(wp - c) - (rad + 0.022 * sin(phase * 0.7 + fk * 2.1));
        body = smin(body, lobe, 0.13);
    }

    // Multi-lobed nucleus: three blobs, fused, slowly drifting.
    nucleus_d = 1e9;
    for (int k = 0; k < 3; ++k) {
        float a = phase * 0.11 + float(k) * 2.0944;
        vec2 c = vec2(cos(a), sin(a)) * 0.105;
        float lobe = length(p - c) - 0.098;
        nucleus_d = (k == 0) ? lobe : smin(nucleus_d, lobe, 0.055);
    }

    // Cytoplasmic granules: high-frequency noise, thresholded into specks.
    granule = smoothstep(0.62, 0.82, fbm(p * 16.0 + vec2(phase * 0.05, 0.0)));

    return body;
}

/// Smooth maximum — the counterpart to smin(), and what CARVES a shape instead
/// of growing one. smax(body, -hole, k) subtracts `hole` from `body` with a
/// rounded lip rather than a knife edge, which is how a body gets a mouth or
/// cleft that reads as soft membrane instead of a bite taken with scissors.
float smax(float a, float b, float k) { return -smin(-a, -b, k); }

/// Capsule from `a` to `b` of radius `r`. The workhorse for every thin feature
/// below — antibody arms, microvilli.
float sdf_segment(vec2 p, vec2 a, vec2 b, float r) {
    vec2 pa = p - a, ba = b - a;
    float h = clamp(dot(pa, ba) / max(dot(ba, ba), 1e-8), 0.0, 1.0);
    return length(pa - ba * h) - r;
}

// ---------------------------------------------------------------------------
// ARBOR GRABBER — Macrophage.
//
// The heaviest silhouette in the roster, and it has to read that way from the
// build menu onward: the Macrophage is the tower you place when a clump needs
// deleting, so it is the widest, lumpiest, slowest-moving thing on the tissue.
// Its tower silhouette previews the released unit's defining feature: roots
// that fork into fine branching fingers reaching in every direction.
// ---------------------------------------------------------------------------
float sdf_tapered_segment(vec2 p, vec2 a, vec2 b, float ra, float rb) {
    vec2 ba = b - a;
    float h = clamp(dot(p - a, ba) / max(dot(ba, ba), 1e-8), 0.0, 1.0);
    return length(p - a - ba * h) - mix(ra, rb, h);
}

float sdf_macrophage(vec2 p, float phase, float tier,
                      out float nucleus_d, out float granule) {
    float wx = fbm(p * 2.8 + vec2(phase * 0.10, 0.0)) - 0.5;
    float wy = fbm(p * 2.8 + vec2(4.8, -phase * 0.08)) - 0.5;
    vec2 wp = p + vec2(wx, wy) * 0.15;
    float a = atan(wp.y, wp.x);
    float body = length(wp) - (0.32 + 0.025 * sin(a * 9.0 + phase * 0.46)
                                      + 0.012 * sin(a * 15.0 - phase * 0.34));

    int arm_count = int(clamp(2.0 + tier, 3.0, 5.0));
    for (int k = 0; k < 5; ++k) {
        if (k >= arm_count) break;
        float fk = float(k);
        float ang = fk * 6.2831853 / float(arm_count) +
                    0.16 * sin(phase * 0.31 + fk * 1.9);
        vec2 dir = vec2(cos(ang), sin(ang));
        vec2 side = vec2(-dir.y, dir.x);
        float wave = 0.045 * sin(phase * 0.58 + fk * 2.37);
        vec2 root = dir * 0.16;
        vec2 joint = dir * 0.43 + side * wave;
        vec2 tip = dir * (0.62 + 0.025 * sin(phase * 0.42 + fk)) + side * wave * 0.4;
        float tree = sdf_tapered_segment(p, root, joint, 0.105, 0.045);
        tree = min(tree, sdf_tapered_segment(p, joint, tip, 0.047, 0.014));

        for (int j = 0; j < 2; ++j) {
            float sign_side = j == 0 ? -1.0 : 1.0;
            vec2 origin = mix(joint, tip, 0.42 + float(j) * 0.28);
            vec2 branch_dir = normalize(dir * 0.80 + side * sign_side * 0.58);
            vec2 branch_tip = origin + branch_dir * (0.16 - float(j) * 0.025);
            tree = min(tree, sdf_tapered_segment(p, origin, branch_tip, 0.030, 0.006));
            vec2 twig_side = vec2(-branch_dir.y, branch_dir.x);
            for (int f = 0; f < 2; ++f) {
                float sf = f == 0 ? -1.0 : 1.0;
                vec2 finger_dir = normalize(branch_dir * 0.82 + twig_side * sf * 0.42);
                tree = min(tree, sdf_tapered_segment(p, branch_tip,
                                                     branch_tip + finger_dir * 0.075,
                                                     0.008, 0.002));
            }
        }
        body = smin(body, tree, 0.075);
    }

    // Uneven body lobes bridge the roots and keep the central mass amoeboid.
    for (int k = 0; k < 6; ++k) {
        float fk = float(k);
        float ang = fk * 1.0472 + 0.13 * sin(phase * 0.27 + fk);
        vec2 c = vec2(cos(ang), sin(ang)) * (0.23 + 0.025 * sin(fk * 2.1));
        body = smin(body, length(wp - c) - (0.080 + 0.015 * sin(fk * 3.7)), 0.11);
    }

    vec2 nc = vec2(-0.045, 0.018);
    float n1 = length(p - nc - vec2(0.040, 0.028)) - 0.074;
    float n2 = length(p - nc - vec2(-0.040, -0.020)) - 0.067;
    nucleus_d = smin(n1, n2, 0.045);
    granule = smoothstep(0.61, 0.80, fbm(p * 14.0 + vec2(phase * 0.045, 0.0)));
    return body;
}

// ---------------------------------------------------------------------------
// TESLA — Cytotoxic T.
//
// A KILLER CELL CAUGHT MID-KILL. The body this replaces was a spiked ball with
// a long electrode down its nose: it read as the tesla coil the internal role
// is named after rather than as immune tissue, which made it the one tower in
// the roster that did not look like it belonged next to the Neutrophil and the
// Macrophage. This is the same organism drawn from its real anatomy instead.
//
// The silhouette is POLARISED, and that is the whole design. A cytotoxic T cell
// about to kill is not round: it flattens its leading face against the target
// into an immunological synapse, sweeps its lytic granules up against that
// face, and drags the rest of itself — nucleus included — into a trailing tail
// called the uropod. So this body is a dart:
//
//   * a BROAD, SHALLOWLY CONCAVE front face at local +x (which entity.vert has
//     already rotated onto the aim), with its two lamellipodial corners curling
//     forward past it — the synapse, pressed flat against what it is killing;
//   * a compact soma behind that, holding a big reniform nucleus notched around
//     the payload rather than sitting centred in the cell;
//   * a tapering UROPOD trailing off local -x with a couple of knobs on it,
//     swaying slowly as the cell crawls.
//
// Nothing else on the field is asymmetric front-to-back like that. The
// Neutrophil and the Macrophage are radial lumps, and the Goblet Cell — the only other directional
// body — is a narrow stalk swelling into a round cup, i.e. widest at the BACK
// of its mass with a bore drilled through the front. This one is widest at the
// FRONT and comes to a point at the back, so the two never resolve to the same
// blob at forty pixels.
//
// The discharge language survives the redesign, because the attack still reads
// as a violet chain: instead of arcs crawling over a spiked ball, the crackle
// is confined to the synapse face and a few short filaments lance off it down
// the aim. Same "which way am I pointing" job the electrode did, played by the
// organ that actually does the killing.
// ---------------------------------------------------------------------------

/// The carve that flattens the front. Every feature of the synapse is a
/// function of this one circle, so main()'s shading can rebuild the identical
/// distance and hug the exact edge the carve produced.
const vec2  kCtlCleftC = vec2(0.720, 0.0);
const float kCtlCleftR = 0.530;

/// Signed distance to the synapse face, negative inside the cell. The ripple
/// micro-ruffles that membrane without disturbing the rest of the body.
float ctl_cleft(vec2 p, float phase) {
    float ripple = 0.008 * sin(p.y * 34.0 + phase * 1.3);
    return length(p - kCtlCleftC) - (kCtlCleftR + ripple);
}

float sdf_cytotoxic(vec2 p, float phase, float tier, out float nucleus_d,
                    out float granule_d, out float synapse_glow, out float lance,
                    out float speckle) {
    // Gentle warp only. The dart IS the read here; a Neutrophil-strength wobble
    // chews the flat face and the tail point back into a lumpy oval.
    float wx = fbm(p * 5.2 + vec2(phase * 0.10, 0.0)) - 0.5;
    float wy = fbm(p * 5.2 + vec2(4.4, -phase * 0.09)) - 0.5;
    vec2 wp = p + vec2(wx, wy) * 0.030;

    // Soma: the mass of the cell, sitting just behind centre.
    float body = length(wp - vec2(-0.030, 0.0)) - 0.225;

    // Lamellipod: a wide, shallow plate spanning the front. Squashed 2.6:1 on
    // x, so it adds width without adding reach — the cell gets a face, not a
    // nose.
    body = smin(body, length((wp - vec2(0.105, 0.0)) * vec2(2.60, 0.86)) - 0.300, 0.115);

    // The two corners of that face, curled forward. They are what stop the
    // front reading as an oval with its end chopped off: with them the leading
    // edge is a crescent that visibly WRAPS whatever it is touching.
    for (int k = 0; k < 2; ++k) {
        float side = (k == 0) ? 1.0 : -1.0;
        float y = side * (0.278 + 0.012 * sin(phase * 0.5 + side));
        body = smin(body, length(wp - vec2(0.150, y)) - 0.072, 0.090);
    }

    // Uropod. Two capsules of falling radius rather than one long taper, so the
    // tail has a knee in it and reads as dragged rather than as a cone glued
    // on. It sways: this is the only part of the body that moves much, and a
    // tail sweeping behind a held-still face is what makes the cell look like
    // it is leaning into the kill.
    float sway = 0.045 * sin(phase * 0.55);
    vec2 t0 = vec2(-0.150, 0.0);
    vec2 t1 = vec2(-0.295, 0.020 + sway);
    vec2 t2 = vec2(-0.475, 0.060 + sway * 2.0);
    float tail = sdf_segment(wp, t0, t1, 0.092);
    tail = smin(tail, sdf_segment(wp, t1, t2, 0.038), 0.055);
    // Knobs on the tail. The uropod is the one part of a crawling T cell still
    // covered in microvilli, and two bumps sell that for one smin() each.
    tail = smin(tail, length(wp - mix(t1, t2, 0.45) - vec2(0.0, -0.055)) - 0.030, 0.040);
    tail = smin(tail, length(wp - mix(t1, t2, 0.85) - vec2(0.010, 0.048)) - 0.024, 0.035);
    body = smin(body, tail, 0.075);

    // Flatten the face LAST, so the carve cuts the lamellipod and both corners
    // together and leaves one continuous concave edge instead of scalloping
    // each lobe separately. Tight k on purpose: this membrane is pressed
    // against something, and a soft lip there looks like it is melting.
    body = smax(body, -ctl_cleft(p, phase), 0.035);

    // Reniform nucleus, pushed into the back half and notched on its forward
    // side so it curls AROUND the payload. A centred round core read as a
    // bullseye and flattened the polarity the rest of the shape is built on.
    vec2 nc = vec2(-0.088, -0.006);
    nucleus_d = length((p - nc) * vec2(1.12, 0.94)) - 0.152;
    nucleus_d = smax(nucleus_d, -(length(p - nc - vec2(0.150, 0.0)) - 0.108), 0.048);

    // Lytic granules, docked at the synapse in a line across the face. Count is
    // 2 + tier, so 3..5 — this shape's countable upgrade tell. It used to be
    // the microvillus count (9 + 2*tier), which nobody could count.
    float n = 2.0 + tier;
    granule_d = 1e9;
    for (int k = 0; k < 5; ++k) {
        if (float(k) >= n) break;
        float fk = float(k);
        // Spread ACROSS the face, not around a circle: these are queued at a
        // wall, and a ring of them just reads as a second nucleus.
        float t = (n <= 1.0) ? 0.5 : fk / (n - 1.0);
        float y = mix(-0.150, 0.150, t) + 0.014 * sin(phase * 0.7 + fk * 2.3);
        // Bowed forward with |y|, following the concave membrane they are
        // docked against, so the row sits a constant depth behind the face.
        float x = 0.126 + 0.30 * y * y + 0.010 * sin(phase * 0.9 + fk);
        granule_d = min(granule_d,
                        length(p - vec2(x, y)) - (0.026 + 0.008 * fract(sin(fk * 45.1) * 43758.5453)));
    }

    // The synapse itself: a hot band hugging the inside of the face, broken
    // into filaments by the same high-power-|sin| trick the old surface arcs
    // used, so it crackles instead of glowing like a bulb. Confined to the
    // face — carried past the corners it would just outline the whole cell.
    float cleft = ctl_cleft(p, phase);
    // Band the crackle to the flat part of the face. `cleft` is a distance to a
    // CIRCLE, so its contours keep wrapping round past the corners and into the
    // lamellipod, where they painted stray filaments across the middle of the
    // cell; the taper has to close before the corners begin (they sit at
    // |y| ~ 0.28), not at the full half-width of the body.
    float across = 1.0 - smoothstep(0.100, 0.255, abs(p.y));
    float band = (1.0 - smoothstep(0.0, 0.038, abs(cleft))) * across;
    float fil = 0.55 + 0.45 * pow(abs(sin(p.y * 16.0 + phase * 2.4 + fbm(p * 7.0) * 5.0)), 6.0);
    // ...and hold it INSIDE the membrane — this is charge in the cell, not a
    // halo around it — in the narrow strip between the edge and the docked
    // granules. Wider than that and the granules simply cover it: they sit
    // 0.038 off the face, which is the whole clearance this has to live in.
    synapse_glow = band * fil * (0.84 + 0.16 * sin(phase * 1.7))
                 * (1.0 - smoothstep(-0.034, -0.002, body));

    // ...plus short filaments lancing off that face down the aim. These live
    // OUTSIDE the membrane and main() lets them draw proud of the silhouette,
    // because a discharge that stops dead at the edge reads as paint. Gated on
    // `body` for the same reason the band above is: without it the annulus
    // around the carve circle also lights up the two corner lobes from within.
    float reach = smoothstep(0.0, 0.020, cleft) * (1.0 - smoothstep(0.020, 0.135, cleft));
    float strand = pow(abs(sin(p.y * 21.0 + phase * 3.1)), 8.0);
    lance = reach * across * strand * smoothstep(-0.004, 0.020, body);

    speckle = smoothstep(0.60, 0.82, fbm(p * 15.0 + vec2(phase * 0.05, 0.0)));
    return body;
}

// ---------------------------------------------------------------------------
// HYDRO — Goblet Cell.
//
// This slot used to be the B Cell, whose body was an elongated antibody factory
// pointing down its own beam. There is no beam any more, so there is no reason
// to keep the shape: the tower now fires bursts of simulated fluid, and its
// silhouette should say "reservoir with a mouth", not "emitter with a barrel".
//
// A goblet cell is called that because it is literally goblet-shaped: a narrow
// basal stalk, a swollen theca crammed with mucin granules, and a flared apical
// mouth those granules are dumped out of. That reads at forty pixels, it points
// down the jet the same way the old body pointed down the beam, and it makes
// the tower legible even when it is between bursts and nothing is coming out.
//
// The granules are the animated part and they are not decoration — they DRIFT
// APICALLY, toward the mouth, and they thin out near it, so the cell always
// looks like it is loading itself. That is the only cue available for a tower
// whose actual weapon lives entirely outside the sprite.
// ---------------------------------------------------------------------------
float sdf_goblet(vec2 p, float phase, float tier, out float nucleus_d,
                 out float granule, out float mouth_glow, out float rim_band) {
    // Membrane wobble, same trick every other cell body here uses: enough to
    // stop the outline reading as vector art, not enough to lose the goblet.
    float wx = fbm(p * 5.0 + vec2(phase * 0.07, 0.0)) - 0.5;
    float wy = fbm(p * 5.0 + vec2(3.7, -phase * 0.06)) - 0.5;
    vec2 wp = p + vec2(wx, wy) * 0.050;

    // Basal stalk: a slim capsule anchoring the cell into the tissue behind it.
    float stalk = sdf_segment(wp, vec2(-0.430, 0.0), vec2(-0.145, 0.0), 0.088);

    // Theca: the swollen reservoir. Slightly taller than it is long, so the
    // cell reads as a cup seen from the side rather than as a ball.
    float theca = length((wp - vec2(0.050, 0.0)) * vec2(1.06, 0.90)) - 0.285;

    // Apical lip: a box that WIDENS toward +x, which is what turns a ball with
    // a hole in it into a goblet. Tier flares it a little more, so an upgraded
    // cell visibly has a bigger mouth on it.
    float flare = 0.185 + 0.150 * clamp((p.x - 0.090) / 0.330, 0.0, 1.0)
                + 0.012 * tier;
    float lip = max(abs(wp.y) - flare, max(0.070 - wp.x, wp.x - 0.425));

    float body = smin(stalk, theca, 0.085);
    body = smin(body, lip, 0.070);

    // Hollow the mouth out. The bore runs from mid-theca to past the lip, so
    // the cell is genuinely OPEN at the front — light gets into the reservoir
    // and the jet has somewhere to have come from.
    float bore_w = 0.105 + 0.135 * clamp((p.x + 0.020) / 0.430, 0.0, 1.0);
    float bore = max(abs(wp.y) - bore_w, max(-0.060 - wp.x, wp.x - 0.560));
    body = smax(body, -bore, 0.045);

    // Nucleus: squashed into the base, which is exactly where a real goblet
    // cell keeps it — shoved down by the mass of granules above.
    nucleus_d = length((p - vec2(-0.235, 0.0)) * vec2(1.35, 0.85)) - 0.105;

    // Mucin granules. They rise apically on a loop and shrink as they near the
    // mouth, as though being discharged; the loop is per-granule and offset, so
    // the reservoir churns instead of pulsing in unison.
    float g = 1e9;
    float count = 4.0 + 2.0 * tier;
    for (int k = 0; k < 10; ++k) {
        if (float(k) >= count) break;
        float fk = float(k);
        float lane = fract(sin(fk * 34.31) * 43758.5453);
        float rise = fract(phase * 0.055 + fk * 0.173);
        // Path: up out of the base, along the axis, out through the bore.
        vec2 c = vec2(mix(-0.135, 0.330, rise),
                      (lane - 0.5) * mix(0.230, 0.070, rise));
        float r = mix(0.048, 0.020, rise) * (0.75 + 0.5 * lane);
        g = min(g, length(p - c) - r);
    }
    granule = 1.0 - smoothstep(0.0, 0.014, g);

    // The wet meniscus sitting in the mouth, and the ring of lip around it.
    mouth_glow = 1.0 - smoothstep(0.0, 0.150, length(p - vec2(0.360, 0.0)));
    rim_band = (1.0 - smoothstep(0.0, 0.035, abs(bore)))
             * smoothstep(0.10, 0.20, p.x);
    return body;
}

// ---------------------------------------------------------------------------
// Tower drop shadow.
//
// The named-agent pass carried no shadow at all (entity.vert still notes the
// original reasoning: few instances, already telegraphed). That held while the
// substrate was a near-black floor a pale cell could not help but sit on top
// of. Against a vivid red lumen it does not: a tower is a light shape on a
// mid-light ground, and without a contact shadow it reads as a decal pasted on
// the lane rather than a cell sitting in it.
//
// Cheap enough at tower counts, and it fits: entity.vert pads the quad to
// kPad = 1.3, so v_local spans +-0.65 while the widest body reaches ~0.49 —
// room for an offset blob. Direction matches the chaff pass's shadow drift and
// tissue.frag's key light, so everything on screen is lit from the same place.
// ---------------------------------------------------------------------------
// FIBRIN CLOT -- the Fibrin Clot active ability's temporary barrier.
//
// A real clot is a mesh of fibrin strands with platelets caught in it, and
// that is what sells this as biology rather than as a wall: a rounded bar
// (the mask the sim actually carved, see game/abilities) whose long edges are
// frayed by noise into loose strands, with a row of dark platelet bodies
// tangled through it. The quad is square and the bar sits along its x axis,
// so the aspect squashes the box to the thickness the sim blocked.
//
// `dissolve` is 1 - v_tint.a: as the clot's clock runs down the renderer
// fades it, and the bar thins here in lockstep so the horde is seen to get
// its lane back at the moment the mask hands it back.
// ---------------------------------------------------------------------------
float sdf_clot(vec2 p, float aspect, float phase, float dissolve, out float platelet_d,
               out float strand) {
    vec2 he = vec2(0.5, 0.5 / max(aspect, 1.0));
    // Fray the long edges: a slow writhe along the bar, sharper across it.
    float w = fbm(vec2(p.x * 6.0 + phase * 0.05, p.y * 12.0 + 3.7)) - 0.5;
    vec2 wp = vec2(p.x, p.y + w * he.y * 0.55);
    float r = he.y * 0.85;
    vec2 q = abs(wp) - (he - vec2(r));
    float box = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
    // Thins to nothing as it dissolves, edge-first, so the ends fall apart
    // before the middle.
    float thin = dissolve * he.y * (0.9 + 0.6 * abs(p.x) / he.x);
    float body = box + thin;

    // Platelets: a row of fused dark ovals along the bar, jittered so it
    // reads as a tangle and not as beads on a string.
    platelet_d = 1e9;
    for (int k = 0; k < 7; ++k) {
        float fk = float(k);
        float u = (fk - 3.0) / 3.4;
        float jx = (fract(sin(fk * 12.9898) * 43758.5453) - 0.5) * 0.06;
        float jy = (fract(sin(fk * 78.233) * 43758.5453) - 0.5) * he.y * 0.9;
        vec2 c = vec2(u * he.x + jx, jy);
        float rad = he.y * (0.34 + 0.16 * fract(sin(fk * 39.4) * 43758.5453));
        vec2 d = p - c;
        d.y *= 1.35;
        platelet_d = smin(platelet_d, length(d) - rad, he.y * 0.3);
    }
    platelet_d += dissolve * he.y * 0.8;

    // Strand texture: fine fibres running roughly along the bar.
    strand = smoothstep(0.35, 0.65, vnoise(vec2(p.x * 22.0, p.y * 90.0 + phase * 0.02)));
    return body;
}

// ---------------------------------------------------------------------------
// COLLAGEN SCAR -- the Fibroblast's wall (sim/scar).
//
// A ROPE: three collagen cords laid side by side down the bar, crimped a
// little out of step so they read as twisted together, each one made of
// SEGMENTS -- the bundles the cell laid one after another. Opaque and shaded
// as solid round cords (a lit crown, a shaded underside, grooves where they
// meet, a twist striation), so the wall reads as a woven thing standing in
// the lane. Every animation is a segment having its moment:
//
//   build   thin threads spin out from the middle (where the cell sits) to
//           the ends first, then segment by segment, middle outward, they
//           thicken into full cords;
//   damage  segments are TORN OUT -- outer cords and the ends first, the
//           middle cord last. A lost segment simply is not there: the cord
//           ends round off either side of the gap and the lane shows through.
//           Nothing darkens; the rope just comes apart;
//   repair  threads bridge the gaps and the lost segments thicken back in,
//           bright while they are fresh.
// ---------------------------------------------------------------------------
const float kScarCordGap = 0.60;   // cord spacing across, half-widths
const float kScarCordR   = 0.42;   // full cord radius
const float kScarSegLen  = 1.45;   // segment length along a cord
const float kScarThreadR = 0.075;  // a scaffold thread's radius
const float kScarFar = 50.0;

float scar_cord_y(float x, float cord, float seed) {
    return (cord - 1.0) * kScarCordGap + 0.085 * sin(x * 1.7 + cord * 2.1 + seed * 6.0);
}

// 0..1, how much of a segment is there; `fresh` how newly laid it is.
float scar_seg_presence(float cord, float xc, float h, float aspect, float build,
                        float damage, float repair, out float fresh) {
    // Outer cords and the ends are at risk first, the middle cord last.
    float risk = clamp(0.60 * abs(cord - 1.0) + 0.45 * smoothstep(aspect - 2.6, aspect, abs(xc)), 0.0, 1.0);
    float gone = 0.05 + 0.92 * clamp((1.0 - risk) * 0.72 + h * 0.28, 0.0, 1.0);
    float g_dmg = 1.0 - smoothstep(gone - 0.07, gone, damage);
    // Laid middle first, out to the ends.
    float came = 0.12 + 0.55 * clamp(abs(xc) / max(aspect, 1.0), 0.0, 1.0) + 0.14 * h;
    float g_bld = smoothstep(came, came + 0.22, build);
    fresh = max((1.0 - smoothstep(0.0, 0.30, build - came)) * step(came, build),
                repair * (1.0 - smoothstep(0.0, 0.22, gone - damage)));
    return min(g_dmg, g_bld);
}

// Signed distance to the rope, half-width units. `across` = the pixel's
// position across the nearest cord (-1..1), `seg_h` its segment's random
// number, `fresh` how newly laid that segment is, `thread` 1 on a bare
// scaffold thread.
float scar_wall(vec2 q, float aspect, float seed, float build, float damage, float repair,
                out float across, out float seg_h, out float fresh, out float thread) {
    float wall = kScarFar;
    float best = kScarFar;
    across = 0.0; seg_h = 0.0; fresh = 0.0; thread = 0.0;
    float front = aspect * smoothstep(0.0, 0.45, build) + 0.35;
    float thread_show = max(1.0 - smoothstep(0.80, 1.0, build), repair);
    for (int k = 0; k < 3; ++k) {
        float cord = float(k);
        float yc = scar_cord_y(q.x, cord, seed);
        // The cords end staggered, the middle one longest, like a rope's end.
        float end = aspect - 0.30 - 0.32 * abs(cord - 1.0);
        float off = hash21(vec2(cord * 3.1, seed * 7.7)) * kScarSegLen;
        float si = floor((q.x + off) / kScarSegLen);
        for (int j = -1; j <= 1; ++j) {
            float s = si + float(j);
            float x0 = max(s * kScarSegLen - off, -end);
            float x1 = min(s * kScarSegLen - off + kScarSegLen, end);
            if (x1 <= x0) continue;
            float xc = 0.5 * (x0 + x1);
            float hl = 0.5 * (x1 - x0);
            float h = hash21(vec2(s * 1.37 + cord * 17.1, cord * 3.7 + seed * 11.3));
            float fr;
            float g = scar_seg_presence(cord, xc, h, aspect, build, damage, repair, fr);
            float r = g > 0.001 ? kScarCordR * (0.92 + 0.14 * h) * (0.25 + 0.75 * g) : 0.0;
            float rt = kScarThreadR * thread_show * step(abs(xc) - hl, front);
            float rad = max(r, rt);
            if (rad <= 0.0) continue;
            // A capsule down the crimped centreline, its ends rounded so a
            // gap shows cord ends and a joint a slight waist.
            float dx = max(abs(q.x - xc) - max(hl - 0.55 * rad, 0.0), 0.0);
            float d = length(vec2(dx, q.y - yc)) - rad;
            wall = smin(wall, d, 0.14);
            if (d < best) {
                best = d;
                across = clamp((q.y - yc) / rad, -1.0, 1.0);
                seg_h = h;
                fresh = r >= rt ? fr : 0.0;
                thread = r < rt ? 1.0 : 0.0;
            }
        }
    }
    return wall;
}

// Shades one scar fragment. `q` and `shadow_q` in half-width units, `px` the
// size of a pixel in them. Straight alpha out; the shadow separately.
vec4 shade_scar(vec2 q, vec2 shadow_q, float aspect, float seed, float build, float damage,
                float repair, float px, out float shadow) {
    float across, seg_h, fresh, thread;
    float wall = scar_wall(q, aspect, seed, build, damage, repair, across, seg_h, fresh, thread);
    float a = 1.0 - smoothstep(-px, px, wall);

    float s_across, s_h, s_fresh, s_thread;
    float sw = scar_wall(shadow_q, aspect, seed, build, damage, repair, s_across, s_h, s_fresh, s_thread);
    shadow = (1.0 - smoothstep(-0.25, 0.30, sw)) * 0.40;

    // A solid round cord, lit from above: the crown catches the light, the
    // underside and the grooves between cords fall into shade.
    float nz = sqrt(max(1.0 - across * across, 0.0));
    float lit = 0.45 * across + 0.90 * nz;
    vec3 rgb = mix(vec3(0.13, 0.28, 0.68), vec3(0.29, 0.55, 0.96), smoothstep(-0.25, 0.75, lit));
    rgb *= 0.93 + 0.12 * seg_h;
    // The twist: fine fibres running round the cord at a slant.
    float twist = sin((q.x * 2.4 + across * 1.9 + seg_h * 5.0) * 3.14159);
    rgb *= 1.0 + 0.07 * twist * nz;
    // A wet highlight streak along each cord's crown.
    rgb = mix(rgb, vec3(0.74, 0.90, 1.00), pow(max(lit, 0.0), 10.0) * 0.55);
    // Fresh collagen is bright as it knits in; a bare thread is pale.
    rgb = mix(rgb, vec3(0.56, 0.82, 1.00), fresh * 0.45);
    rgb = mix(rgb, vec3(0.80, 0.93, 1.00), thread * 0.85);
    // A firm edge, so the rope holds its shape on the bright lane.
    float edge = 1.0 - smoothstep(0.0, 0.11, -wall);
    rgb = mix(rgb, vec3(0.12, 0.17, 0.46), edge * 0.70 * (1.0 - thread));
    return vec4(rgb, a);
}
// "Nothing here" for the distances below that pass through smin: a 1e9
// would swallow the body whole in float (swarmer.frag says why).
const float kFar = 8.0;

// ---------------------------------------------------------------------------
// BUILDER -- a fibroblast, crawling.
//
// Same spindle as ever -- a body drawn out along the heading with a process
// off each end -- but built the way the other cells here are built: a chain
// of soft lobes fused with smin() under a domain-warped membrane, so it reads
// as a sac of liquid rather than a cut-out. Local +x is the heading.
//
// THE CRAWL. A migrating fibroblast inches: the leading lamella reaches out
// and sticks, the body stretches after it, then contracts and drags its tail
// up. One stroke per cycle (fibro_stroke: +1 reach, -1 contract) drives the
// stretch, the lamella's reach and the tail, and a swell rolls down the lobe
// chain front to back once a stroke -- the peristaltic wave that makes it
// look like it is pulling itself along. `move` (0..1, its speed over its
// profile speed) scales all of it, so a cell standing still only breathes.
//
// SECRETION (`secrete` 0..1 while it lays or mends a wall; it has turned to
// lie along the wall by then): it draws out long and thin, its vesicles run
// to the two poles, and each pole spins out a crimped collagen thread. The
// particle layer carries the threads on from there to the wall.
// ---------------------------------------------------------------------------
// The lobe chain, nose to tail: centre x and radius.
const float kFibroLobeX[6] = float[6](0.255, 0.115, -0.035, -0.180, -0.300, -0.385);
const float kFibroLobeR[6] = float[6](0.122, 0.166, 0.182, 0.150, 0.100, 0.058);

// The crawl stroke: +1 the reach, -1 the contraction. Two sines so the stride
// never quite repeats.
float fibro_stroke(float c) { return 0.82 * sin(c) + 0.18 * sin(2.0 * c + 0.9); }

float sdf_fibroblast_cell(vec2 p, float phase, float move, float secrete, float load,
                          out float nucleus_d, out float nucleolus, out float vesicle,
                          out float lamella, out float thread_d, out float pole_glow) {
    float c = phase * 1.15;
    float stroke = fibro_stroke(c) * move;
    float pull = sin(3.14159 * clamp(secrete, 0.0, 1.0));

    float wx = fbm(p * 4.0 + vec2(phase * 0.10, 0.0)) - 0.5;
    float wy = fbm(p * 4.0 + vec2(3.1, -phase * 0.09)) - 0.5;
    vec2 wp = p + vec2(wx, wy) * 0.050;

    // Stretch on the reach, bunch on the contraction, long and thin while it
    // secretes; volume kept, so longer is thinner.
    float sx = 1.0 + 0.09 * stroke + 0.16 * pull;
    vec2 bp = vec2(wp.x / sx, wp.y * sx);
    bp.x += 0.020 * min(stroke, 0.0);

    // The lobe chain, with the crawl's swell rolling down it.
    float roll = fract(c / 6.2831853);
    float body = kFar;
    for (int k = 0; k < 6; ++k) {
        float fk = float(k);
        float wave = pow(max(cos(6.2831853 * (roll - fk / 6.0)), 0.0), 3.0);
        float r = kFibroLobeR[k] * (1.0 - 0.10 * pull) + (0.006 + 0.024 * move) * wave;
        vec2 lc = vec2(kFibroLobeX[k], 0.014 * sin(phase * 0.31 + fk * 0.9));
        body = smin(body, length((bp - lc) * vec2(0.78, 1.0)) - r, 0.11);
    }

    // Leading lamella: a flat fan on the nose, its edge ruffling. Reaches on
    // the stroke, pulls in on the contraction.
    float reach = 0.032 * max(stroke, 0.0);
    vec2 fc = vec2(0.345 + reach, 0.0);
    float lam = length((bp - fc) * vec2(1.9, 1.10)) - 0.120;
    for (int k = 0; k < 4; ++k) {
        float fk = float(k);
        float a = (fk - 1.5) * 0.44;
        float ruff = 0.5 + 0.5 * sin(phase * 1.9 - fk * 1.4);
        vec2 rc = fc + vec2(cos(a) * 0.070, sin(a) * 0.105) * (1.0 + 0.18 * ruff);
        lam = smin(lam, length(bp - rc) - (0.034 + 0.010 * ruff), 0.045);
    }
    body = smin(body, lam, 0.08);
    lamella = smoothstep(0.26, 0.42, bp.x);

    // Two filopodia off the lamella's corners, flicking.
    for (int k = 0; k < 2; ++k) {
        float side = k == 0 ? 1.0 : -1.0;
        float a = side * (0.62 + 0.10 * sin(phase * 1.3 + side));
        vec2 root = fc + vec2(cos(a) * 0.07, sin(a) * 0.12);
        vec2 tip = root + vec2(cos(a * 0.6), sin(a * 0.6)) * (0.075 + 0.03 * sin(phase * 1.7 + side * 2.0));
        body = smin(body, sdf_tapered_segment(bp, root, tip, 0.013, 0.004), 0.02);
    }

    // Trailing tail: dragged out on the reach, snapped up on the contraction.
    float tail_len = (0.16 + 0.05 * stroke) * (1.0 - 0.7 * pull);
    vec2 t0 = vec2(-0.40, 0.0);
    vec2 t1 = t0 + vec2(-tail_len, 0.05 * sin(phase * 0.6) + 0.015 * stroke);
    body = smin(body, sdf_tapered_segment(bp, t0, t1, 0.034, 0.008), 0.035);

    // Secretion threads, crimped, out of both poles.
    thread_d = kFar;
    pole_glow = 0.0;
    if (secrete > 0.0) {
        float out_t = smoothstep(0.0, 0.55, secrete) * (1.0 - smoothstep(0.82, 1.0, secrete));
        for (int k = 0; k < 2; ++k) {
            float side = k == 0 ? 1.0 : -1.0;
            vec2 root = vec2(side * 0.38, 0.0);
            float len = 0.38 * out_t;
            float x = (bp.x - root.x) * side;
            float crimp = 0.020 * sin(x * 40.0 - phase * 4.0) * clamp(x / 0.08, 0.0, 1.0);
            float th = mix(0.028, 0.009, clamp(x / max(len, 1e-3), 0.0, 1.0));
            float d = length(vec2(max(abs(x - 0.5 * len) - 0.5 * len, 0.0), bp.y - crimp)) - th;
            thread_d = min(thread_d, d);
            pole_glow = max(pole_glow, out_t * exp(-dot(bp - root, bp - root) / 0.003));
        }
        body = smin(body, thread_d, 0.03);
    }

    // Nucleus: an ellipsoid in the waist, lagging the crawl a beat.
    vec2 nc = vec2(-0.040 - 0.028 * stroke, 0.006 * sin(phase * 0.5));
    vec2 np = (bp - nc);
    nucleus_d = (length(np * vec2(1.0 / 0.130, 1.0 / 0.085)) - 1.0) * 0.085;
    nucleolus = 0.0;
    for (int k = 0; k < 2; ++k) {
        vec2 oc = nc + vec2(0.032 - 0.066 * float(k), 0.014 - 0.030 * float(k));
        nucleolus = max(nucleolus, 1.0 - smoothstep(0.011, 0.020, length(bp - oc)));
    }

    // Procollagen vesicles -- the wall it carries. They stream back down the
    // cell as it crawls, refill while it rests, and on secretion run out to
    // the poles and go.
    float vd = kFar;
    for (int k = 0; k < 9; ++k) {
        float fk = float(k);
        if (fk >= load * 9.0) break;
        float h1 = fract(sin(fk * 12.9898) * 43758.5453);
        float h2 = fract(sin(fk * 78.233) * 43758.5453);
        float x = mix(0.27, -0.27, fract(h1 + phase * 0.030 * (0.35 + move)));
        float half_w = 0.13 * (1.0 - smoothstep(0.10, 0.30, abs(x)));
        vec2 vc = vec2(x, (h2 * 2.0 - 1.0) * half_w);
        // Kept off the nucleus.
        vec2 off = vc - nc;
        float in_n = length(off * vec2(1.0 / 0.15, 1.0 / 0.10));
        if (in_n < 1.0) vc = nc + off / max(in_n, 0.2);
        float r = 0.017 + 0.008 * h1;
        if (secrete > 0.0) {
            float side = vc.x >= nc.x ? 1.0 : -1.0;
            float go = smoothstep(h1 * 0.25, 0.40 + h1 * 0.25, secrete);
            vc = mix(vc, vec2(side * 0.38, 0.0), go);
            r *= 1.0 - go;
        }
        vd = min(vd, length(bp - vc) - r);
    }
    vesicle = 1.0 - smoothstep(-0.006, 0.004, vd);
    return body;
}

// Shades the cell. Straight-alpha colour for the body; `shadow` separately.
vec4 shade_fibroblast_cell(vec2 p, float phase, float move, float secrete, float load, float engaged_rim,
                           vec3 hue, out float shadow) {
    float nucleus_d, nucleolus, vesicle, lamella, thread_d, pole_glow;
    float body_d = sdf_fibroblast_cell(p, phase, move, secrete, load, nucleus_d, nucleolus,
                                       vesicle, lamella, thread_d, pole_glow);
    float a = 1.0 - smoothstep(-0.022, 0.008, body_d);
    vec2 sp = p - vec2(0.070, -0.060);
    float sd = length(vec2(sp.x * 0.92, sp.y * 2.0)) - 0.40;
    shadow = (1.0 - smoothstep(-0.12, 0.05, sd)) * 0.34;

    // Wet: pale and thin at the rim and over the lamella, a deep clear blue
    // where the cell is thick -- the subsurface ramp every body here uses.
    float depth = clamp(-body_d * 5.6, 0.0, 1.0) * (1.0 - 0.25 * lamella);
    vec3 cyto = mix(vec3(0.50, 0.76, 1.00), vec3(0.14, 0.35, 0.80), depth);
    cyto = mix(cyto, hue, 0.22);
    // Rough ER: a fine streaky grain along the long axis.
    float er = smoothstep(0.55, 0.80, fbm(vec2(p.x * 9.0 + phase * 0.03, p.y * 24.0)));
    float in_cyto = smoothstep(0.0, 0.030, nucleus_d);
    vec3 rgb = mix(cyto, vec3(0.74, 0.89, 1.00), er * in_cyto * 0.32 * depth);

    // Vesicles: pearly beads.
    rgb = mix(rgb, vec3(0.84, 0.95, 1.00), vesicle * in_cyto * 0.70);

    // Nucleus: deep indigo, chromatin mottle, a pale envelope, nucleoli.
    float nuc = 1.0 - smoothstep(-0.010, 0.010, nucleus_d);
    float chromatin = fbm(p * 15.0 + vec2(phase * 0.03, 0.0));
    rgb = mix(rgb, mix(vec3(0.10, 0.14, 0.40), vec3(0.27, 0.35, 0.70), chromatin), nuc * 0.92);
    rgb = mix(rgb, vec3(0.56, 0.71, 0.98), (1.0 - smoothstep(0.0, 0.016, abs(nucleus_d))) * 0.50);
    rgb = mix(rgb, vec3(0.05, 0.07, 0.25), nucleolus * nuc * 0.70);

    // The threads it spins and the hot poles they leave from.
    float thread = 1.0 - smoothstep(-0.004, 0.008, thread_d);
    rgb = mix(rgb, vec3(0.90, 0.97, 1.00), thread * 0.85);
    rgb = mix(rgb, vec3(0.86, 0.96, 1.00), clamp(pole_glow, 0.0, 1.0) * 0.35);

    // Membrane rim, and a soft wet highlight off the key light.
    float rim = 1.0 - smoothstep(0.0, 0.032, abs(body_d));
    rgb = mix(rgb, vec3(0.74, 0.90, 1.00), rim * (0.45 + engaged_rim));
    vec2 hl = p - vec2(-0.05, 0.075);
    rgb = mix(rgb, vec3(1.0), exp(-dot(hl, hl) / 0.008) * smoothstep(0.0, -0.05, body_d) * 0.22);

    // The lamella is a thin sheet: a little of the ground shows through it.
    float alpha = a;
    return vec4(rgb, alpha);
}

const vec2 kEntityShadowDir = vec2(0.085, -0.070);

float entity_shadow(vec2 p, float radius) {
    float sd = length(p - kEntityShadowDir) - radius;
    return (1.0 - smoothstep(-0.10, 0.03, sd)) * 0.42;
}

/// Composites a lit body over its own drop shadow into one straight-alpha
/// result, so the pair resolves correctly against the destination blend.
vec4 over_shadow(vec3 rgb, float body_a, float shadow_a) {
    const vec3 kShadowRgb = vec3(0.05, 0.01, 0.02);
    shadow_a *= u_shadows;
    float out_a = body_a + shadow_a * (1.0 - body_a);
    if (out_a <= 0.001) return vec4(0.0);
    return vec4((rgb * body_a + kShadowRgb * shadow_a * (1.0 - body_a)) / out_a, out_a);
}

/// A tower's wounds. The horde chews on towers now (sim/hostile), and for the
/// five tower bodies (shape 16-20) the renderer sends the tower's remaining
/// INTEGRITY FRACTION in v_tint.a instead of an alpha (towers are never
/// translucent; see submit_entities in Renderer.cpp). Every tower branch
/// mixes its tint at 0.18 or less -- the bodies are authored colours, not
/// tinted discs -- so a colour change through the tint would be invisible,
/// and the wound has to be applied here, after each body has been shaded.
///
/// What it looks like: the cell goes dull and bruised, from the inside out.
/// Lesions in a dark violet-red -- the bruise colour, deliberately not any
/// pathogen family's colour, so it reads as "this cell is hurt" rather than
/// "there is a virus here" -- creep across the body as a noise field whose
/// threshold falls with integrity, and the whole body desaturates and darkens
/// with it. At a third of its integrity a tower is unmistakably sick before
/// the player opens its panel. `body_d` is the branch's own membrane SDF, so
/// the lesions never stray outside the silhouette.
vec3 wounded(vec3 rgb, float body_d, float integrity) {
    float hurt = clamp(1.0 - integrity, 0.0, 1.0);
    if (hurt <= 0.001) return rgb;
    const vec3 kBruise = vec3(0.36, 0.10, 0.22);
    // Lesion field: two octaves of value noise in body space, thresholded so
    // that more of the body is lesion as integrity falls. Anchored to the
    // interior (body_d < 0) and fading out at the rim so the membrane stays
    // readable as the tower's edge.
    float n = vnoise(v_local * 9.0 + 3.7) * 0.65 + vnoise(v_local * 21.0 + 11.3) * 0.35;
    float lesion = smoothstep(1.0 - hurt * 0.95, 1.0 - hurt * 0.95 + 0.18, n);
    float interior = clamp(-body_d * 6.0, 0.0, 1.0);
    lesion *= interior;
    // Whole-body sickness: desaturate and darken with the damage, on top of
    // the lesions, so even the healthy patches look worn.
    float lum = dot(rgb, vec3(0.30, 0.59, 0.11));
    vec3 dull = mix(rgb, vec3(lum), hurt * 0.45) * mix(1.0, 0.72, hurt);
    return mix(dull, kBruise, lesion * 0.85);
}

void main() {
    float pulse = sin(v_anim_phase) * 0.04;
    float d;
    float alpha_mul = 1.0;

    if (v_shape_id == 1u) {
        // Ring: solid disk minus a slightly smaller disk.
        float outer = sdf_circle(v_local, 0.5 + pulse);
        float inner = sdf_circle(v_local, 0.34 + pulse);
        float a_outer = 1.0 - smoothstep(-0.05, 0.0, outer);
        float a_inner = 1.0 - smoothstep(-0.05, 0.0, inner);
        float a = clamp(a_outer - a_inner, 0.0, 1.0);
        if (a <= 0.0) discard;
        o_color = vec4(v_tint.rgb, a * v_tint.a);
        return;
    } else if (v_shape_id == 3u) {
        // Telegraph countdown ring (DESIGN.md §9.5): closes in and thickens
        // as `progress` -> 1, then flashes bright at the centre right as the
        // wind-up completes, so the player has a legible "about to land" tell.
        float progress = clamp(v_anim_phase, 0.0, 1.0);
        float ring_r = mix(0.48, 0.12, progress);
        float thickness = mix(0.05, 0.15, progress);
        float dist = length(v_local);
        float ring_d = abs(dist - ring_r) - thickness;
        float a = 1.0 - smoothstep(-0.04, 0.03, ring_d);
        float flash = smoothstep(0.85, 1.0, progress);
        float fill = flash * (1.0 - smoothstep(0.0, ring_r, dist));
        a = clamp(a + fill, 0.0, 1.0);
        if (a <= 0.0) discard;
        vec3 rgb = mix(v_tint.rgb, vec3(1.0, 0.95, 0.6), flash);
        o_color = vec4(rgb, a * v_tint.a);
        return;
    } else if (v_shape_id == 4u) {
        // Elite death burst: an expanding, fading ring plus a few radial
        // spark streaks. `progress` drives both the expansion and the fade,
        // so the burst reads as one punchy pop rather than a static shape.
        float t = clamp(v_anim_phase, 0.0, 1.0);
        float r = mix(0.05, 0.55, t);
        float dist = length(v_local);
        float ring_d = abs(dist - r) - mix(0.03, 0.16, t);
        float a = (1.0 - smoothstep(-0.03, 0.05, ring_d)) * (1.0 - t);
        float ang = atan(v_local.y, v_local.x);
        float spark = pow(abs(sin(ang * 5.0 + t * 3.0)), 8.0);
        a = clamp(a + spark * (1.0 - smoothstep(r * 0.5, r, dist)) * (1.0 - t) * 0.6, 0.0, 1.0);
        if (a <= 0.0) discard;
        o_color = vec4(v_tint.rgb, a * v_tint.a);
        return;
    } else if (v_shape_id == 5u) {
        // FIBRIN CLOT. See sdf_clot above.
        float dissolve = 1.0 - clamp(v_tint.a, 0.0, 1.0);
        float platelet_d, strand;
        float body_d = sdf_clot(v_local, v_shape_param, v_anim_phase, dissolve, platelet_d, strand);

        float a = 1.0 - smoothstep(-0.020, 0.008, body_d);
        // Shadow is a bar too, not a disc: the same rounded box, un-frayed,
        // offset the way entity_shadow offsets its disc.
        float he_y = 0.5 / max(v_shape_param, 1.0);
        vec2 hp = v_local - kEntityShadowDir;
        float rr = he_y * 0.85;
        vec2 hq = abs(hp) - (vec2(0.5, he_y) - vec2(rr));
        float sd = length(max(hq, 0.0)) + min(max(hq.x, hq.y), 0.0) - rr;
        float sh = (1.0 - smoothstep(-0.10, 0.03, sd)) * 0.42 * (1.0 - dissolve);
        if (a <= 0.0 && sh <= 0.0) discard;

        float depth = clamp(-body_d / max(he_y, 1e-3), 0.0, 1.0);
        // Pale straw fibrin, warming toward the tint in the interior, with
        // fibre streaks a touch lighter.
        vec3 fibrin = mix(vec3(1.00, 0.97, 0.88), v_tint.rgb, depth * 0.85);
        fibrin = mix(fibrin, vec3(1.00, 0.99, 0.94), strand * 0.35 * depth);
        // Platelets: dusky plum bodies with a wet rim.
        float plt = 1.0 - smoothstep(-0.008, 0.010, platelet_d);
        float plt_rim = 1.0 - smoothstep(0.0, 0.016, abs(platelet_d));
        vec3 rgb = mix(fibrin, vec3(0.55, 0.30, 0.36), plt * 0.85);
        rgb = mix(rgb, vec3(0.82, 0.58, 0.60), plt_rim * 0.50);
        float rim = 1.0 - smoothstep(0.0, 0.030, abs(body_d));
        rgb = mix(rgb, vec3(1.0, 0.98, 0.90), rim * 0.55);

        // Alpha is spent on thinning above, not on translucency: a clot the
        // horde cannot cross should never look see-through, so the body is
        // drawn opaque right up to the strands that remain.
        o_color = over_shadow(rgb, a, sh);
        if (o_color.a <= 0.001) discard;
        return;
    } else if (v_shape_id == 6u) {
        // COLLAGEN SCAR. See scar_wall above. The renderer sends the wall's
        // state, not a colour, in the tint (Renderer.cpp, submit_entities):
        // r = assembled 0..1, g = repair freshness 1..0, b = a per-wall
        // seed, a = the integrity shown.
        float aspect = max(v_shape_param, 1.0);
        // Half-width units: the bar's thickness runs -1..1 across.
        vec2 q = v_local * (2.0 * aspect);
        float px = max(fwidth(q.x), fwidth(q.y)) * 0.75;
        // The drop shadow falls the way every other body's does in WORLD
        // space, so it is counter-rotated into the bar's frame. Walls are a
        // half-width of about one world unit, so that is the unit used.
        float cr = cos(v_rotation), sr = sin(v_rotation);
        vec2 sdw = vec2(0.30, -0.26);
        vec2 shadow_q = q - vec2(sdw.x * cr + sdw.y * sr, -sdw.x * sr + sdw.y * cr);
        float sh;
        vec4 wall = shade_scar(q, shadow_q, aspect, v_tint.b * 7.31, v_tint.r,
                               1.0 - clamp(v_tint.a, 0.0, 1.0), v_tint.g, px, sh);
        o_color = over_shadow(wall.rgb, wall.a, sh);
        if (o_color.a <= 0.001) discard;
        return;
    } else if (v_shape_id == 16u) {
        // GUNNER (Neutrophil). Tower shape ids start at 16; see
        // kTowerShapeBase in TowerSystem.cpp.
        float nucleus_d, granule;
        float body_d = sdf_neutrophil(v_local, v_anim_phase, nucleus_d, granule);

        float a = 1.0 - smoothstep(-0.030, 0.010, body_d);
        float sh = entity_shadow(v_local, 0.46);
        if (a <= 0.0 && sh <= 0.0) discard;

        // Fake subsurface scattering: how deep inside the membrane this pixel
        // is. Thin edges stay pale and translucent, the interior goes denser —
        // the difference between a flat white disc and something wet.
        float depth = clamp(-body_d * 5.0, 0.0, 1.0);

        // Neutrophils are near-colourless in life; keep the body pale and let
        // v_tint carry the team/tier hue rather than repainting the cell.
        vec3 cytoplasm = mix(vec3(0.94, 0.95, 0.97), vec3(0.66, 0.70, 0.80), depth);
        cytoplasm = mix(cytoplasm, v_tint.rgb, 0.18);

        // Granules, only in the cytoplasm (never over the nucleus).
        float in_cyto = smoothstep(0.0, 0.05, nucleus_d);
        cytoplasm = mix(cytoplasm, vec3(0.99, 0.93, 0.72), granule * in_cyto * 0.45);

        // The lobed nucleus: darker, violet-ish, the way a stained smear reads.
        float nuc = 1.0 - smoothstep(-0.012, 0.012, nucleus_d);
        vec3 rgb = mix(cytoplasm, vec3(0.42, 0.36, 0.55), nuc * 0.85);

        // Membrane rim. Bright band at the silhouette edge, which is what sells
        // a wet cell boundary and separates the tower from the dark tissue.
        float rim = 1.0 - smoothstep(0.0, 0.055, abs(body_d));
        rgb = mix(rgb, vec3(1.0), rim * 0.68);

        // Towers carry integrity, not alpha, in v_tint.a: see wounded().
        o_color = over_shadow(wounded(rgb, body_d, v_tint.a), a, sh);
        if (o_color.a <= 0.001) discard;
        return;
    } else if (v_shape_id == 17u) {
        // ARBOR GRABBER (Macrophage). Its tower silhouette previews the
        // released unit's defining feature: roots that fork into fine fingers
        // reaching out in every direction.
        float nucleus_d, granule;
        float tier = floor(v_shape_param + 0.001);
        float body_d = sdf_macrophage(v_local, v_anim_phase, tier,
                                      nucleus_d, granule);

        float a = 1.0 - smoothstep(-0.022, 0.008, body_d);
        float sh = entity_shadow(v_local, 0.38);
        if (a <= 0.0 && sh <= 0.0) discard;

        float depth = clamp(-body_d * 4.7, 0.0, 1.0);
        const vec3 kMacroHue = vec3(1.00, 0.56, 0.14);
        vec3 cytoplasm = mix(vec3(1.00, 0.93, 0.82), vec3(0.78, 0.32, 0.06), depth);
        cytoplasm = mix(cytoplasm, kMacroHue, 0.46);
        float in_cyto = smoothstep(0.0, 0.05, nucleus_d);
        vec3 rgb = mix(cytoplasm, vec3(1.00, 0.80, 0.55), granule * in_cyto * 0.35);

        float nuc = 1.0 - smoothstep(-0.012, 0.012, nucleus_d);
        rgb = mix(rgb, vec3(0.36, 0.16, 0.06), nuc * 0.86);
        float rim = 1.0 - smoothstep(0.0, 0.042, abs(body_d));
        rgb = mix(rgb, vec3(1.00, 0.94, 0.84), rim * 0.68);

        o_color = over_shadow(wounded(rgb, body_d, v_tint.a), a, sh);
        if (o_color.a <= 0.001) discard;
        return;
    } else if (v_shape_id == 18u) {
        // TESLA (Cytotoxic T). THE PURPLE TOWER, and it has to be purple all
        // the way down. The other cells sit on a near-white cytoplasm and let a
        // tint do the identifying; this body is the smallest and busiest of the
        // five, and at that size a pale one hands its hue back to its own
        // detail. So the interior runs to a deep saturated violet and the tint
        // goes in harder than anywhere else in this file except the Macrophage.
        float nucleus_d, granule_d, synapse_glow, lance, speckle;
        float body_d = sdf_cytotoxic(v_local, v_anim_phase, v_shape_param,
                                     nucleus_d, granule_d, synapse_glow, lance, speckle);

        float a = 1.0 - smoothstep(-0.020, 0.008, body_d);
        float sh = entity_shadow(v_local, 0.29);
        float spark = max(synapse_glow, lance);
        if (a <= 0.0 && sh <= 0.0 && spark <= 0.005) discard;
        // The lancing filaments are allowed to draw proud of the membrane; see
        // sdf_cytotoxic. The synapse band is not — it lives inside the cell.
        a = max(a, lance * 0.80);

        // Shallower ramp than the 6.0 this used to run at. This body is thin —
        // a flat plate and a tail — so almost none of it is far enough inside
        // the membrane to reach the saturated end of a steep ramp, and the
        // whole cell came out the pale lilac the surface colour alone gives.
        float depth = clamp(-body_d * 4.2, 0.0, 1.0);
        // Identity hue, shared verbatim with palette_for() in vfx/Particles.cpp
        // and with the Chain field tint in Renderer.cpp: the body, the granules
        // it sheds and its discharge all have to be the same violet.
        const vec3 kTeslaHue = vec3(0.76, 0.66, 1.00);

        // Mixed toward that hue only lightly. kTeslaHue is itself a PALE violet
        // — it has to be, it doubles as a particle colour on a dark red field —
        // so leaning on it hard would wash the body out instead of saturating
        // it. The purple comes from the gradient here; the hue mix only pulls
        // it onto the roster's exact violet.
        vec3 cytoplasm = mix(vec3(0.62, 0.47, 0.95), vec3(0.24, 0.11, 0.54), depth);
        cytoplasm = mix(cytoplasm, kTeslaHue, 0.18);
        // Cytoplasmic speckle, suppressed over the nucleus so the two never
        // fight for the same pixels — same treatment the Macrophage gives its
        // granulation.
        float in_cyto = smoothstep(0.0, 0.045, nucleus_d);
        vec3 rgb = mix(cytoplasm, vec3(0.82, 0.74, 1.00), speckle * in_cyto * 0.30);

        float nuc = 1.0 - smoothstep(-0.012, 0.012, nucleus_d);
        // Chromatin mottling. This nucleus fills most of the back half, and one
        // flat colour over that much area reads as a hole punched through the
        // sprite rather than as an organelle inside it.
        float chromatin = fbm(v_local * 14.0 + vec2(v_anim_phase * 0.03, 0.0));
        rgb = mix(rgb, mix(vec3(0.15, 0.08, 0.33), vec3(0.34, 0.23, 0.57), chromatin), nuc * 0.92);
        // Nuclear envelope, so the core sits INSIDE the cell instead of on it.
        rgb = mix(rgb, vec3(0.74, 0.64, 0.96),
                  (1.0 - smoothstep(0.0, 0.018, abs(nucleus_d))) * 0.55);

        // Granules get a hot white core, not just a pale tint — they are the
        // payload, they are the tier readout, and they have to out-read the
        // cytoplasm they are docked in.
        float gran = 1.0 - smoothstep(-0.006, 0.008, granule_d);
        rgb = mix(rgb, vec3(0.86, 0.74, 1.00), gran * 0.95);
        rgb = mix(rgb, vec3(1.0), (1.0 - smoothstep(-0.014, -0.004, granule_d)) * 0.45);

        // Narrower and dimmer membrane rim than the blobby towers get. Theirs
        // is a wide body with a thin bright edge; this one is small enough that
        // a 0.035 band of near-white was a third of its area, and it took the
        // purple back off the cell the gradient had just put on.
        float rim = 1.0 - smoothstep(0.0, 0.024, abs(body_d));
        rgb = mix(rgb, vec3(0.93, 0.88, 1.00), rim * 0.50);

        // LAST. The synapse is the brightest thing on the cell, and painting it
        // before the membrane rim let the rim write white over the crackle and
        // erase it.
        // Kept VIOLET rather than white: it sits right next to the rim and the
        // granule speculars, and three white features stacked in the same
        // twenty pixels just read as one blown-out smear.
        rgb = mix(rgb, vec3(0.93, 0.87, 1.00), clamp(synapse_glow, 0.0, 1.0) * 0.85);
        rgb = mix(rgb, vec3(1.00, 0.98, 1.00), clamp(lance, 0.0, 1.0) * 0.95);

        o_color = over_shadow(wounded(rgb, body_d, v_tint.a), a, sh);
        if (o_color.a <= 0.001) discard;
        return;
    } else if (v_shape_id == 19u) {
        // HYDRO (Goblet Cell).
        float nucleus_d, granule, mouth_glow, rim_band;
        float body_d = sdf_goblet(v_local, v_anim_phase, v_shape_param,
                                  nucleus_d, granule, mouth_glow, rim_band);

        float a = 1.0 - smoothstep(-0.022, 0.008, body_d);
        float sh = entity_shadow(v_local, 0.30);
        if (a <= 0.0 && sh <= 0.0) discard;
        // The meniscus sitting in the mouth reads slightly past the membrane,
        // so the cell looks charged even on the frames between bursts.
        a = max(a, mouth_glow * 0.45);

        float depth = clamp(-body_d * 5.5, 0.0, 1.0);
        const vec3 kMucinHue = vec3(0.55, 0.98, 0.74);

        // Cytoplasm reads WET rather than solid: a pale surface over a deeper,
        // more saturated interior, which is the same value structure the fluid
        // pass gives an actual puddle. The tower and its output then look like
        // the same substance, which is most of why the weapon reads as coming
        // out of this specific cell.
        vec3 cytoplasm = mix(vec3(0.93, 1.00, 0.95), vec3(0.30, 0.72, 0.56), depth);
        cytoplasm = mix(cytoplasm, kMucinHue, 0.34);
        vec3 rgb = cytoplasm;

        // Mucin granules: bright, slightly milky beads. Suppressed over the
        // nucleus so the two organelles never fight for the same pixels.
        float in_cyto = smoothstep(0.0, 0.04, nucleus_d);
        rgb = mix(rgb, vec3(0.88, 1.00, 0.88), granule * in_cyto * 0.85);
        rgb = mix(rgb, vec3(1.00, 1.00, 0.96), pow(granule * in_cyto, 3.0) * 0.55);

        // Basal nucleus, flattened against the stalk end.
        float nuc = 1.0 - smoothstep(-0.010, 0.010, nucleus_d);
        rgb = mix(rgb, vec3(0.20, 0.45, 0.44), nuc * 0.88);
        rgb = mix(rgb, vec3(0.74, 0.98, 0.88),
                  (1.0 - smoothstep(0.0, 0.014, abs(nucleus_d))) * 0.45);

        // The wet bore: a bright inner wall plus the pooled meniscus, so the
        // mouth reads as full of liquid rather than as a notch cut out.
        rgb = mix(rgb, vec3(0.72, 1.00, 0.86), rim_band * 0.70);
        rgb = mix(rgb, vec3(0.80, 1.00, 0.90), mouth_glow * 0.62);
        rgb = mix(rgb, vec3(1.0), pow(mouth_glow, 3.0) * 0.75);

        float rim = 1.0 - smoothstep(0.0, 0.042, abs(body_d));
        rgb = mix(rgb, vec3(0.95, 1.00, 0.97), rim * 0.60);

        o_color = over_shadow(wounded(rgb, body_d, v_tint.a), a, sh);
        if (o_color.a <= 0.001) discard;
        return;
    } else if (v_shape_id == 20u) {
        // BUILDER (Fibroblast), the legacy stationary emitter the gym's
        // `tower` command still places: the deployed cell's own body
        // (swarmer.frag kind 5), standing still, its hue the roster's.
        float sh;
        vec4 cell = shade_fibroblast_cell(v_local, v_anim_phase * 0.6, 0.0, 0.0, 1.0, 0.0,
                                          vec3(0.42, 0.74, 1.00), sh);
        float nucleus_d, nucleolus, vesicle, lamella, thread_d, pole_glow;
        float body_d = sdf_fibroblast_cell(v_local, v_anim_phase * 0.6, 0.0, 0.0, 1.0, nucleus_d,
                                           nucleolus, vesicle, lamella, thread_d, pole_glow);
        o_color = over_shadow(wounded(cell.rgb, body_d, v_tint.a), cell.a, sh);
        if (o_color.a <= 0.001) discard;
        return;
    } else if (v_shape_id == 2u) {
        d = sdf_diamond(v_local, 0.45 + pulse);
    } else {
        d = sdf_circle(v_local, 0.5 + pulse);
    }

    float alpha = (1.0 - smoothstep(-0.06, 0.0, d)) * alpha_mul;
    // Elites get the same contact shadow; overlays (rings, telegraphs, bursts)
    // return before this point and stay shadowless, which is right — they are
    // UI drawn in world space, not objects sitting on the tissue.
    float sh = entity_shadow(v_local, 0.46);
    if (alpha <= 0.0 && sh <= 0.0) discard;
    // Rim light so an elite holds its own silhouette on a bright lane.
    float erim = 1.0 - smoothstep(0.0, 0.09, abs(d));
    vec3 ergb = mix(v_tint.rgb * mix(1.22, 0.80, clamp(-d * 3.0, 0.0, 1.0)),
                    vec3(1.0), erim * 0.45);
    o_color = over_shadow(ergb, alpha * v_tint.a, sh);
    if (o_color.a <= 0.001) discard;
}
