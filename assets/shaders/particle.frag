#version 450 core
// Cosmetic particle pass — fragment stage. Owner: Wave 6D.
// Procedural SDF/noise only, per the project's no-binary-assets rule.
//
// One branch per vfx::ParticleKind (src/vfx/Particles.h). The branch is
// uniform across every fragment of an instance (v_kind is `flat`), so on any
// modern GPU a whole quad takes one path — there is no per-fragment divergence
// cost beyond the wavefronts that straddle two instances of different kinds.
//
//   Tracer — tiny bright dot with a motion streak trailing behind the head.
//   Spark  — round soft glow with a hot core, fades out.
//   Ring   — expanding hollow ring; band thins as it grows (shockwave/ping).
//   Shard  — angular convex fragment, silhouette varied by the per-particle seed.
//   Mist   — soft drifting blob, value-noise interior, fades in AND out.
//   Beam   — dead-straight core with energy packets racing along its length.
//   Bolt   — jagged multi-segment lightning with a branch fork, seed-varied.
//   Fragment — one torn wedge of a dead cell: membrane arc, cytoplasm, ragged sides.
//   Globule  — a soft-edged lump of cell matter (nucleus piece, granule).
//
// FRAGMENT and GLOBULE are MATTER, not light: no glow, no white core, no
// additive pass. They are shaded the way swarmer.frag shades the living cell
// they came out of, so a dying unit reads as that cell tearing apart.
//
// BEAM vs BOLT (DESIGN brief: "Straight beam = Laser, crazy branching
// lightning = T Cell" must be instantly distinguishable). Three independent
// tells, so they can never converge:
//   1. Beam's centreline is exactly v_local.y == 0. Bolt's wanders +/-0.34 in
//      width units through 7 linearly-interpolated nodes -> sharp kinks.
//   2. Beam's quad is hairline (half_w = 1.2% of length); Bolt's is 30% of
//      length. Same segment, wildly different footprint.
//   3. Beam's animation travels ALONG the line (packets); Bolt's is a
//      stroboscopic whole-stroke flicker plus a fork that only exists over the
//      middle of the span.
// tests/test_render_particles.cpp asserts (1) and (2) in pixels.

in vec2 v_local;
flat in vec4  v_tint;
flat in uint  v_kind;
flat in float v_age;
flat in float v_seed;
flat in float v_aspect;
flat in float v_shape;

layout(location = 1) uniform float u_time;

out vec4 o_color;

const uint kTracer = 0u;
const uint kSpark  = 1u;
const uint kRing   = 2u;
const uint kShard  = 3u;
const uint kMist   = 4u;
const uint kBeam   = 5u;
const uint kBolt   = 6u;
const uint kFragment = 7u;
const uint kGlobule  = 8u;

const float kPi = 3.14159265;

// Fallback half-angle for a Fragment spawned without a shape (a sixth of a
// cell plus a little overlap). SwarmerDeath always authors its own, from the
// piece count in towers.json's death_vfx.
const float kFragmentHalfSpan = kPi / 6.0 + 0.05;

float hash11(float p) {
    p = fract(p * 0.1031);
    p *= p + 33.33;
    p *= p + p;
    return fract(p);
}

float hash21(vec2 p) {
    vec3 p3 = fract(p.xyx * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

float vnoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = hash21(i);
    float b = hash21(i + vec2(1.0, 0.0));
    float c = hash21(i + vec2(0.0, 1.0));
    float d = hash21(i + vec2(1.0, 1.0));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

/// Lateral displacement of a Bolt's centreline at `t` in [0,1] along the span.
/// LINEAR interpolation between nodes on purpose — smoothstep would round the
/// corners off and the stroke would start reading as a wobbly beam. Windowed
/// by sin(pi*t) so the bolt is pinned to both endpoints of its segment.
float bolt_offset(float t, float seed) {
    float k = t * 7.0;
    float i = floor(k);
    float f = k - i;
    float a = hash11(i + seed * 31.7) * 2.0 - 1.0;
    float b = hash11(i + 1.0 + seed * 31.7) * 2.0 - 1.0;
    return mix(a, b, f) * sin(kPi * clamp(t, 0.0, 1.0));
}

void main() {
    // Capsule distance in width units. h == 0 for the round kinds (aspect 1),
    // where this collapses to length(v_local) — one expression, every kind.
    float h = max(0.5 * v_aspect - 0.5, 0.0);
    float cap_d = length(v_local - vec2(clamp(v_local.x, -h, h), 0.0));
    // Normalized position along the major axis, 0 at the tail, 1 at the head.
    float t_along = clamp(v_local.x / max(v_aspect, 1e-4) + 0.5, 0.0, 1.0);

    vec3  rgb = v_tint.rgb;
    float alpha = 0.0;

    if (v_kind == kSpark) {
        float d = length(v_local);
        float glow = 1.0 - smoothstep(0.0, 0.5, d);
        float core = 1.0 - smoothstep(0.0, 0.16, d);
        alpha = glow * glow * (1.0 - v_age);
        rgb = mix(rgb, vec3(1.0), core * 0.75);

    } else if (v_kind == kTracer) {
        float body  = 1.0 - smoothstep(0.05, 0.5, cap_d);
        float along = mix(0.12, 1.0, t_along * t_along); // tail fades out
        float core  = (1.0 - smoothstep(0.0, 0.22, cap_d)) * pow(t_along, 6.0);
        alpha = body * along * (1.0 - 0.8 * v_age);
        rgb = mix(rgb, vec3(1.0), core * 0.9);

    } else if (v_kind == kRing) {
        float d = length(v_local);
        float thick = mix(0.17, 0.035, v_age);           // thins as it expands
        float band = 1.0 - smoothstep(0.0, thick, abs(d - 0.44));
        alpha = band * band * (1.0 - v_age);
        rgb = mix(rgb, vec3(1.0), band * 0.5 * (1.0 - v_age));

    } else if (v_kind == kShard) {
        // Convex 3-gon: intersection of three half-planes whose angles and
        // offsets both come off the per-particle seed, so a shatter burst is
        // visibly made of DIFFERENT fragments rather than one repeated sprite.
        float d = -1.0;
        for (int i = 0; i < 3; ++i) {
            float a = 2.0943951 * float(i) + v_seed * 6.2831853;
            vec2  n = vec2(cos(a), sin(a));
            float off = 0.20 + 0.16 * hash11(v_seed * 37.0 + float(i));
            d = max(d, dot(v_local, n) - off);
        }
        float fill = 1.0 - smoothstep(-0.03, 0.02, d);
        float rim  = 1.0 - smoothstep(0.0, 0.08, abs(d));
        alpha = clamp(fill * 0.8 + rim * 0.7, 0.0, 1.0) * (1.0 - v_age * v_age);
        rgb = mix(rgb, vec3(1.0), rim * 0.55);

    } else if (v_kind == kMist) {
        float d = length(v_local);
        vec2  drift = vec2(v_seed * 23.0 - u_time * 0.30, v_seed * 11.0 + u_time * 0.18);
        float n = vnoise(v_local * 3.5 + drift);
        n = mix(n, vnoise(v_local * 8.0 - drift * 0.6), 0.35);
        float body = 1.0 - smoothstep(0.10, 0.5, d);
        float fade = sin(kPi * v_age);                   // fades in and back out
        alpha = body * body * mix(0.35, 1.0, n) * fade * 0.75;

    } else if (v_kind == kBeam) {
        float core = 1.0 - smoothstep(0.0, 0.18, cap_d);
        float glow = 1.0 - smoothstep(0.0, 0.5, cap_d);
        // "Tiny antibody particles racing forward": discrete bright packets
        // sliding along the beam, so a continuously-on beam still has motion.
        float packets = pow(max(sin((t_along * 9.0 - u_time * 11.0) * 6.2831853), 0.0), 6.0);
        float shimmer = 0.80 + 0.20 * sin(t_along * 40.0 - u_time * 26.0);
        alpha = (glow * 0.45 + core * (0.65 + 0.9 * packets)) * shimmer;
        alpha *= 1.0 - 0.5 * v_age;
        rgb = mix(rgb, vec3(1.0), clamp(core * 0.55 + packets * 0.8, 0.0, 1.0));

    } else if (v_kind == kFragment) {
        // Local +x is the wedge's bisector and the quad's centre is the
        // cell's old centre, so the wedge fans out from the origin.
        float r = length(v_local);
        vec2  dir = v_local / max(r, 1e-4);
        float ang = atan(v_local.y, v_local.x);
        // The torn sides wander as they run outward, differently per piece.
        float tear = (vnoise(vec2(r * 12.0, v_seed * 53.0)) - 0.5) * 0.26;
        float half_span = v_shape > 0.0 ? v_shape : kFragmentHalfSpan;
        float side = (abs(ang) - (half_span + tear)) * max(r, 0.04);
        // The membrane keeps the living cell's soft wobble.
        float outer = r - (0.43 + 0.05 * (vnoise(dir * 2.2 + v_seed * 17.0) - 0.5));
        // The middle, where the nucleus sat, tears out too.
        float inner = 0.06 + 0.06 * vnoise(dir * 3.0 + v_seed * 29.0) - r;
        float d = max(max(side, outer), inner);
        float body = 1.0 - smoothstep(-0.012, 0.010, d);

        // swarmer.frag's cytoplasm: lighter just under the membrane, denser
        // deep inside, a fine grain; the torn edges a shade darker than the
        // inside of the piece; the membrane a pale line round the arc.
        float depth = clamp(-outer * 5.0, 0.0, 1.0);
        float grain = smoothstep(0.62, 0.84, vnoise(v_local * 24.0 + v_seed * 41.0));
        float cut = 1.0 - smoothstep(0.0, 0.035, -max(side, inner));
        float membrane = 1.0 - smoothstep(0.0, 0.045, abs(outer));
        rgb = min(v_tint.rgb * mix(1.10, 0.84, depth), vec3(1.0));
        rgb = mix(rgb, vec3(1.0), grain * 0.16);
        rgb *= 1.0 - 0.14 * cut;
        rgb = mix(rgb, vec3(1.0), membrane * 0.55);
        alpha = body;

    } else if (v_kind == kGlobule) {
        float r = length(v_local);
        vec2  dir = v_local / max(r, 1e-4);
        float d = r - (0.38 + max(v_shape, 0.0) * (vnoise(dir * 1.8 + v_seed * 31.0) - 0.5));
        float body = 1.0 - smoothstep(-0.03, 0.012, d);
        // Solid, a shade darker at the edge: a lump, never a bubble. A lighter
        // rim reads as a hollow ring at the sizes granules are drawn.
        float depth = clamp(-d * 5.0, 0.0, 1.0);
        rgb = v_tint.rgb * mix(0.86, 1.0, depth);
        alpha = body;

    } else { // kBolt
        const float kAmp = 0.34;
        float off = bolt_offset(t_along, v_seed) * kAmp;
        // Slope correction keeps the stroke a roughly constant apparent width
        // through the steep diagonal runs between nodes.
        float eps  = 0.02;
        float off2 = bolt_offset(t_along + eps, v_seed) * kAmp;
        float slope = (off2 - off) / (eps * max(v_aspect, 1e-3));
        float d = abs(v_local.y - off) * inversesqrt(1.0 + slope * slope);

        float core = 1.0 - smoothstep(0.0, 0.040, d);
        float glow = 1.0 - smoothstep(0.0, 0.24, d);

        // A shorter fork peeling off the main channel over the middle of the
        // span — the "branching" half of the Bolt read.
        float win  = smoothstep(0.30, 0.40, t_along) * (1.0 - smoothstep(0.70, 0.86, t_along));
        float boff = bolt_offset(t_along, v_seed + 7.31) * kAmp * 1.2;
        float bd   = abs(v_local.y - boff);
        float branch = (1.0 - smoothstep(0.0, 0.09, bd)) * win;

        // Stroboscopic whole-stroke flicker, quantized to ~24 Hz. Floored well
        // above zero so a bolt is never invisible on the frame you screenshot.
        float flicker = 0.68 + 0.32 * hash11(floor(u_time * 24.0) + v_seed * 11.0);

        alpha = clamp(core + glow * 0.32 + branch * 0.55, 0.0, 1.0) * flicker;
        alpha *= 1.0 - v_age * v_age;
        rgb = mix(rgb, vec3(1.0), clamp(core * 0.8 + branch * 0.3, 0.0, 1.0));
    }

    // ParticleInstance::tint_rgba8 is already CPU-faded (frozen contract), so
    // the alpha channel folds in on top of the shader-side curve.
    alpha *= v_tint.a;
    if (alpha <= 0.004) discard;   // fill-rate saver at quarter-million counts
    o_color = vec4(rgb, clamp(alpha, 0.0, 1.0));
}
