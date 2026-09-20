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
//       the aspect as for 5, and v_tint.a its remaining INTEGRITY fraction)
// Tower shape ids start at 16 (kTowerShapeBase in TowerSystem.cpp) and run in
// TowerType declaration order, so id == 16 + TowerType:
//   16 = Neutrophil   17 = Macrophage (branching multi-grabber)
//   18 = Interferon   19 = Cytotoxic T
//   20 = Goblet Cell  21 = Fibroblast
// (21 was the NK Cell's rotor before the swarmer roster retired it.)
// Any other id falls back to the filled blob.

in vec2  v_local;
in vec4  v_tint;
flat in uint v_shape_id;
in float v_anim_phase;
/// Extra per-shape parameter (EntityInstance::shape_param); meaning is defined
/// by v_shape_id, 0 for shapes that don't declare one. Every tower body (16-20)
/// reads it as the tower's TIER, 1-3, and spends it on a countable feature —
/// phagosomes, crystal reach, lytic granules, mucin granules, blades — so an
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

/// Rotates a point. For features that must turn independently of the quad.
vec2 rot2(vec2 p, float a) {
    float c = cos(a), s = sin(a);
    return vec2(p.x * c - p.y * s, p.x * s + p.y * c);
}

/// Capsule from `a` to `b` of radius `r`. The workhorse for every thin feature
/// below — crystal spicules, antibody arms, microvilli.
float sdf_segment(vec2 p, vec2 a, vec2 b, float r) {
    vec2 pa = p - a, ba = b - a;
    float h = clamp(dot(pa, ba) / max(dot(ba, ba), 1e-8), 0.0, 1.0);
    return length(pa - ba * h) - r;
}

/// Regular hexagon. The one deliberately HARD primitive in this file: every
/// cell here is built from circles and noise, so a shape with straight edges
/// and corners reads as "not alive" instantly — which is exactly the Interferon
/// crystal's job.
float sdf_hexagon(vec2 p, float r) {
    const vec3 k = vec3(-0.866025404, 0.5, 0.577350269);
    p = abs(p);
    p -= 2.0 * min(dot(k.xy, p), 0.0) * k.xy;
    p -= vec2(clamp(p.x, -k.z * r, k.z * r), r);
    return length(p) * sign(p.y);
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
// CRYO — Interferon.
//
// SILHOUETTE CONTRAST is the design goal. Five of six towers are amoeboid
// blobs; if the sixth is another blob in another colour then hue is doing all
// the work, and the roster stops being readable the moment two towers overlap
// or the player is colour-blind. So this is the hard-edged one: a six-fold
// crystal with straight facets and needle tips, grown through a soft cell
// membrane. Nothing else on the field has a corner on it.
//
// It is also honest about the mechanism. Interferon is a signalling protein,
// not a cell that stabs things — the crystal is the SIGNAL crystallising out of
// the cell, which is why it pierces the membrane instead of being contained by
// it, and why the forward face carries a bright aperture with vapour venting
// past it along the cone the tower actually fires.
// ---------------------------------------------------------------------------
const float kCryoReach = 0.455;

/// One arm of the flake, repeated six times by angular folding. Folding rather
/// than looping means the branch geometry is written once and costs one
/// evaluation however many arms there are.
float cryo_flake(vec2 q, float reach, float w) {
    const float kSector = 1.04719755; // 2*pi/6
    float a = atan(q.y, q.x);
    float k = mod(a + kSector * 0.5, kSector) - kSector * 0.5;
    vec2 fq = vec2(cos(k), sin(k)) * length(q);
    // Mirror about the arm's own axis so ONE branch expression draws the pair.
    fq.y = abs(fq.y);

    vec2 br = vec2(cos(0.95), sin(0.95));
    float d = sdf_segment(fq, vec2(0.020, 0.0), vec2(reach, 0.0), w);
    d = min(d, sdf_segment(fq, vec2(reach * 0.40, 0.0),
                           vec2(reach * 0.40, 0.0) + br * reach * 0.34, w * 0.72));
    d = min(d, sdf_segment(fq, vec2(reach * 0.72, 0.0),
                           vec2(reach * 0.72, 0.0) + br * reach * 0.20, w * 0.55));
    return d;
}

float sdf_interferon(vec2 p, float phase, float tier, out float crystal_d,
                     out float facet, out float aperture, out float vapour) {
    // The cell underneath. Barely warped: it is mostly a soft halo for the
    // crystal to sit in, and a wobbling membrane would fight the hard edges.
    float wx = fbm(p * 5.0 + vec2(phase * 0.07, 0.0)) - 0.5;
    float wy = fbm(p * 5.0 + vec2(6.3, -phase * 0.06)) - 0.5;
    float membrane = length(p + vec2(wx, wy) * 0.045) - 0.250;

    // The crystal turns slowly INSIDE the quad, independently of where the
    // tower is aiming, so a re-aim doesn't snap the whole flake to a new angle.
    vec2 cp = rot2(p, phase * 0.06);
    crystal_d = min(sdf_hexagon(cp, 0.135),
                    cryo_flake(cp, kCryoReach + 0.012 * tier, 0.026));

    // Concentric hexagonal contours read as internal facets / cleavage planes.
    float band = abs(fract(sdf_hexagon(cp, 0.095) * 9.0) - 0.5);
    facet = 1.0 - smoothstep(0.12, 0.38, band);

    // Emission aperture, pinned to local +x — which entity.vert has already
    // rotated onto the aim, so it is always the face pointed at the target.
    float ang = atan(p.y, p.x);
    float face = smoothstep(0.90, 0.25, abs(ang));
    aperture = face * (1.0 - smoothstep(0.0, 0.085, abs(length(p) - 0.250)));

    // Cold vapour venting forward past the membrane. This lives OUTSIDE the
    // body, so main() composites it over the lit result rather than shading it.
    vapour = face * (1.0 - smoothstep(0.05, 0.26, length(p - vec2(0.30, 0.0))))
           * (0.55 + 0.45 * fbm(p * 9.0 + vec2(-phase * 0.5, phase * 0.2)));

    return smin(membrane, crystal_d, 0.035);
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
// Neutrophil and the Macrophage are radial lumps, the Interferon is a crystal,
// and the Goblet Cell — the only other directional
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
// Scar tissue is collagen laid down in dense parallel bundles, and that is
// the read here: the same rounded bar the sim carved, less frayed than the
// clot (this is a structure, not a tangle), striped end to end with fibre
// bundles that run ALONG the bar. No platelets -- collagen is the fibroblast's
// own product, nothing is caught in it.
//
// `damage` is 1 - v_tint.a: the horde chews the wall from its faces, so the
// bar loses thickness in noisy bites along its length and cracks open along
// the fibre lines as its integrity falls, and at nothing it is gone. The
// bites are seeded by position, not by time, so a wall that is being eaten
// looks eaten in one place rather than shimmering everywhere.
// ---------------------------------------------------------------------------
float sdf_scar(vec2 p, float aspect, float phase, float damage, out float fibre,
               out float crack) {
    vec2 he = vec2(0.5, 0.5 / max(aspect, 1.0));
    // A slight, slow writhe along the long edges: collagen creeps.
    float w = fbm(vec2(p.x * 7.0 + phase * 0.02, p.y * 10.0 + 1.9)) - 0.5;
    vec2 wp = vec2(p.x, p.y + w * he.y * 0.30);
    float r = he.y * 0.70;
    vec2 q = abs(wp) - (he - vec2(r));
    float box = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;

    // Bites: the faces are chewed in where the noise says the crowd was.
    // Deeper toward the ends first, so a wall dies from its tips inward and
    // the last of it is the middle still holding.
    float bite = vnoise(vec2(p.x * 9.0 + 5.1, 0.0)) * 0.7 + vnoise(vec2(p.x * 23.0, 2.2)) * 0.3;
    float thin = damage * he.y * (0.55 + 0.9 * bite) * (0.8 + 0.5 * abs(p.x) / he.x);
    float body = box + thin;

    // Fibre bundles: bands running along the bar, gently waved.
    float band = vnoise(vec2(p.x * 9.0 + phase * 0.01, p.y * 85.0));
    fibre = smoothstep(0.38, 0.62, band);
    // Cracks open along the bands as integrity falls: long in x, thin in y,
    // so they read as splits between fibre bundles and not as pockmarks.
    float cn = vnoise(vec2(p.x * 7.0 + 2.3, p.y * 75.0 + 7.0)) * 0.7 + vnoise(vec2(p.x * 19.0, p.y * 140.0)) * 0.3;
    crack = smoothstep(1.0 - damage * 0.85, 1.0 - damage * 0.85 + 0.10, cn) * step(0.0, -body);
    return body;
}

// ---------------------------------------------------------------------------
// BUILDER -- Fibroblast.
//
// A fibroblast is the spindle of the cell world: a long tapered body with
// an oval nucleus in its waist and thin processes trailing from both tips,
// crawling through the matrix it lays down. So this is the one tower body
// that is not round at all: a fusiform soma, warped just enough to read as a
// membrane, a central ellipsoid nucleus, and `2 + tier` fine processes
// reaching out of the two ends, which is the countable feature an upgrade
// adds. Inside, faint fibre streaks run along the long axis -- the collagen
// the cell is full of, and the same striping its scars carry, so the wall
// and the cell that laid it read as one material.
// ---------------------------------------------------------------------------
float sdf_fibroblast(vec2 p, float phase, float tier, out float nucleus_d,
                     out float fibre, out float process_d) {
    float wx = fbm(p * 5.0 + vec2(phase * 0.08, 0.0)) - 0.5;
    float wy = fbm(p * 5.0 + vec2(3.1, -phase * 0.07)) - 0.5;
    vec2 wp = p + vec2(wx, wy) * 0.035;

    // Fusiform soma: an ellipse whose across-axis squash grows toward the
    // tips, so the ends draw to points instead of rounding off.
    float taper = 2.3 + 3.2 * smoothstep(0.16, 0.44, abs(wp.x));
    float body = length(vec2(wp.x, wp.y * taper)) - 0.44;
    // A slight bow along the length -- fibroblasts are never quite straight.
    body += 0.010 * sin(wp.x * 5.0 + phase * 0.3) * (1.0 - abs(wp.x) * 1.4);

    // Processes: fine tapering filaments from each tip, fanned a little,
    // 2 + tier of them split across the two ends.
    process_d = 1e9;
    int n = 2 + int(clamp(tier, 1.0, 3.0));
    for (int k = 0; k < 5; ++k) {
        if (k >= n) break;
        float side = (k % 2 == 0) ? 1.0 : -1.0;
        float fk = float(k / 2);
        float fan = (fk - 0.5 * float((n - 1) / 2)) * 0.32 + 0.12 * sin(phase * 0.4 + fk * 1.7);
        vec2 root = vec2(side * 0.41, 0.0);
        vec2 dir = normalize(vec2(side * 1.0, fan));
        float len = 0.16 + 0.05 * fk;
        vec2 tip = root + dir * len;
        // Thin at the tip, a touch thicker at the root.
        float d = sdf_segment(p, root, tip, 0.012);
        float along = clamp(dot(p - root, dir) / len, 0.0, 1.0);
        process_d = min(process_d, d + along * 0.006);
    }
    body = smin(body, process_d, 0.03);

    // Nucleus: an ellipsoid in the waist, long axis with the cell.
    vec2 np = p - vec2(0.0, 0.0);
    nucleus_d = length(vec2(np.x, np.y * 1.9)) - 0.115;

    // Collagen streaks along the long axis.
    fibre = smoothstep(0.42, 0.62, vnoise(vec2(p.x * 12.0 + phase * 0.02, p.y * 60.0)));
    return body;
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
        // COLLAGEN SCAR. See sdf_scar above.
        float damage = 1.0 - clamp(v_tint.a, 0.0, 1.0);
        float fibre, crack;
        float body_d = sdf_scar(v_local, v_shape_param, v_anim_phase, damage, fibre, crack);

        float a = 1.0 - smoothstep(-0.018, 0.008, body_d);
        float he_y = 0.5 / max(v_shape_param, 1.0);
        vec2 hp = v_local - kEntityShadowDir;
        float rr = he_y * 0.70;
        vec2 hq = abs(hp) - (vec2(0.5, he_y) - vec2(rr));
        float sd = length(max(hq, 0.0)) + min(max(hq.x, hq.y), 0.0) - rr;
        float sh = (1.0 - smoothstep(-0.10, 0.03, sd)) * 0.42 * (1.0 - damage * 0.6);
        if (a <= 0.0 && sh <= 0.0) discard;

        float depth = clamp(-body_d / max(he_y, 1e-3), 0.0, 1.0);
        // Collagen: pale, faintly warm, denser and rosier in the interior.
        vec3 collagen = mix(vec3(1.00, 0.95, 0.92), v_tint.rgb, depth * 0.80);
        // Fibre bundles a shade lighter and, between them, a shade deeper.
        collagen = mix(collagen, vec3(1.00, 0.98, 0.96), fibre * 0.30 * depth);
        collagen = mix(collagen, v_tint.rgb * 0.80, (1.0 - fibre) * 0.18 * depth);
        // Cracks: dark seams where the wall is giving way.
        vec3 rgb = mix(collagen, vec3(0.38, 0.16, 0.20), crack * 0.85);
        // Chewed faces go bruised, like a wounded tower does.
        float lum = dot(rgb, vec3(0.30, 0.59, 0.11));
        rgb = mix(rgb, mix(vec3(lum), vec3(0.50, 0.22, 0.30), 0.5), damage * 0.35 * (1.0 - depth * 0.5));
        float rim = 1.0 - smoothstep(0.0, 0.028, abs(body_d));
        rgb = mix(rgb, vec3(1.0, 0.97, 0.95), rim * 0.55);

        // Opaque up to whatever is left: a wall the horde cannot cross should
        // never look see-through, and the damage is spent on the shape.
        o_color = over_shadow(rgb, a, sh);
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
        const vec3 kMacroHue = vec3(0.98, 0.42, 0.58);
        vec3 cytoplasm = mix(vec3(1.00, 0.91, 0.92), vec3(0.64, 0.12, 0.27), depth);
        cytoplasm = mix(cytoplasm, kMacroHue, 0.46);
        float in_cyto = smoothstep(0.0, 0.05, nucleus_d);
        vec3 rgb = mix(cytoplasm, vec3(1.00, 0.72, 0.78), granule * in_cyto * 0.35);

        float nuc = 1.0 - smoothstep(-0.012, 0.012, nucleus_d);
        rgb = mix(rgb, vec3(0.31, 0.09, 0.20), nuc * 0.86);
        float rim = 1.0 - smoothstep(0.0, 0.042, abs(body_d));
        rgb = mix(rgb, vec3(1.00, 0.91, 0.94), rim * 0.68);

        o_color = over_shadow(wounded(rgb, body_d, v_tint.a), a, sh);
        if (o_color.a <= 0.001) discard;
        return;
    } else if (v_shape_id == 18u) {
        // CRYO (Interferon).
        float crystal_d, facet, aperture, vapour;
        float body_d = sdf_interferon(v_local, v_anim_phase, v_shape_param,
                                      crystal_d, facet, aperture, vapour);

        // Tighter AA than the blobby towers: this body's whole point is that it
        // has straight edges, and a soft edge on a facet is a wasted facet.
        float a = 1.0 - smoothstep(-0.018, 0.007, body_d);
        float sh = entity_shadow(v_local, 0.28);
        float vap_a = vapour * 0.50;
        if (a <= 0.0 && sh <= 0.0 && vap_a <= 0.003) discard;

        float depth = clamp(-body_d * 5.5, 0.0, 1.0);
        const vec3 kCryoHue = vec3(0.52, 0.84, 1.00);

        vec3 cytoplasm = mix(vec3(0.93, 0.98, 1.00), vec3(0.50, 0.72, 0.92), depth);
        cytoplasm = mix(cytoplasm, kCryoHue, 0.35);

        float ice = 1.0 - smoothstep(-0.006, 0.008, crystal_d);
        vec3 rgb = mix(cytoplasm, vec3(0.80, 0.94, 1.00), ice * 0.85);
        rgb = mix(rgb, vec3(1.0), ice * facet * 0.55);
        float ice_rim = 1.0 - smoothstep(0.0, 0.014, abs(crystal_d));
        rgb = mix(rgb, vec3(1.0), ice_rim * 0.70);
        rgb = mix(rgb, vec3(0.88, 0.99, 1.00), aperture * 0.85);

        float rim = 1.0 - smoothstep(0.0, 0.040, abs(body_d));
        rgb = mix(rgb, vec3(0.92, 0.99, 1.00), rim * 0.55);

        // The vent plume is atmosphere in FRONT of the cell, so it composites
        // over the finished body rather than being mixed into its shading —
        // otherwise it would only ever show up where the body already is, which
        // is precisely where it isn't.
        vec4 lit = over_shadow(wounded(rgb, body_d, v_tint.a), a, sh);
        float out_a = lit.a + vap_a * (1.0 - lit.a);
        if (out_a <= 0.001) discard;
        o_color = vec4((lit.rgb * lit.a + kCryoHue * vap_a * (1.0 - lit.a)) / out_a, out_a);
        return;
    } else if (v_shape_id == 19u) {
        // TESLA (Cytotoxic T). THE PURPLE TOWER, and it has to be purple all
        // the way down. The other cells sit on a near-white cytoplasm and let a
        // tint do the identifying; this body is the smallest and busiest of the
        // six, and at that size a pale one hands its hue back to its own
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
    } else if (v_shape_id == 20u) {
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
    } else if (v_shape_id == 21u) {
        // BUILDER (Fibroblast). See sdf_fibroblast above.
        float nucleus_d, fibre, process_d;
        float body_d = sdf_fibroblast(v_local, v_anim_phase, v_shape_param,
                                      nucleus_d, fibre, process_d);

        float a = 1.0 - smoothstep(-0.022, 0.008, body_d);
        // A thin body: the shadow is a squashed disc so it stays under the cell.
        vec2 sp = v_local - kEntityShadowDir;
        float sd = length(vec2(sp.x, sp.y * 2.4)) - 0.42;
        float sh = (1.0 - smoothstep(-0.10, 0.03, sd)) * 0.42;
        if (a <= 0.0 && sh <= 0.0) discard;

        // Shallow ramp: this body is thin end to end, so almost none of it
        // is far inside the membrane.
        float depth = clamp(-body_d * 5.0, 0.0, 1.0);
        // Identity hue, shared verbatim with palette_for() in vfx/Particles.cpp:
        // salmon, the colour of fresh granulation tissue.
        const vec3 kCollagenHue = vec3(1.00, 0.72, 0.64);
        vec3 cytoplasm = mix(vec3(1.00, 0.92, 0.88), vec3(0.82, 0.48, 0.44), depth);
        cytoplasm = mix(cytoplasm, kCollagenHue, 0.22);
        // Collagen streaks, suppressed over the nucleus.
        float in_cyto = smoothstep(0.0, 0.04, nucleus_d);
        vec3 rgb = mix(cytoplasm, vec3(1.00, 0.86, 0.80), fibre * in_cyto * 0.45 * depth);

        // Nucleus: dusky mauve, with a chromatin mottle so it reads as an
        // organelle and not a hole.
        float nuc = 1.0 - smoothstep(-0.010, 0.010, nucleus_d);
        float chromatin = fbm(v_local * 16.0 + vec2(v_anim_phase * 0.03, 0.0));
        rgb = mix(rgb, mix(vec3(0.42, 0.20, 0.30), vec3(0.60, 0.34, 0.42), chromatin), nuc * 0.90);
        rgb = mix(rgb, vec3(0.92, 0.70, 0.72), (1.0 - smoothstep(0.0, 0.016, abs(nucleus_d))) * 0.50);

        // The processes are brighter than the soma they leave, so the tier
        // count reads even where they are a pixel wide.
        float proc = 1.0 - smoothstep(-0.004, 0.010, process_d);
        rgb = mix(rgb, vec3(1.00, 0.90, 0.86), proc * 0.55 * smoothstep(0.30, 0.42, abs(v_local.x)));

        float rim = 1.0 - smoothstep(0.0, 0.030, abs(body_d));
        rgb = mix(rgb, vec3(1.00, 0.96, 0.94), rim * 0.55);

        o_color = over_shadow(wounded(rgb, body_d, v_tint.a), a, sh);
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
