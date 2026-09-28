// gui/Color.h — colour helpers for the UI framework.
//
// Colours are authored as straight (non-premultiplied) sRGB in 0..1, exactly
// like the hex values in the design canvas, and packed PREMULTIPLIED into
// RGBA8 at the point they enter a vertex or a shape record. The whole gui
// pipeline blends premultiplied (ONE, ONE_MINUS_SRC_ALPHA): that is what makes
// soft shadows, SDF fringes and layer composites add up without dark halos.
#pragma once

#include "core/Math.h"
#include "core/Types.h"

#include <string_view>

namespace immune::gui {

using Color = Vec4;

/// 0xRRGGBB (alpha 1) — the form the canvas and ui_theme.json use.
constexpr Color rgb(u32 hex) {
    return Color{static_cast<f32>((hex >> 16) & 0xFF) / 255.0f,
                 static_cast<f32>((hex >> 8) & 0xFF) / 255.0f,
                 static_cast<f32>(hex & 0xFF) / 255.0f, 1.0f};
}

/// 0xRRGGBB with an explicit alpha.
constexpr Color rgba(u32 hex, f32 a) {
    Color c = rgb(hex);
    c.a = a;
    return c;
}

constexpr Color with_alpha(Color c, f32 a) { return Color{c.r, c.g, c.b, a}; }

constexpr Color mix(Color a, Color b, f32 t) {
    return Color{math::lerp(a.r, b.r, t), math::lerp(a.g, b.g, t), math::lerp(a.b, b.b, t),
                 math::lerp(a.a, b.a, t)};
}

constexpr Color kTransparent{0.0f, 0.0f, 0.0f, 0.0f};
constexpr Color kWhite{1.0f, 1.0f, 1.0f, 1.0f};
constexpr Color kBlack{0.0f, 0.0f, 0.0f, 1.0f};

/// Packs a straight-alpha colour as premultiplied RGBA8 (R in the low byte),
/// the layout gui.vert unpacks with unpackUnorm4x8.
inline u32 pack_premul(Color c) {
    const f32 a = math::saturate(c.a);
    auto q = [](f32 v) { return static_cast<u32>(math::saturate(v) * 255.0f + 0.5f); };
    return q(c.r * a) | (q(c.g * a) << 8) | (q(c.b * a) << 16) | (q(a) << 24);
}

/// Parses "#RRGGBB", "#RRGGBBAA", "RRGGBB" or "#RGB". False on anything else.
bool parse_hex_color(std::string_view text, Color& out);

} // namespace immune::gui
