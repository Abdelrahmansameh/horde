#version 450 core
// Adapt the GUI vertex stream to the ACTUAL gameplay chaff.frag interface.
// No virus silhouette or material is duplicated here.
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_color;
layout(location = 3) in uint a_mode_record;
layout(location = 0) uniform vec4 u_proj;

struct Material {
    vec4 rect; vec4 geom; vec4 wobble; vec4 fx; vec4 shadow;
    vec4 dash; vec4 extra; uvec4 colors0; uvec4 colors1;
};
layout(std430, binding = 0) readonly buffer Materials { Material materials[]; };

out vec2 v_local;
out vec4 v_tint;
flat out uint v_flags;
out float v_anim_phase;
out float v_wobble;
flat out vec2 v_shadow_offset;

void main() {
    Material m = materials[a_mode_record & 0x0FFFFFFFu];
    gl_Position = vec4(a_pos * u_proj.xy + u_proj.zw, 0.0, 1.0);
    v_local = a_uv;
    // GUI colours are premultiplied; chaff.frag uses straight-alpha tint.
    v_tint = vec4(a_color.rgb / max(a_color.a, 0.0001), a_color.a);
    v_flags = 0u; // ordinary, whole virus; no combat status or density LOD
    v_anim_phase = m.geom.x;
    v_wobble = m.geom.z;
    float c = cos(m.geom.y), s = sin(m.geom.y);
    vec2 delta = vec2(0.10, -0.24); // chaff.vert's directional shadow/body lift
    v_shadow_offset = vec2(delta.x * c + delta.y * s, -delta.x * s + delta.y * c);
}
