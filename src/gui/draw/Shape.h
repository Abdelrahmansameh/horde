// gui/draw/Shape.h — the SDF shapes every widget is drawn from.
//
// WHY SHAPES ARE SHADER-EVALUATED
// A widget shape is one quad; gui.frag evaluates the shape's signed distance at
// every pixel. That single number yields the anti-aliased edge, the plum
// outline, the soft drop shadow, the cytoplasm inset and its rim highlight, the
// organelle dots, dashes along the outline, and clipping for fluid inside the
// cell. It is the same technique entity.frag uses for every tower and pathogen,
// and it is why a membrane can wobble live (the wobble is a function of the
// u_time uniform) without the CPU rebuilding any geometry.
//
// The membrane look is reverse-engineered from the design canvas
// (docs/ui-concepts/canvas): a hard offset shadow, a flesh or lavender body
// with a 3.5 px plum stroke, a cream cytoplasm inset ~6 px, a translucent white
// rim on the inner edge, a few faint organelle dots in the band, and edges
// that bow gently outward.
//
// ShapeDesc is what widget code writes. ShapeRecord is the GPU form, mirrored
// field-for-field by `struct Shape` in assets/shaders/gui.frag; changing one
// without the other is a contract break (test_gui_draw.cpp checks the size).
#pragma once

#include "core/Math.h"
#include "core/Types.h"
#include "gui/Color.h"

namespace immune::gui {

enum class ShapeKind : u32 {
    /// Rounded rectangle; a capsule when radius >= min(half_size). With
    /// `band`, `bulge` or `wobble_amp` it is a membrane.
    Box = 0,
    /// Circle or ellipse (a cell, a node, a pip), same membrane options as Box.
    Ellipse = 1,
    /// Ring or arc of radius half_size.x and thickness stroke_width, drawn in
    /// `stroke`; `fill` (if visible) draws the full-circle track underneath.
    /// Cooldown rings, progress arcs, spinning dashed halos.
    Arc = 2,
    /// Box track with a liquid inside filled to `level` along `fill_axis`,
    /// with a sloshing surface and bubbles: the organ integrity blood bar and
    /// fluid inside cells.
    Fluid = 3,
    /// Radial gradient over the ellipse inscribed in the rect: `fill` at the
    /// centre (up to `radial_inner`), `fill2` at and beyond the edge. Glows,
    /// halos and the critical-state vignette.
    Radial = 4,
};

enum class FillAxis : u32 {
    Horizontal = 0,  ///< Fills left to right.
    Vertical = 1,    ///< Fills bottom to top.
};

struct ShapeDesc {
    ShapeKind kind = ShapeKind::Box;
    Vec2 center{0.0f, 0.0f};
    Vec2 half_size{0.0f, 0.0f};
    f32 radius = 0.0f;      ///< Corner radius (Box, Fluid).
    f32 rotation = 0.0f;    ///< Radians, about `center`.

    Color fill = kTransparent;
    /// Box/Ellipse with band > 0: the cytoplasm colour inside the band.
    /// Box/Ellipse with band == 0 and vertical_gradient: the bottom colour.
    /// Radial: the edge colour. Fluid: unused.
    Color fill2 = kTransparent;
    bool vertical_gradient = false;
    /// Membrane thickness: the region deeper than this is painted `fill2`.
    f32 band = 0.0f;

    Color stroke = kTransparent;
    f32 stroke_width = 0.0f;
    /// Highlight stroke along the inner edge of the band (or just inside the
    /// outline when band == 0).
    Color rim = kTransparent;
    f32 rim_width = 0.0f;

    Color shadow = kTransparent;
    Vec2 shadow_offset{0.0f, 0.0f};
    f32 shadow_blur = 0.0f;
    f32 shadow_spread = 0.0f;

    /// Box/Fluid: how far (px) each edge bows outward at its middle.
    f32 bulge = 0.0f;
    /// Live outline wobble. Amplitude in px; the lobe count is derived from
    /// the perimeter so long and short panels wobble at the same scale.
    f32 wobble_amp = 0.0f;
    f32 wobble_seed = 0.0f;
    f32 wobble_wavelength = 90.0f;
    f32 wobble_speed = 0.0f;   ///< Radians per second of the lowest harmonic.

    /// Organelle dots scattered along the band (Box/Ellipse with band > 0).
    Color decor = kTransparent;
    u32 decor_count = 0;

    /// Dashes on the stroke (and on Arc). Zero length = solid.
    f32 dash_length = 0.0f;
    f32 dash_gap = 0.0f;
    f32 dash_offset = 0.0f;

    /// Arc only.
    f32 arc_start = 0.0f;              ///< Radians, 0 = +x, clockwise on screen.
    f32 arc_sweep = math::kTwoPi;

    /// Fluid only.
    f32 level = 0.0f;                  ///< 0..1
    FillAxis fill_axis = FillAxis::Horizontal;
    Color liquid = kTransparent;
    Color bubble = kTransparent;
    f32 liquid_inset = 3.0f;           ///< Gap between the track edge and the liquid.
    f32 wave_amp = 1.5f;               ///< Surface slosh amplitude, px.

    /// Radial only: where `fill` starts fading, as a fraction of the radius.
    f32 radial_inner = 0.0f;

    /// Stable handle for a Box whose edges should be treated as a capsule.
    static ShapeDesc capsule(Vec2 center, Vec2 half) {
        ShapeDesc d;
        d.center = center;
        d.half_size = half;
        d.radius = math::min(half.x, half.y);
        return d;
    }
};

/// GPU form. std430 layout: nine 16-byte rows. Mirrored by `struct Shape` in
/// gui.frag. Text quads reuse the record for their style (see TextStyle).
struct ShapeRecord {
    f32 rect[4];     ///< cx, cy, hw, hh (logical px, after the draw transform)
    f32 geom[4];     ///< radius, stroke_width, band, rotation
    f32 wobble[4];   ///< amp, seed, lobes, speed
    f32 fx[4];       ///< bulge, rim_width, shadow_blur, decor_count
    f32 shadow[4];   ///< offset x, offset y, spread, kind
    f32 dash[4];     ///< length, gap, offset, perimeter
    f32 extra[4];    ///< Box: gradient | Arc: start, sweep | Fluid: level, axis, wave, inset | Radial: inner
    u32 colors0[4];  ///< fill, fill2, stroke, shadow (premultiplied RGBA8)
    u32 colors1[4];  ///< rim, decor, liquid, bubble
};
static_assert(sizeof(ShapeRecord) == 144, "ShapeRecord must match gui.frag's std430 Shape");

/// Approximate perimeter of the shape's base outline (drives wobble lobes and
/// dash spacing on boxes).
f32 shape_perimeter(ShapeKind kind, Vec2 half, f32 radius);

/// Packs a desc (already in target space: centre, sizes and rotation final).
ShapeRecord make_record(const ShapeDesc& d);

/// How far outside `half_size` the shape can paint (shadow, stroke, wobble,
/// bulge) — the quad has to cover it.
f32 shape_paint_margin(const ShapeDesc& d);

} // namespace immune::gui
