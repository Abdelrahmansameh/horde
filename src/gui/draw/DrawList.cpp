#include "gui/draw/DrawList.h"

#include "core/Math.h"

#include <cassert>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>

namespace immune::gui {

namespace {

Vec2 perp(Vec2 d) { return Vec2{-d.y, d.x}; }

Rect bounds_of(const Affine2& t, Rect r) {
    const Vec2 c[4] = {t.apply(r.min), t.apply(Vec2{r.max.x, r.min.y}), t.apply(r.max),
                       t.apply(Vec2{r.min.x, r.max.y})};
    Rect b{c[0], c[0]};
    for (const Vec2& p : c) {
        b.min.x = math::min(b.min.x, p.x);
        b.min.y = math::min(b.min.y, p.y);
        b.max.x = math::max(b.max.x, p.x);
        b.max.y = math::max(b.max.y, p.y);
    }
    return b;
}

Rect intersect(Rect a, Rect b) {
    Rect r{Vec2{math::max(a.min.x, b.min.x), math::max(a.min.y, b.min.y)},
           Vec2{math::min(a.max.x, b.max.x), math::min(a.max.y, b.max.y)}};
    if (r.max.x < r.min.x) r.max.x = r.min.x;
    if (r.max.y < r.min.y) r.max.y = r.min.y;
    return r;
}

bool same_rect(Rect a, Rect b) {
    return a.min.x == b.min.x && a.min.y == b.min.y && a.max.x == b.max.x && a.max.y == b.max.y;
}

} // namespace

void DrawList::reset(Vec2 viewport, f32 device_px, f32 time) {
    viewport_ = viewport;
    device_px_ = device_px > 0.0f ? device_px : 1.0f;
    time_ = time;
    vertices_.clear();
    indices_.clear();
    records_.clear();
    cmds_.clear();
    transforms_.assign(1, Affine2{});
    alphas_.assign(1, 1.0f);
    clips_.assign(1, Rect{Vec2{0.0f, 0.0f}, viewport});
    stencil_stack_.clear();
    stencil_depth_stack_.assign(1, 0);
    layer_opacity_.clear();
    max_layer_depth_ = 0;
}

// ---- State stacks ------------------------------------------------------------

void DrawList::push_transform(const Affine2& t) { transforms_.push_back(transforms_.back() * t); }

void DrawList::pop_transform() {
    assert(transforms_.size() > 1 && "unbalanced pop_transform");
    if (transforms_.size() > 1) transforms_.pop_back();
}

void DrawList::push_alpha(f32 alpha) { alphas_.push_back(alphas_.back() * math::saturate(alpha)); }

void DrawList::pop_alpha() {
    assert(alphas_.size() > 1 && "unbalanced pop_alpha");
    if (alphas_.size() > 1) alphas_.pop_back();
}

void DrawList::push_clip_rect(Rect r) {
    clips_.push_back(intersect(clips_.back(), bounds_of(transform(), r)));
}

void DrawList::pop_clip_rect() {
    assert(clips_.size() > 1 && "unbalanced pop_clip_rect");
    if (clips_.size() > 1) clips_.pop_back();
}

void DrawList::push_clip_shape(const ShapeDesc& shape) {
    const u32 depth = stencil_depth_stack_.back();
    DrawCmd cmd;
    cmd.kind = CmdKind::StencilPush;
    cmd.first_index = static_cast<u32>(indices_.size());
    cmd.clip = clips_.back();
    cmd.stencil_depth = depth;
    cmd.layer_depth = static_cast<u32>(layer_opacity_.size());
    cmds_.push_back(cmd);
    const u32 before = static_cast<u32>(indices_.size());
    // Recorded through shape() so it is transformed like any other shape;
    // shape() appends to the cmd we just pushed since it is the back().
    ShapeDesc solid = shape;
    solid.shadow = kTransparent;
    solid.fill = kWhite;
    const ShapeRecord r = [&] {
        const Affine2& t = transform();
        const f32 s = t.uniform_scale();
        ShapeDesc d = solid;
        d.center = t.apply(d.center);
        d.half_size *= s;
        d.radius *= s;
        d.bulge *= s;
        d.wobble_amp *= s;
        d.wobble_wavelength *= s;
        d.rotation += t.rotation();
        return make_record(d);
    }();
    const u32 idx = push_record(r);
    shape_quad(r, idx, 2.0f + std::fabs(r.wobble[0]) + math::max(r.fx[0], 0.0f));
    cmds_.back().index_count = static_cast<u32>(indices_.size()) - before;
    stencil_stack_.push_back(StencilEntry{cmds_.back().first_index, cmds_.back().index_count});
    stencil_depth_stack_.back() = depth + 1;
}

void DrawList::pop_clip_shape() {
    assert(!stencil_stack_.empty() && "unbalanced pop_clip_shape");
    if (stencil_stack_.empty()) return;
    const StencilEntry e = stencil_stack_.back();
    stencil_stack_.pop_back();
    const u32 depth = stencil_depth_stack_.back();
    DrawCmd cmd;
    cmd.kind = CmdKind::StencilPop;
    cmd.first_index = e.first_index;
    cmd.index_count = e.index_count;
    cmd.clip = clips_.back();
    cmd.stencil_depth = depth;
    cmd.layer_depth = static_cast<u32>(layer_opacity_.size());
    cmds_.push_back(cmd);
    stencil_depth_stack_.back() = depth > 0 ? depth - 1 : 0;
}

void DrawList::push_layer(f32 opacity) {
    layer_opacity_.push_back(math::saturate(opacity));
    const u32 depth = static_cast<u32>(layer_opacity_.size());
    max_layer_depth_ = math::max(max_layer_depth_, depth);
    DrawCmd cmd;
    cmd.kind = CmdKind::LayerBegin;
    cmd.first_index = static_cast<u32>(indices_.size());
    cmd.clip = Rect{Vec2{0.0f, 0.0f}, viewport_};
    cmd.layer_depth = depth;
    cmds_.push_back(cmd);
    stencil_depth_stack_.push_back(0);
}

void DrawList::pop_layer() {
    assert(!layer_opacity_.empty() && "unbalanced pop_layer");
    if (layer_opacity_.empty()) return;
    const u32 depth = static_cast<u32>(layer_opacity_.size());
    const f32 opacity = layer_opacity_.back();
    layer_opacity_.pop_back();
    stencil_depth_stack_.pop_back();

    DrawCmd cmd;
    cmd.kind = CmdKind::LayerEnd;
    cmd.first_index = static_cast<u32>(indices_.size());
    cmd.clip = clips_.back();
    cmd.stencil_depth = stencil_depth_stack_.back();
    cmd.layer_depth = depth;  // the layer being composited; drawn into depth - 1
    cmds_.push_back(cmd);
    // Full-viewport composite. GL textures are bottom-up, hence the v flip.
    const Vec2 v = viewport_;
    quad(Vec2{0.0f, 0.0f}, Vec2{v.x, 0.0f}, v, Vec2{0.0f, v.y}, Vec2{0.0f, 1.0f}, Vec2{1.0f, 1.0f},
         Vec2{1.0f, 0.0f}, Vec2{0.0f, 0.0f}, pack_premul(Color{1.0f, 1.0f, 1.0f, opacity}),
         pack_mode(VertexMode::Layer, 0));
    cmds_.back().index_count = static_cast<u32>(indices_.size()) - cmd.first_index;
}

void DrawList::ensure_draw_cmd() {
    const u32 depth = stencil_depth_stack_.back();
    const u32 layer = static_cast<u32>(layer_opacity_.size());
    if (!cmds_.empty()) {
        const DrawCmd& b = cmds_.back();
        if (b.kind == CmdKind::Draw && same_rect(b.clip, clips_.back()) && b.stencil_depth == depth &&
            b.layer_depth == layer) {
            return;
        }
    }
    DrawCmd cmd;
    cmd.kind = CmdKind::Draw;
    cmd.first_index = static_cast<u32>(indices_.size());
    cmd.clip = clips_.back();
    cmd.stencil_depth = depth;
    cmd.layer_depth = layer;
    cmds_.push_back(cmd);
}

void DrawList::quad(Vec2 p0, Vec2 p1, Vec2 p2, Vec2 p3, Vec2 uv0, Vec2 uv1, Vec2 uv2, Vec2 uv3,
                    u32 color, u32 mode_record) {
    const u32 base = static_cast<u32>(vertices_.size());
    vertices_.push_back(Vertex{p0, uv0, color, mode_record});
    vertices_.push_back(Vertex{p1, uv1, color, mode_record});
    vertices_.push_back(Vertex{p2, uv2, color, mode_record});
    vertices_.push_back(Vertex{p3, uv3, color, mode_record});
    const u32 idx[6] = {base, base + 1, base + 2, base, base + 2, base + 3};
    indices_.insert(indices_.end(), idx, idx + 6);
}

u32 DrawList::push_record(const ShapeRecord& r) {
    records_.push_back(r);
    return static_cast<u32>(records_.size() - 1);
}

void DrawList::shape_quad(const ShapeRecord& r, u32 record_index, f32 margin) {
    const Vec2 c{r.rect[0], r.rect[1]};
    const Vec2 e{r.rect[2] + margin, r.rect[3] + margin};
    const f32 cs = std::cos(r.geom[3]), sn = std::sin(r.geom[3]);
    auto corner = [&](f32 x, f32 y) { return c + Vec2{cs * x - sn * y, sn * x + cs * y}; };
    quad(corner(-e.x, -e.y), corner(e.x, -e.y), corner(e.x, e.y), corner(-e.x, e.y),
         Vec2{-e.x, -e.y}, Vec2{e.x, -e.y}, Vec2{e.x, e.y}, Vec2{-e.x, e.y}, 0xFFFFFFFFu,
         pack_mode(VertexMode::Shape, record_index));
}

// ---- Primitives ---------------------------------------------------------------

u32 DrawList::shape(const ShapeDesc& in) {
    const Affine2& t = transform();
    const f32 s = t.uniform_scale();
    ShapeDesc d = in;
    if (d.kind == ShapeKind::Arc) d.half_size.y = d.half_size.x;
    d.center = t.apply(d.center);
    d.half_size *= s;
    d.radius *= s;
    d.rotation += t.rotation();
    d.band *= s;
    d.stroke_width *= s;
    d.rim_width *= s;
    d.shadow_offset = t.apply_vector(d.shadow_offset);
    d.shadow_blur *= s;
    d.shadow_spread *= s;
    d.bulge *= s;
    d.wobble_amp *= s;
    d.wobble_wavelength *= s;
    d.dash_length *= s;
    d.dash_gap *= s;
    d.dash_offset *= s;
    d.liquid_inset *= s;
    d.wave_amp *= s;
    const f32 a = alphas_.back();
    if (a < 1.0f) {
        for (Color* c : {&d.fill, &d.fill2, &d.stroke, &d.rim, &d.shadow, &d.decor, &d.liquid, &d.bubble}) {
            c->a *= a;
        }
    }
    const ShapeRecord r = make_record(d);
    ensure_draw_cmd();
    const u32 idx = push_record(r);
    const u32 first = static_cast<u32>(indices_.size());
    shape_quad(r, idx, shape_paint_margin(d));
    (void)first;
    cmds_.back().index_count = static_cast<u32>(indices_.size()) - cmds_.back().first_index;
    return idx;
}

u32 DrawList::text_style(const TextStyleRecord& s) {
    ShapeRecord r{};
    const f32 k = transform().uniform_scale();
    r.geom[0] = s.outline_width * k;
    r.geom[1] = s.softness * k;
    r.colors0[2] = pack_premul(with_alpha(s.outline, s.outline.a * alphas_.back()));
    return push_record(r);
}

void DrawList::glyph(Rect dst, Vec2 uv0, Vec2 uv1, Color color, u32 style_record) {
    ensure_draw_cmd();
    const Affine2& t = transform();
    quad(t.apply(dst.min), t.apply(Vec2{dst.max.x, dst.min.y}), t.apply(dst.max),
         t.apply(Vec2{dst.min.x, dst.max.y}), uv0, Vec2{uv1.x, uv0.y}, uv1, Vec2{uv0.x, uv1.y},
         pack_premul(with_alpha(color, color.a * alphas_.back())), pack_mode(VertexMode::Text, style_record));
    cmds_.back().index_count = static_cast<u32>(indices_.size()) - cmds_.back().first_index;
}

void DrawList::image(Rect dst, Vec2 uv0, Vec2 uv1, Color tint) {
    ensure_draw_cmd();
    const Affine2& t = transform();
    quad(t.apply(dst.min), t.apply(Vec2{dst.max.x, dst.min.y}), t.apply(dst.max),
         t.apply(Vec2{dst.min.x, dst.max.y}), uv0, Vec2{uv1.x, uv0.y}, uv1, Vec2{uv0.x, uv1.y},
         pack_premul(with_alpha(tint, tint.a * alphas_.back())), pack_mode(VertexMode::Image, 0));
    cmds_.back().index_count = static_cast<u32>(indices_.size()) - cmds_.back().first_index;
}

void DrawList::round_fan(Vec2 c, f32 radius, f32 a0, f32 a1, u32 core, u32 edge) {
    const f32 fr = device_px_;
    const f32 rc = math::max(radius - fr * 0.5f, 0.0f);
    const f32 ro = radius + fr * 0.5f;
    const f32 span = std::fabs(a1 - a0);
    const i32 segs = math::clamp(static_cast<i32>(std::ceil(span * ro / (fr * 3.0f))), 4, 48);
    const u32 center = static_cast<u32>(vertices_.size());
    const u32 mode = pack_mode(VertexMode::Solid, 0);
    vertices_.push_back(Vertex{c, Vec2{}, core, mode});
    for (i32 i = 0; i <= segs; ++i) {
        const f32 a = a0 + (a1 - a0) * static_cast<f32>(i) / static_cast<f32>(segs);
        const Vec2 dir{std::cos(a), std::sin(a)};
        vertices_.push_back(Vertex{c + dir * rc, Vec2{}, core, mode});
        vertices_.push_back(Vertex{c + dir * ro, Vec2{}, edge, mode});
    }
    for (i32 i = 0; i < segs; ++i) {
        const u32 ic0 = center + 1 + static_cast<u32>(i) * 2, io0 = ic0 + 1;
        const u32 ic1 = ic0 + 2, io1 = ic1 + 1;
        const u32 tri[9] = {center, ic0, ic1, ic0, io0, io1, ic0, io1, ic1};
        indices_.insert(indices_.end(), tri, tri + 9);
    }
}

void DrawList::stroke_single(std::span<const Vec2> in, bool closed, f32 width, u32 color, LineCap cap,
                             LineJoin join, f32 miter_limit) {
    // Drop coincident points: they have no direction.
    scratch_.clear();
    for (const Vec2& p : in) {
        if (scratch_.empty() || math::length_sq(p - scratch_.back()) > 1e-8f) scratch_.push_back(p);
    }
    if (closed && scratch_.size() > 2 && math::length_sq(scratch_.front() - scratch_.back()) <= 1e-8f) {
        scratch_.pop_back();
    }
    const usize n = scratch_.size();
    const f32 fr = device_px_;
    const f32 hw = width * 0.5f;
    // Hairlines thinner than a pixel keep a one-pixel footprint and fade.
    const f32 alpha = math::min(1.0f, width / fr);
    const f32 core_half = math::max(hw - fr * 0.5f, 0.0f);
    const f32 outer_half = math::max(hw, fr * 0.5f) + fr * 0.5f;
    Color base = Color{((color >> 0) & 0xFF) / 255.0f, ((color >> 8) & 0xFF) / 255.0f,
                       ((color >> 16) & 0xFF) / 255.0f, ((color >> 24) & 0xFF) / 255.0f};
    // `color` is already premultiplied; scale all four channels for coverage.
    auto scaled = [&](f32 k) {
        auto q = [](f32 v) { return static_cast<u32>(math::saturate(v) * 255.0f + 0.5f); };
        return q(base.r * k) | (q(base.g * k) << 8) | (q(base.b * k) << 16) | (q(base.a * k) << 24);
    };
    const u32 core = scaled(alpha);
    const u32 edge = 0;
    const u32 mode = pack_mode(VertexMode::Solid, 0);

    if (n == 0) return;
    if (n == 1) {
        if (cap == LineCap::Round) round_fan(scratch_[0], hw, 0.0f, math::kTwoPi, core, edge);
        return;
    }

    struct Station {
        Vec2 p;
        Vec2 n;
        f32 s;
    };
    std::vector<Station> st;
    st.reserve(n + 16);
    for (usize i = 0; i < n; ++i) {
        const bool first = i == 0, last = i + 1 == n;
        const Vec2 p = scratch_[i];
        if (!closed && (first || last)) {
            const Vec2 d = first ? math::normalize_safe(scratch_[1] - p)
                                 : math::normalize_safe(p - scratch_[n - 2]);
            st.push_back(Station{p, perp(d), 1.0f});
            continue;
        }
        const Vec2 prev = scratch_[(i + n - 1) % n];
        const Vec2 next = scratch_[(i + 1) % n];
        const Vec2 din = math::normalize_safe(p - prev);
        const Vec2 dout = math::normalize_safe(next - p);
        const Vec2 nin = perp(din), nout = perp(dout);
        const f32 cosang = din.x * dout.x + din.y * dout.y;
        if (join == LineJoin::Round && cosang < 0.97f) {
            const f32 a0 = std::atan2(nin.y, nin.x);
            f32 a1 = std::atan2(nout.y, nout.x);
            f32 da = a1 - a0;
            while (da > math::kPi) da -= math::kTwoPi;
            while (da < -math::kPi) da += math::kTwoPi;
            const i32 steps = math::clamp(static_cast<i32>(std::ceil(std::fabs(da) / 0.26f)), 2, 16);
            for (i32 k = 0; k <= steps; ++k) {
                const f32 a = a0 + da * static_cast<f32>(k) / static_cast<f32>(steps);
                st.push_back(Station{p, Vec2{std::cos(a), std::sin(a)}, 1.0f});
            }
            continue;
        }
        const Vec2 m = math::normalize_safe(nin + nout);
        const f32 dotm = m.x * nout.x + m.y * nout.y;
        f32 s = dotm > 1e-3f ? 1.0f / dotm : miter_limit + 1.0f;
        if (s > miter_limit) {
            // Bevel: the two segment normals back to back.
            st.push_back(Station{p, nin, 1.0f});
            st.push_back(Station{p, nout, 1.0f});
            continue;
        }
        st.push_back(Station{p, m, s});
    }

    const u32 base_v = static_cast<u32>(vertices_.size());
    for (const Station& s : st) {
        const Vec2 o = s.n * (outer_half * s.s);
        const Vec2 ci = s.n * (core_half * s.s);
        vertices_.push_back(Vertex{s.p + o, Vec2{}, edge, mode});
        vertices_.push_back(Vertex{s.p + ci, Vec2{}, core, mode});
        vertices_.push_back(Vertex{s.p - ci, Vec2{}, core, mode});
        vertices_.push_back(Vertex{s.p - o, Vec2{}, edge, mode});
    }
    const usize count = st.size();
    const usize segs = closed ? count : count - 1;
    for (usize i = 0; i < segs; ++i) {
        const u32 a = base_v + static_cast<u32>(i) * 4;
        const u32 b = base_v + static_cast<u32>((i + 1) % count) * 4;
        for (u32 k = 0; k < 3; ++k) {
            const u32 q[6] = {a + k, a + k + 1, b + k + 1, a + k, b + k + 1, b + k};
            indices_.insert(indices_.end(), q, q + 6);
        }
    }

    if (!closed && cap == LineCap::Round) {
        const Vec2 n0 = st.front().n;
        const Vec2 n1 = st.back().n;
        const f32 a0 = std::atan2(n0.y, n0.x);
        const f32 a1 = std::atan2(n1.y, n1.x);
        round_fan(st.front().p, math::max(hw, fr * 0.5f), a0, a0 + math::kPi, core, edge);
        round_fan(st.back().p, math::max(hw, fr * 0.5f), a1, a1 - math::kPi, core, edge);
    }
}

void DrawList::stroke_polyline(std::span<const Vec2> points, bool closed, const StrokeStyle& style) {
    if (points.empty() || style.color.a <= 0.0f || style.width <= 0.0f) return;
    ensure_draw_cmd();
    const Affine2& t = transform();
    const f32 s = t.uniform_scale();
    std::vector<Vec2> pts;
    pts.reserve(points.size() + 1);
    for (const Vec2& p : points) pts.push_back(t.apply(p));
    const f32 width = style.width * s;
    const u32 color = pack_premul(with_alpha(style.color, style.color.a * alphas_.back()));

    const f32 dash = style.dash_length * s, gap = style.dash_gap * s;
    if (dash <= 0.0f || gap <= 0.0f) {
        stroke_single(pts, closed, width, color, style.cap, style.join, style.miter_limit);
    } else {
        if (closed && !pts.empty()) pts.push_back(pts.front());
        const f32 period = dash + gap;
        // SVG stroke-dashoffset: how far INTO the pattern the line starts.
        // Animating it downward (the canvas's `flow` keyframe) moves the
        // dashes forward along the line.
        f32 phase = std::fmod(style.dash_offset * s, period);
        if (phase < 0.0f) phase += period;
        std::vector<Vec2> piece;
        bool on = phase < dash;
        f32 remaining = on ? dash - phase : period - phase;
        if (on) piece.push_back(pts[0]);
        for (usize i = 0; i + 1 < pts.size(); ++i) {
            Vec2 a = pts[i];
            const Vec2 b = pts[i + 1];
            f32 seg = math::length(b - a);
            while (seg > 0.0f) {
                if (remaining >= seg) {
                    remaining -= seg;
                    if (on) piece.push_back(b);
                    seg = 0.0f;
                } else {
                    const Vec2 cut = a + (b - a) * (remaining / seg);
                    seg -= remaining;
                    a = cut;
                    if (on) {
                        piece.push_back(cut);
                        stroke_single(piece, false, width, color, style.cap, style.join, style.miter_limit);
                        piece.clear();
                        remaining = gap;
                    } else {
                        piece.push_back(cut);
                        remaining = dash;
                    }
                    on = !on;
                }
            }
        }
        if (on && piece.size() > 1) {
            stroke_single(piece, false, width, color, style.cap, style.join, style.miter_limit);
        }
    }
    cmds_.back().index_count = static_cast<u32>(indices_.size()) - cmds_.back().first_index;
}

void DrawList::fill_convex(std::span<const Vec2> points, Color color) {
    if (points.size() < 3 || color.a <= 0.0f) return;
    ensure_draw_cmd();
    const Affine2& t = transform();
    std::vector<Vec2> pts;
    for (const Vec2& p : points) pts.push_back(t.apply(p));
    f32 area = 0.0f;
    for (usize i = 0; i < pts.size(); ++i) {
        const Vec2 a = pts[i], b = pts[(i + 1) % pts.size()];
        area += a.x * b.y - b.x * a.y;
    }
    const f32 outward = area > 0.0f ? -1.0f : 1.0f;  // screen y points down
    const f32 fr = device_px_ * 0.5f;
    const u32 core = pack_premul(with_alpha(color, color.a * alphas_.back()));
    const u32 mode = pack_mode(VertexMode::Solid, 0);
    const usize n = pts.size();
    const u32 base = static_cast<u32>(vertices_.size());
    for (usize i = 0; i < n; ++i) {
        const Vec2 p = pts[i];
        const Vec2 din = math::normalize_safe(p - pts[(i + n - 1) % n]);
        const Vec2 dout = math::normalize_safe(pts[(i + 1) % n] - p);
        Vec2 m = math::normalize_safe(perp(din) + perp(dout)) * outward;
        const f32 d = math::max(m.x * perp(dout).x * outward + m.y * perp(dout).y * outward, 0.25f);
        m = m * (1.0f / d);
        vertices_.push_back(Vertex{p - m * fr, Vec2{}, core, mode});
        vertices_.push_back(Vertex{p + m * fr, Vec2{}, 0u, mode});
    }
    for (usize i = 1; i + 1 < n; ++i) {
        const u32 tri[3] = {base, base + static_cast<u32>(i) * 2, base + static_cast<u32>(i + 1) * 2};
        indices_.insert(indices_.end(), tri, tri + 3);
    }
    for (usize i = 0; i < n; ++i) {
        const u32 a = base + static_cast<u32>(i) * 2, b = base + static_cast<u32>((i + 1) % n) * 2;
        const u32 q[6] = {a, a + 1, b + 1, a, b + 1, b};
        indices_.insert(indices_.end(), q, q + 6);
    }
    cmds_.back().index_count = static_cast<u32>(indices_.size()) - cmds_.back().first_index;
}

// ---- Path ------------------------------------------------------------------------

Path::Contour& Path::current() {
    if (contours_.empty() || contours_.back().closed) {
        contours_.push_back(Contour{{cursor_}, false});
    }
    return contours_.back();
}

Path& Path::move_to(Vec2 p) {
    contours_.push_back(Contour{{p}, false});
    cursor_ = start_ = last_ctrl_ = p;
    return *this;
}

Path& Path::line_to(Vec2 p) {
    current().points.push_back(p);
    cursor_ = last_ctrl_ = p;
    return *this;
}

Path& Path::quad_to(Vec2 c, Vec2 p) {
    // Degree-elevate: a quadratic is a cubic with these controls.
    const Vec2 p0 = cursor_;
    cubic_to(p0 + (c - p0) * (2.0f / 3.0f), p + (c - p) * (2.0f / 3.0f), p);
    last_ctrl_ = c;
    return *this;
}

namespace {
void flatten_cubic(std::vector<Vec2>& out, Vec2 p0, Vec2 p1, Vec2 p2, Vec2 p3, f32 tol, int depth) {
    // Flat when both controls are within tol of the chord.
    const Vec2 d = p3 - p0;
    const f32 len = math::length(d);
    auto dist = [&](Vec2 p) {
        if (len < 1e-6f) return math::length(p - p0);
        return std::fabs((p.x - p0.x) * d.y - (p.y - p0.y) * d.x) / len;
    };
    if (depth >= 12 || (dist(p1) <= tol && dist(p2) <= tol)) {
        out.push_back(p3);
        return;
    }
    const Vec2 p01 = (p0 + p1) * 0.5f, p12 = (p1 + p2) * 0.5f, p23 = (p2 + p3) * 0.5f;
    const Vec2 p012 = (p01 + p12) * 0.5f, p123 = (p12 + p23) * 0.5f;
    const Vec2 mid = (p012 + p123) * 0.5f;
    flatten_cubic(out, p0, p01, p012, mid, tol, depth + 1);
    flatten_cubic(out, mid, p123, p23, p3, tol, depth + 1);
}
} // namespace

Path& Path::cubic_to(Vec2 c1, Vec2 c2, Vec2 p) {
    Contour& c = current();
    flatten_cubic(c.points, cursor_, c1, c2, p, tol_, 0);
    cursor_ = p;
    last_ctrl_ = c2;
    return *this;
}

Path& Path::arc(Vec2 center, f32 radius, f32 a0, f32 a1) {
    const f32 span = std::fabs(a1 - a0);
    const f32 step = 2.0f * std::acos(math::clamp(1.0f - tol_ / math::max(radius, tol_), -1.0f, 1.0f));
    const i32 segs = math::clamp(static_cast<i32>(std::ceil(span / math::max(step, 0.02f))), 2, 256);
    const Vec2 first = center + Vec2{std::cos(a0), std::sin(a0)} * radius;
    if (contours_.empty() || contours_.back().closed) move_to(first); else line_to(first);
    for (i32 i = 1; i <= segs; ++i) {
        const f32 a = a0 + (a1 - a0) * static_cast<f32>(i) / static_cast<f32>(segs);
        contours_.back().points.push_back(center + Vec2{std::cos(a), std::sin(a)} * radius);
    }
    cursor_ = last_ctrl_ = contours_.back().points.back();
    return *this;
}

Path& Path::close() {
    if (!contours_.empty()) contours_.back().closed = true;
    cursor_ = last_ctrl_ = start_;
    return *this;
}

bool Path::svg(std::string_view d) {
    usize i = 0;
    auto skip = [&] {
        while (i < d.size() && (std::isspace(static_cast<unsigned char>(d[i])) || d[i] == ',')) ++i;
    };
    auto number = [&](f32& out) {
        skip();
        if (i >= d.size()) return false;
        char* end = nullptr;
        const std::string tmp(d.data() + i, d.size() - i);
        const f32 v = std::strtof(tmp.c_str(), &end);
        const usize used = static_cast<usize>(end - tmp.c_str());
        if (used == 0) return false;
        i += used;
        out = v;
        return true;
    };
    auto point = [&](Vec2& p, bool rel) {
        f32 x = 0, y = 0;
        if (!number(x) || !number(y)) return false;
        p = rel ? cursor_ + Vec2{x, y} : Vec2{x, y};
        return true;
    };
    char cmd = 0;
    while (true) {
        skip();
        if (i >= d.size()) return true;
        if (std::isalpha(static_cast<unsigned char>(d[i]))) cmd = d[i++];
        else if (cmd == 0) return false;
        const bool rel = std::islower(static_cast<unsigned char>(cmd)) != 0;
        const Vec2 before_ctrl = last_ctrl_;
        const char prev = last_cmd_;
        switch (std::toupper(static_cast<unsigned char>(cmd))) {
            case 'M': {
                Vec2 p;
                if (!point(p, rel)) return false;
                move_to(p);
                cmd = rel ? 'l' : 'L';  // subsequent pairs are implicit lines
                break;
            }
            case 'L': {
                Vec2 p;
                if (!point(p, rel)) return false;
                line_to(p);
                break;
            }
            case 'H': {
                f32 x = 0;
                if (!number(x)) return false;
                line_to(Vec2{rel ? cursor_.x + x : x, cursor_.y});
                break;
            }
            case 'V': {
                f32 y = 0;
                if (!number(y)) return false;
                line_to(Vec2{cursor_.x, rel ? cursor_.y + y : y});
                break;
            }
            case 'C': {
                Vec2 c1, c2, p;
                const Vec2 origin = cursor_;
                if (!point(c1, rel)) return false;
                cursor_ = origin;
                if (!point(c2, rel)) return false;
                cursor_ = origin;
                if (!point(p, rel)) return false;
                cursor_ = origin;
                cubic_to(c1, c2, p);
                break;
            }
            case 'S': {
                Vec2 c2, p;
                const Vec2 origin = cursor_;
                if (!point(c2, rel)) return false;
                cursor_ = origin;
                if (!point(p, rel)) return false;
                cursor_ = origin;
                const bool smooth = prev == 'C' || prev == 'S';
                const Vec2 c1 = smooth ? origin * 2.0f - before_ctrl : origin;
                cubic_to(c1, c2, p);
                break;
            }
            case 'Q': {
                Vec2 c, p;
                const Vec2 origin = cursor_;
                if (!point(c, rel)) return false;
                cursor_ = origin;
                if (!point(p, rel)) return false;
                cursor_ = origin;
                quad_to(c, p);
                break;
            }
            case 'T': {
                Vec2 p;
                const Vec2 origin = cursor_;
                if (!point(p, rel)) return false;
                cursor_ = origin;
                const bool smooth = prev == 'Q' || prev == 'T';
                const Vec2 c = smooth ? origin * 2.0f - before_ctrl : origin;
                quad_to(c, p);
                break;
            }
            case 'Z':
                close();
                last_cmd_ = 'Z';
                cmd = 0;  // numbers after Z without a new command are malformed
                continue;
            default:
                return false;  // A (elliptical arc) is not used by the UI's paths
        }
        last_cmd_ = static_cast<char>(std::toupper(static_cast<unsigned char>(cmd)));
        if (last_cmd_ == 'M') last_cmd_ = 'L';
    }
}

void Path::stroke(DrawList& dl, const StrokeStyle& style) const {
    for (const Contour& c : contours_) dl.stroke_polyline(c.points, c.closed, style);
}

f32 Path::length() const {
    f32 total = 0.0f;
    for (const Contour& c : contours_) {
        for (usize i = 1; i < c.points.size(); ++i) total += math::length(c.points[i] - c.points[i - 1]);
        if (c.closed && c.points.size() > 1) total += math::length(c.points.front() - c.points.back());
    }
    return total;
}

} // namespace immune::gui
