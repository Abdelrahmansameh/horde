#version 450 core
// gui.frag — every UI pixel: SDF shapes, tessellated curves, SDF text, icons
// and layer composites, selected per vertex by v_mode (gui::VertexMode).
//
// Output is PREMULTIPLIED (blend ONE, ONE_MINUS_SRC_ALPHA).
//
// The shape outline functions (sd_box, sd_ellipse, phase, membrane_offset,
// base_distance) are mirrored in C++ by src/gui/draw/ShapeSdf.h for hit
// testing. Change them together; test_gui_draw.cpp checks they agree.

struct Shape {
    vec4 rect;     // cx, cy, hw, hh
    vec4 geom;     // radius, stroke_width, band, rotation
    vec4 wobble;   // amp, seed, lobes, speed
    vec4 fx;       // bulge, rim_width, shadow_blur, decor_count
    vec4 shadow;   // offset x, offset y, spread, kind
    vec4 dash;     // length, gap, offset, perimeter
    vec4 extra;    // per kind (see gui::ShapeRecord)
    uvec4 colors0; // fill, fill2, stroke, shadow
    uvec4 colors1; // rim, decor, liquid, bubble
};

layout(std430, binding = 0) readonly buffer Shapes { Shape shapes[]; };

layout(binding = 0) uniform sampler2D u_font;   // R8 SDF glyph atlas
layout(binding = 1) uniform sampler2D u_icons;  // premultiplied RGBA icon atlas
layout(binding = 2) uniform sampler2D u_layer;  // offscreen layer being composited

layout(location = 1) uniform float u_time;
layout(location = 2) uniform float u_device_px;    // logical px per framebuffer px
layout(location = 3) uniform int u_stencil_write;  // 1 while writing a clip shape
layout(location = 4) uniform vec2 u_font_atlas_size;

in vec2 v_uv;
in vec4 v_color;
flat in uint v_mode;
flat in uint v_record;

out vec4 frag;

const float TAU = 6.28318530718;
const uint MODE_SOLID = 0u, MODE_SHAPE = 1u, MODE_TEXT = 2u, MODE_IMAGE = 3u, MODE_LAYER = 4u;
const uint KIND_BOX = 0u, KIND_ELLIPSE = 1u, KIND_ARC = 2u, KIND_FLUID = 3u, KIND_RADIAL = 4u;
// Must match FontLibrary::kSpread.
const float FONT_SPREAD = 12.0;

vec4 over(vec4 top, vec4 under) { return top + under * (1.0 - top.a); }
vec4 col(uint c) { return unpackUnorm4x8(c); }

// Coverage of a signed distance (logical px, negative inside), one
// framebuffer pixel of anti-aliasing.
float cov(float d) { return clamp(0.5 - d / u_device_px, 0.0, 1.0); }

// ---- Outline (mirrored in ShapeSdf.h) ------------------------------------------

float sd_box(vec2 p, vec2 b, float r) {
    r = clamp(r, 0.0, min(b.x, b.y));
    vec2 q = abs(p) - b + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

float sd_ellipse(vec2 p, vec2 ab) {
    ab = max(ab, vec2(1e-3));
    float k0 = length(p / ab);
    float k1 = length(p / (ab * ab));
    if (k1 < 1e-6) return -min(ab.x, ab.y);
    return k0 * (k0 - 1.0) / k1;
}

float phase(float seed, float k) {
    float v = seed * 0.6180339887 + k * 0.4142135624 + seed * k * 0.1319;
    return fract(v) * TAU;
}

float membrane_offset(Shape s, vec2 p) {
    float hw = s.rect.z, hh = s.rect.w;
    float off = 0.0;
    if (s.fx.x != 0.0) {
        float nx = clamp(p.x / max(hw, 1e-3), -1.0, 1.0);
        float ny = clamp(p.y / max(hh, 1e-3), -1.0, 1.0);
        off += s.fx.x * ((1.0 - nx * nx) * ny * ny + (1.0 - ny * ny) * nx * nx);
    }
    if (s.wobble.x != 0.0) {
        float th = atan(p.y / max(hh, 1e-3), p.x / max(hw, 1e-3));
        float L = s.wobble.z;
        float seed = s.wobble.y;
        float t = u_time * s.wobble.w;
        off += s.wobble.x * (0.68 * sin(L * th + phase(seed, 0.0) + t) +
                             0.26 * sin((2.0 * L + 1.0) * th + phase(seed, 1.0) - 1.37 * t) +
                             0.06 * sin((3.0 * L - 1.0) * th + phase(seed, 2.0) + 1.93 * t));
    }
    return off;
}

float base_distance(Shape s, uint kind, vec2 p) {
    if (kind == KIND_ELLIPSE) return sd_ellipse(p, s.rect.zw) - membrane_offset(s, p);
    return sd_box(p, s.rect.zw, s.geom.x) - membrane_offset(s, p);
}

// ---- Shape shading --------------------------------------------------------------

float hash1(float n) { return fract(sin(n * 12.9898) * 43758.5453); }

// Organelle dots: a few faint ellipses just inside the cytoplasm's edge, as
// the canvas scatters them (Main.dc.html's organ panel).
float decor_dots(Shape s, vec2 p) {
    int n = int(s.fx.w);
    float band = s.geom.z;
    float r = max(band * 0.8, 3.0);
    vec2 inner = max(s.rect.zw - band - r * 1.3, vec2(1.0));
    float a = 0.0;
    for (int i = 0; i < 12; ++i) {
        if (i >= n) break;
        float fi = float(i);
        float th = TAU * (fi + 0.5 + 0.35 * sin(s.wobble.y * 3.1 + fi * 1.7)) / float(n);
        vec2 v = vec2(cos(th), sin(th));
        vec2 q = v / max(abs(v.x) / inner.x, abs(v.y) / inner.y);
        float k = 0.8 + 0.25 * hash1(fi + s.wobble.y);
        a = max(a, cov(sd_ellipse(p - q, vec2(r, r * 0.72) * k)));
    }
    return a;
}

// Distance along a round-capped dash pattern: `along` is the position along
// the stroke, `across` the distance from its centreline.
float dash_distance(vec4 dash, float along, float across, float half_width) {
    float period = dash.x + dash.y;
    float x = mod(along + dash.z, period);  // dash.z = SVG stroke-dashoffset
    float dx = x <= dash.x ? 0.0 : min(x - dash.x, period - x);
    return dx > 0.0 ? length(vec2(dx, across)) - half_width : across - half_width;
}

vec4 shade_arc(Shape s, vec2 p) {
    float R = s.rect.z;
    float th = s.geom.y;
    float len = length(p);
    float across = abs(len - R);
    vec4 c = vec4(0.0);
    vec4 track = col(s.colors0.x);
    if (track.a > 0.0) c = track * cov(across - th * 0.5);

    float start = s.extra.x, sweep = s.extra.y;
    float a = mod(atan(p.y, p.x) - start, TAU);
    float d;
    if (sweep >= TAU - 1e-3 || a <= sweep) {
        d = across - th * 0.5;
        if (s.dash.x > 0.0) d = dash_distance(s.dash, a * R, across, th * 0.5);
    } else {
        vec2 c0 = R * vec2(cos(start), sin(start));
        vec2 c1 = R * vec2(cos(start + sweep), sin(start + sweep));
        d = min(length(p - c0), length(p - c1)) - th * 0.5;
    }
    return over(col(s.colors0.z) * cov(d), c);
}

vec4 shade_fluid_body(Shape s, vec2 p) {
    vec4 body = col(s.colors0.x);
    float level = s.extra.x;
    if (level <= 0.001) return body;
    float inset = s.extra.w;
    uint axis = uint(s.extra.y);
    float wave = s.extra.z;
    vec2 half_in = max(s.rect.zw - inset, vec2(0.5));
    float din = sd_box(p, half_in, max(s.geom.x - inset, 0.0));
    float front;
    vec2 q;  // (along the fill axis, across it)
    if (axis == 0u) {
        float x_level = -half_in.x + level * 2.0 * half_in.x;
        float w = level < 0.999 ? wave * sin(p.y * 0.35 + u_time * 3.1) : 0.0;
        front = p.x - (x_level + w);
        q = p;
    } else {
        float y_level = half_in.y - level * 2.0 * half_in.y;
        float w = level < 0.999 ? wave * (sin(p.x * 0.12 + u_time * 2.3) + 0.5 * sin(p.x * 0.31 - u_time * 1.7)) : 0.0;
        front = (y_level + w) - p.y;
        q = vec2(-p.y, p.x);
    }
    vec4 liquid = col(s.colors1.z);
    vec4 bubble = col(s.colors1.w);
    if (bubble.a > 0.0) {
        float drift = u_time * 5.0;
        float cell = 13.0;
        float ci = floor((q.x + drift) / cell);
        float across = axis == 0u ? half_in.y : half_in.x;
        float b = 0.0;
        for (int k = -1; k <= 1; ++k) {
            float c = ci + float(k);
            float h = hash1(c);
            vec2 centre = vec2(c * cell + cell * 0.5 - drift, (h - 0.5) * across * 0.8);
            float r = 0.7 + 0.5 * h;
            b = max(b, cov(sd_ellipse(q - centre, vec2(4.5, 3.6) * r * min(1.0, across / 6.0))));
        }
        liquid = over(bubble * b, liquid);
    }
    return over(liquid * cov(max(din, front)), body);
}

vec4 shade_shape(Shape s, vec2 p) {
    uint kind = uint(s.shadow.w);
    if (kind == KIND_RADIAL) {
        float r = length(p / max(s.rect.zw, vec2(1e-3)));
        return mix(col(s.colors0.x), col(s.colors0.y), smoothstep(s.extra.x, 1.0, r));
    }
    if (kind == KIND_ARC) return shade_arc(s, p);

    float d = base_distance(s, kind, p);
    vec4 c = vec4(0.0);

    vec4 shadow = col(s.colors0.w);
    if (shadow.a > 0.0) {
        float ds = base_distance(s, kind, p - s.shadow.xy) - s.shadow.z;
        float blur = s.fx.z;
        float a = blur > u_device_px ? 1.0 - smoothstep(-blur, blur, ds) : cov(ds);
        c = shadow * a;
    }

    vec4 body;
    float stroke_w = s.geom.y;
    float rim_w = s.fx.y;
    vec4 rim = col(s.colors1.x);
    if (kind == KIND_FLUID) {
        body = shade_fluid_body(s, p);
        if (rim.a > 0.0 && rim_w > 0.0) {
            float dr = abs(d + stroke_w * 0.5 + rim_w * 0.5) - rim_w * 0.5;
            body = over(rim * cov(dr), body);
        }
    } else {
        vec4 fill = col(s.colors0.x);
        vec4 fill2 = col(s.colors0.y);
        body = fill;
        float band = s.geom.z;
        if (band > 0.0) {
            float din = d + band;
            float inner = cov(din);
            body = fill2 * inner + body * (1.0 - inner);
            if (s.fx.w > 0.0) body = over(col(s.colors1.y) * decor_dots(s, p) * inner, body);
            if (rim.a > 0.0 && rim_w > 0.0) body = over(rim * cov(abs(din) - rim_w * 0.5), body);
        } else {
            if (s.extra.x > 0.5) {
                float t = clamp((p.y + s.rect.w) / max(2.0 * s.rect.w, 1e-3), 0.0, 1.0);
                body = mix(fill, fill2, t);
            }
            if (rim.a > 0.0 && rim_w > 0.0) {
                float dr = abs(d + stroke_w * 0.5 + rim_w * 0.5) - rim_w * 0.5;
                body = over(rim * cov(dr), body);
            }
        }
    }
    c = over(body * cov(d), c);

    vec4 stroke = col(s.colors0.z);
    if (stroke.a > 0.0 && stroke_w > 0.0) {
        float ds = abs(d) - stroke_w * 0.5;
        if (s.dash.x > 0.0) {
            // Arc-length along the outline, approximated from the normalized
            // angle; exact on circles, close enough on boxes.
            float th = atan(p.y / max(s.rect.w, 1e-3), p.x / max(s.rect.z, 1e-3));
            float along = (th + 3.14159265) / TAU * s.dash.w;
            ds = dash_distance(s.dash, along, abs(d), stroke_w * 0.5);
        }
        c = over(stroke * cov(ds), c);
    }
    return c;
}

// ---- Text -----------------------------------------------------------------------

vec4 shade_text() {
    Shape st = shapes[v_record];
    float v = texture(u_font, v_uv).r;
    float dist_atlas = (v * 255.0 - 128.0) / (128.0 / FONT_SPREAD);  // atlas px, + inside
    vec2 g = fwidth(v_uv) * u_font_atlas_size;
    float atlas_per_px = max(0.5 * (g.x + g.y), 1e-4);
    float d = dist_atlas / atlas_per_px;  // framebuffer px, + inside
    float soft = st.geom.y / u_device_px;
    float w = 0.5 + soft;
    vec4 c = v_color * smoothstep(-w, w, d);
    float ow = st.geom.x / u_device_px;
    if (ow > 0.0) {
        vec4 oc = col(st.colors0.z);
        c = over(c, oc * smoothstep(-w, w, d + ow));
    }
    return c;
}

void main() {
    if (u_stencil_write != 0) {
        // Clip-shape pass: colour writes are off; only the stencil matters.
        if (v_mode == MODE_SHAPE) {
            Shape s = shapes[v_record];
            if (base_distance(s, uint(s.shadow.w), v_uv) > 0.0) discard;
        }
        frag = vec4(1.0);
        return;
    }
    if (v_mode == MODE_SHAPE) {
        frag = shade_shape(shapes[v_record], v_uv);
    } else if (v_mode == MODE_TEXT) {
        frag = shade_text();
    } else if (v_mode == MODE_IMAGE) {
        frag = texture(u_icons, v_uv) * v_color;
    } else if (v_mode == MODE_LAYER) {
        frag = texture(u_layer, v_uv) * v_color;
    } else {
        frag = v_color;
    }
    if (frag.a <= 0.0) discard;
}
