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
//   16 = GUNNER (Neutrophil) procedural body; tower shape ids start at 16,
//        see kTowerShapeBase in TowerSystem.cpp
//   21 = BLADE (NK Cell) procedural body
// Any other id falls back to the filled blob.

in vec2  v_local;
in vec4  v_tint;
flat in uint v_shape_id;
in float v_anim_phase;
/// Extra per-shape parameter (EntityInstance::shape_param); meaning is defined
/// by v_shape_id, 0 for shapes that don't declare one. Shape 21 reads it as
/// the NK Cell's blade count.
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
        if (a <= 0.0) discard;

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
        rgb = mix(rgb, vec3(1.0), rim * 0.55);

        o_color = vec4(rgb, a * v_tint.a);
        return;
    } else if (v_shape_id == 21u) {
        // BLADE (NK Cell). Tower shape ids start at 16; see kTowerShapeBase
        // in TowerSystem.cpp ("nk_cell" is index 5 in kTowerNames -> 21).
        float nucleus_d, radial_t, granule_glow, edge_glow;
        float body_d = sdf_nk_cell(v_local, v_anim_phase, v_shape_param, v_rotation,
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
        rgb = mix(rgb, vec3(1.0), rim * 0.5);

        o_color = vec4(rgb, a * v_tint.a);
        return;
    } else if (v_shape_id == 2u) {
        d = sdf_diamond(v_local, 0.45 + pulse);
    } else {
        d = sdf_circle(v_local, 0.5 + pulse);
    }

    float alpha = (1.0 - smoothstep(-0.06, 0.0, d)) * alpha_mul;
    if (alpha <= 0.0) discard;
    o_color = vec4(v_tint.rgb, alpha * v_tint.a);
}
