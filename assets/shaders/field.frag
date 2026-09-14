#version 450 core
// Damage-field VFX pass — fragment stage.
// Procedural SDF/noise only, per the project's no-binary-assets rule.
//
// DESIGN.md §9.5: tower AoEs render as literal fluid/chemical fields (toxin
// clouds, histamine blooms, antibody tides, complement lightning) that must
// read as translucent atmosphere, never opaque cover — alpha is deliberately
// capped well below 1 everywhere in this shader.
//
// WHAT A FIELD IS FOR. It is the tower's ATTACK, drawn where the attack
// actually lands, and it is the only part of the attack that persists between
// frames — the particle layer's flashes are gone in a tenth of a second. So
// each shape here gets a signature that names its tower on sight, rather than
// the generic radial glow every shape used to share:
//
//   shape_id (see field.vert / render::FieldGpuInstance in Renderer.cpp):
//     0 = Circle, TIMED       Macrophage shell / Histamine nova — roiling
//                             digestive burst behind a hard shock rim
//     1 = Rect                B-Cell beam — hot centreline with antibody
//                             packets running down it
//     2 = Cone                Interferon signal — crystalline striations
//                             fanning out, riming over toward the far edge
//     3 = Chain               Cytotoxic T discharge — crackling rings plus
//                             radial filaments
//     4 = Circle, PERSISTENT  unclaimed (was the NK Cell rotor disc) —
//                             deliberately the quietest thing in this file
//     5 = Slow zone           Interferon slow circle (sim/zone/SlowZones.h):
//                             a rimed disc of frost that agents wade through
//
// The tint arrives already set to the casting tower's identity hue (see
// submit_fields), so nothing here picks a colour from scratch — the shapes only
// decide where the light goes and how hot the core gets.
//
// v_intensity is the CPU-computed persistent-vs-burst driver (see
// Renderer::submit_fields): persistent fields hold a steady mid alpha with a
// slow shader-side breathing pulse; burst fields (lifetime > 0) start bright
// and fade as their remaining lifetime runs out, selling the "nova" flash
// DESIGN.md asks for.

in vec2  v_local;
in vec4  v_tint;
flat in uint  v_shape_id;
/// Cone: cos(arc_radians). Rect: the box's width/height ASPECT, which is how
/// this stage knows which of the two axes the beam runs along (see field.vert).
/// Unused (-1) by the circle shapes.
flat in float v_arc_cos;
flat in float v_falloff;
flat in float v_intensity;

out vec4 o_color;

layout(location = 1) uniform float u_time;

const float kTau = 6.28318530;

// Two octaves only. This pass can cover a large fraction of the screen when a
// mortar lands, so it is fill-rate bound in a way entity.frag never is — the
// four-octave fbm that file can afford would be paid for here on every pixel of
// a 13-world-unit disc.
float hash21(vec2 p) {
    p = fract(p * vec2(127.1, 311.7));
    p += dot(p, p + 34.23);
    return fract(p.x * p.y);
}

float vnoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash21(i + vec2(0, 0)), hash21(i + vec2(1, 0)), u.x),
               mix(hash21(i + vec2(0, 1)), hash21(i + vec2(1, 1)), u.x), u.y);
}

float fbm2(vec2 p) {
    return 0.65 * vnoise(p) + 0.35 * vnoise(p * 2.03 + 11.7);
}

// DamageField::falloff semantics mirrored from sim/damage/DamageField.cpp's
// falloff_multiplier: 0 = flat, 1 = linear, 2 = quadratic fade to the edge.
float falloff_glow(float t, float falloff_exp) {
    float x = clamp(1.0 - t, 0.0, 1.0);
    return falloff_exp <= 0.0 ? 1.0 : pow(x, max(falloff_exp, 0.1));
}

void main() {
    vec3 rgb = v_tint.rgb;
    float alpha;
    float cap = 0.85;

    if (v_shape_id == 1u) {
        // ---------------------------------------------------------------
        // B-CELL BEAM. A Rect, but never a box: the damage rect really is
        // long and thin, and drawing it as an evenly-lit rectangle threw away
        // the one thing that makes a beam read as a beam — a hot line down the
        // middle with something travelling along it.
        //
        // sim::Rect is axis-aligned and the beam is axis-snapped (see
        // system_laser), so the long axis is whichever of the two the aspect
        // says is longer. No rotation is involved and none is possible.
        // ---------------------------------------------------------------
        bool horizontal = v_arc_cos >= 1.0;
        float across = horizontal ? abs(v_local.y) : abs(v_local.x);
        float along  = horizontal ? v_local.x : v_local.y;

        float core   = 1.0 - smoothstep(0.0, 0.17, across);
        float sheath = 1.0 - smoothstep(0.0, 0.50, across);
        // Antibody packets running muzzle-to-tip. Same idea as the Beam
        // particle's packets in particle.frag, at a lower frequency so the
        // two layers beat against each other instead of moiring.
        float packets = pow(max(sin((along + 0.5) * 7.0 * kTau - u_time * 5.0), 0.0), 5.0);
        float ends = 1.0 - smoothstep(0.44, 0.50, abs(along));

        alpha = (sheath * 0.30 + core * (0.50 + 0.75 * packets)) * ends;
        alpha *= falloff_glow(clamp(across / 0.5, 0.0, 1.0), v_falloff);
        rgb = mix(rgb, vec3(1.0), clamp(core * 0.55 + packets * 0.7, 0.0, 1.0));

    } else if (v_shape_id == 2u) {
        // ---------------------------------------------------------------
        // INTERFERON CONE. Rotation is already applied at the vertex stage, so
        // the cone always opens along local +x here.
        //
        // Two cues carry "this is cold", and neither is the colour: STRIATIONS
        // fanning out from the emitter (a signal propagating, not a fog
        // sitting), and RIME thickening toward the far edge, which is exactly
        // where kCryoInnerFraction stops merely slowing and starts locking
        // agents down. The visual gradient and the mechanical one agree.
        // ---------------------------------------------------------------
        float dist = length(v_local);
        float t = clamp(dist / 0.5, 0.0, 1.0);
        float edge = 1.0 - smoothstep(0.46, 0.58, dist);
        float cos_theta = dist > 0.0001 ? v_local.x / dist : 1.0;
        float wedge = smoothstep(v_arc_cos - 0.08, v_arc_cos + 0.03, cos_theta);

        float ang = atan(v_local.y, v_local.x);
        // Integer angular frequency, so the fan closes on itself at +-pi
        // instead of showing a seam straight down the middle of the cone.
        float striate = 0.60 + 0.40 * pow(abs(sin(ang * 22.0 + sin(dist * 9.0 - u_time * 1.6))), 3.0);
        float rime = smoothstep(0.30, 1.0, t) * (0.45 + 0.55 * fbm2(v_local * 12.0 - u_time * 0.12));

        alpha = edge * wedge * falloff_glow(t, v_falloff) * striate;
        rgb = mix(rgb, vec3(1.0), clamp(rime * 0.40 + pow(1.0 - t, 2.5) * 0.30, 0.0, 1.0));

    } else if (v_shape_id == 3u) {
        // ---------------------------------------------------------------
        // CYTOTOXIC T LYSIS ZONE. This used to be concentric rings sweeping
        // outward plus radial filaments — a sonar ping with lightning on top,
        // and the single most sci-fi thing in the game. Nothing about a CTL is
        // electrical: it secretes perforin, which PUNCHES PORES in the target
        // membrane, and granzymes then enter through them.
        //
        // So the field is now the membrane losing integrity: a soft blotchy
        // bloom (fbm, not a clean radial ramp, because tissue damage is not
        // uniform) speckled with bright PORES that open in place instead of
        // travelling outward. The pores are the whole read — a moving ring is
        // a wavefront, a field of stationary punctures is a surface being
        // perforated, and only one of those is what this tower does.
        //
        // Pores are built from the position WITHIN each lattice cell, never
        // from a cell-uniform value: a per-cell constant paints the whole cell,
        // which turns the lattice into a grid of solid squares rather than a
        // scatter of round holes.
        //
        // Still lives for kTeslaArcSeconds (0.12s), so it stays a flash, and
        // the fill is kept deliberately low — the punctures carry it, and a
        // bright disc would fog the agents actually being killed inside it.
        // ---------------------------------------------------------------
        float dist = length(v_local);
        float t = clamp(dist / 0.5, 0.0, 1.0);
        float edge = 1.0 - smoothstep(0.40, 0.56, dist);

        // Blotchy interior. The slow drift keeps it alive across the flash
        // without ever reading as a sweep.
        float wet = fbm2(v_local * 7.0 + vec2(u_time * 0.5, -u_time * 0.4));

        // Perforin pores on a jittered lattice, CLIPPED TO THE DISC by `edge`.
        // Without that clip the lattice keeps painting past the field boundary
        // and the flash reads as a rectangular screen door laid over the
        // tissue rather than as damage confined to the kill zone.
        vec2 g = v_local * 20.0;
        vec2 gi = floor(g);
        vec2 gf = fract(g) - 0.5;
        float pick = hash21(gi);
        // Jitter breaks the lattice up so it does not read as printed halftone
        // — but a pore is only sampled against its OWN cell, so jitter plus the
        // pore's outer radius must stay under half a cell (0.5) or the dot runs
        // off the edge and comes back with a straight side. That is what put
        // square-cornered blotches in the field the first time round. Sampling
        // the 3x3 neighbourhood would lift the limit, and costs nine hashes per
        // pixel on a fill-rate-bound shader; small pores are the cheaper answer
        // and the more accurate one.
        //   0.26 (jitter) + 0.12 (max rad) + 0.09 (soft edge) = 0.47 < 0.5
        vec2 jitter = (vec2(hash21(gi + 11.3), hash21(gi + 47.7)) - 0.5) * 0.52;
        float rad = 0.05 + 0.07 * hash21(gi + 21.9);
        float hole = 1.0 - smoothstep(rad, rad + 0.09, length(gf - jitter));
        // Only some cells ever host a pore. The opening PHASE must come from a
        // different hash than the host test: drawing both from `pick` locks
        // every hosting cell into the half of the cycle where it is shut, and
        // the pores never appear at all.
        float host = step(0.58, pick);
        float phase = hash21(gi + 3.7) * 6.28318530;
        float open = 0.35 + 0.65 * pow(clamp(sin(u_time * 3.0 + phase) * 0.5 + 0.5, 0.0, 1.0), 2.0);
        float pore = host * hole * open * edge * (1.0 - t * 0.45);

        alpha = edge * falloff_glow(t, v_falloff) * mix(0.26, 0.62, wet);
        alpha = clamp(alpha + pore * 0.75, 0.0, 1.0);

        // Pores go hot; the surrounding membrane stays the tower's violet, so
        // the punctures read against it instead of washing the whole disc out.
        rgb = mix(rgb, vec3(1.0), clamp(pore * 0.85, 0.0, 1.0));
        cap = 0.72;

    } else if (v_shape_id == 5u) {
        // ---------------------------------------------------------------
        // INTERFERON SLOW ZONE. A circle a swarmer left on the ground, that
        // slows what walks through it. It has to read as a PLACE — a patch of
        // tissue that has gone cold and stays that way for a few seconds —
        // rather than as an explosion, so nothing here flashes or expands.
        //
        // Cues: a crystalline rim (hexagonal-ish striations, because it is
        // frost, not fog), a frosted fill that thickens toward the edge, and
        // slow-drifting rime inside. The fill stays low so the slowed agents
        // inside it remain legible; the rim carries the boundary.
        // ---------------------------------------------------------------
        float dist = length(v_local);
        float t = clamp(dist / 0.5, 0.0, 1.0);
        float edge = 1.0 - smoothstep(0.44, 0.52, dist);
        float rim = 1.0 - smoothstep(0.0, 0.07, abs(dist - 0.44));
        float ang = atan(v_local.y, v_local.x);
        // Six-fold striation so the rim reads as crystal rather than as a
        // drawn circle; slow drift so it is alive without ever "pulsing".
        float crystal = 0.55 + 0.45 * pow(abs(sin(ang * 6.0 + dist * 14.0 - u_time * 0.8)), 2.0);
        float rime = fbm2(v_local * 9.0 + vec2(u_time * 0.15, -u_time * 0.1));
        float fill = mix(0.16, 0.34, rime) * (0.35 + 0.65 * smoothstep(0.0, 1.0, t));

        alpha = edge * (fill + rim * 0.75 * crystal);
        rgb = mix(rgb, vec3(1.0), clamp(rim * crystal * 0.55 + rime * 0.18, 0.0, 1.0));
        cap = 0.62;

    } else if (v_shape_id == 4u) {
        // ---------------------------------------------------------------
        // PERSISTENT CIRCLE, unclaimed since the NK Cell's rotor disc was
        // retired. Deliberately the quietest branch in the file: a boundary
        // ring that says where the disc stops, and barely any fill, so
        // whatever body sits on top of it stays the star.
        // ---------------------------------------------------------------
        float dist = length(v_local);
        float rim = 1.0 - smoothstep(0.0, 0.045, abs(dist - 0.47));
        float fill = (1.0 - smoothstep(0.26, 0.52, dist));
        alpha = fill * 0.16 + rim * 0.55;
        rgb = mix(rgb, vec3(1.0), rim * 0.35);
        cap = 0.42;

    } else {
        // ---------------------------------------------------------------
        // MACROPHAGE SHELL / HISTAMINE NOVA (Circle, timed).
        //
        // The Macrophage is the roster's "consequential answer to a clump", and
        // a soft radial gradient does not read as consequence. Two changes make
        // it land: the interior ROILS (this is enzymatic digestion, not fire —
        // hence turbulence rather than a clean falloff), and it is bounded by a
        // hard SHOCK RIM. The rim is what supplies the punch; the burst lives
        // 0.30s and has to sell the whole shell in that window.
        // ---------------------------------------------------------------
        float dist = length(v_local);
        float t = clamp(dist / 0.5, 0.0, 1.0);
        float roil = fbm2(v_local * 7.0 + vec2(u_time * 0.9, -u_time * 0.6));
        float body = falloff_glow(t, v_falloff) * mix(0.55, 1.20, roil)
                   * (1.0 - smoothstep(0.42, 0.56, dist));
        float shock = 1.0 - smoothstep(0.0, 0.085, abs(dist - 0.455));

        alpha = clamp(body + shock * 0.85, 0.0, 1.0);
        rgb = mix(rgb, vec3(1.0), clamp(shock * 0.70 + pow(1.0 - t, 3.0) * 0.55, 0.0, 1.0));
    }

    alpha *= v_intensity;
    if (alpha <= 0.003) discard;
    o_color = vec4(rgb, clamp(alpha, 0.0, cap));
}
