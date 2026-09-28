#include "ui/front/Backdrop.h"

#include "core/Rng.h"
#include "gui/core/Gui.h"
#include "gui/draw/Shape.h"
#include "gui/style/Theme.h"

#include <cmath>

namespace immune::ui {

using gui::DrawList;
using gui::ShapeDesc;
using gui::ShapeKind;

TissueBackdrop::TissueBackdrop(std::string id, u32 seed) : Widget(std::move(id)) {
    blocks_pointer = true;
    // Canvas cells: blobs ~105-120 px wide and ~95-105 px tall on a ~128 x 113
    // grid, each with a 15-20 px nucleus near its middle, a 4-6 px nucleolus
    // in that, and now and then a small dot beside it.
    Rng rng(seed);
    cells_.reserve(static_cast<usize>(kCols * kRows));
    for (i32 i = 0; i < kCols * kRows; ++i) {
        Cell c{};
        c.offset = Vec2{rng.range_f(-9.0f, 9.0f), rng.range_f(-7.0f, 7.0f)};
        c.half = Vec2{rng.range_f(51.0f, 59.0f), rng.range_f(46.0f, 52.0f)};
        c.rotation = rng.range_f(-0.18f, 0.18f);
        c.seed = rng.range_f(0.0f, 100.0f);
        c.nucleus = Vec2{rng.range_f(-6.0f, 6.0f), rng.range_f(-6.0f, 6.0f)};
        c.nucleus_half = Vec2{rng.range_f(15.0f, 20.0f), rng.range_f(11.0f, 15.0f)};
        c.nucleolus = c.nucleus + Vec2{rng.range_f(-4.0f, 4.0f), rng.range_f(-3.0f, 3.0f)};
        c.nucleolus_r = rng.range_f(4.1f, 5.9f);
        c.dot = rng.next_f32() < 0.45f;
        c.dot_pos = Vec2{rng.range_f(-30.0f, 30.0f), rng.range_f(-30.0f, 28.0f)};
        c.dot_r = rng.range_f(3.0f, 5.0f);
        cells_.push_back(c);
    }
}

void TissueBackdrop::draw_self(DrawList& dl) {
    const gui::Theme& th = gui()->theme();
    const Rect r = rect();
    const Vec2 quad[4] = {r.min, Vec2{r.max.x, r.min.y}, r.max, Vec2{r.min.x, r.max.y}};
    dl.fill_convex(quad, th.color("tissue"));

    const gui::Color cell_color = th.color("tissue_cell");
    const gui::Color nucleus_color = th.color("tissue_nucleus");
    const gui::Color nucleolus_color = th.color("tissue_nucleolus");
    dl.push_clip_rect(r);
    // Rows alternate by half a pitch, like the canvas's staggered rows.
    const i32 row0 = static_cast<i32>(std::floor(r.min.y / kPitchY)) - 1;
    const i32 row1 = static_cast<i32>(std::ceil(r.max.y / kPitchY)) + 1;
    for (i32 row = row0; row <= row1; ++row) {
        const f32 stagger = (row & 1) != 0 ? kPitchX * 0.5f : 0.0f;
        const i32 col0 = static_cast<i32>(std::floor((r.min.x - stagger) / kPitchX)) - 1;
        const i32 col1 = static_cast<i32>(std::ceil((r.max.x - stagger) / kPitchX)) + 1;
        for (i32 col = col0; col <= col1; ++col) {
            const i32 ci = ((row % kRows + kRows) % kRows) * kCols + ((col % kCols + kCols) % kCols);
            const Cell& c = cells_[static_cast<usize>(ci)];
            const Vec2 centre = Vec2{col * kPitchX + stagger, row * kPitchY} + c.offset;
            ShapeDesc body;
            body.kind = ShapeKind::Box;
            body.center = centre;
            body.half_size = c.half;
            body.radius = c.half.y * 0.62f;
            body.rotation = c.rotation;
            body.fill = cell_color;
            body.bulge = 4.0f;
            body.wobble_amp = 3.0f;
            body.wobble_wavelength = 70.0f;
            body.wobble_seed = c.seed;
            body.wobble_speed = 0.25f;
            dl.shape(body);

            ShapeDesc nucleus;
            nucleus.kind = ShapeKind::Ellipse;
            nucleus.center = centre + c.nucleus;
            nucleus.half_size = c.nucleus_half;
            nucleus.fill = nucleus_color;
            dl.shape(nucleus);
            ShapeDesc nucleolus = nucleus;
            nucleolus.center = centre + c.nucleolus;
            nucleolus.half_size = Vec2{c.nucleolus_r, c.nucleolus_r};
            nucleolus.fill = nucleolus_color;
            dl.shape(nucleolus);
            if (c.dot) {
                ShapeDesc dot = nucleus;
                dot.center = centre + c.dot_pos;
                dot.half_size = Vec2{c.dot_r, c.dot_r};
                dl.shape(dot);
            }
        }
    }
    dl.pop_clip_rect();
}

VesselStroke::VesselStroke(std::string id) : Widget(std::move(id)) {}

void VesselStroke::draw_self(DrawList& dl) {
    dl.push_transform(gui::Affine2::translate(rect().min));
    for (const Layer& l : layers) {
        gui::StrokeStyle s = l.style;
        if (l.flow_speed != 0.0f) s.dash_offset -= dl.time() * l.flow_speed;
        path.stroke(dl, s);
    }
    dl.pop_transform();
}

} // namespace immune::ui
