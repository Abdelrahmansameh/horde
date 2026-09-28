// gui/draw/ShapeSdf.h — CPU mirror of the outline distance in gui.frag.
//
// Hit testing has to agree with what is on screen: a click on the wobbling
// edge of a membrane must land exactly where the pixels are. So the outline
// function is written twice, here and in gui.frag (shape_outline_distance),
// with the same constants, and test_gui_draw.cpp renders shapes headless and
// checks the two agree at sample points.
#pragma once

#include "core/Math.h"
#include "core/Types.h"
#include "gui/draw/Shape.h"

#include <cmath>

namespace immune::gui::sdf {

inline f32 box(Vec2 p, Vec2 b, f32 r) {
    r = math::clamp(r, 0.0f, math::min(b.x, b.y));
    const Vec2 q{std::fabs(p.x) - b.x + r, std::fabs(p.y) - b.y + r};
    const Vec2 m{math::max(q.x, 0.0f), math::max(q.y, 0.0f)};
    return math::length(m) + math::min(math::max(q.x, q.y), 0.0f) - r;
}

/// Ellipse distance, first-order approximation (exact on circles). Used for
/// both the edge and hit tests, so its error is shared and invisible.
inline f32 ellipse(Vec2 p, Vec2 ab) {
    ab = Vec2{math::max(ab.x, 1e-3f), math::max(ab.y, 1e-3f)};
    const f32 k0 = math::length(Vec2{p.x / ab.x, p.y / ab.y});
    const f32 k1 = math::length(Vec2{p.x / (ab.x * ab.x), p.y / (ab.y * ab.y)});
    if (k1 < 1e-6f) return -math::min(ab.x, ab.y);
    return k0 * (k0 - 1.0f) / k1;
}

/// Seeded phase for wobble harmonic k. Deliberately arithmetic (no sin hash)
/// so CPU and GPU produce the same value.
inline f32 phase(f32 seed, f32 k) {
    const f32 v = seed * 0.6180339887f + k * 0.4142135624f + seed * k * 0.1319f;
    return (v - std::floor(v)) * math::kTwoPi;
}

/// Outward displacement of a Box/Ellipse/Fluid outline at local point p:
/// bulge plus the three-harmonic wobble. Mirrors membrane_offset() in gui.frag.
inline f32 membrane_offset(const ShapeRecord& s, Vec2 p, f32 time) {
    const f32 hw = s.rect[2], hh = s.rect[3];
    f32 off = 0.0f;
    const f32 bulge = s.fx[0];
    if (bulge != 0.0f) {
        const f32 nx = math::clamp(p.x / math::max(hw, 1e-3f), -1.0f, 1.0f);
        const f32 ny = math::clamp(p.y / math::max(hh, 1e-3f), -1.0f, 1.0f);
        off += bulge * ((1.0f - nx * nx) * ny * ny + (1.0f - ny * ny) * nx * nx);
    }
    const f32 amp = s.wobble[0];
    if (amp != 0.0f) {
        const f32 th = std::atan2(p.y / math::max(hh, 1e-3f), p.x / math::max(hw, 1e-3f));
        const f32 lobes = s.wobble[2];
        const f32 seed = s.wobble[1];
        const f32 t = time * s.wobble[3];
        off += amp * (0.68f * std::sin(lobes * th + phase(seed, 0.0f) + t) +
                      0.26f * std::sin((2.0f * lobes + 1.0f) * th + phase(seed, 1.0f) - 1.37f * t) +
                      0.06f * std::sin((3.0f * lobes - 1.0f) * th + phase(seed, 2.0f) + 1.93f * t));
    }
    return off;
}

/// Signed distance to the outline (negative inside) in the shape's local,
/// unrotated frame. Arc and Radial report their painted extent.
inline f32 outline_distance(const ShapeRecord& s, Vec2 p, f32 time) {
    const Vec2 half{s.rect[2], s.rect[3]};
    const auto kind = static_cast<ShapeKind>(static_cast<u32>(s.shadow[3]));
    switch (kind) {
        case ShapeKind::Box:
        case ShapeKind::Fluid:
            return box(p, half, s.geom[0]) - membrane_offset(s, p, time);
        case ShapeKind::Ellipse:
            return ellipse(p, half) - membrane_offset(s, p, time);
        case ShapeKind::Arc:
            return std::fabs(math::length(p) - half.x) - s.geom[1] * 0.5f;
        case ShapeKind::Radial:
            return ellipse(p, half);
    }
    return 1e9f;
}

/// World (target-space) point inside the shape? `slop` widens the test, e.g.
/// by half the stroke so a click on the outline counts.
inline bool contains(const ShapeRecord& s, Vec2 point, f32 time, f32 slop = 0.0f) {
    Vec2 p{point.x - s.rect[0], point.y - s.rect[1]};
    const f32 rot = s.geom[3];
    if (rot != 0.0f) {
        const f32 cs = std::cos(-rot), sn = std::sin(-rot);
        p = Vec2{cs * p.x - sn * p.y, sn * p.x + cs * p.y};
    }
    return outline_distance(s, p, time) <= slop;
}

} // namespace immune::gui::sdf
