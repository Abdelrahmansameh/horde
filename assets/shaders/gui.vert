#version 450 core
// gui.vert — the UI framework's single vertex shader (src/gui/backend).
// Vertex layout mirrors gui::Vertex (src/gui/draw/DrawList.h): 24 bytes.

layout(location = 0) in vec2 a_pos;          // logical px
layout(location = 1) in vec2 a_uv;           // mode-dependent
layout(location = 2) in vec4 a_color;        // premultiplied RGBA8, normalized
layout(location = 3) in uint a_mode_record;  // mode << 28 | record

// Logical px -> NDC: ndc = pos * u_proj.xy + u_proj.zw (y flipped).
layout(location = 0) uniform vec4 u_proj;

out vec2 v_uv;
out vec4 v_color;
flat out uint v_mode;
flat out uint v_record;

void main() {
    gl_Position = vec4(a_pos * u_proj.xy + u_proj.zw, 0.0, 1.0);
    v_uv = a_uv;
    v_color = a_color;
    v_mode = a_mode_record >> 28u;
    v_record = a_mode_record & 0x0FFFFFFFu;
}
