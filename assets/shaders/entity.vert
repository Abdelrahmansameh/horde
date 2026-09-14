#version 450 core
// Named-agent / tower instanced pass — vertex stage.
//
// CONTRACT: mirrors render::EntityInstance (src/render/Renderer.h) byte for
// byte, 32 bytes/instance — same shape as ChaffInstance but flags -> shape_id.

layout(location = 0) in vec2 a_corner; // shared unit quad, [-0.5, 0.5]

layout(location = 1) in vec2  i_position;   // offset  0
layout(location = 2) in float i_scale;      // offset  8
layout(location = 3) in float i_rotation;   // offset 12
layout(location = 4) in vec4  i_tint;       // offset 16, normalized
layout(location = 5) in uint  i_shape_id;   // offset 20
layout(location = 6) in float i_anim_phase; // offset 24
layout(location = 7) in float i_shape_param; // offset 28

layout(location = 0) uniform mat4 u_view_projection;

out vec2  v_local;
out vec4  v_tint;
flat out uint v_shape_id;
out float v_anim_phase;
/// Per-shape extra parameter, meaning defined by v_shape_id. Every tower body
/// reads it as the tier; the clot bar as its aspect. Zero for everything else.
out float v_shape_param;
/// The instance's world rotation. v_local is the UNROTATED corner, so a shape
/// drawn from it turns with the quad; counter-rotating by this angle puts a
/// feature back into world alignment so it stays put while the sprite spins.
/// Nothing reads it since the NK Cell's rotor was retired; kept for the next
/// shape that wants a world-aligned feature.
out float v_rotation;

// Same height-sell trick as the chaff pass, no shadow (named agents are few
// and already telegraphed; a shadow would add cost without much payoff).
const float kHeightFrac = 0.12;
const float kPad = 1.3;

void main() {
    float s = sin(i_rotation);
    float c = cos(i_rotation);
    vec2 corner = a_corner * kPad;
    vec2 local = vec2(corner.x * c - corner.y * s,
                      corner.x * s + corner.y * c) * i_scale;
    vec2 center_world = i_position + vec2(0.0, kHeightFrac * i_scale);

    v_local = corner;
    v_tint = i_tint;
    v_shape_id = i_shape_id;
    v_anim_phase = i_anim_phase;
    v_shape_param = i_shape_param;
    v_rotation = i_rotation;

    gl_Position = u_view_projection * vec4(center_world + local, 0.0, 1.0);
}
