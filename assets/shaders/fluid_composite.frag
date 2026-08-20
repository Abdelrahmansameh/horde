#version 450 core
// Goblet Cell fluid — pass 2 of 2, fragment stage. THIS is the liquid.
//
// Input is the thickness field pass one summed (see fluid.frag for the channel
// contract). Everything that makes the result read as a fluid rather than as a
// pile of sprites happens here, and all of it comes from one idea: a liquid is
// recognised by its SURFACE, and a surface is a threshold plus a normal.
//
//   1. THRESHOLD. Coverage is a smoothstep across the thickness field, not the
//      field itself. That single step is what fuses overlapping droplets into
//      one body with one unbroken outline — the seam between two blobs exists
//      in the field, but it lives above the threshold, so it does not exist on
//      the surface. It is also what gives the fluid a crisp meniscus edge
//      instead of the soft fog every additive particle system produces.
//
//   2. NORMALS FROM THE GRADIENT. The screen-space gradient of a bounded height
//      derived from thickness gives a normal per pixel, and normals buy the two
//      cues that actually sell "wet": a moving specular glint, and a fresnel
//      rim that brightens wherever the surface turns away. Without them this
//      would be a flat green silhouette no matter how good the simulation is.
//
//   3. THICKNESS AS DEPTH. Beer-Lambert: the deeper the mucus, the more it
//      absorbs, so a thin film is pale and translucent and the middle of a
//      slug is dark and saturated. That gradient is what stops a puddle from
//      reading as a decal.
//
// THE NORMALS ARE READ THROUGH A BLUR, and that is not an optimisation — it is
// the difference between liquid and coral. Thickness is a sum of discrete
// blobs, so its raw gradient carries every individual blob as a bump; shading
// that directly gives a pebbled, brain-coral surface where each particle is
// separately visible, which is exactly the thing the surface exists to hide.
// Low-passing the field before differentiating leaves the shape (a large, slow
// feature) and removes the particles (small, fast ones).
//
// Output is PREMULTIPLIED (the pass blends ONE / ONE_MINUS_SRC_ALPHA) so the
// specular can exceed the coverage carrying it. See submit_fluid.

in vec2 v_uv;

layout(binding = 0) uniform sampler2D u_thickness;
layout(location = 0) uniform vec2 u_texel;   // 1 / target resolution
layout(location = 1) uniform float u_time;

out vec4 frag_color;

// Thickness at which the mucus is fully opaque and fully saturated. Set well
// ABOVE what the body of a fresh jet reaches, so the common case lands in the
// middle of the ramp and a slug still has visible internal depth variation.
// Tuned to it exactly and every burst would render as one flat dark shape.
const float kFullThickness = 9.0;

// Surface threshold and the width of the band across it. The band is what makes
// the edge anti-aliased and slightly soft — a hard step reads as cut-out vinyl.
// The threshold sits BELOW one isolated particle's peak so that lone droplets
// thrown off a splash still render, which is most of the spectacle.
const float kSurfaceIso = 0.30;
const float kSurfaceBand = 0.40;

// Mucin palette. Matches palette_for(TowerType::GobletCell) in vfx/Particles.cpp
// and the Goblet Cell's own body in entity.frag, so the cell, its spray, and
// the puddle it leaves are visibly one substance.
const vec3 kShallow = vec3(0.74, 1.00, 0.88);
const vec3 kMid     = vec3(0.34, 0.86, 0.66);
const vec3 kDeep    = vec3(0.06, 0.36, 0.31);
const vec3 kFoam    = vec3(0.94, 1.00, 0.97);

// Upper-left key light, matching the direction the entity pass throws its
// contact shadows so towers and their output agree about where the light is.
const vec3 kLightDir = vec3(-0.46, 0.55, 0.70);

float hash21(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
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

/// Low-passed thickness. Four BILINEAR taps placed on texel corners, so each
/// fetch is already the mean of a 2x2 block and the four together average a 4x4
/// neighbourhood for the price of four samples. See the header note on why the
/// blur is load-bearing.
float thickness_lp(vec2 uv) {
    vec2 o = u_texel;
    return 0.25 * (texture(u_thickness, uv + vec2( o.x,  o.y)).r +
                   texture(u_thickness, uv + vec2(-o.x,  o.y)).r +
                   texture(u_thickness, uv + vec2( o.x, -o.y)).r +
                   texture(u_thickness, uv + vec2(-o.x, -o.y)).r);
}

/// Bounded height from raw thickness. The sqrt is doing real work: thickness
/// through a ball of fluid grows roughly with the chord length, so its square
/// root is close to the actual surface profile. Using thickness directly gives
/// a conical, faceted-looking blob; the sqrt gives a rounded one.
float height_of(float thickness) {
    return sqrt(clamp(thickness / kFullThickness, 0.0, 1.0));
}

void main() {
    vec4 field = texture(u_thickness, v_uv);
    float thickness = field.r;

    float coverage = smoothstep(kSurfaceIso, kSurfaceIso + kSurfaceBand, thickness);
    // Cheap reject FIRST, before the sixteen taps below. Most of the screen has
    // no fluid on it at all and pays exactly one fetch.
    if (coverage <= 0.003) discard;

    // Mass-weighted averages. Guarded because the divide happens on texels that
    // may sit right at the edge of the field where thickness is near zero.
    float inv_mass = 1.0 / max(thickness, 1e-4);
    vec2 flow = field.gb * inv_mass;
    float foam = clamp(field.a * inv_mass, 0.0, 1.0);

    // ---- Normal from the blurred screen-space gradient --------------------
    // Central differences two texels out, over the low-passed field.
    vec2 g = u_texel * 2.0;
    float hl = height_of(thickness_lp(v_uv - vec2(g.x, 0.0)));
    float hr = height_of(thickness_lp(v_uv + vec2(g.x, 0.0)));
    float hd = height_of(thickness_lp(v_uv - vec2(0.0, g.y)));
    float hu = height_of(thickness_lp(v_uv + vec2(0.0, g.y)));

    // The 3.4 is the surface's apparent relief. Tuned by eye: lower and the
    // fluid flattens into a sticker, higher and every droplet turns into a
    // chrome bearing.
    vec3 nrm = normalize(vec3((hl - hr) * 3.4, (hd - hu) * 3.4, 1.0));

    float depth = clamp(thickness / kFullThickness, 0.0, 1.0);

    // ---- Body colour ------------------------------------------------------
    // Beer-Lambert-ish absorption over three stops rather than two: pale at the
    // film, a saturated mid-green through the body, dark only where the mucus
    // has really piled up. Two stops put the whole jet at one value.
    vec3 body = depth < 0.5 ? mix(kShallow, kMid, smoothstep(0.0, 0.5, depth))
                            : mix(kMid, kDeep, smoothstep(0.5, 1.0, depth));

    // Internal texture: mucus is not homogeneous, and a perfectly flat interior
    // is the fastest way to make a big puddle read as painted-on. The noise is
    // advected ALONG the flow, so a travelling jet shows streaks running down
    // its length while a settled puddle shows slow marbling. Kept subtle — this
    // is a tint, not a pattern.
    vec2 flow_dir = length(flow) > 1e-5 ? normalize(flow) : vec2(1.0, 0.0);
    vec2 streak_uv = v_uv / u_texel * 0.055 - flow_dir * u_time * 1.6;
    float marble = vnoise(streak_uv * vec2(1.0, 2.6));
    body *= mix(0.93, 1.07, marble);

    // ---- Foam -------------------------------------------------------------
    // Churned fluid goes white and breaks up. The speckle is fine and additive
    // only — it lightens froth, never darkens it, because a darkening speckle
    // punches holes that read as damage in the surface rather than as bubbles.
    // Gated hard on `foam` so a clean beam in flight never gets any.
    float bubbles = vnoise(v_uv / u_texel * 0.30 + u_time * 0.9);
    float froth = foam * smoothstep(0.45, 0.95, bubbles);
    body = mix(body, kFoam, clamp(foam * 0.22 + froth * 0.35, 0.0, 1.0));

    // ---- Lighting ---------------------------------------------------------
    vec3 L = normalize(kLightDir);
    // Half-vector against a straight-on view; the game is top-down and
    // orthographic, so the view direction really is constant.
    vec3 H = normalize(L + vec3(0.0, 0.0, 1.0));
    float ndh = clamp(dot(nrm, H), 0.0, 1.0);
    float ndl = clamp(dot(nrm, L), 0.0, 1.0);
    // One tight glint. Wide soft lobes were making the surface read as polished
    // chrome piping; a small hot highlight against a mostly-diffuse body is
    // what water actually does at this scale.
    float spec = pow(ndh, 90.0);

    // Fresnel off the surface tilt. Strongest exactly at the rim, where the
    // surface turns away from the viewer — the single most important cue that
    // this has volume rather than being a flat shape.
    float fresnel = pow(1.0 - clamp(nrm.z, 0.0, 1.0), 3.0);

    vec3 rgb = body * (0.62 + 0.46 * ndl);
    rgb += kShallow * fresnel * 0.55;

    // ---- Opacity ----------------------------------------------------------
    // Thin films let the lane show through; the body of a slug is nearly
    // opaque. Foam is opaque regardless, because froth is full of air.
    //
    // The FLOOR here is higher than a physically-minded reading would suggest,
    // and deliberately: this fluid usually lands on top of a dense, high-
    // contrast crowd of pathogens, and at low opacity the agents underneath
    // punch straight through the surface and destroy the read. It has to look
    // like something is covering them.
    float alpha = coverage * mix(0.66, 0.97, smoothstep(0.0, 0.55, depth));
    alpha = mix(alpha, coverage * 0.98, foam * 0.5);
    // Edges stay a little more transparent than the interior even after the
    // threshold, so the meniscus reads as a wet lip rather than as an outline.
    alpha *= mix(0.86, 1.0, coverage);

    // Premultiply, then add the glint ON TOP of the premultiplied colour. That
    // is the whole point of the premultiplied blend: the highlight is light
    // leaving the surface and does not have to fit inside its opacity.
    vec3 premul = rgb * alpha + vec3(1.0, 1.0, 0.96) * spec * coverage * 1.15;

    frag_color = vec4(premul, alpha);
}
