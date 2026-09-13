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

// Family id packed into bits 8..15 by ChaffBatcher (see build_chaff_instances).
const uint CHAFF_FAMILY_SHIFT = 8u;
const uint CHAFF_FAMILY_MASK  = 0xFFu;
// Local crowding, 0..255 over [0, lod_blob_threshold], packed into bits 16..23
// by the same batcher. See its comment for what it is for; see the shadow block
// below for what this stage does with it.
const uint CHAFF_CROWD_SHIFT = 16u;
const uint CHAFF_CROWD_MASK  = 0xFFu;
// Hit flash, 0..255, packed into the last free byte (24..31) by the same
// batcher. See sim/chaff/HitFlash.h for what it is; see the flash block at the
// bottom of main() for what this stage does with it.
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

// ---------------------------------------------------------------------------
// VIRUS — an icosahedral capsid ringed with receptor spikes.
//
// Real virions read as "faceted ball wearing a crown of knobs" (that crown is
// literally why coronaviruses are named that). Both halves of that are cheap in
// polar coordinates:
//   - the facets are a low-amplitude cosine on the angle, which flattens the
//     circle into a polygon-ish outline without any polygon SDF;
//   - the spikes are a sharpened cosine at a higher frequency, pushed OUTWARD
//     from the same radius, each tipped with a knob via the smoothstep.
// The spike phase is offset by the per-agent anim_phase so neighbouring virions
// are not rotationally identical, which is what stops a cluster reading as a
// repeated stamp.
// ---------------------------------------------------------------------------
float sdf_virus(vec2 p, float r, float phase, float pulse) {
    float a = atan(p.y, p.x);
    float d = length(p);

    // Faceted capsid: 5-fold flattening, very shallow.
    float facet = cos(a * 5.0 + phase * 0.3) * 0.022;

    // Receptor spikes: 11 of them, sharp, standing off the capsid surface.
    float spike_wave = cos(a * 11.0 + phase);
    float spikes = pow(max(spike_wave, 0.0), 3.0) * 0.17;

    float radius = r + facet + spikes + pulse;
    return d - radius;
}

// ---------------------------------------------------------------------------
// BACTERIA — a rod (bacillus) with a flagellum.
//
// A capsule SDF is the right primitive: real bacilli are cylinders with
// hemispherical caps, which is exactly what a capsule is. The rod is oriented
// along local +x, and the vertex stage already rotates local space by the
// agent's heading, so a bacterium automatically swims lengthwise — which is
// both correct and free.
//
// The flagellum is a sine-displaced line trailing the rod, beating at the
// agent's own tempo. It is a huge part of reading as "bacterium" rather than
// "pill", and costs one extra distance evaluation.
// ---------------------------------------------------------------------------
float sdf_capsule(vec2 p, float half_len, float r) {
    p.x -= clamp(p.x, -half_len, half_len);
    return length(p) - r;
}

float sdf_bacteria(vec2 p, float r, float phase, float pulse, out float flagellum) {
    // The rod is pushed FORWARD in the quad rather than centred. Local space
    // only spans +-0.95 (chaff.vert's kPad), and a centred rod eats nearly all
    // of it, leaving the flagellum about a tenth of a body length -- a stub
    // that read as a rendering artifact rather than a tail. Offsetting the body
    // buys the rear half of the quad for the flagellum without widening the
    // quad (which would cost fill rate on all ten thousand instances). Pushed
    // to 0.30 (from 0.20) to claw back the front margin the body wasn't using
    // -- front cap+radius lands at ~0.91, tail tip at ~0.91 behind, both still
    // shy of the 0.95 pad -- so the longer flagellum below has somewhere to go.
    const float kBodyOffset = 0.30;
    vec2 bp = p - vec2(kBodyOffset, 0.0);

    float half_len = r * 0.52;
    float body = sdf_capsule(bp, half_len, r * 0.50 + pulse);

    // Flagellum: a long, tapering, beating filament trailing the rear cap.
    float rear = kBodyOffset - half_len;           // x of the rear cap centre
    float tail_x = p.x - rear;                     // 0 at the cap, negative behind
    float along = clamp(-tail_x / 0.90, 0.0, 1.0); // 0 at cap, 1 at the tip

    // Amplitude grows toward the tip, which is what a real beating filament
    // does and what makes it read as propulsion rather than a drawn line.
    float beat = sin(tail_x * 11.0 - phase * 3.0) * (0.02 + 0.075 * along);
    float thickness = mix(0.042, 0.010, along);    // tapers to a point
    float tail_d = abs(p.y - beat) - thickness;

    // Behind the rod only, and fading out at the tip so it does not end in a
    // hard chop against the quad edge.
    float in_span = step(p.x, rear) * (1.0 - smoothstep(0.75, 1.0, along));
    flagellum = (1.0 - smoothstep(-0.008, 0.018, tail_d)) * in_span;

    return body;
}

void main() {
    uint family = (v_flags >> CHAFF_FAMILY_SHIFT) & CHAFF_FAMILY_MASK;

    // Tempo pulse: small so 10k instances read as "alive", not "flickering".
    float pulse = sin(v_anim_phase) * 0.05 * v_wobble;

    float body_d;
    float flagellum = 0.0;
    float rim_scale = 1.0;   // per-species rim tightness

    if (family == FAM_VIRUS) {
        body_d = sdf_virus(v_local, 0.36, v_anim_phase, pulse);
        rim_scale = 0.8;     // crisper edge; a capsid is a hard shell
    } else if (family == FAM_BACTERIA) {
        body_d = sdf_bacteria(v_local, 0.60, v_anim_phase, pulse, flagellum);
        rim_scale = 1.2;     // softer; a bacterium is a wet sac
    } else {
        body_d = length(v_local) - (0.5 + pulse);
    }

    float body_alpha = 1.0 - smoothstep(-0.06 * rim_scale, 0.0, body_d);
    body_alpha = max(body_alpha, flagellum);

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
                         mix(1.0, 0.18, crowd) * sprite_fade;

    if (body_alpha <= 0.0 && shadow_alpha <= 0.0) discard;

    vec3 rgb = v_tint.rgb;

    // ---- Cheap interior shading -------------------------------------------
    // One smoothstep on the already-computed distance. Interior goes denser and
    // the edge lifts toward white, which fakes a wet membrane over a darker
    // cytoplasm. This is the whole "biological" budget at this sprite size, and
    // it is what separates a cell from a flat dot.
    float depth = clamp(-body_d * 3.4, 0.0, 1.0);          // 0 at edge, 1 deep inside
    rgb *= mix(1.30, 0.62, depth);                          // bright rim, dark core
    float rim = 1.0 - smoothstep(0.0, 0.11, abs(body_d));
    rgb = mix(rgb, mix(rgb, vec3(1.0), 0.72), rim * 0.80);  // membrane highlight

    if (family == FAM_VIRUS) {
        // Dense genetic core: a small hot centre, which is what makes a virion
        // read as "shell around cargo" rather than a spiky blob.
        float core = 1.0 - smoothstep(0.10, 0.19, length(v_local));
        rgb = mix(rgb, mix(v_tint.rgb, vec3(1.0), 0.35), core * 0.5);
    } else if (family == FAM_BACTERIA) {
        // Nucleoid: a lengthwise darker band, the DNA mass down the rod.
        // Centred on the offset body, not on the quad.
        vec2 bp = v_local - vec2(0.20, 0.0);
        float band = 1.0 - smoothstep(0.0, 0.14, abs(bp.y));
        float along = 1.0 - smoothstep(0.14, 0.38, abs(bp.x));
        rgb *= mix(1.0, 0.72, band * along * (1.0 - rim));
    }

    if ((v_flags & FLAG_MARKED) != 0u) rgb = mix(rgb, vec3(1.0), 0.25);
    if ((v_flags & FLAG_SLOWED) != 0u) rgb = mix(rgb, vec3(0.55, 0.75, 1.0), 0.35);

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
    if (hit_flash > 0.0) {
        rgb = mix(rgb, u_hit_flash_color[min(family, FAM_COUNT - 1u)].rgb, hit_flash);
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
