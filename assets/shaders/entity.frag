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
    } else if (v_shape_id == 2u) {
        d = sdf_diamond(v_local, 0.45 + pulse);
    } else {
        d = sdf_circle(v_local, 0.5 + pulse);
    }

    float alpha = (1.0 - smoothstep(-0.06, 0.0, d)) * alpha_mul;
    if (alpha <= 0.0) discard;
    o_color = vec4(v_tint.rgb, alpha * v_tint.a);
}
