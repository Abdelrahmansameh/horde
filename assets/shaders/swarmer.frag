#version 450 core
// Swarmer pass — fragment stage.
//
// One swarmer: a small cell released by a tower, and it looks like the cell
// that released it. The Neutrophil's units ARE little neutrophils (the same
// sdf_neutrophil entity.frag draws the tower with, at a fraction of the size),
// the Macrophage's are little macrophages (sdf_macrophage, maw facing the
// heading), and the Cytotoxic T's, the Interferon's and the Goblet Cell's are
// the neutrophil body again in their own identity hue — violet, ice blue,
// jade — so a cloud reads as "which tower" by colour and "what kind" by
// silhouette, exactly as the tower bodies do.
//
// The SDFs below are copied from entity.frag rather than shared, because GLSL
// has no include and the two passes have different instance layouts. If you
// retouch a body there, retouch it here. The noise is one octave shorter
// (three, not four): at a couple of world units across the fourth octave is
// sub-pixel, and this pass draws hundreds of bodies to the entity pass's
// dozens.
//
// Kind arrives in bits 8..11 of the flags (sim::SwarmerKind), the tower's
// tier in bits 12..15, tint is the releasing tower's hue
// (Renderer::submit_swarmers), and an ENGAGED unit (bit 0) gets a hotter rim
// so a squad that is firing or a clump that has latched reads as such at a
// glance.

in vec2 v_local;
flat in vec4  v_tint;
flat in float v_phase;
flat in uint  v_flags;
flat in vec2  v_heading;

out vec4 frag_color;

const uint kAttached = 1u;

// ===========================================================================
// Organic-shading toolkit (entity.frag).
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
    for (int k = 0; k < 3; ++k) {
        v += a * vnoise(p);
        p *= 2.02;
        a *= 0.5;
    }
    return v;
}

float smin(float a, float b, float k) {
    float h = clamp(0.5 + 0.5 * (b - a) / k, 0.0, 1.0);
    return mix(b, a, h) - k * h * (1.0 - h);
}

float smax(float a, float b, float k) { return -smin(-a, -b, k); }

// ---------------------------------------------------------------------------
// Neutrophil body (entity.frag: sdf_neutrophil). Domain-warped membrane with
// five uneven pseudopods, a three-lobed nucleus, granular cytoplasm.
// ---------------------------------------------------------------------------
float sdf_neutrophil(vec2 p, float phase, out float nucleus_d, out float granule) {
    float wx = fbm(p * 3.1 + vec2(phase * 0.13, 0.0)) - 0.5;
    float wy = fbm(p * 3.1 + vec2(5.2, -phase * 0.11)) - 0.5;
    vec2 wp = p + vec2(wx, wy) * 0.17;

    float body = length(wp) - 0.34;
    for (int k = 0; k < 5; ++k) {
        float fk = float(k);
        float a = phase * 0.17 + fk * 1.2566;
        float dist = 0.28 + 0.10 * fract(sin(fk * 12.9898) * 43758.5453);
        float rad  = 0.10 + 0.045 * fract(sin(fk * 78.233) * 43758.5453);
        vec2 c = vec2(cos(a), sin(a)) * dist;
        float lobe = length(wp - c) - (rad + 0.022 * sin(phase * 0.7 + fk * 2.1));
        body = smin(body, lobe, 0.13);
    }

    nucleus_d = 1e9;
    for (int k = 0; k < 3; ++k) {
        float a = phase * 0.11 + float(k) * 2.0944;
        vec2 c = vec2(cos(a), sin(a)) * 0.105;
        float lobe = length(p - c) - 0.098;
        nucleus_d = (k == 0) ? lobe : smin(nucleus_d, lobe, 0.055);
    }

    granule = smoothstep(0.62, 0.82, fbm(p * 16.0 + vec2(phase * 0.05, 0.0)));
    return body;
}

// ---------------------------------------------------------------------------
// Macrophage body (entity.frag: sdf_macrophage). Ruffled membrane, rear
// pseudopods, a maw carved out of local +x with two lips, kidney nucleus,
// phagosomes and the chambered vesicle at the maw. Local +x is the heading.
// ---------------------------------------------------------------------------
const float kMacroBodyR = 0.300;

float sdf_macrophage(vec2 p, float phase, float tier, out float nucleus_d,
                     out float vesicle_d, out float loaded_glow, out float loaded_spec,
                     out float granule) {
    float wx = fbm(p * 2.4 + vec2(phase * 0.09, 0.0)) - 0.5;
    float wy = fbm(p * 2.4 + vec2(3.7, -phase * 0.08)) - 0.5;
    vec2 wp = p + vec2(wx, wy) * 0.19;

    float ang = atan(wp.y, wp.x);
    float ruffle = 0.024 * sin(ang * 7.0 + phase * 0.45)
                 + 0.013 * sin(ang * 13.0 - phase * 0.31);
    float body = length(wp) - (kMacroBodyR + ruffle);

    for (int k = 0; k < 5; ++k) {
        float fk = float(k);
        float a = 0.95 + fk * 0.87 + 0.30 * fract(sin(fk * 12.9898) * 43758.5453)
                + 0.10 * sin(phase * 0.33 + fk);
        float dist = 0.205 + 0.055 * fract(sin(fk * 78.233) * 43758.5453);
        float rad  = 0.110 + 0.035 * fract(sin(fk * 39.425) * 43758.5453);
        body = smin(body, length(wp - vec2(cos(a), sin(a)) * dist) - rad, 0.17);
    }

    for (int k = 0; k < 2; ++k) {
        float side = (k == 0) ? 1.0 : -1.0;
        float a = side * (0.78 + 0.06 * sin(phase * 0.4 + side));
        body = smin(body, length(p - vec2(cos(a), sin(a)) * 0.315) - 0.100, 0.10);
    }
    float maw = length(p - vec2(0.385 + 0.015 * sin(phase * 0.4), 0.0)) - 0.160;
    body = smax(body, -maw, 0.050);

    vec2 nc = vec2(-0.115, 0.030);
    float n1 = length(p - nc - vec2( 0.045,  0.035)) - 0.078;
    float n2 = length(p - nc - vec2(-0.045, -0.020)) - 0.072;
    nucleus_d = smin(n1, n2, 0.050);
    nucleus_d = smax(nucleus_d, -(length(p - nc - vec2(0.020, -0.105)) - 0.070), 0.035);

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
    float loaded_d = length(p - vec2(0.185, 0.0)) - (0.062 + 0.006 * sin(phase * 1.1));
    loaded_glow = 1.0 - smoothstep(-0.010, 0.022, loaded_d);
    loaded_spec = 1.0 - smoothstep(0.0, 0.030, length(p - vec2(0.163, 0.024)));
    vesicle_d = min(vesicle_d, loaded_d);

    granule = smoothstep(0.60, 0.80, fbm(p * 13.0 + vec2(phase * 0.04, 0.0)));
    return body;
}

// Same key light as entity.frag.
const vec2 kEntityShadowDir = vec2(0.085, -0.070);

float entity_shadow(vec2 p, float radius) {
    float sd = length(p - kEntityShadowDir) - radius;
    return (1.0 - smoothstep(-0.10, 0.03, sd)) * 0.42;
}

vec4 over_shadow(vec3 rgb, float body_a, float shadow_a) {
    const vec3 kShadowRgb = vec3(0.05, 0.01, 0.02);
    float out_a = body_a + shadow_a * (1.0 - body_a);
    if (out_a <= 0.001) return vec4(0.0);
    return vec4((rgb * body_a + kShadowRgb * shadow_a * (1.0 - body_a)) / out_a, out_a);
}

void main() {
    uint kind = (v_flags >> 8u) & 15u;
    float tier = float(max((v_flags >> 12u) & 15u, 1u));
    bool attached = (v_flags & kAttached) != 0u;

    // The vertex stage hands over the unrotated quad; turn it so local +x is
    // the heading, the way entity.vert does for the towers. The bodies are
    // authored in a [-0.5, 0.5] frame; the swarmer quad is padded 1.6x, so
    // v_local runs to +-0.8 and the body's 0.45 reach lands at ~0.9 of the
    // instance radius, matching kWallContactFraction.
    vec2 p = vec2(v_local.x * v_heading.x + v_local.y * v_heading.y,
                  -v_local.x * v_heading.y + v_local.y * v_heading.x);
    float phase = v_phase * 0.6;

    // Engaged units run a hotter rim: the one state change a player must be
    // able to see at a glance.
    float engaged_rim = attached ? 0.30 + 0.20 * (0.5 + 0.5 * sin(v_phase * 2.3)) : 0.0;

    if (kind == 2u) {
        // BOMBER: a small macrophage, amber, maw forward, phagosomes counted
        // by tier exactly as the tower's are.
        float nucleus_d, vesicle_d, loaded_glow, loaded_spec, granule;
        float body_d = sdf_macrophage(p, phase, tier, nucleus_d, vesicle_d, loaded_glow,
                                      loaded_spec, granule);

        float a = 1.0 - smoothstep(-0.028, 0.010, body_d);
        float sh = entity_shadow(p, 0.37);
        if (a <= 0.0 && sh <= 0.0) discard;

        float depth = clamp(-body_d * 4.5, 0.0, 1.0);
        vec3 hue = v_tint.rgb;

        vec3 cytoplasm = mix(vec3(1.00, 0.93, 0.80), vec3(0.86, 0.46, 0.13), depth);
        cytoplasm = mix(cytoplasm, hue, 0.42);
        float in_cyto = smoothstep(0.0, 0.05, nucleus_d);
        cytoplasm = mix(cytoplasm, vec3(1.00, 0.86, 0.55), granule * in_cyto * 0.40);

        float ves = 1.0 - smoothstep(-0.010, 0.010, vesicle_d);
        float ves_rim = 1.0 - smoothstep(0.0, 0.020, abs(vesicle_d));
        vec3 rgb = mix(cytoplasm, vec3(1.00, 0.66, 0.18), ves * 0.88);
        rgb = mix(rgb, vec3(1.00, 0.90, 0.62), ves_rim * 0.55);
        rgb = mix(rgb, vec3(1.00, 0.78, 0.24), loaded_glow * 0.92);
        rgb = mix(rgb, vec3(1.00, 0.98, 0.90), loaded_spec * 0.85);

        float nuc = 1.0 - smoothstep(-0.012, 0.012, nucleus_d);
        rgb = mix(rgb, vec3(0.40, 0.23, 0.24), nuc * 0.85);

        float rim = 1.0 - smoothstep(0.0, 0.050, abs(body_d));
        rgb = mix(rgb, vec3(1.00, 0.93, 0.78), rim * (0.70 + engaged_rim));

        frag_color = over_shadow(rgb, a * v_tint.a, sh * v_tint.a);
        if (frag_color.a <= 0.001) discard;
        return;
    }

    // Everything else: a small neutrophil. The Neutrophil's own units keep
    // the near-colourless body the tower has (tint mixed in lightly); the
    // other three are the same cell repainted in their tower's hue, so the
    // colour carries "which tower" the way it does for every other read.
    float nucleus_d, granule;
    float body_d = sdf_neutrophil(p, phase, nucleus_d, granule);

    float a = 1.0 - smoothstep(-0.030, 0.010, body_d);
    float sh = entity_shadow(p, 0.46);
    if (a <= 0.0 && sh <= 0.0) discard;

    float depth = clamp(-body_d * 5.0, 0.0, 1.0);
    bool repaint = kind != 1u;
    vec3 cytoplasm = mix(vec3(0.94, 0.95, 0.97), vec3(0.66, 0.70, 0.80), depth);
    cytoplasm = mix(cytoplasm, v_tint.rgb, repaint ? 0.62 : 0.18);

    float in_cyto = smoothstep(0.0, 0.05, nucleus_d);
    vec3 speck = repaint ? mix(vec3(1.0), v_tint.rgb, 0.35) : vec3(0.99, 0.93, 0.72);
    cytoplasm = mix(cytoplasm, speck, granule * in_cyto * 0.45);

    // Lobed nucleus: violet on the neutrophil, a deep shade of the tint on the
    // repaints so it still reads darker than the cytoplasm around it.
    vec3 nucleus_rgb = repaint ? v_tint.rgb * 0.45 : vec3(0.42, 0.36, 0.55);
    float nuc = 1.0 - smoothstep(-0.012, 0.012, nucleus_d);
    vec3 rgb = mix(cytoplasm, nucleus_rgb, nuc * 0.85);

    float rim = 1.0 - smoothstep(0.0, 0.055, abs(body_d));
    rgb = mix(rgb, vec3(1.0), rim * (0.68 + engaged_rim));

    frag_color = over_shadow(rgb, a * v_tint.a, sh * v_tint.a);
    if (frag_color.a <= 0.001) discard;
}
