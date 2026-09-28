#include "gui/draw/Shape.h"

#include <cmath>

namespace immune::gui {

f32 shape_perimeter(ShapeKind kind, Vec2 half, f32 radius) {
    const f32 w = math::max(half.x, 0.0f) * 2.0f;
    const f32 h = math::max(half.y, 0.0f) * 2.0f;
    switch (kind) {
        case ShapeKind::Ellipse:
        case ShapeKind::Radial: {
            // Ramanujan's approximation.
            const f32 a = half.x, b = half.y;
            return math::kPi * (3.0f * (a + b) - std::sqrt((3.0f * a + b) * (a + 3.0f * b)));
        }
        case ShapeKind::Arc:
            return math::kTwoPi * half.x;
        case ShapeKind::Box:
        case ShapeKind::Fluid: {
            const f32 r = math::clamp(radius, 0.0f, math::min(half.x, half.y));
            return 2.0f * (w + h) - (8.0f - math::kTwoPi) * r;
        }
    }
    return 0.0f;
}

ShapeRecord make_record(const ShapeDesc& d) {
    ShapeRecord r{};
    r.rect[0] = d.center.x;
    r.rect[1] = d.center.y;
    r.rect[2] = math::max(d.half_size.x, 0.0f);
    r.rect[3] = math::max(d.half_size.y, 0.0f);

    r.geom[0] = d.radius;
    r.geom[1] = d.stroke_width;
    r.geom[2] = d.band;
    r.geom[3] = d.rotation;

    const f32 perimeter = shape_perimeter(d.kind, d.half_size, d.radius);
    f32 lobes = 0.0f;
    if (d.wobble_amp != 0.0f) {
        lobes = std::round(perimeter / math::max(d.wobble_wavelength, 8.0f));
        lobes = math::max(lobes, 2.0f);
    }
    r.wobble[0] = d.wobble_amp;
    r.wobble[1] = d.wobble_seed;
    r.wobble[2] = lobes;
    r.wobble[3] = d.wobble_speed;

    r.fx[0] = d.bulge;
    r.fx[1] = d.rim_width;
    r.fx[2] = d.shadow_blur;
    r.fx[3] = static_cast<f32>(d.decor_count);

    r.shadow[0] = d.shadow_offset.x;
    r.shadow[1] = d.shadow_offset.y;
    r.shadow[2] = d.shadow_spread;
    r.shadow[3] = static_cast<f32>(static_cast<u32>(d.kind));

    r.dash[0] = d.dash_length;
    r.dash[1] = d.dash_gap;
    r.dash[2] = d.dash_offset;
    r.dash[3] = perimeter;

    switch (d.kind) {
        case ShapeKind::Box:
        case ShapeKind::Ellipse:
            r.extra[0] = d.vertical_gradient ? 1.0f : 0.0f;
            break;
        case ShapeKind::Arc:
            r.extra[0] = d.arc_start;
            r.extra[1] = d.arc_sweep;
            break;
        case ShapeKind::Fluid:
            r.extra[0] = math::saturate(d.level);
            r.extra[1] = static_cast<f32>(static_cast<u32>(d.fill_axis));
            r.extra[2] = d.wave_amp;
            r.extra[3] = d.liquid_inset;
            break;
        case ShapeKind::Radial:
            r.extra[0] = math::saturate(d.radial_inner);
            break;
    }

    r.colors0[0] = pack_premul(d.fill);
    r.colors0[1] = pack_premul(d.fill2);
    r.colors0[2] = pack_premul(d.stroke);
    r.colors0[3] = pack_premul(d.shadow);
    r.colors1[0] = pack_premul(d.rim);
    r.colors1[1] = pack_premul(d.decor);
    r.colors1[2] = pack_premul(d.liquid);
    r.colors1[3] = pack_premul(d.bubble);
    return r;
}

f32 shape_paint_margin(const ShapeDesc& d) {
    f32 m = d.stroke_width * 0.5f + std::fabs(d.wobble_amp) + math::max(d.bulge, 0.0f);
    if (d.shadow.a > 0.0f) {
        const f32 off = math::max(std::fabs(d.shadow_offset.x), std::fabs(d.shadow_offset.y));
        m = math::max(m, off + d.shadow_blur + d.shadow_spread + std::fabs(d.wobble_amp) +
                             math::max(d.bulge, 0.0f));
    }
    if (d.kind == ShapeKind::Arc) m += d.stroke_width * 0.5f;
    return m + 2.0f;  // AA fringe
}

} // namespace immune::gui
