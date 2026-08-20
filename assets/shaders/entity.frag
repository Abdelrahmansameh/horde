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
// Tower shape ids start at 16 (kTowerShapeBase in TowerSystem.cpp) and run in
// TowerType declaration order, so id == 16 + TowerType:
//   16 = GUNNER (Neutrophil)   17 = MORTAR (Macrophage)
//   18 = CRYO   (Interferon)   19 = TESLA  (Cytotoxic T)
//   20 = HYDRO  (Goblet Cell)   21 = BLADE  (NK Cell)
// Any other id falls back to the filled blob.

in vec2  v_local;
in vec4  v_tint;
flat in uint v_shape_id;
in float v_anim_phase;
/// Extra per-shape parameter (EntityInstance::shape_param); meaning is defined
/// by v_shape_id, 0 for shapes that don't declare one. Every tower body (16-21)
/// reads it as the tower's TIER, 1-3, and spends it on a countable feature —
/// phagosomes, crystal reach, microvilli, antibodies, blades — so an upgrade
/// is legible from the silhouette instead of only from the stat panel.
in float v_shape_param;
/// Instance world rotation; see entity.vert. Used to hold a feature still while
/// the quad spins.
in float v_rotation;

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

// ---------------------------------------------------------------------------
// BLADE — NK Cell.
//
// NK cells are small, dense granular lymphocytes -- tighter and rounder than
// the Gunner's amoeboid neutrophil, with a single large eccentric nucleus
// rather than a lobed one. The "blade" isn't a literal weapon: it's a stylised
// take on the real kill mechanism -- an NK cell reorients its microtubule-
// organizing centre toward the target and fires perforin/granzyme down the
// cytoskeleton at the immunological synapse.
//
// SCALE. Unlike every other tower this quad is sized to the tower's RANGE, not
// its footprint (see tower_sprite_size in TowerSystem.cpp), because
// system_blade's damage really does cover the whole disc. So local radius
// kNkReach maps to exactly st.range in world units, and the blades are drawn
// out to it: the silhouette is a truthful readout of the kill zone, and the
// glowing tips mark its boundary. That makes the body double as the tower's
// own range indicator, which is why it must re-render on upgrade.
//
// Because the quad is ~10x the size of every other tower's, it covers ~10x the
// pixels, so the antialiasing widths below are deliberately much tighter than
// the Gunner's -- reusing those would smear the edges into mush at this scale.
//
// LOOKING GOOD AT THAT SIZE is the real problem, and three things do the work:
//   1. The blades SWEEP (each centreline lags by kNkSweep radians from hub to
//      rim) instead of being straight spokes. A straight spoke at this length
//      reads as a static star; a trailing curve reads as rotation even in a
//      still frame, and covers far more of the disc.
//   2. They taper hard and FADE toward the tip, dissolving into the field glow
//      rather than ending in a hard edge -- so a huge sprite doesn't read as a
//      few thin sticks over empty space.
//   3. The hub stays small in local space (kNkHubR is roughly the real
//      footprint over the range), so growing the quad doesn't inflate the cell
//      into a blob; it keeps the "tiny cell, long reach" silhouette.
//
// comp::Transform::rotation (driven by system_blade's continuous
// kBladeSpinRadPerSec spin) already rotates the whole instance quad in the
// vertex stage, so the geometry below is defined in a FIXED local orientation
// -- the spin comes for free and needs no phase term, unlike the neutrophil's
// writhing warp.
// ---------------------------------------------------------------------------
const float kNkReach   = 0.5;    // local radius == st.range in world units
const float kNkHubR    = 0.085;  // cell body; big enough to still read as a cell
const float kNkRootW   = 0.075;  // TRAILING half-width where the blade leaves the hub
const float kNkLeadFrac = 0.30;  // leading half-width, as a fraction of trailing
const float kNkSweep   = 0.55;   // radians the blade lags across its length
const float kNkGlowR   = 0.022;  // perforin granule radius at each tip
const float kNkTipFade = 0.55;   // blade alpha at the rim
const int   kNkMaxBlades = 5;    // tier 3 -> 2 + 3

/// Returns the blade/hub signed distance. `blade_count` comes straight from
/// EntityInstance::shape_param (2 + tower tier).
float sdf_nk_cell(vec2 p, float phase, float blade_count, float spin,
                  out float nucleus_d, out float radial_t, out float granule_glow,
                  out float edge_glow) {
    float r = length(p);
    radial_t = clamp(r / kNkReach, 0.0, 1.0);

    // THE HUB DOES NOT SPIN. It is the cell; the blades are what sweep around
    // it. The hub and its nucleus are evaluated in `hp` -- `p` rotated back to
    // world alignment -- so they hold still while the rotor turns. Drawn in the
    // raw local frame the eccentric nucleus visibly orbits the centre, which
    // reads as the whole cell tumbling rather than as a rotor spinning.
    // Everything else (blades, leading edges, tip granules) deliberately stays
    // in `p` so it rides the spin.
    //
    // The angle is +spin, NOT -spin. entity.vert hands us the UNROTATED corner
    // and rotates the quad's world offset by R(+rotation); `p` is therefore
    // already expressed in the spinning frame, so applying R(+rotation) is what
    // cancels it back to world. Negating instead double-rotates, and the
    // nucleus orbits backwards at twice the speed.
    float cs = cos(spin);
    float sn = sin(spin);
    vec2 hp = vec2(p.x * cs - p.y * sn, p.x * sn + p.y * cs);

    // Hub: a small, barely-writhing lymphocyte. Much less domain warp than the
    // neutrophil -- NK cells are round and compact, not amoeboid, and at this
    // quad size a large warp would wobble the hub distractingly.
    float wx = fbm(hp * 11.0 + vec2(phase * 0.05, 0.0)) - 0.5;
    float wy = fbm(hp * 11.0 + vec2(9.1, -phase * 0.04)) - 0.5;
    float hub = length(hp + vec2(wx, wy) * 0.010) - kNkHubR;

    // One large, dense nucleus -- not lobed like the neutrophil's. Kept a touch
    // off-centre (a real lymphocyte's is), but only a touch: at the full
    // eccentricity it read as the core having drifted loose of the rotor rather
    // than as a cell nucleus.
    nucleus_d = length(hp - vec2(0.005, -0.004)) - kNkHubR * 0.52;

    // At least THREE arms, always. Two arms 180 degrees apart, both swept the
    // same way, fuse through the hub into a single continuous S-curve: it reads
    // as one sinuous flagellum, not as a rotor. Three-fold is the lowest
    // symmetry with no opposing pair to line up, so it reads as a pinwheel
    // immediately -- the same "avoid the degenerate silhouette" reasoning that
    // gives sdf_neutrophil five lobes instead of four.
    float n = max(blade_count, 3.0);
    float ang = atan(p.y, p.x);
    float blades = 1e9;
    granule_glow = 0.0;
    edge_glow = 0.0;

    for (int k = 0; k < kNkMaxBlades; ++k) {
        if (float(k) >= n) break;
        float base = float(k) * (6.28318530 / n);

        // Swept centreline: the angle the blade occupies drifts with radius.
        float centre = base - kNkSweep * radial_t;
        float da = mod(ang - centre + 3.14159265, 6.28318530) - 3.14159265;
        // Angular offset -> arc length, so the blade keeps a constant physical
        // width instead of fanning out into a wedge as r grows.
        float arc = da * r;

        // Convex taper: broad most of the way out, then falling off fast to a
        // point. A linear taper produces an even sliver that reads as a
        // tentacle; holding the width and losing it late is the blade profile.
        float taper = pow(1.0 - radial_t, 0.6);

        // ASYMMETRIC cross-section, and the single biggest "is this a blade?"
        // cue. The tower spins toward +angle, so the +arc side is the leading
        // edge: keep it thin and hard (a cutting edge) and let the trailing
        // side carry the mass. A symmetric blade is just a rounded spoke.
        float w = kNkRootW * taper * (arc > 0.0 ? kNkLeadFrac : 1.0);
        blades = min(blades, max(abs(arc) - w, r - kNkReach));

        // Specular line riding the leading edge -- what actually sells "sharp".
        if (r < kNkReach) {
            edge_glow = max(edge_glow,
                            1.0 - smoothstep(0.0, 0.010, abs(arc - kNkRootW * taper * kNkLeadFrac)));
        }

        // Perforin granule riding the tip, exactly on the range boundary.
        vec2 tip = vec2(cos(base - kNkSweep), sin(base - kNkSweep)) * kNkReach;
        granule_glow = max(granule_glow, 1.0 - smoothstep(-0.004, kNkGlowR, length(p - tip)));
    }

    // Fuse rather than union so the blades grow out of the cell, no seam.
    return smin(hub, blades, 0.030);
}

/// Smooth maximum — the counterpart to smin(), and what CARVES a shape instead
/// of growing one. smax(body, -hole, k) subtracts `hole` from `body` with a
/// rounded lip rather than a knife edge, which is how the Macrophage gets a maw
/// that reads as a mouth of soft membrane instead of a bite taken with scissors.
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
// MORTAR — Macrophage.
//
// The heaviest silhouette in the roster, and it has to read that way from the
// build menu onward: the Macrophage is the tower you place when a clump needs
// deleting, so it is the widest, lumpiest, slowest-moving thing on the tissue.
//
// Three features carry it, and all three are real macrophage anatomy:
//   1. MEMBRANE RUFFLES. A live macrophage's surface is in constant ruffling
//      motion. A low-order angular ripple on the radius buys that without a
//      second fbm, and it is what stops the body reading as the big smooth ball
//      it replaces.
//   2. THE MAW. Local +x is the aim direction (entity.vert rotates the whole
//      quad), so a bite carved out of the LEADING edge with smax() gives the
//      tower a mouth that always faces what it is about to shell, flanked by
//      two lip lobes. Strongest "which way am I pointing" cue on any tower
//      here, and it costs one subtraction.
//   3. PHAGOSOMES. Real macrophages are stuffed with vesicles mid-digestion.
//      Here they double as ammunition — `loaded_glow` marks one fat vesicle
//      held right at the maw, brighter than the rest and breathing, so the
//      tower visibly has a shell chambered.
//
// Pseudopods are pushed to the flanks and rear deliberately: a pod reaching
// forward would fill the maw and destroy the directional read.
// ---------------------------------------------------------------------------
const float kMacroBodyR = 0.300;

float sdf_macrophage(vec2 p, float phase, float tier, out float nucleus_d,
                     out float vesicle_d, out float loaded_glow, out float loaded_spec,
                     out float granule) {
    // Coarser and slower warp than the neutrophil's: this is a big cell, and
    // fine high-frequency writhe at this size reads as boiling noise.
    float wx = fbm(p * 2.4 + vec2(phase * 0.09, 0.0)) - 0.5;
    float wy = fbm(p * 2.4 + vec2(3.7, -phase * 0.08)) - 0.5;
    vec2 wp = p + vec2(wx, wy) * 0.19;

    float ang = atan(wp.y, wp.x);
    // Both harmonics use INTEGER multiples of the angle so the ripple closes on
    // itself at +-pi; a fractional factor puts a seam down the cell's left side.
    float ruffle = 0.024 * sin(ang * 7.0 + phase * 0.45)
                 + 0.013 * sin(ang * 13.0 - phase * 0.31);
    float body = length(wp) - (kMacroBodyR + ruffle);


    // FIVE pseudopods, walked around the REAR 250 degrees only. Both numbers
    // are load-bearing:
    //   - Five, not four. Four lobes at mirrored angles resolve into a rounded
    //     SQUARE, the one silhouette a cell must never have; sdf_neutrophil
    //     documents the same trap and dodges it the same way.
    //   - Rear only. The front sector belongs to the maw and its lips, and a
    //     pod growing into it fills the mouth and kills the directional read.
    // Same fixed-hash-per-index idiom as the neutrophil, so a given lobe is
    // stable frame to frame but the set is irregular.
    for (int k = 0; k < 5; ++k) {
        float fk = float(k);
        float a = 0.95 + fk * 0.87 + 0.30 * fract(sin(fk * 12.9898) * 43758.5453)
                + 0.10 * sin(phase * 0.33 + fk);
        // Short and fat, fused hard. Reaching pods turn this body into a star;
        // the Macrophage has to stay the CHUNKY silhouette, so the pods read as
        // bulges in a heavy cell rather than as limbs.
        float dist = 0.205 + 0.055 * fract(sin(fk * 78.233) * 43758.5453);
        float rad  = 0.110 + 0.035 * fract(sin(fk * 39.425) * 43758.5453);
        body = smin(body, length(wp - vec2(cos(a), sin(a)) * dist) - rad, 0.17);
    }

    // Lips first, THEN the bite — carving last is what gives them a concave
    // inner face instead of two beads stuck either side of a hole.
    for (int k = 0; k < 2; ++k) {
        float side = (k == 0) ? 1.0 : -1.0;
        float a = side * (0.78 + 0.06 * sin(phase * 0.4 + side));
        body = smin(body, length(p - vec2(cos(a), sin(a)) * 0.315) - 0.100, 0.10);
    }
    // A SHALLOW bite. The obvious mistake here is to carve deep for a dramatic
    // mouth: at any depth past about a third of the radius the cutter meets the
    // pseudopods either side and the whole body resolves into a C, or worse an
    // X, which throws away the one thing this tower's silhouette is for.
    float maw = length(p - vec2(0.385 + 0.015 * sin(phase * 0.4), 0.0)) - 0.160;
    body = smax(body, -maw, 0.050);

    // Kidney-bean nucleus: two fused blobs with a notch bitten out of one side.
    // The indentation is the whole point — a round nucleus here would be
    // indistinguishable from one more phagosome.
    vec2 nc = vec2(-0.115, 0.030);
    float n1 = length(p - nc - vec2( 0.045,  0.035)) - 0.078;
    float n2 = length(p - nc - vec2(-0.045, -0.020)) - 0.072;
    nucleus_d = smin(n1, n2, 0.050);
    nucleus_d = smax(nucleus_d, -(length(p - nc - vec2(0.020, -0.105)) - 0.070), 0.035);

    // Phagosomes. Count is 3 + tier, so an upgrade shows up in the body itself.
    vesicle_d = 1e9;
    float vesicles = 3.0 + tier;
    for (int k = 0; k < 6; ++k) {
        if (float(k) >= vesicles) break;
        float fk = float(k);
        float a = phase * 0.12 + fk * 1.70 + 1.20;
        float dist = 0.145 + 0.055 * fract(sin(fk * 91.71) * 43758.5453);
        float r = 0.030 + 0.020 * fract(sin(fk * 27.13) * 43758.5453);
        vesicle_d = min(vesicle_d, length(p - vec2(cos(a), sin(a)) * dist) - r);
    }
    // The chambered shell, held at the maw and breathing.
    float loaded_d = length(p - vec2(0.185, 0.0)) - (0.062 + 0.006 * sin(phase * 1.1));
    loaded_glow = 1.0 - smoothstep(-0.010, 0.022, loaded_d);
    // A specular cap up and left, matching the key light the rest of the scene
    // is lit by (see kEntityShadowDir below, and tissue.frag). Filling the
    // shell white instead reads as a hole in the cell rather than a wet vesicle.
    loaded_spec = 1.0 - smoothstep(0.0, 0.030, length(p - vec2(0.163, 0.024)));
    vesicle_d = min(vesicle_d, loaded_d);

    granule = smoothstep(0.60, 0.80, fbm(p * 13.0 + vec2(phase * 0.04, 0.0)));
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
// A cytotoxic T cell is genuinely small and almost entirely nucleus: a huge
// dense core, a rind of cytoplasm, and a fuzz of microvilli. That anatomy
// happens to already be a tesla coil, so the shape needs no invention — spiked
// ball, hot core, arcs crawling over the surface.
//
// The piece of real cell biology doing gameplay work here is GRANULE
// POLARISATION: before a T cell kills, it drags its lytic granules to the face
// touching the target (the immunological synapse). So the granules cluster at
// local +x behind a single long electrode spike, and since local +x is the aim,
// the cell visibly loads its payload toward whatever it is about to discharge
// into. Same directional-read job the Macrophage's maw does, different organ.
// ---------------------------------------------------------------------------
float sdf_cytotoxic(vec2 p, float phase, float tier, out float nucleus_d,
                    out float granule_d, out float arc_glow, out float tip_glow) {
    float wx = fbm(p * 6.0 + vec2(phase * 0.10, 0.0)) - 0.5;
    float wy = fbm(p * 6.0 + vec2(4.4, -phase * 0.09)) - 0.5;
    float body = length(p + vec2(wx, wy) * 0.035) - 0.235;

    // Microvilli, folded the way the crystal's arms are. Count is 9 + 2*tier,
    // and the fuse radius is TINY (0.022) on purpose: a generous smin rounds
    // them into bumps, and a ball of blunt bumps is just a lumpy ball.
    float sector = 6.28318530 / (9.0 + 2.0 * tier);
    float ang = atan(p.y, p.x);
    float kk = mod(ang + sector * 0.5, sector) - sector * 0.5;
    vec2 fq = vec2(cos(kk), sin(kk)) * length(p);
    float t = clamp((fq.x - 0.185) / 0.150, 0.0, 1.0);
    float villi = max(abs(fq.y) - 0.030 * (1.0 - t), max(0.185 - fq.x, fq.x - 0.335));
    body = smin(body, villi, 0.022);

    // The synapse electrode: one long spike along local +x, thicker than a
    // microvillus and reaching past them, so the discharge axis is unambiguous.
    // It has to out-reach the microvilli by a clear margin AND be visibly
    // thicker at the root, or it just reads as one villus that came out long.
    float et = clamp((p.x - 0.200) / 0.320, 0.0, 1.0);
    float electrode = max(abs(p.y) - 0.055 * (1.0 - 0.80 * et),
                          max(0.200 - p.x, p.x - 0.520));
    body = smin(body, electrode, 0.030);
    tip_glow = 1.0 - smoothstep(0.0, 0.080, length(p - vec2(0.500, 0.0)));

    // Nucleus at 0.158 against a 0.235 body: two thirds of the radius, about
    // right for a lymphocyte, and it leaves only a rind of cytoplasm.
    // 0.135 against a 0.235 body, pushed back off centre. It was 0.158 and
    // centred, which left a rind too thin for the granules to read in at all —
    // an accurate lymphocyte nucleus that hid the tower's whole payload tell.
    nucleus_d = length(p - vec2(-0.058, 0.012)) - 0.135;

    // Lytic granules, polarised toward the synapse.
    granule_d = 1e9;
    for (int k = 0; k < 4; ++k) {
        float fk = float(k);
        float ga = -0.55 + 0.37 * fk + 0.10 * sin(phase * 0.9 + fk);
        // Pushed forward of the nucleus, not overlapping it: a granule drawn
        // on top of the core is just a lighter patch of nucleus.
        vec2 c = vec2(0.165, 0.0) + vec2(cos(ga), sin(ga)) * 0.050;
        granule_d = min(granule_d,
                        length(p - c) - (0.024 + 0.010 * fract(sin(fk * 45.1) * 43758.5453)));
    }

    // Surface crackle. A high power on |sin| turns a smooth wave into a few
    // narrow bright filaments; the fbm term inside the phase stops them being
    // evenly spaced, which is the difference between lightning and a grating.
    float r = length(p);
    float band = 1.0 - smoothstep(0.0, 0.055, abs(r - 0.262));
    arc_glow = band * pow(abs(sin(ang * 4.0 + phase * 2.6 + fbm(p * 7.0) * 5.0)), 7.0);

    // ...plus a discharge running out along the electrode to the tip.
    float lead = 1.0 - smoothstep(0.0, 0.030, abs(p.y - 0.035 * sin(p.x * 26.0 + phase * 7.0)));
    arc_glow = max(arc_glow, lead * smoothstep(0.20, 0.28, p.x)
                                  * (1.0 - smoothstep(0.40, 0.47, p.x)) * 0.9);
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
const vec2 kEntityShadowDir = vec2(0.085, -0.070);

float entity_shadow(vec2 p, float radius) {
    float sd = length(p - kEntityShadowDir) - radius;
    return (1.0 - smoothstep(-0.10, 0.03, sd)) * 0.42;
}

/// Composites a lit body over its own drop shadow into one straight-alpha
/// result, so the pair resolves correctly against the destination blend.
vec4 over_shadow(vec3 rgb, float body_a, float shadow_a) {
    const vec3 kShadowRgb = vec3(0.05, 0.01, 0.02);
    float out_a = body_a + shadow_a * (1.0 - body_a);
    if (out_a <= 0.001) return vec4(0.0);
    return vec4((rgb * body_a + kShadowRgb * shadow_a * (1.0 - body_a)) / out_a, out_a);
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

        o_color = over_shadow(rgb, a * v_tint.a, sh);
        if (o_color.a <= 0.001) discard;
        return;
    } else if (v_shape_id == 21u) {
        // BLADE (NK Cell). Tower shape ids start at 16; see kTowerShapeBase
        // in TowerSystem.cpp ("nk_cell" is index 5 in kTowerNames -> 21).
        float nucleus_d, radial_t, granule_glow, edge_glow;
        // 2 + tier blades; v_shape_param carries the raw tier for every tower,
        // and the "+2" lives here rather than on the CPU so this file is the
        // one place that decides what a tier looks like.
        float body_d = sdf_nk_cell(v_local, v_anim_phase, 2.0 + v_shape_param, v_rotation,
                                   nucleus_d, radial_t, granule_glow, edge_glow);

        // Tight AA band: this quad is ~10x the on-screen size of other towers.
        float a = 1.0 - smoothstep(-0.005, 0.002, body_d);
        // Feather the blade toward the rim so it dissolves into the damage
        // field instead of ending in a hard chopped-off edge. Radius-driven, so
        // the hub (deep inside kNkReach) is untouched.
        a *= mix(1.0, kNkTipFade, smoothstep(0.35, 1.0, radial_t));
        a = max(a, granule_glow * 0.95); // granules stay solid at the rim
        if (a <= 0.0) discard;

        float depth = clamp(-body_d * 26.0, 0.0, 1.0);

        // The NK Cell's identity hue, hardcoded to mirror palette_for()'s
        // magenta-pink entry in vfx/Particles.cpp so the body, its rotor-sweep
        // trails and its slash VFX all read as the same tower. It can't come
        // from v_tint: TowerSystem::build gives every tower a white sprite
        // tint, so mixing toward it would be a no-op. Same reasoning as
        // sdf_neutrophil hardcoding its own pale cytoplasm.
        const vec3 kNkHue = vec3(1.00, 0.52, 0.86);

        // Pale granular cytoplasm, warmed slightly toward the identity hue.
        vec3 cytoplasm = mix(vec3(0.96, 0.93, 0.96), vec3(0.74, 0.62, 0.76), depth);
        cytoplasm = mix(cytoplasm, kNkHue, 0.20);

        // Blades run hotter the further out they go, so the rotor reads as
        // energised rather than as pale limbs stuck onto a cell.
        vec3 rgb = mix(cytoplasm, kNkHue, smoothstep(0.10, 1.0, radial_t) * 0.75);

        // Dense eccentric nucleus, stained dark violet. Confined to the hub.
        float nuc = 1.0 - smoothstep(-0.004, 0.004, nucleus_d);
        rgb = mix(rgb, vec3(0.30, 0.22, 0.38), nuc * 0.88);

        // Hard specular line down each leading edge. Suppressed over the hub so
        // the cell body doesn't get a stripe through it.
        float lead = edge_glow * smoothstep(0.12, 0.30, radial_t);
        rgb = mix(rgb, vec3(1.0), lead * 0.85);

        // Perforin granules glow hot white-pink at the blade tips -- the
        // payload about to be fired, and incidentally a range marker.
        rgb = mix(rgb, vec3(1.0, 0.85, 0.95), granule_glow * 0.9);

        // Membrane rim, same "wet cell boundary" treatment as the Gunner.
        float rim = 1.0 - smoothstep(0.0, 0.010, abs(body_d));
        rgb = mix(rgb, vec3(1.0), rim * 0.62);

        o_color = over_shadow(rgb, a * v_tint.a, entity_shadow(v_local, 0.30));
        if (o_color.a <= 0.001) discard;
        return;
    } else if (v_shape_id == 17u) {
        // MORTAR (Macrophage). Warm and heavy: the amber goes in much harder
        // than the Gunner's 0.18 tint mix, because this tower's whole read is
        // "the big orange one", and a near-white body would hand that job back
        // to hue-matching against the particles.
        float nucleus_d, vesicle_d, loaded_glow, loaded_spec, granule;
        float body_d = sdf_macrophage(v_local, v_anim_phase, v_shape_param,
                                      nucleus_d, vesicle_d, loaded_glow, loaded_spec, granule);

        float a = 1.0 - smoothstep(-0.028, 0.010, body_d);
        float sh = entity_shadow(v_local, 0.37);
        if (a <= 0.0 && sh <= 0.0) discard;

        float depth = clamp(-body_d * 4.5, 0.0, 1.0);
        const vec3 kMacroHue = vec3(1.00, 0.66, 0.24);

        vec3 cytoplasm = mix(vec3(1.00, 0.93, 0.80), vec3(0.86, 0.46, 0.13), depth);
        cytoplasm = mix(cytoplasm, kMacroHue, 0.42);
        float in_cyto = smoothstep(0.0, 0.05, nucleus_d);
        cytoplasm = mix(cytoplasm, vec3(1.00, 0.86, 0.55), granule * in_cyto * 0.40);

        // Phagosomes read as the shells they are: hot amber with a bright wet
        // rim, and the chambered one at the maw hotter still.
        float ves = 1.0 - smoothstep(-0.010, 0.010, vesicle_d);
        float ves_rim = 1.0 - smoothstep(0.0, 0.020, abs(vesicle_d));
        vec3 rgb = mix(cytoplasm, vec3(1.00, 0.66, 0.18), ves * 0.88);
        rgb = mix(rgb, vec3(1.00, 0.90, 0.62), ves_rim * 0.55);
        rgb = mix(rgb, vec3(1.00, 0.78, 0.24), loaded_glow * 0.92);
        rgb = mix(rgb, vec3(1.00, 0.98, 0.90), loaded_spec * 0.85);

        float nuc = 1.0 - smoothstep(-0.012, 0.012, nucleus_d);
        rgb = mix(rgb, vec3(0.40, 0.23, 0.24), nuc * 0.85);

        float rim = 1.0 - smoothstep(0.0, 0.050, abs(body_d));
        rgb = mix(rgb, vec3(1.00, 0.93, 0.78), rim * 0.70);

        o_color = over_shadow(rgb, a * v_tint.a, sh);
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
        vec4 lit = over_shadow(rgb, a * v_tint.a, sh);
        float out_a = lit.a + vap_a * (1.0 - lit.a);
        if (out_a <= 0.001) discard;
        o_color = vec4((lit.rgb * lit.a + kCryoHue * vap_a * (1.0 - lit.a)) / out_a, out_a);
        return;
    } else if (v_shape_id == 19u) {
        // TESLA (Cytotoxic T).
        float nucleus_d, granule_d, arc_glow, tip_glow;
        float body_d = sdf_cytotoxic(v_local, v_anim_phase, v_shape_param,
                                     nucleus_d, granule_d, arc_glow, tip_glow);

        float a = 1.0 - smoothstep(-0.018, 0.007, body_d);
        float sh = entity_shadow(v_local, 0.29);
        float spark = max(arc_glow, tip_glow);
        if (a <= 0.0 && sh <= 0.0 && spark <= 0.005) discard;
        // Arcs are allowed to draw slightly proud of the membrane — an arc that
        // stops dead at the silhouette reads as a painted-on texture.
        a = max(a, spark * 0.85);

        float depth = clamp(-body_d * 6.0, 0.0, 1.0);
        const vec3 kTeslaHue = vec3(0.76, 0.66, 1.00);

        vec3 cytoplasm = mix(vec3(0.93, 0.90, 1.00), vec3(0.50, 0.42, 0.82), depth);
        cytoplasm = mix(cytoplasm, kTeslaHue, 0.42);

        float nuc = 1.0 - smoothstep(-0.012, 0.012, nucleus_d);
        // Chromatin mottling. This nucleus covers two thirds of the body, and
        // one flat colour over that much area reads as a hole punched through
        // the sprite rather than as an organelle inside it.
        float chromatin = fbm(v_local * 14.0 + vec2(v_anim_phase * 0.03, 0.0));
        vec3 rgb = mix(cytoplasm, mix(vec3(0.24, 0.16, 0.44), vec3(0.46, 0.35, 0.70), chromatin),
                       nuc * 0.90);
        // Nuclear envelope, so the core sits INSIDE the cell instead of on it.
        rgb = mix(rgb, vec3(0.74, 0.64, 0.96),
                  (1.0 - smoothstep(0.0, 0.018, abs(nucleus_d))) * 0.55);

        // Granules get a hot white core, not just a pale tint — they are the
        // payload, and they have to out-read the cytoplasm they sit in.
        float gran = 1.0 - smoothstep(-0.006, 0.008, granule_d);
        rgb = mix(rgb, vec3(0.90, 0.80, 1.00), gran * 0.95);
        rgb = mix(rgb, vec3(1.0), (1.0 - smoothstep(-0.014, -0.004, granule_d)) * 0.75);

        float rim = 1.0 - smoothstep(0.0, 0.035, abs(body_d));
        rgb = mix(rgb, vec3(0.98, 0.95, 1.00), rim * 0.62);

        // LAST. These are discharges — the brightest thing on the cell — and
        // painting them before the membrane rim meant the rim wrote white over
        // the crackle and erased it.
        rgb = mix(rgb, vec3(0.88, 0.82, 1.00), clamp(arc_glow, 0.0, 1.0) * 0.95);
        rgb = mix(rgb, vec3(1.00, 0.98, 1.00), tip_glow * 0.95);

        o_color = over_shadow(rgb, a * v_tint.a, sh);
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

        o_color = over_shadow(rgb, a * v_tint.a, sh);
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
