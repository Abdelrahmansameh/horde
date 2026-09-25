#version 450 core
// Swarmer pass — fragment stage.
//
// One swarmer: a small cell released by a tower, and it looks like the cell
// that released it. The Neutrophil's units ARE little neutrophils (the same
// sdf_neutrophil entity.frag draws the tower with, at a fraction of the size),
// the Macrophage's are little macrophages (sdf_arbor_macrophage, branching
// pseudopods facing the heading), and the Cytotoxic T's, the Interferon's and the Goblet Cell's are
// the neutrophil body again in their own identity hue — violet, ice blue,
// jade — so a cloud reads as "which tower" by colour and "what kind" by
// silhouette, exactly as the tower bodies do. The Fibroblast's builders are
// little fibroblasts: a spindle (sdf_fibroblast_unit below, a cut-down
// version of entity.frag's sdf_fibroblast) crawling heading-first toward
// the site it will wall off.
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
flat in vec4  v_arm0;
flat in vec4  v_arm1;
flat in vec4  v_arm2;
flat in float v_world_per_local;

// render::kShadowsEnabled as 0/1: a global kill switch for the drop shadow,
// applied once in over_shadow().
layout(location = 2) uniform float u_shadows;

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
// One soft resting pincer pair on local +x. It disappears while the live
// attack pair is out, so the cell never appears to have extra limbs.
//
// "Nothing here" is kFar, not 1e9: these distances go through smin, whose
// mix(b, a, 1.0) is b + (a - b), and at 1e9 a float has no room left for
// the body's 0.3 -- the whole cell would vanish. kFar is well outside any
// quad this pass draws and still exact to a few ulps.
const float kFar = 8.0;

float macrophage_arms(vec2 p, float phase, bool hidden) {
    if (hidden) return kFar;
    vec2 q = vec2(p.x, abs(p.y));
    float breath = sin(phase * 0.38);
    vec2 root = vec2(0.245, 0.135);
    vec2 shoulder = vec2(0.420, 0.295 + breath * 0.012);
    vec2 curl = vec2(0.665 + breath * 0.012, 0.300);
    vec2 tip = vec2(0.695 + breath * 0.018, 0.100 + breath * 0.014);
    float arm = 1e9;
    vec2 a = root;
    for (int j = 1; j <= 10; ++j) {
        float t = float(j) / 10.0;
        float u = 1.0 - t;
        vec2 b = u*u*u*root + 3.0*u*u*t*shoulder + 3.0*u*t*t*curl + t*t*t*tip;
        vec2 ba = b - a;
        float h = clamp(dot(q - a, ba) / max(dot(ba, ba), 1e-8), 0.0, 1.0);
        float along = (float(j - 1) + h) / 10.0;
        float radius = mix(0.090, 0.032, along);
        arm = min(arm, length(q - a - ba * h) - radius);
        a = b;
    }
    return arm;
}

// ---------------------------------------------------------------------------
// ARBOR GRABBER -- Macrophage. Every arm is a tapered trunk, three side
// branches and a fan of terminal fingers. The roots are fused into a body SDF
// that shifts and thins under arm load. `fake_mass` controls how much of that
// apparent volume loss is compensated while the arms are grown.
// ---------------------------------------------------------------------------
float tapered_segment(vec2 p, vec2 a, vec2 b, float ra, float rb) {
    vec2 ba = b - a;
    float h = clamp(dot(p - a, ba) / max(dot(ba, ba), 1e-8), 0.0, 1.0);
    return length(p - a - ba * h) - mix(ra, rb, h);
}

float arbor_tree(vec2 p, vec4 arm, float phase, float index, out float finger_glow) {
    finger_glow = 0.0;
    if (arm.w < 0.5 || arm.x <= 0.02) return kFar;

    float reach = max(arm.x, 0.22);
    float grip = clamp(arm.z, 0.0, 1.0);
    vec2 dir = vec2(cos(arm.y), sin(arm.y));
    vec2 side = vec2(-dir.y, dir.x);
    float sway = sin(phase * 0.72 + index * 2.17) * reach * 0.055;
    vec2 root = dir * 0.12;
    vec2 joint = dir * (reach * 0.53) + side * sway;
    vec2 tip = dir * reach + side * sway * 0.28;

    float tree = tapered_segment(p, root, joint, 0.115, 0.050);
    tree = min(tree, tapered_segment(p, joint, tip, 0.052, 0.020));

    // Alternating side branches keep the silhouette tree-like rather than
    // reading as a smooth tentacle. Each branch ends in two finer twigs.
    for (int k = 0; k < 3; ++k) {
        float fk = float(k);
        float t = 0.42 + fk * 0.17;
        float sign_side = mod(fk + index, 2.0) < 1.0 ? -1.0 : 1.0;
        vec2 origin = mix(root, tip, t) + side * sway * sin(t * 3.14159);
        float branch_angle = sign_side * (0.54 + 0.08 * sin(phase * 0.55 + fk + index));
        vec2 branch_dir = dir * cos(branch_angle) + side * sin(branch_angle);
        float branch_len = reach * (0.19 - fk * 0.025);
        vec2 end = origin + branch_dir * branch_len;
        tree = min(tree, tapered_segment(p, origin, end, 0.040, 0.012));

        vec2 branch_side = vec2(-branch_dir.y, branch_dir.x);
        for (int j = 0; j < 2; ++j) {
            float sj = j == 0 ? -1.0 : 1.0;
            vec2 twig_dir = normalize(branch_dir * 0.78 + branch_side * sj * 0.48);
            vec2 twig_end = end + twig_dir * branch_len * 0.42;
            tree = min(tree, tapered_segment(p, end, twig_end, 0.014, 0.005));
        }
    }

    // Five fingers fan around the prey. As grip rises they curl back around
    // the target instead of simply converging to one sharp point.
    for (int k = 0; k < 5; ++k) {
        float fk = float(k) - 2.0;
        float fan = fk * mix(0.23, 0.14, grip);
        vec2 finger_dir = dir * cos(fan) + side * sin(fan);
        float finger_len = min(0.18, reach * 0.16) * (1.0 - 0.06 * abs(fk));
        vec2 finger_root = tip - dir * 0.018 + side * fk * 0.010;
        vec2 finger_mid = finger_root + finger_dir * finger_len * 0.58;
        vec2 finger_end = finger_root + finger_dir * finger_len - dir * grip * finger_len * 0.62;
        tree = min(tree, tapered_segment(p, finger_root, finger_mid, 0.018, 0.009));
        tree = min(tree, tapered_segment(p, finger_mid, finger_end, 0.010, 0.004));
        finger_glow = max(finger_glow,
                          (1.0 - smoothstep(0.0, 0.040, length(p - finger_end))) * grip);
    }
    return tree;
}

float sdf_arbor_macrophage(vec2 p, float phase, vec4 a0, vec4 a1, vec4 a2,
                           float fake_mass, out float nucleus_d, out float granule) {
    vec4 arms[3] = vec4[3](a0, a1, a2);
    vec2 pull_sum = vec2(0.0);
    float load = 0.0;
    for (int k = 0; k < 3; ++k) {
        if (arms[k].w < 0.5) continue;
        float strain = smoothstep(0.18, 0.78, arms[k].x);
        vec2 d = vec2(cos(arms[k].y), sin(arms[k].y));
        pull_sum += d * strain;
        load += strain;
    }
    float load01 = clamp(load / 2.2, 0.0, 1.0);
    float uncompensated = load01 * (1.0 - clamp(fake_mass, 0.0, 1.0));
    vec2 pull_dir = length(pull_sum) > 1e-5 ? normalize(pull_sum) : vec2(1.0, 0.0);
    vec2 pull_side = vec2(-pull_dir.y, pull_dir.x);
    vec2 shifted = p - pull_dir * (0.085 * load01);
    float along = dot(shifted, pull_dir);
    float across = dot(shifted, pull_side);
    vec2 body_p = pull_dir * (along / (1.0 + 0.42 * load01)) +
                  pull_side * (across / max(0.72, 1.0 - 0.20 * uncompensated));

    float wx = fbm(body_p * 2.7 + vec2(phase * 0.11, 0.0)) - 0.5;
    float wy = fbm(body_p * 2.7 + vec2(4.1, -phase * 0.09)) - 0.5;
    vec2 wp = body_p + vec2(wx, wy) * 0.15;
    float radius = 0.345 * (1.0 - 0.20 * uncompensated + 0.045 * fake_mass * load01);
    float angle = atan(wp.y, wp.x);
    float ruffle = 0.022 * sin(angle * 9.0 + phase * 0.52)
                 + 0.011 * sin(angle * 15.0 - phase * 0.37);
    float body = length(wp) - (radius + ruffle);

    // Idle membrane fingers keep the cell alive between grabs. Under load,
    // active arm shoulders dominate and these lobes visually flow into them.
    for (int k = 0; k < 7; ++k) {
        float fk = float(k);
        float a = fk * 0.8976 + 0.16 * sin(phase * 0.31 + fk * 1.7);
        float dist = 0.25 + 0.045 * sin(phase * 0.23 + fk * 2.3);
        float rad = 0.078 + 0.022 * sin(fk * 4.11 + 1.3);
        body = smin(body, length(body_p - vec2(cos(a), sin(a)) * dist) - rad, 0.10);
    }

    // Broad roots visibly pull the body's edge into every active tree. This
    // is the body/arm junction seen in the microscopy reference, not a cord
    // pasted onto a fixed circular core.
    for (int k = 0; k < 3; ++k) {
        if (arms[k].w < 0.5) continue;
        vec2 d = vec2(cos(arms[k].y), sin(arms[k].y));
        vec2 root_end = d * min(0.43, arms[k].x * 0.36);
        float shoulder = tapered_segment(p, d * 0.04, root_end, 0.19, 0.075);
        body = smin(body, shoulder, 0.12);
    }

    vec2 nc = pull_dir * (0.035 * load01) + vec2(-0.065, 0.025);
    float n1 = length(p - nc - vec2(0.040, 0.030)) - 0.076;
    float n2 = length(p - nc - vec2(-0.042, -0.020)) - 0.068;
    nucleus_d = smin(n1, n2, 0.045);
    granule = smoothstep(0.60, 0.80, fbm(p * 13.5 + vec2(phase * 0.05, 0.0)));
    return body;
}

const float kMacroBodyR = 0.300;

float sdf_macrophage(vec2 p, float phase, float tier, bool hide_pincers,
                     out float nucleus_d,
                     out float vesicle_d, out float loaded_glow, out float loaded_spec,
                     out float granule) {
    // Match the tower's wider, slightly taller tank body. Arm coordinates stay
    // in the original local space so their reach and thickness do not change.
    vec2 body_p = vec2(p.x / 1.40, p.y / 1.20);
    float wx = fbm(body_p * 2.4 + vec2(phase * 0.09, 0.0)) - 0.5;
    float wy = fbm(body_p * 2.4 + vec2(3.7, -phase * 0.08)) - 0.5;
    vec2 wp = body_p + vec2(wx, wy) * 0.19;

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
        body = smin(body, length(body_p - vec2(cos(a), sin(a)) * 0.315) - 0.100, 0.10);
    }
    float maw = length(body_p - vec2(0.385 + 0.015 * sin(phase * 0.4), 0.0)) - 0.160;
    body = smax(body, -maw, 0.050);

    // Fuse the resting pseudopods after carving the maw; preserve the interior.
    body = smin(body, macrophage_arms(p, phase, hide_pincers), 0.075);

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

// ---------------------------------------------------------------------------
// BUILDER — a small fibroblast. A fusiform body drawn to points at both
// ends, an oval nucleus in the waist, one trailing process. The tower's body
// in entity.frag has the full version with the tier's worth of processes;
// at unit size one is all that reads.
// ---------------------------------------------------------------------------
float sdf_fibroblast_unit(vec2 p, float phase, out float nucleus_d) {
    float wx = fbm(p * 5.0 + vec2(phase * 0.08, 0.0)) - 0.5;
    float wy = fbm(p * 5.0 + vec2(3.1, -phase * 0.07)) - 0.5;
    vec2 wp = p + vec2(wx, wy) * 0.035;
    float taper = 2.3 + 3.2 * smoothstep(0.16, 0.44, abs(wp.x));
    float body = length(vec2(wp.x, wp.y * taper)) - 0.44;
    body += 0.010 * sin(wp.x * 5.0 + phase * 0.3) * (1.0 - abs(wp.x) * 1.4);
    // One trailing process off the back tip, waving with the crawl.
    vec2 root = vec2(-0.41, 0.0);
    vec2 tip = root + vec2(-0.17, 0.06 * sin(phase * 0.6));
    vec2 pa = p - root, ba = tip - root;
    float h = clamp(dot(pa, ba) / max(dot(ba, ba), 1e-8), 0.0, 1.0);
    float proc = length(pa - ba * h) - (0.014 - 0.006 * h);
    body = smin(body, proc, 0.03);
    nucleus_d = length(vec2(p.x, p.y * 1.9)) - 0.115;
    return body;
}

float entity_shadow(vec2 p, float radius) {
    float sd = length(p - kEntityShadowDir) - radius;
    return (1.0 - smoothstep(-0.10, 0.03, sd)) * 0.42;
}

vec4 over_shadow(vec3 rgb, float body_a, float shadow_a) {
    const vec3 kShadowRgb = vec3(0.05, 0.01, 0.02);
    shadow_a *= u_shadows;
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

    if (kind == 6u) {
        vec4 arm0 = v_arm0;
        vec4 arm1 = v_arm1;
        vec4 arm2 = v_arm2;
        arm0.x /= v_world_per_local;
        arm1.x /= v_world_per_local;
        arm2.x /= v_world_per_local;
        float fake_mass = float((v_flags >> 16u) & 255u) / 255.0;

        float nucleus_d, granule;
        float body_d = sdf_arbor_macrophage(p, phase, arm0, arm1, arm2,
                                             fake_mass, nucleus_d, granule);
        float finger0, finger1, finger2;
        float tree0 = arbor_tree(p, arm0, phase, 0.0, finger0);
        float tree1 = arbor_tree(p, arm1, phase, 1.0, finger1);
        float tree2 = arbor_tree(p, arm2, phase, 2.0, finger2);
        body_d = smin(body_d, tree0, 0.065);
        body_d = smin(body_d, tree1, 0.065);
        body_d = smin(body_d, tree2, 0.065);

        float a = 1.0 - smoothstep(-0.024, 0.010, body_d);
        float sh = entity_shadow(p, 0.39);
        if (a <= 0.0 && sh <= 0.0) discard;

        float depth = clamp(-body_d * 4.8, 0.0, 1.0);
        vec3 cytoplasm = mix(vec3(1.00, 0.93, 0.82), vec3(0.78, 0.32, 0.06), depth);
        cytoplasm = mix(cytoplasm, v_tint.rgb, 0.48);
        float inside_nucleus = smoothstep(0.0, 0.05, nucleus_d);
        cytoplasm = mix(cytoplasm, vec3(1.00, 0.80, 0.55),
                        granule * inside_nucleus * 0.34);

        float nucleus = 1.0 - smoothstep(-0.012, 0.012, nucleus_d);
        vec3 rgb = mix(cytoplasm, vec3(0.36, 0.16, 0.06), nucleus * 0.84);
        float finger_glow = max(finger0, max(finger1, finger2));
        rgb = mix(rgb, vec3(1.00, 0.96, 0.88), finger_glow * 0.92);
        float rim = 1.0 - smoothstep(0.0, 0.045, abs(body_d));
        rgb = mix(rgb, vec3(1.00, 0.94, 0.84), rim * (0.66 + engaged_rim));

        frag_color = over_shadow(rgb, a * v_tint.a, sh * v_tint.a);
        if (frag_color.a <= 0.001) discard;
        return;
    }

    if (kind == 2u) {
        // The generic bomber keeps the old macrophage body with its resting
        // pseudopod pair, for authored scenes.
        float nucleus_d, vesicle_d, loaded_glow, loaded_spec, granule;
        float body_d = sdf_macrophage(p, phase, tier, false,
                                      nucleus_d, vesicle_d, loaded_glow,
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

    if (kind == 5u) {
        // BUILDER: a small fibroblast, salmon, spindle along the heading.
        float nucleus_d;
        float body_d = sdf_fibroblast_unit(p, phase, nucleus_d);

        float a = 1.0 - smoothstep(-0.026, 0.010, body_d);
        vec2 sp = p - vec2(0.085, -0.070);
        float sd = length(vec2(sp.x, sp.y * 2.4)) - 0.42;
        float sh = (1.0 - smoothstep(-0.10, 0.03, sd)) * 0.42;
        if (a <= 0.0 && sh <= 0.0) discard;

        float depth = clamp(-body_d * 5.0, 0.0, 1.0);
        vec3 hue = v_tint.rgb;
        vec3 cytoplasm = mix(vec3(1.00, 0.92, 0.88), vec3(0.82, 0.48, 0.44), depth);
        cytoplasm = mix(cytoplasm, hue, 0.30);
        // Collagen streaks along the long axis.
        float fibre = smoothstep(0.42, 0.62, vnoise(vec2(p.x * 12.0 + phase * 0.02, p.y * 60.0)));
        float in_cyto = smoothstep(0.0, 0.04, nucleus_d);
        vec3 rgb = mix(cytoplasm, vec3(1.00, 0.86, 0.80), fibre * in_cyto * 0.45 * depth);

        float nuc = 1.0 - smoothstep(-0.010, 0.010, nucleus_d);
        rgb = mix(rgb, vec3(0.46, 0.22, 0.32), nuc * 0.88);
        rgb = mix(rgb, vec3(0.92, 0.70, 0.72), (1.0 - smoothstep(0.0, 0.016, abs(nucleus_d))) * 0.45);

        float rim = 1.0 - smoothstep(0.0, 0.040, abs(body_d));
        rgb = mix(rgb, vec3(1.00, 0.96, 0.94), rim * (0.60 + engaged_rim));

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
