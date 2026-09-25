#version 450 core
// Chaff instanced sprite pass — fragment stage.
// Procedural only: the pathogen silhouette is an SDF, never a texture
// (no binary assets in this project). Wave 1C owns the body.
//
// DESIGN.md §6 readability rule: colour = family (v_tint, set by the CPU
// batcher from render::family_color), silhouette size = threat tier (baked
// into i_scale before this stage runs), animation tempo = speed tier (baked
// into v_anim_phase's rate of advance). This shader adds the *within*-family
// texture: the species silhouette, a tempo-driven pulse, and the drop shadow.
//
// ---------------------------------------------------------------------------
// BUDGET — read this before adding anything here
// ---------------------------------------------------------------------------
// This runs for every fragment of every one of up to ~10,000 instances. At
// gameplay zoom an agent covers roughly 6-12 pixels, so anything subtle is
// invisible and merely expensive. Two consequences, both deliberate:
//
//   1. SILHOUETTE OVER INTERIOR. What survives at 8 pixels is the outline, not
//      organelles. So each family gets a distinct *shape* (spiked sphere,
//      rod, ...) and only the cheapest interior hint. The elaborate layered
//      treatment — domain-warped membranes, fake subsurface, organelle fields —
//      belongs in entity.frag, which draws dozens of towers, not thousands of
//      agents.
//   2. NO fbm/NOISE LOOPS. Every effect here is closed-form trig or algebra.
//      A 4-octave noise call per fragment is affordable on a tower and is not
//      affordable here.
//
// Dense crowds are NOT this shader's problem: past the LOD threshold the
// batcher stops emitting instances and the blob/density pass takes over, and
// that pass is where a packed horde is meant to read as one organic mass.

in vec2  v_local;
in vec4  v_tint;
flat in uint v_flags;
in float v_anim_phase;
in float v_wobble;
flat in vec2 v_shadow_offset;

out vec4 o_color;

// Mirrors sim/chaff/ChaffBuffers.h chaff_flags (low 8 bits).
const uint FLAG_MARKED  = 1u << 1;
const uint FLAG_SLOWED  = 1u << 2;
const uint FLAG_HIDDEN  = 1u << 3;
// Renderer-only live-agent bits. These never feed back into chaff simulation:
// a replicating virus is drawn as complementary parent halves while this flag
// is set, then as two ordinary whole viruses once its split timer finishes.
const uint FLAG_SPLIT_ACTIVE  = 1u << 6;
const uint FLAG_SPLIT_NEGATIVE_HALF = 1u << 7;
// Riding a host (chaff_flags::kLatched, moved up here by the batcher because
// sim bit 6 is the split morph's in this word). Local +x points INTO the host
// and v_wobble carries the throb clock instead of the family wobble; see
// latch_throb() below.
const uint FLAG_LATCHED = 1u << 15;

// Family id packed into bits 8..15 by ChaffBatcher (see build_chaff_instances).
// The mask stops one bit short of the byte: bit 15 is FLAG_LATCHED.
const uint CHAFF_FAMILY_SHIFT = 8u;
const uint CHAFF_FAMILY_MASK  = 0x7Fu;
// Local crowding, 0..255 over [0, lod_blob_threshold], packed into bits 16..23
// by the same batcher. See its comment for what it is for; see the shadow block
// below for what this stage does with it.
const uint CHAFF_CROWD_SHIFT = 16u;
const uint CHAFF_CROWD_MASK  = 0xFFu;
// Hit flash, 0..255, packed into the last free byte (24..31) by the same
// batcher. While FLAG_SPLIT_ACTIVE is set this instead holds the split reveal
// 0..255, and the flash is deferred for the tiny duration of that morph.
const uint CHAFF_FLASH_SHIFT = 24u;
const uint CHAFF_FLASH_MASK  = 0xFFu;
// Mirrors PathogenFamily's declaration order in core/Types.h.
const uint FAM_VIRUS    = 0u;
const uint FAM_BACTERIA = 1u;
const uint FAM_COUNT    = 2u;   // == immune::kFamilyCount

// What each family flares toward when it is hit, authored per family in
// enemies.json and uploaded once per frame by Renderer::submit_chaff.
//
// A UNIFORM RATHER THAN A PER-INSTANCE ATTRIBUTE, on purpose: the colour is a
// property of the FAMILY, and there are two of those against ten thousand
// instances. Sending it per agent would push the same three bytes up the bus
// five thousand times each and force a frozen 32-byte instance layout wider to
// do it. Locations 0 and 1 belong to chaff.vert's view-projection and time.
layout(location = 2) uniform vec4 u_hit_flash_color[FAM_COUNT];
// x = local reveal distance, y = seam softness. Uploaded from each family's
// replication_split config every frame so live config edits redraw immediately.
layout(location = 4) uniform vec4 u_replication_split_params[FAM_COUNT];
// The latch throb (render/LatchThrob.h), refreshed every frame like the two
// above. shape = (throb, slosh, wave, wave_count), skin = (ripple, stream,
// squash, probe), pump = (glow, -, -, -).
layout(location = 6) uniform vec4 u_latch_throb_shape[FAM_COUNT];
layout(location = 8) uniform vec4 u_latch_throb_skin[FAM_COUNT];
layout(location = 10) uniform vec4 u_latch_throb_pump[FAM_COUNT];
// render::kShadowsEnabled as 0/1: a global kill switch for the drop shadow.
layout(location = 12) uniform float u_shadows;

// ---------------------------------------------------------------------------
// VIRUS — an icosahedral capsid ringed with stalked receptor knobs.
//
// The cartoon-virus silhouette: a faceted ball with a ring of LOLLIPOPS
// standing off it -- a thin stalk out of the shell, a round knob on the end
// (the receptor proteins a real virion docks with, drawn the way every
// illustrator draws them). Cheap in polar coordinates:
//   - the facets are a low-amplitude cosine on the angle, which flattens the
//     circle into a polygon-ish outline without any polygon SDF;
//   - the knobs are ONE stalk+knob evaluated once, after folding the angle
//     into the nearest of N identical sectors, then unioned with the capsid.
// The ring is rotated by the per-agent anim_phase so neighbouring virions are
// not rotationally identical, which is what stops a cluster reading as a
// repeated stamp.
// ---------------------------------------------------------------------------
// `spike_scale` is 1 for a virion in the lane; a feeding one draws its
// receptors in on every push (see latch_throb).
const float kVirusKnobs = 9.0;

float sdf_virus(vec2 p, float r, float phase, float pulse, float spike_scale) {
    float a = atan(p.y, p.x);
    float d = length(p);

    // Faceted capsid: 5-fold flattening, very shallow.
    float facet = cos(a * 5.0 + phase * 0.3) * 0.022;
    float capsid = d - (r + facet + pulse);

    // Fold the angle into one sector so a single stalk+knob, drawn along the
    // folded +x axis, repeats round the whole ring.
    float sector = 6.28318530 / kVirusKnobs;
    float fa = mod(a + phase + 0.5 * sector, sector) - 0.5 * sector;
    vec2 q = d * vec2(cos(fa), sin(fa));

    // Stalk: a thin capsule rooted inside the shell, so it never detaches
    // from a capsid that is breathing under it. Knob: a disc on its tip.
    float reach = 0.10 * spike_scale;
    float stalk_x = clamp(q.x, r - 0.05, r + reach);
    float stalk = length(vec2(q.x - stalk_x, q.y)) - 0.055;
    float knob = length(q - vec2(r + reach, 0.0)) - 0.095;

    return min(capsid, min(stalk, knob));
}

// ---------------------------------------------------------------------------
// LATCH THROB -- a passenger pumping itself into its host.
//
// A latched virion is planted with local +x pointing INTO its host, parked
// on the host's footprint (sim/hostile ring_radius) with the host drawn over
// it. Sitting there rigid it read as a decal stuck to the cell. This is the
// motion of a body with liquid moving through it, toward +x, on one master
// STROKE that everything below shares: -1 is the push (a squeeze toward the
// host), +1 the refill. Closed-form trig only, per BUDGET above; the branch
// that reaches it is uniform across an instance, and a few dozen agents are
// latched against thousands walking, so the crowd never pays for it.
//
// The stroke is three sines at incommensurate rates, so the rhythm never
// quite repeats; latch_throb() turns it into the outline deformation:
//
//   throb   the whole body breathing with the stroke;
//   slosh   the stroke shifting volume front-to-back: on the push the back
//           flattens and the front swells, so each beat reads as a shove
//           INTO the host rather than a balloon inflating;
//   wave    peristaltic slugs rolling round the outline from the far side
//           toward the host, fading where they would meet the membrane;
//   ripple  a fine two-frequency shimmer on the skin -- the "noisy" in
//           noisy throbbing -- so the surface is never still between beats.
//
// main() adds the rest on the same stroke: a bellows SQUASH of the whole
// local frame (shorter along x, fatter across, on the push), the crown
// flattening, a PROBE -- a tapering tube reaching into the host with beads
// travelling down it -- and a GLOW that lights the push, which is the one
// part of this that still reads when the sprite is six pixels wide.
//
// `a` is the polar angle from +x, `T` the throb clock (2*pi per stroke, from
// v_wobble). Returns a radial offset in local units to add to the capsid
// radius.
// ---------------------------------------------------------------------------
float latch_stroke(float T) {
    return 0.55 * sin(T) + 0.30 * sin(T * 1.83 + 1.7) + 0.15 * sin(T * 3.11 + 0.4);
}

float latch_throb(float a, float T, float stroke, vec4 shape, float ripple) {
    float aa = abs(a);
    // Crests sit where aa * count + 1.2 T = pi/2 + 2 pi n, so they slide to
    // smaller aa -- toward +x, the host -- as T advances. Squared, not cubed:
    // a broad swell rolling round the body reads as liquid, a narrow bump
    // between the receptor stalks reads as another stalk.
    float slug = pow(max(sin(aa * shape.w + T * 1.2), 0.0), 2.0);
    float far_side = smoothstep(0.35, 1.3, aa);
    float skin = sin(a * 7.0 + T * 2.3) * sin(a * 13.0 - T * 1.7);
    return shape.x * stroke
         - shape.y * stroke * cos(a)
         + shape.z * slug * far_side
         + ripple * skin;
}

// ---------------------------------------------------------------------------
// BACTERIA — a fluid rod (bacillus) studded with receptors, trailing a
// flagellum.
//
// A capsule SDF is the right primitive: real bacilli are cylinders with
// hemispherical caps, which is exactly what a capsule is. The rod is oriented
// along local +x, and the vertex stage already rotates local space by the
// agent's heading, so a bacterium automatically swims lengthwise — which is
// both correct and free.
//
// But a rigid capsule read as a pill. This one is a WET SAC, borrowing the
// Macrophage's fluid-mass treatment (entity.frag: sdf_macrophage) within the
// budget above — where the Macrophage domain-warps with fbm, this does it with
// two sine products, and every other deformation is one trig term:
//
//   flex    the whole rod bows into a slow banana, alternating sides, so the
//           body is never straight for long;
//   undulate a travelling wave runs nose-to-tail down the rod, the same wave
//           the flagellum continues behind it, so body and tail read as one
//           swimming organism;
//   pulse   a peristaltic swell rolls down the length, with a slow whole-body
//           breath under it;
//   warp    the domain warp: lumps that drift over the membrane so the outline
//           is never the same twice.
//
// RECEPTORS: the virus's stalk+knob lollipops, standing off the capsule's
// outline. A capsule has no rotational symmetry to fold an angle into, so the
// ring is done along the outline's own coordinate instead: the outline is
// unrolled into one length (nose cap, top flank, rear cap, bottom flank), the
// pixel's nearest point on it is looked up as a position along that length,
// and that position is rounded to the nearest of N evenly spaced slots. The
// slot's root and normal are recovered from the same unrolling, so one
// stalk+knob is evaluated per pixel and stands on the capsule's true normal --
// vertical on the flanks, radial on the caps. Done in the deformed frame, so
// the ring flexes and pulses with the body it is rooted in.
// ---------------------------------------------------------------------------
float sdf_capsule(vec2 p, float half_len, float r) {
    p.x -= clamp(p.x, -half_len, half_len);
    return length(p) - r;
}

float sdf_segment_r(vec2 p, vec2 a, vec2 b, float r) {
    vec2 ba = b - a;
    float h = clamp(dot(p - a, ba) / dot(ba, ba), 0.0, 1.0);
    return length(p - a - ba * h) - r;
}

const float kHalfPi = 1.57079633;

// The capsule's outline as one coordinate: 0 at the nose (+x), increasing
// counter-clockwise -- over the nose cap, back along the top flank, round the
// rear cap, forward along the bottom flank -- for a total of 2*pi*r + 4h.
// Evaluated for any point, as the coordinate of its nearest outline point.
float capsule_perimeter(vec2 p, float h, float r) {
    float cap = kHalfPi * r;
    vec2 v = p - vec2(clamp(p.x, -h, h), 0.0);
    float a = atan(v.y, v.x);
    if (p.x > h)  return r * a;
    if (p.x < -h) return cap + 2.0 * h + r * ((a < 0.0 ? a + 6.28318530 : a) - kHalfPi);
    return v.y > 0.0 ? cap + (h - p.x) : 3.0 * cap + 2.0 * h + (p.x + h);
}

// The inverse: the outline point at coordinate s, and its outward normal.
vec2 capsule_point(float s, float h, float r, out vec2 n) {
    float cap = kHalfPi * r;
    if (s < cap) {                        // nose cap (s >= -cap by construction)
        n = vec2(cos(s / r), sin(s / r));
        return vec2(h, 0.0) + n * r;
    }
    s -= cap;
    if (s < 2.0 * h) {                    // top flank
        n = vec2(0.0, 1.0);
        return vec2(h - s, r);
    }
    s -= 2.0 * h;
    if (s < 2.0 * cap) {                  // rear cap
        float a = kHalfPi + s / r;
        n = vec2(cos(a), sin(a));
        return vec2(-h, 0.0) + n * r;
    }
    s -= 2.0 * cap;                       // bottom flank
    n = vec2(0.0, -1.0);
    return vec2(-h + s, -r);
}

const float kBacteriaReceptors = 12.0;
// The rod's proportions in local units, shared by the body and its receptors.
const float kBacteriaHalfLen = 0.27;
const float kBacteriaRadius  = 0.255;
// The rod is pushed FORWARD in the quad rather than centred. Local space only
// spans +-0.95 (chaff.vert's kPad), and a centred rod eats nearly all of it,
// leaving the flagellum a stub that read as a rendering artifact rather than a
// tail. Offsetting the body buys the rear half of the quad for the flagellum
// without widening the quad (which would cost fill rate on all ten thousand
// instances). Front receptor tip lands at ~0.90, tail tip ~0.92 behind, both
// shy of the pad.
const float kBacteriaBodyOffset = 0.25;

float sdf_bacteria(vec2 p, float phase, float pulse, out float flagellum,
                   out vec2 skin_p) {
    const float h = kBacteriaHalfLen;
    const float r = kBacteriaRadius;
    vec2 bp = p - vec2(kBacteriaBodyOffset, 0.0);

    // The travelling wave the body and tail share: crests advance toward -x
    // (nose to tail, then down the flagellum) as the phase grows.
    const float kWaveNum = 10.0;

    // Flex: bow the whole rod. Quadratic in x so the middle stays put and the
    // ends swing; the sign alternates on a slow clock.
    float flex = 0.22 * sin(phase * 0.37);
    bp.y -= flex * (bp.x * bp.x - 0.35 * h * h);
    // Undulate: the wave itself, gentle in the body (the tail whips harder).
    bp.y -= 0.028 * sin(kWaveNum * -bp.x - phase) * smoothstep(-h - r, h, bp.x);
    // Warp: lumps drifting across the membrane.
    float wx = sin(bp.y * 8.3 + phase * 0.90) * cos(bp.x * 5.1 - phase * 0.61);
    float wy = sin(bp.x * 6.7 - phase * 0.73) * cos(bp.y * 4.3 + phase * 0.52);
    bp += vec2(wx, wy) * 0.030;
    skin_p = bp;

    // Pulse: a peristaltic swell rolling tailward, over a whole-body breath.
    float swell = 0.055 * sin(bp.x * 7.0 + phase * 0.9) + 0.025 * sin(phase * 0.55);
    float body = sdf_capsule(bp, h, r + r * swell + pulse);

    // Receptors: nearest-slot lookup along the unrolled outline (see above).
    float perim = 6.28318530 * r + 4.0 * h;
    float slot_len = perim / kBacteriaReceptors;
    // A slow creep round the outline: enough that no two neighbours in a
    // crowd share a ring, too slow to read as knobs sliding on the skin.
    float ring_drift = phase * 0.003 * perim;
    float s_here = capsule_perimeter(bp, h, r) - ring_drift;
    float s_slot = floor(s_here / slot_len + 0.5) * slot_len + ring_drift;
    // Fold back into [-cap, perim - cap), the range the unrolling produces.
    s_slot = mod(s_slot + kHalfPi * r, perim) - kHalfPi * r;
    vec2 n;
    vec2 root = capsule_point(s_slot, h, r, n);
    // Each receptor nods on its own beat so the ring never reads as a stamp.
    float reach = 0.085 + 0.02 * sin(phase * 1.3 + s_slot * 11.0);
    vec2 tip = root + n * reach;
    float stalk = sdf_segment_r(bp, root - n * 0.06, tip, 0.038);
    float knob = length(bp - tip) - 0.062;
    float receptors = min(stalk, knob);
    body = min(body, receptors);

    // Flagellum: a long, tapering, beating filament trailing the rear cap,
    // continuing the body's wave in the UNDEFORMED frame so it stays rooted
    // where the cap actually is.
    float rear = kBacteriaBodyOffset - h;           // x of the rear cap centre
    float tail_x = p.x - rear;                      // 0 at the cap, negative behind
    float along = clamp(-tail_x / 0.90, 0.0, 1.0);  // 0 at cap, 1 at the tip

    // Snake-like undulation: a travelling wave that runs FROM the body TOWARD
    // the tip. `s` counts distance behind the cap, so sin(k*s - w*t) advances
    // in +s as t grows -- the crests slither down the tail and off the end.
    //
    // Tuned for GAMEPLAY zoom, not the close-up: at the campaign view height a
    // bacterium is ~14 px long, so a subtle wave is sub-pixel. About 1.5
    // wavelengths in the span, a swing of a quarter of the quad, and a wave
    // that advances at the family tempo itself (~1.5 cycles/s) rather than 3x
    // it -- fast enough to be alive, slow enough that the eye follows a bend
    // down the tail instead of seeing a buzz.
    float s = -tail_x;
    float amp = 0.26 * along * along;               // rooted at the cap, whips at the tip
    float wave_arg = kWaveNum * s - phase;
    float beat = sin(wave_arg) * amp;

    // Perpendicular distance, not vertical: a plain abs(p.y - beat) thins the
    // stroke wherever the curve is steep, so the bends looked pinched. Dividing
    // by the curve's arc-length factor keeps the snake a uniform width around
    // every bend.
    float slope = cos(wave_arg) * kWaveNum * amp;
    float thickness = mix(0.065, 0.014, along);     // tapers to a point
    float tail_d = abs(p.y - beat) * inversesqrt(1.0 + slope * slope) - thickness;

    // Behind the rod only, and fading out at the tip so it does not end in a
    // hard chop against the quad edge.
    float in_span = step(p.x, rear) * (1.0 - smoothstep(0.75, 1.0, along));
    flagellum = (1.0 - smoothstep(-0.008, 0.018, tail_d)) * in_span;

    return body;
}

void main() {
    uint family = (v_flags >> CHAFF_FAMILY_SHIFT) & CHAFF_FAMILY_MASK;
    uint fam_slot = min(family, FAM_COUNT - 1u);

    // Tempo pulse: small so 10k instances read as "alive", not "flickering".
    float pulse = sin(v_anim_phase) * 0.05 * v_wobble;

    // Feeding on a host: the tempo pulse gives way to the latch throb, which
    // owns the whole deformation (and v_wobble is the throb clock, not the
    // wobble amount). `stroke` beats -1..1 with it for everything below.
    bool latched = (v_flags & FLAG_LATCHED) != 0u;
    float stroke = 0.0;
    float throb_T = v_wobble;
    float spike_phase = v_anim_phase;
    float spike_scale = 1.0;
    vec2 body_p = v_local;
    vec4 throb_skin = u_latch_throb_skin[fam_slot];
    if (latched) {
        stroke = latch_stroke(throb_T);
        // The bellows: the frame the capsid is evaluated in compresses
        // toward the host on the push and stretches on the refill, roughly
        // area-preserving so it reads as one body changing shape.
        float sq = throb_skin.z * stroke;
        body_p = vec2(v_local.x / (1.0 + sq), v_local.y / (1.0 - 0.5 * sq));
        float angle = atan(body_p.y, body_p.x);
        pulse = latch_throb(angle, throb_T, stroke, u_latch_throb_shape[fam_slot], throb_skin.x);
        // The crown flattens on the push and twitches on its own clock: a
        // gripping crown, not a painted one.
        spike_scale = 1.0 + 0.2 * stroke;
        spike_phase += 0.35 * sin(throb_T * 2.9 + 1.0);
    }

    float body_d;
    float flagellum = 0.0;
    float rim_scale = 1.0;   // per-species rim tightness
    float core_shade = 0.62; // per-species interior darkening (see depth below)
    vec2 bacteria_skin = v_local;

    if (family == FAM_VIRUS) {
        body_d = sdf_virus(body_p, 0.36, spike_phase, pulse, spike_scale);
        rim_scale = 0.8;     // crisper edge; a capsid is a hard shell
        if (latched && throb_skin.w > 0.0) {
            // The probe: a tapering tube rooted inside the capsid and reaching
            // +x into the host, its wall swelling round the beads travelling
            // down it (crests at x * 42 - 3 T = pi/2 + 2 pi n, so they move
            // toward +x). Unioned into the body so the shading below treats
            // it as one membrane. Unsquashed frame: the root stays inside the
            // capsid through the whole stroke, so it never detaches.
            float x0 = 0.30;
            float x1 = x0 + throb_skin.w;
            float along = clamp((v_local.x - x0) / throb_skin.w, 0.0, 1.0);
            float beads = 0.5 + 0.5 * sin(v_local.x * 42.0 - throb_T * 3.0);
            float thick = mix(0.085, 0.04, along) * (0.85 + 0.45 * beads * beads);
            vec2 q = vec2(v_local.x - clamp(v_local.x, x0, x1), v_local.y);
            body_d = min(body_d, length(q) - thick);
        }
    } else if (family == FAM_BACTERIA) {
        body_d = sdf_bacteria(v_local, v_anim_phase, pulse, flagellum, bacteria_skin);
        rim_scale = 1.2;     // softer; a bacterium is a wet sac
        core_shade = 1.18;   // ...and a lit one: the interior stays as bright as
                             // the membrane, no darkening at all under the streaks
    } else {
        body_d = length(v_local) - (0.5 + pulse);
    }

    float body_alpha = 1.0 - smoothstep(-0.06 * rim_scale, 0.0, body_d);
    body_alpha = max(body_alpha, flagellum);

    // A replication begins as two complementary hemispheres occupying the
    // parent's original position. Their local frame is shared, so opposite
    // x halves give the two true pieces of one virion; revealing further
    // inward while the instances pull apart completes them into daughter
    // viruses. This keeps the parent on screen throughout the transformation.
    float split_mask = 1.0;
    bool splitting = (v_flags & FLAG_SPLIT_ACTIVE) != 0u && family == FAM_VIRUS;
    float split_reveal = float((v_flags >> CHAFF_FLASH_SHIFT) & CHAFF_FLASH_MASK) *
                         (1.0 / 255.0);
    if (splitting) {
        vec4 split_params = u_replication_split_params[fam_slot];
        float reveal_distance = split_reveal * max(0.0, split_params.x);
        // The two split instances share their local frame; one retains x <= 0
        // and the other x >= 0 at the start, which recreates the parent shell.
        float half_x = (v_flags & FLAG_SPLIT_NEGATIVE_HALF) != 0u ? -v_local.x : v_local.x;
        float seam = max(0.0, split_params.y);
        split_mask = smoothstep(-seam, seam, half_x + reveal_distance);
        body_alpha *= split_mask;
    }

    // How much of this sprite is being drawn at all. The LOD crossfade hands it
    // down in the tint alpha: an agent inside the blob band is drawn at partial
    // sprite alpha and deposits the complementary fraction of its mass into the
    // density field, and the two are supposed to sum to one agent.
    //
    // EVERYTHING the sprite puts on screen has to obey it, shadow included.
    // While the shadow ignored it, a crowd crossing into the band faded its
    // bodies out from under shadows that stayed at full strength -- a cell's
    // worth of agents reduced to a pile of near-black discs, with a hard edge
    // where the neighbouring broadphase cell had a different occupancy. Those
    // were the dark squares: not a blob artifact, the sprite pass fading out
    // the wrong half of itself.
    float sprite_fade = v_tint.a;
    if ((v_flags & FLAG_HIDDEN) != 0u) sprite_fade *= 0.35;

    // Drop shadow. Weighted much harder than it used to be, because the
    // substrate is no longer a dark low-saturation floor that every agent
    // automatically out-values. Against a vivid red lumen, a family's hue can
    // sit within a hundredth of the background's luminance — hue is all that
    // separates them then, and hue alone does not carry a six-pixel sprite.
    // A dark contact shadow plus the
    // bright rim below gives EVERY family its own local contrast regardless of
    // what it is sitting on, which is exactly how the medical-illustration
    // reference reads magenta virions against red plasma.
    // Retired toward the interior of a crowd, though. The shadow earns its
    // keep against the LANE -- it is what separates a virion from red plasma of
    // the same luminance -- and an agent deep in a horde has no lane under it,
    // only other agents. There its shadow is just 0.46 of black over a
    // neighbour's body, and forty of those composite to a hole. Keeping it at
    // the rim and dropping it inside is also what a medical illustrator does:
    // the mass gets ONE contact shadow, around the outside.
    float crowd = float((v_flags >> CHAFF_CROWD_SHIFT) & CHAFF_CROWD_MASK) * (1.0 / 255.0);
    float shadow_d = length(v_local - v_shadow_offset) - 0.52;
    float shadow_alpha = (1.0 - smoothstep(-0.18, 0.02, shadow_d)) * 0.46 *
                         mix(1.0, 0.18, crowd) * sprite_fade * split_mask * u_shadows;

    if (body_alpha <= 0.0 && shadow_alpha <= 0.0) discard;

    vec3 rgb = v_tint.rgb;

    // ---- Cheap interior shading -------------------------------------------
    // One smoothstep on the already-computed distance. Interior goes denser and
    // the edge lifts toward white, which fakes a wet membrane over a darker
    // cytoplasm. This is the whole "biological" budget at this sprite size, and
    // it is what separates a cell from a flat dot.
    float depth = clamp(-body_d * 3.4, 0.0, 1.0);          // 0 at edge, 1 deep inside
    rgb *= mix(1.30, core_shade, depth);                    // bright rim, dark core
    // The push lights the whole body. Colour survives any zoom; at gameplay
    // distance this is most of what "feeding" looks like.
    if (latched) rgb *= 1.0 - u_latch_throb_pump[fam_slot].x * stroke;
    float rim = 1.0 - smoothstep(0.0, 0.11, abs(body_d));
    // A feeding membrane glistens on the push: wet, not lacquered.
    float rim_gain = latched ? 0.80 - 0.15 * stroke : 0.80;
    rgb = mix(rgb, mix(rgb, vec3(1.0), 0.72), rim * rim_gain);  // membrane highlight

    if (family == FAM_VIRUS) {
        // Dense genetic core: a small hot centre, which is what makes a virion
        // read as "shell around cargo" rather than a knobbed blob.
        vec2 core_p = v_local;
        float core_gain = 0.5;
        if (latched) {
            // The cargo is what is being pumped: every squeeze shoves the core
            // toward the host and dims it, and it drifts back on the release.
            core_p.x -= 0.06 * max(-stroke, 0.0);
            core_gain += 0.15 * stroke;
        }
        float core = 1.0 - smoothstep(0.10, 0.19, length(core_p));
        rgb = mix(rgb, mix(v_tint.rgb, vec3(1.0), 0.35), core * core_gain);
        if (latched) {
            // ...down a beaded channel from the core through the probe, the
            // beads travelling +x on the same wave the probe's wall swells to.
            float chan = (1.0 - smoothstep(0.0, 0.075, abs(v_local.y))) *
                         smoothstep(0.02, 0.12, v_local.x) *
                         (1.0 - smoothstep(0.30 + throb_skin.w - 0.05, 0.30 + throb_skin.w,
                                           v_local.x));
            float beads = 0.5 + 0.5 * sin(v_local.x * 42.0 - throb_T * 3.0);
            beads *= beads;
            rgb = mix(rgb, mix(v_tint.rgb, vec3(1.0), 0.55), throb_skin.y * chan * beads);
        }
    } else if (family == FAM_BACTERIA) {
        // Cytoplasm streaks: pale veins running the length of the rod, bowing
        // and pulsing with it (bacteria_skin is the deformed frame), branching
        // where the line families cross. There is deliberately NO dark
        // nucleoid any more: the reference for this body is a lit translucent
        // sac, and a dark band down the middle read as a pill's shadow.
        vec2 sp = bacteria_skin;
        // The veins fan out from the centre line: wider apart mid-rod, drawn
        // together toward the caps, like grain in a leaf.
        float fan = 1.0 + 0.9 * sp.x * sp.x;
        float vein_y = sp.y * fan + 0.04 * sin(sp.x * 5.5 + 0.4);
        float v1 = sin(vein_y * 44.0 + 1.3 * sin(sp.x * 9.0 + 1.3));
        float v2 = sin(vein_y * 27.0 - 1.1 * sin(sp.x * 7.0 - 0.7) + 2.1);
        float v3 = sin(vein_y * 61.0 + 0.9 * sin(sp.x * 12.0 + 2.6) + 0.8);
        float streak = max(max(smoothstep(0.84, 0.98, v1) * 0.9,
                               smoothstep(0.86, 0.99, v2)),
                           smoothstep(0.90, 0.99, v3) * 0.55);
        // Interior only, fading before the rim and just short of the caps.
        float inner = smoothstep(0.0, 0.45, depth) *
                      (1.0 - smoothstep(0.24, 0.36, abs(sp.x)));
        rgb = mix(rgb, mix(v_tint.rgb, vec3(1.0), 0.6), streak * inner * 0.8);
    }

    if ((v_flags & FLAG_MARKED) != 0u) rgb = mix(rgb, vec3(1.0), 0.25);
    if ((v_flags & FLAG_SLOWED) != 0u) {
        // A dark mucus-coated body reads at gameplay zoom; the cool edge keeps
        // its silhouette distinct from the contact shadow beneath it.
        rgb = mix(rgb, vec3(0.05, 0.19, 0.27), 0.78);
        rgb = mix(rgb, vec3(0.48, 0.82, 0.88), rim * 0.32);
    }

    // ---- Hit flash ---------------------------------------------------------
    // LAST, so it wins over both debuff tints above and over all of the
    // interior shading. That ordering is the whole point of doing this here
    // instead of pre-mixing the flash into v_tint on the CPU: a tint mix goes
    // in at the TOP of this function and then gets multiplied by the 1.30/0.62
    // depth ramp and re-tinted by the rim, so a "white" agent would come out
    // as a shaded grey-ish one whose brightness depended on which pixel of it
    // you looked at. A hit is an event, not a material property -- it is
    // allowed to flatten the body it lands on.
    //
    // Deliberately NOT touching alpha. The flash says "this was hit", and an
    // agent that also became more opaque while it said so would fight the LOD
    // crossfade, which owns alpha and needs it to keep meaning exactly one
    // thing (render/ChaffBatcher.h's crossfade contract).
    float hit_flash = float((v_flags >> CHAFF_FLASH_SHIFT) & CHAFF_FLASH_MASK) * (1.0 / 255.0);
    if (!splitting && hit_flash > 0.0) {
        rgb = mix(rgb, u_hit_flash_color[fam_slot].rgb, hit_flash);
    }

    float body_a = body_alpha * sprite_fade;

    // Standard "body over shadow" compositing so the whole sprite + shadow
    // resolves to one straight-alpha output for the destination blend.
    const vec3 kShadowRgb = vec3(0.03, 0.02, 0.03);
    float out_a = body_a + shadow_alpha * (1.0 - body_a);
    if (out_a <= 0.001) discard;
    vec3 out_rgb = (rgb * body_a + kShadowRgb * shadow_alpha * (1.0 - body_a)) / out_a;

    o_color = vec4(out_rgb, out_a);
}
