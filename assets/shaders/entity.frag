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
// Any other id falls back to the filled blob.

in vec2  v_local;
in vec4  v_tint;
flat in uint v_shape_id;
in float v_anim_phase;

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
    } else if (v_shape_id == 2u) {
        d = sdf_diamond(v_local, 0.45 + pulse);
    } else {
        d = sdf_circle(v_local, 0.5 + pulse);
    }

    float alpha = (1.0 - smoothstep(-0.06, 0.0, d)) * alpha_mul;
    if (alpha <= 0.0) discard;
    o_color = vec4(v_tint.rgb, alpha * v_tint.a);
}
