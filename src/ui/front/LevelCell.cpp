#include "ui/front/LevelCell.h"

#include "gui/anim/Anim.h"
#include "gui/core/Gui.h"
#include "gui/draw/DrawList.h"
#include "gui/style/Theme.h"

#include <string>

namespace immune::ui {

using gui::DrawList;
using gui::ShapeDesc;
using gui::ShapeKind;

LevelCell::LevelCell(gui::Gui& g, std::string id, const CampaignLevel& level)
    : Button(std::move(id)), gui_(g) {
    layout.width = gui::Size::px(kSize);
    layout.height = gui::Size::px(kSize);
    hover_scale = 1.05f;
    set_level(level);
}

void LevelCell::set_level(const CampaignLevel& level) {
    level_ = level;
    number_ = std::to_string(level.number);
    enabled = !level.locked;
    // The hit shape: the membrane (the cell's rect corners do not click).
    shape = membrane(62.0f);
    shape.fill = gui::kTransparent;
}

ShapeDesc LevelCell::membrane(f32 radius_units) const {
    ShapeDesc d;
    d.kind = ShapeKind::Ellipse;
    d.half_size = Vec2{radius_units, radius_units} * kUnit;
    d.wobble_amp = 1.6f;
    d.wobble_wavelength = 55.0f;
    d.wobble_speed = 0.7f;
    d.wobble_seed = static_cast<f32>(level_.number) * 7.31f;
    return d;
}

void LevelCell::draw_thumb(DrawList& dl, Vec2 c) const {
    const gui::Theme& th = gui_.theme();
    // The canvas maps a 116-unit square onto the cell centre.
    const f32 span = 116.0f * kUnit;
    const Vec2 origin = c - Vec2{span, span} * 0.5f;
    auto to_px = [&](Vec2 t) { return origin + t * span; };

    const Vec2 quad[4] = {origin, origin + Vec2{span, 0}, origin + Vec2{span, span}, origin + Vec2{0, span}};
    dl.fill_convex(quad, th.color("tissue"));

    std::vector<Vec2> pts;
    // Wall, rim, lumen: the vessel's three bands, as in the HUD's world
    // render, at the canvas's +8 / +4.5 unit margins over the lumen.
    const struct { const char* color; f32 extra; } bands[3] = {
        {"vessel_wall", 8.0f}, {"vessel_rim", 4.5f}, {"vessel_lumen", 0.0f}};
    for (const auto& band : bands) {
        gui::StrokeStyle s;
        s.color = th.color(band.color);
        for (const LevelThumb::Lane& lane : level_.thumb.lanes) {
            pts.clear();
            for (Vec2 p : lane.points) pts.push_back(to_px(p));
            s.width = lane.width * span + band.extra * kUnit;
            dl.stroke_polyline(pts, false, s);
        }
    }
    gui::StrokeStyle edge;
    edge.color = th.color("vessel_wall");
    edge.width = 4.0f * kUnit;
    for (const LevelThumb::Obstacle& o : level_.thumb.obstacles) {
        pts.clear();
        for (Vec2 p : o.outline) pts.push_back(to_px(p));
        dl.fill_polygon(pts, th.color("tissue_cell"));
        dl.stroke_polyline(pts, true, edge);
    }
    for (Vec2 s : level_.thumb.spawns) {
        ShapeDesc d;
        d.kind = ShapeKind::Ellipse;
        d.center = to_px(s);
        d.half_size = Vec2{4.0f, 4.0f} * kUnit;
        d.fill = th.color("spawn");
        d.stroke = th.color("plum");
        d.stroke_width = 2.0f * kUnit;
        dl.shape(d);
    }
}

void LevelCell::draw_self(DrawList& dl) {
    const gui::Theme& th = gui_.theme();
    const Vec2 c = center();
    const f32 t = dl.time();
    const bool locked = level_.locked;

    // Gold halo on the level to play next.
    if (frontier && !locked) {
        ShapeDesc halo;
        halo.kind = ShapeKind::Arc;
        halo.center = c;
        halo.half_size = Vec2{72.0f, 72.0f} * kUnit;
        halo.stroke = gui::with_alpha(th.color("gold"), gui::loop::halo_opacity(t));
        halo.stroke_width = 6.0f * kUnit;
        dl.shape(halo);
    }
    // Spinning dashed white ring on the selected level.
    if (selected) {
        ShapeDesc ring;
        ring.kind = ShapeKind::Arc;
        ring.center = c;
        ring.half_size = Vec2{78.0f, 78.0f} * kUnit;
        ring.stroke = gui::kWhite;
        ring.stroke_width = 6.0f * kUnit;
        ring.dash_length = 16.0f * kUnit;
        ring.dash_gap = 11.0f * kUnit;
        ring.dash_offset = -gui::loop::spin_rotation(t) * ring.half_size.x;
        dl.shape(ring);
    }

    // Drop shadow, then the lanes clipped to the membrane.
    ShapeDesc body = membrane(58.0f);
    body.center = c;
    ShapeDesc shadow = body;
    shadow.center = c + Vec2{0.0f, 7.0f * kUnit};
    shadow.fill = gui::with_alpha(th.color("dim"), 0.4f);
    dl.shape(shadow);
    ShapeDesc clip = membrane(57.0f);
    clip.center = c;
    dl.push_clip_shape(clip);
    draw_thumb(dl, c);
    if (locked) {
        ShapeDesc veil = body;
        veil.fill = gui::with_alpha(th.color("level_lock_veil"), 0.55f);
        dl.shape(veil);
    }
    dl.pop_clip_shape();
    if (locked) {
        const Vec2 half = Vec2{14.0f, 18.0f} * kUnit;
        gui_.icons().draw(dl, "glyph_lock", Rect{c - half, c + half}, gui::kWhite, gui_.scale());
    }

    // The rim: a lavender (grey when locked) band inside a plum outline.
    ShapeDesc rim = membrane(63.2f);
    rim.center = c;
    rim.fill = th.color(locked ? "level_ring_locked" : "lavender");
    rim.band = 9.5f * kUnit;
    rim.fill2 = gui::kTransparent;
    rim.stroke = th.color("plum");
    rim.stroke_width = 4.0f * kUnit;
    dl.shape(rim);

    // Number badge on the upper-left shoulder.
    ShapeDesc badge = th.shape("level.badge");
    badge.center = c + Vec2{-46.0f, -44.0f} * kUnit;
    badge.half_size = Vec2{19.0f, 19.0f} * kUnit;
    if (locked) badge.fill = th.color("badge_muted");
    badge.stroke_width *= kUnit;
    dl.shape(badge);
    const gui::TextStyle& num = th.text("level_number");
    const Vec2 ns = gui_.text().measure(number_, num);
    gui_.text().draw(dl, number_, num, badge.center - ns * 0.5f);

    // Cleared check on the lower-right.
    if (level_.cleared) {
        ShapeDesc check = th.shape("level.check");
        check.center = c + Vec2{44.0f, 42.0f} * kUnit;
        check.half_size = Vec2{17.0f, 17.0f} * kUnit;
        check.stroke_width *= kUnit;
        dl.shape(check);
        const Vec2 tick[3] = {c + Vec2{36.0f, 42.0f} * kUnit, c + Vec2{42.0f, 48.0f} * kUnit,
                              c + Vec2{52.0f, 37.0f} * kUnit};
        gui::StrokeStyle s;
        s.color = gui::kWhite;
        s.width = 4.5f * kUnit;
        dl.stroke_polyline(tick, false, s);
    }
}

} // namespace immune::ui
