#version 450 core
// Damage-field VFX pass — fragment stage. Owner: Wave 4G.
// Procedural SDF only, per the project's no-binary-assets rule.
//
// DESIGN.md §9.5: tower AoEs render as literal fluid/chemical fields (toxin
// clouds, histamine blooms, antibody tides, complement lightning) that must
// read as translucent atmosphere, never opaque cover — alpha is deliberately
// capped well below 1 everywhere in this shader.
//
// shape_id (see field.vert / render::FieldGpuInstance in Renderer.cpp):
//   0 = Circle/persistent glow (toxin cloud)   1 = Rect (antibody tide)
//   2 = Cone (directional spray)                3 = Chain (complement crackle)
//
// v_intensity is the CPU-computed persistent-vs-burst driver (see
// Renderer::submit_fields): persistent fields hold a steady mid alpha with a
// slow shader-side breathing pulse; burst fields (lifetime > 0) start bright
// and fade as their remaining lifetime runs out, selling the "nova" flash
// DESIGN.md asks for.

in vec2  v_local;
in vec4  v_tint;
flat in uint  v_shape_id;
flat in float v_arc_cos;
flat in float v_falloff;
flat in float v_intensity;

out vec4 o_color;

layout(location = 1) uniform float u_time;

// DamageField::falloff semantics mirrored from sim/damage/DamageField.cpp's
// falloff_multiplier: 0 = flat, 1 = linear, 2 = quadratic fade to the edge.
float falloff_glow(float t, float falloff_exp) {
    float x = clamp(1.0 - t, 0.0, 1.0);
    return falloff_exp <= 0.0 ? 1.0 : pow(x, max(falloff_exp, 0.1));
}

void main() {
    vec3 rgb = v_tint.rgb;
    float alpha;

    if (v_shape_id == 1u) {
        // Rect: box SDF in the padded local frame, true edge at |v_local|=0.5.
        vec2 d = abs(v_local) - vec2(0.5);
        float sdf = length(max(d, 0.0)) + min(max(d.x, d.y), 0.0);
        float edge = 1.0 - smoothstep(0.0, 0.16, sdf);
        // Chebyshev distance from centre, same normalization DamageField.cpp's
        // test_rect uses for its own falloff `t` — the glow's interior gradient
        // matches the damage gradient the player is actually taking.
        float t = clamp(max(abs(v_local.x), abs(v_local.y)) / 0.5, 0.0, 1.0);
        alpha = edge * falloff_glow(t, v_falloff);
    } else {
        float dist = length(v_local);
        float t = clamp(dist / 0.5, 0.0, 1.0);
        float edge = 1.0 - smoothstep(0.46, 0.58, dist);
        alpha = edge * falloff_glow(t, v_falloff);

        if (v_shape_id == 2u) {
            // Cone: angular clip around local +x (rotation already applied at
            // the vertex stage, so the cone always opens along local +x here).
            float cos_theta = dist > 0.0001 ? v_local.x / dist : 1.0;
            alpha *= smoothstep(v_arc_cos - 0.08, v_arc_cos + 0.03, cos_theta);
        } else if (v_shape_id == 3u) {
            // Chain / Complement Cascade: crackling concentric rings sweeping
            // outward over time, for the "lightning" read DESIGN.md §7 asks
            // for, distinct from a Circle field's steady soft glow.
            float ring = abs(sin(dist * 20.0 - u_time * 7.0));
            float crackle = 1.0 - smoothstep(0.0, 0.35, ring);
            alpha *= mix(0.5, 1.0, crackle);
            rgb = mix(rgb, vec3(1.0), 0.35 * crackle);
        }
    }

    alpha *= v_intensity;
    if (alpha <= 0.003) discard;
    o_color = vec4(rgb, clamp(alpha, 0.0, 0.85));
}
