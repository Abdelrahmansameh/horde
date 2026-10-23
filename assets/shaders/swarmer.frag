#version 450 core
// Swarmer pass — fragment stage.
//
// One swarmer: a small cell released by a tower, and it looks like the cell
// that released it. The Neutrophil's units ARE little neutrophils (the same
// sdf_neutrophil entity.frag draws the tower with, at a fraction of the size),
// the Macrophage's are little macrophages (sdf_arbor_macrophage, branching
// pseudopods facing the heading), the Cytotoxic T's are little killer T cells
// (sdf_cytotoxic_unit, the HUD icon's round cell with a fan of receptors on
// its face), and
// the Goblet Cell's are the neutrophil body again in its own jade — so a
// cloud reads as "which tower" by colour and "what kind" by silhouette,
// exactly as the tower bodies do. The Fibroblast's builder is a
// fibroblast (sdf_fibroblast_cell below): a liquid spindle that crawls
// heading-first to its site, then lies along the wall it lays and spins the
// collagen out of both poles.
//
// The SDFs below are copied from entity.frag rather than shared, because GLSL
// has no include and the two passes have different instance layouts. If you
// retouch a body there, retouch it here (the fibroblast's block is the same
// text in both; entity.frag only spells tapered_segment with an sdf_). The
// noise is one octave shorter (three, not four): at a couple of world units
// across the fourth octave is sub-pixel, and this pass draws hundreds of
// bodies to the entity pass's dozens.
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
// Separate membrane coordinates let the shooter push out its skin without
// dragging the nucleus or painting a second shape over the cell.
float sdf_neutrophil(vec2 p, vec2 membrane_p, float phase, out float nucleus_d, out float granule) {
    float wx = fbm(membrane_p * 3.1 + vec2(phase * 0.13, 0.0)) - 0.5;
    float wy = fbm(membrane_p * 3.1 + vec2(5.2, -phase * 0.11)) - 0.5;
    vec2 wp = membrane_p + vec2(wx, wy) * 0.17;

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
// The Neutrophil shooter's MAGAZINE (sim/swarm/Swarmers.h): its rounds, drawn
// as granules floating in the cytoplasm. On a volley they drift to the inside
// of the membrane facing the target and leave from there one by one; on the
// reload new ones pop in at their resting spots. Packed by Renderer.cpp's
// pack_magazine():
//   m0 = {aim angle off local +x, gather 0..1,
//         rounds fired + 256 x magazine size, resting clump radius}
//   m1 = {seconds into the reload (huge when not reloading),
//         seconds between two new rounds, pop seconds, pop start scale}
//   m2 = {pop overshoot, pop peak fraction, pop ease power, granule radius}
// Layout constants are here; every timing and size is towers.json's.
// ---------------------------------------------------------------------------
// Rounds drawn at most; a bigger magazine still fires, the extras just are
// not drawn in the cell.
const int   kMaxGranules  = 24;
// Where the gathered rounds pack, out from the centre along the aim. The sim
// fires from sim::kShooterMuzzleFraction of the size, and one quad unit is
// two sizes, so this is half of that.
const float kGatherEdge   = 0.27;
// How much of a granule's sideways offset from the aim line it keeps once
// gathered: the clump spreads along the membrane rather than stacking.
const float kGatherSquash = 1.0;
// Gathered rounds stack this many deep against the membrane.
const float kGatherRows   = 3.0;

// A real shot resets the sim's seconds_since_shot even on the last round,
// whose magazine has already changed to Reloading. Renderer packs the
// remaining fraction in flags 24..31; zero is fully settled. Keep the
// duration in sync with Renderer.cpp's kShooterPulseSeconds.
const float kShooterPulseSeconds = 0.22;

// {longitudinal stretch, backward recoil, narrow membrane extension}.
vec3 shooter_motion(vec4 magazine) {
    float remaining = float((v_flags >> 24u) & 255u) / 255.0;
    float age = (1.0 - remaining) * kShooterPulseSeconds;
    // A sharp expulsion, a softer opposite squish, then a gentle settle.
    float pulse = exp(-age * 18.0) * cos(age * 30.0) *
                  (1.0 - smoothstep(0.16, kShooterPulseSeconds, age));
    float release = exp(-age * 20.0) *
                    (1.0 - smoothstep(0.12, kShooterPulseSeconds, age));
    float rounds = floor(magazine.z / 256.0);
    float fired = magazine.z - rounds * 256.0;
    // Only wind up before the first round. The rest of the volley is driven
    // by actual shots, rather than by an independent cosmetic oscillator.
    float gather = fired < 0.5 ? magazine.y : 0.0;
    return vec3(1.0 - 0.12 * gather * gather + 0.32 * pulse,
                0.045 * release, 0.160 * release);
}

// Stretch around the fusion pore, then kick back opposite the shot. The
// reciprocal sideways scale conserves area and makes the body feel liquid.
// Forward maps magazine centres; inverse maps the membrane and nucleus.
// Rounds themselves stay round while they ride the same moving cytoplasm.
vec2 shooter_deform(vec2 p, vec2 aim, float stretch, float recoil, bool inverse) {
    vec2 tang = vec2(-aim.y, aim.x);
    vec2 q = vec2(dot(p, aim), dot(p, tang));
    if (inverse) q.x += recoil;
    float scale = inverse ? 1.0 / stretch : stretch;
    q.x = kGatherEdge + (q.x - kGatherEdge) * scale;
    q.y /= scale;
    if (!inverse) q.x -= recoil;
    return aim * q.x + tang * q.y;
}

vec2 shooter_membrane(vec2 p, vec2 aim, float extension) {
    vec2 tang = vec2(-aim.y, aim.x);
    vec2 q = vec2(dot(p, aim), dot(p, tang));
    // Pull only the narrow front strip of the existing SDF forward. This
    // grows a connected, rounded tip out of whichever lobe faces the aim;
    // it uses the same cytoplasm and membrane rim as the rest of the body.
    q.x -= extension * exp(-q.y * q.y / (0.09 * 0.09)) *
           smoothstep(0.04, 0.38, q.x);
    return aim * q.x + tang * q.y;
}

// Ease in up to the overshoot, then settle back to 1.
float pop_scale(float u, float start, float over, float peak, float power) {
    if (u >= 1.0) return 1.0;
    if (u < peak) return mix(start, over, pow(u / max(peak, 1e-4), power));
    float x = (u - peak) / max(1.0 - peak, 1e-4);
    return mix(over, 1.0, x * x * (3.0 - 2.0 * x));
}

// Signed distance to the nearest live granule, and that granule's radius.
// `pore` comes back as how hot the gather point on the membrane is.
float magazine_d(vec2 p, float phase, vec4 m0, vec4 m1, vec4 m2, float stretch, float recoil,
                 out float nearest_r, out float pore) {
    vec2 aim = vec2(cos(m0.x), sin(m0.x));
    vec2 tang = vec2(-aim.y, aim.x);
    float gather = m0.y;
    float magazine = floor(m0.z / 256.0);
    int fired = int(m0.z - magazine * 256.0 + 0.5);
    int rounds = min(int(magazine + 0.5), kMaxGranules);
    float spread = m0.w;
    float radius = m2.w;
    // The whole resting clump turns slowly as one, so it drifts without
    // the rounds ever passing through each other.
    float swirl = phase * 0.04;

    float best = 8.0;   // "no granule": outside any quad, like kFar below
    nearest_r = radius;
    for (int k = 0; k < kMaxGranules; ++k) {
        if (k >= rounds) break;
        if (k < fired) continue;
        float fk = float(k);

        // Reloading: round k appears at (k + 1) spacings and pops.
        float age = m1.x - m1.y * (fk + 1.0);
        if (age < 0.0) continue;
        float r = radius * pop_scale(age / m1.z, m1.w, m2.x, m2.y, m2.z);

        // Resting spot: a sunflower packing of the whole magazine into a disc
        // of `spread` around the middle -- evenly dense, so the rounds read as
        // one clump -- with a small bob each so they float, not sit pinned.
        float ang = fk * 2.39996 + swirl;
        float rad = spread * sqrt((fk + 0.5) / float(max(rounds, 1)));
        vec2 rest = vec2(cos(ang), sin(ang)) * rad +
                    radius * 0.35 * vec2(sin(phase * 0.47 + fk * 1.7), cos(phase * 0.41 + fk * 2.3));

        // Gathered spot: against the membrane along the aim, keeping a squashed
        // share of its sideways offset, in a few rows deep so a big magazine
        // packs into a clump rather than one blob; staggered so it streams in.
        float row = mod(fk, kGatherRows);
        vec2 clump = aim * (kGatherEdge - radius * (0.3 + 1.7 * row)) +
                     tang * dot(rest, tang) * kGatherSquash;
        float g = clamp(gather * 1.35 - fk / float(max(rounds, 1)) * 0.35, 0.0, 1.0);
        g = g * g * (3.0 - 2.0 * g);

        vec2 centre = shooter_deform(mix(rest, clump, g), aim, stretch, recoil, false);
        float d = length(p - centre) - r;
        if (d < best) {
            best = d;
            nearest_r = r;
        }
    }

    // The fusion pore: a warm spot on the membrane where the clump sits,
    // brightening as it gathers and holding through the volley.
    float pd = length(p - aim * (kGatherEdge + 0.05));
    pore = gather * exp(-pd * pd / 0.004);
    return best;
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
        body = smin(body, tapered_segment(bp, root, tip, 0.013, 0.004), 0.02);
    }

    // Trailing tail: dragged out on the reach, snapped up on the contraction.
    float tail_len = (0.16 + 0.05 * stroke) * (1.0 - 0.7 * pull);
    vec2 t0 = vec2(-0.40, 0.0);
    vec2 t1 = t0 + vec2(-tail_len, 0.05 * sin(phase * 0.6) + 0.015 * stroke);
    body = smin(body, tapered_segment(bp, t0, t1, 0.034, 0.008), 0.035);

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
// ---------------------------------------------------------------------------
// LATCHER — a small Cytotoxic T, drawn to the silhouette of its HUD icon
// (assets/ui/icons/tower_cytotoxic_t.svg): a round cell with a gently
// scalloped membrane, a big round nucleus set back from the face, and a fan
// of five short receptors standing off the front (local +x, the heading).
// The icon's red lytic granules are left out — at unit size dots on the face
// read as eyes — so the receptors stay the unit's one feature of its own.
// Colours are the unit's own violet ramp, not the icon's.
// ---------------------------------------------------------------------------
const float kCtlRadius   = 0.335;
const vec2  kCtlNucleusC = vec2(-0.060, -0.030);
const float kCtlNucleusR = 0.190;

// Receptors: rooted just inside the membrane at fixed angles off the heading
// (the icon's 0, +-32 and +-63 degrees), each a round-capped stalk.
const int   kCtlReceptors    = 5;
const float kCtlReceptorStep = 0.550;  // radians between neighbouring roots
const float kCtlReceptorLen  = 0.070;  // root to tip
const float kCtlReceptorW    = 0.030;  // stalk half-width

// The bare membrane (no receptors): a circle scalloped six times like the
// icon's outline, the scallops drifting slowly round the cell.
float ctl_membrane(vec2 wp, float phase) {
    float ang = atan(wp.y, wp.x);
    float wobble = 0.012 * sin(6.0 * ang + phase * 0.35)
                 + 0.005 * sin(11.0 * ang - phase * 0.50 + 1.3);
    return length(wp) - (kCtlRadius + wobble);
}

float sdf_cytotoxic_unit(vec2 p, float phase, out float nucleus_d,
                         out float receptor_d, out float knob,
                         out float synapse_glow, out float lance,
                         out float speckle) {
    float wx = fbm(p * 5.2 + vec2(phase * 0.10, 0.0)) - 0.5;
    float wy = fbm(p * 5.2 + vec2(4.4, -phase * 0.09)) - 0.5;
    vec2 wp = p + vec2(wx, wy) * 0.016;

    float cell = ctl_membrane(wp, phase);

    // Receptors, in the warped frame so they ride the membrane. Each one nods
    // and reaches on its own beat so the fan never reads as a stamp.
    receptor_d = kFar;
    knob = 0.0;
    for (int k = 0; k < kCtlReceptors; ++k) {
        float fk = float(k);
        float ang = (fk - 2.0) * kCtlReceptorStep + 0.06 * sin(phase * 0.9 + fk * 2.3);
        vec2 n = vec2(cos(ang), sin(ang));
        vec2 root = n * (kCtlRadius - 0.020);
        vec2 tip = root + n * (kCtlReceptorLen + 0.012 * sin(phase * 1.3 + fk * 1.9));
        vec2 sa = wp - root, sb = tip - root;
        float stalk = length(sa - sb * clamp(dot(sa, sb) / dot(sb, sb), 0.0, 1.0)) - kCtlReceptorW;
        receptor_d = min(receptor_d, stalk);
        knob = max(knob, 1.0 - smoothstep(-0.006, 0.006, length(wp - tip) - kCtlReceptorW));
    }
    float body = smin(cell, receptor_d, 0.020);

    nucleus_d = length(p - kCtlNucleusC) - kCtlNucleusR;

    // The synapse: a bright band just inside the front of the membrane, and
    // while latched, strands reaching off it into the target.
    float across = (1.0 - smoothstep(0.100, 0.255, abs(p.y))) * smoothstep(0.050, 0.200, p.x);
    float band = (1.0 - smoothstep(0.0, 0.038, abs(cell))) * across;
    float fil = 0.55 + 0.45 * pow(abs(sin(p.y * 16.0 + phase * 2.4 + fbm(p * 7.0) * 5.0)), 6.0);
    synapse_glow = band * fil * (0.84 + 0.16 * sin(phase * 1.7))
                 * (1.0 - smoothstep(-0.034, -0.002, cell));

    float reach = smoothstep(0.0, 0.020, cell) * (1.0 - smoothstep(0.020, 0.135, cell));
    float strand = pow(abs(sin(p.y * 21.0 + phase * 3.1)), 8.0);
    lance = reach * across * strand * smoothstep(-0.004, 0.020, body);

    speckle = smoothstep(0.60, 0.82, fbm(p * 15.0 + vec2(phase * 0.05, 0.0)));
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
        // BUILDER: a fibroblast, crawling heading-first, or secreting along
        // the wall it is laying. See sdf_fibroblast_cell above; the crawl
        // pace, the secretion and the procollagen load arrive in arm row 0
        // (Renderer.cpp, submit_swarmers).
        float sh;
        vec4 cell = shade_fibroblast_cell(p, phase, clamp(v_arm0.x, 0.0, 1.0), max(v_arm0.y, 0.0),
                                          clamp(v_arm0.z, 0.0, 1.0), engaged_rim, v_tint.rgb, sh);
        frag_color = over_shadow(cell.rgb, cell.a * v_tint.a, sh * v_tint.a);
        if (frag_color.a <= 0.001) discard;
        return;
    }

    if (kind == 0u) {
        // LATCHER: a small Cytotoxic T, receptors along the heading. Authored
        // centred in the unit frame; the receptor tips reach ~0.44, the same
        // footprint as the other units.
        vec2 q = p;
        float nucleus_d, receptor_d, knob, synapse_glow, lance, speckle;
        float body_d = sdf_cytotoxic_unit(q, phase, nucleus_d, receptor_d, knob,
                                          synapse_glow, lance, speckle);
        // The discharge off the face only while it is latched and draining.
        lance *= attached ? 1.0 : 0.0;

        float a = 1.0 - smoothstep(-0.020, 0.008, body_d);
        float sh = entity_shadow(q, 0.35);
        if (a <= 0.0 && sh <= 0.0 && lance <= 0.005) discard;
        a = max(a, lance * 0.80);

        // The tower's violet ramp (entity.frag, shape 18), pulled onto the
        // releasing tower's hue.
        float depth = clamp(-body_d * 4.2, 0.0, 1.0);
        vec3 cytoplasm = mix(vec3(0.62, 0.47, 0.95), vec3(0.24, 0.11, 0.54), depth);
        cytoplasm = mix(cytoplasm, v_tint.rgb, 0.18);
        float in_cyto = smoothstep(0.0, 0.045, nucleus_d);
        vec3 rgb = mix(cytoplasm, vec3(0.82, 0.74, 1.00), speckle * in_cyto * 0.30);

        float nuc = 1.0 - smoothstep(-0.012, 0.012, nucleus_d);
        float chromatin = fbm(q * 14.0 + vec2(phase * 0.03, 0.0));
        rgb = mix(rgb, mix(vec3(0.15, 0.08, 0.33), vec3(0.34, 0.23, 0.57), chromatin), nuc * 0.92);
        rgb = mix(rgb, vec3(0.74, 0.64, 0.96),
                  (1.0 - smoothstep(0.0, 0.018, abs(nucleus_d))) * 0.55);

        float rim = 1.0 - smoothstep(0.0, 0.024, abs(body_d));
        rgb = mix(rgb, vec3(0.93, 0.88, 1.00), rim * (0.50 + engaged_rim));

        // Receptor stalks a pale lilac, knobs near-white, so the row reads as
        // studs standing off the membrane rather than as more rim.
        float stalk = 1.0 - smoothstep(-0.004, 0.006, receptor_d);
        rgb = mix(rgb, vec3(0.80, 0.70, 1.00), stalk * 0.70);
        rgb = mix(rgb, vec3(0.97, 0.94, 1.00), knob * 0.90);

        rgb = mix(rgb, vec3(0.93, 0.87, 1.00), clamp(synapse_glow, 0.0, 1.0) * 0.85);
        rgb = mix(rgb, vec3(1.00, 0.98, 1.00), clamp(lance, 0.0, 1.0) * 0.95);

        frag_color = over_shadow(rgb, a * v_tint.a, sh * v_tint.a);
        if (frag_color.a <= 0.001) discard;
        return;
    }

    // Everything else: a small neutrophil. The Neutrophil's own units keep
    // the near-colourless body the tower has (tint mixed in lightly); the
    // Goblet Cell's are the same cell repainted in their tower's hue, so the
    // colour carries "which tower" the way it does for every other read.
    vec3 motion = kind == 1u ? shooter_motion(v_arm0) : vec3(1.0, 0.0, 0.0);
    vec2 body_p = p;
    vec2 membrane_p = p;
    if (kind == 1u) {
        vec2 aim = vec2(cos(v_arm0.x), sin(v_arm0.x));
        body_p = shooter_deform(p, aim, motion.x, motion.y, true);
        membrane_p = shooter_membrane(body_p, aim, motion.z);
    }
    float nucleus_d, granule;
    float body_d = sdf_neutrophil(body_p, membrane_p, phase, nucleus_d, granule);

    float a = 1.0 - smoothstep(-0.030, 0.010, body_d);
    float sh = entity_shadow(membrane_p, 0.46);
    if (a <= 0.0 && sh <= 0.0) discard;

    float depth = clamp(-body_d * 5.0, 0.0, 1.0);
    bool repaint = kind != 1u;
    vec3 cytoplasm = mix(vec3(0.94, 0.95, 0.97), vec3(0.66, 0.70, 0.80), depth);
    cytoplasm = mix(cytoplasm, v_tint.rgb, repaint ? 0.62 : 0.18);

    float in_cyto = smoothstep(0.0, 0.05, nucleus_d);
    vec3 speck = repaint ? mix(vec3(1.0), v_tint.rgb, 0.35) : vec3(0.99, 0.93, 0.72);
    // A shooter's real rounds are drawn below; its noise speckle stays faint
    // so the two never read as the same thing.
    cytoplasm = mix(cytoplasm, speck, granule * in_cyto * (kind == 1u ? 0.18 : 0.45));

    // Lobed nucleus: violet on the neutrophil, a deep shade of the tint on the
    // repaints so it still reads darker than the cytoplasm around it.
    vec3 nucleus_rgb = repaint ? v_tint.rgb * 0.45 : vec3(0.42, 0.36, 0.55);
    float nuc = 1.0 - smoothstep(-0.012, 0.012, nucleus_d);
    vec3 rgb = mix(cytoplasm, nucleus_rgb, nuc * 0.85);

    float rim = 1.0 - smoothstep(0.0, 0.055, abs(body_d));
    rgb = mix(rgb, vec3(1.0), rim * (0.68 + engaged_rim));

    if (kind == 1u) {
        // The magazine: the neutrophil's own white, a tone lighter than the
        // cytoplasm around it, with a pure-white core and a soft grey-violet
        // lip (the cell's deep-cytoplasm shade) so a round still separates
        // from the body and from the nucleus under it. The same white the
        // rounds fly in (Renderer.cpp, submit_projectiles).
        float gr, pore;
        float gd = magazine_d(p, phase, v_arm0, v_arm1, v_arm2, motion.x, motion.y, gr, pore);
        float gmask = 1.0 - smoothstep(-0.006, 0.004, gd);
        float inside = clamp(-gd / max(gr, 1e-3), 0.0, 1.0);
        vec3 grain = mix(vec3(0.95, 0.96, 1.00), vec3(1.0), smoothstep(0.35, 0.95, inside));
        grain = mix(vec3(0.70, 0.73, 0.85), grain, smoothstep(0.0, 0.35, inside));
        rgb = mix(rgb, vec3(1.0), pore * 0.55 * a);
        rgb = mix(rgb, grain, gmask);
        a = max(a, gmask);
    }

    frag_color = over_shadow(rgb, a * v_tint.a, sh * v_tint.a);
    if (frag_color.a <= 0.001) discard;
}
