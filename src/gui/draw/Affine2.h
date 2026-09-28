// gui/draw/Affine2.h — 2D affine transform for the DrawList transform stack.
#pragma once

#include "core/Types.h"

#include <cmath>

namespace immune::gui {

/// Maps p to (a*p.x + c*p.y + tx, b*p.x + d*p.y + ty) — column-major like SVG's
/// matrix(a b c d e f).
struct Affine2 {
    f32 a = 1.0f, b = 0.0f, c = 0.0f, d = 1.0f, tx = 0.0f, ty = 0.0f;

    static Affine2 translate(Vec2 t) { return Affine2{1, 0, 0, 1, t.x, t.y}; }
    static Affine2 scale(f32 s) { return Affine2{s, 0, 0, s, 0, 0}; }
    static Affine2 scale(Vec2 s) { return Affine2{s.x, 0, 0, s.y, 0, 0}; }
    static Affine2 rotate(f32 radians) {
        const f32 cs = std::cos(radians), sn = std::sin(radians);
        return Affine2{cs, sn, -sn, cs, 0, 0};
    }
    /// Scale and/or rotate about `pivot` rather than the origin — how the
    /// canvas's `transform-origin: center` animations (beat, wobble, spin) work.
    static Affine2 about(Vec2 pivot, const Affine2& m) {
        return translate(pivot) * m * translate(-pivot);
    }

    Vec2 apply(Vec2 p) const { return Vec2{a * p.x + c * p.y + tx, b * p.x + d * p.y + ty}; }
    Vec2 apply_vector(Vec2 v) const { return Vec2{a * v.x + c * v.y, b * v.x + d * v.y}; }

    /// Uniform scale factor (sqrt|det|). Shape records carry sizes in this
    /// scale; a non-uniform scale is approximated by it.
    f32 uniform_scale() const { return std::sqrt(std::fabs(a * d - b * c)); }
    f32 rotation() const { return std::atan2(b, a); }
    bool is_identity() const {
        return a == 1.0f && b == 0.0f && c == 0.0f && d == 1.0f && tx == 0.0f && ty == 0.0f;
    }

    Affine2 inverse() const {
        const f32 det = a * d - b * c;
        if (det == 0.0f) return Affine2{};
        const f32 id = 1.0f / det;
        Affine2 r{d * id, -b * id, -c * id, a * id, 0, 0};
        const Vec2 t = r.apply_vector(Vec2{tx, ty});
        r.tx = -t.x;
        r.ty = -t.y;
        return r;
    }

    friend Affine2 operator*(const Affine2& m, const Affine2& n) {
        return Affine2{m.a * n.a + m.c * n.b, m.b * n.a + m.d * n.b,
                       m.a * n.c + m.c * n.d, m.b * n.c + m.d * n.d,
                       m.a * n.tx + m.c * n.ty + m.tx, m.b * n.tx + m.d * n.ty + m.ty};
    }
};

} // namespace immune::gui
