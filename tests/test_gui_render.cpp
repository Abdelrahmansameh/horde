// tests/test_gui_render.cpp — the gui draw layer end to end on real GL.
//
// Renders a showcase of the design canvas's Kit pieces (membrane panel, blood
// bar, ability cells, build card, outlined title, dashed ring, vessel curve,
// clipped level thumbnail, faded group, vignette) through the real GlBackend
// and gui.frag, writes gui_showcase.png for eyeballing, and checks the pixels
// that matter: the colours the canvas specifies, the stencil clip, layer
// opacity without overlap seams, and that the shader's outline agrees with the
// CPU hit test (ShapeSdf.h) everywhere away from the anti-aliased edge.
#include "gui/backend/GlBackend.h"
#include "gui/draw/DrawList.h"
#include "gui/draw/ShapeSdf.h"
#include "gui/icons/IconLibrary.h"
#include "gui/text/Text.h"
#include "platform/FileIO.h"
#include "platform/Window.h"
#include "render/Screenshot.h"

#include <glad/glad.h>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace immune;
using namespace immune::gui;

namespace {

struct Frame {
    std::vector<u8> rgba;
    i32 w = 0, h = 0;
    Color at(i32 x, i32 y) const {
        const usize i = (static_cast<usize>(y) * static_cast<usize>(w) + static_cast<usize>(x)) * 4;
        return Color{rgba[i] / 255.0f, rgba[i + 1] / 255.0f, rgba[i + 2] / 255.0f, rgba[i + 3] / 255.0f};
    }
};

bool near(Color a, Color b, f32 tol = 0.04f) {
    return std::fabs(a.r - b.r) <= tol && std::fabs(a.g - b.g) <= tol && std::fabs(a.b - b.b) <= tol;
}

std::string describe(Color c) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "(%d, %d, %d)", static_cast<int>(c.r * 255 + 0.5f),
                  static_cast<int>(c.g * 255 + 0.5f), static_cast<int>(c.b * 255 + 0.5f));
    return buf;
}

Frame render_frame(GlBackend& backend, const DrawList& dl, const Atlas* fonts, const Atlas* icons, i32 w, i32 h,
             Color clear) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, w, h);
    glClearColor(clear.r, clear.g, clear.b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    backend.render(dl, fonts, icons, w, h, 1.0f);
    glFinish();
    Frame f;
    f.w = w;
    f.h = h;
    render::read_framebuffer_rgba(f.rgba, w, h);
    return f;
}

// Canvas colours (docs/ui-concepts/canvas/project/Main.dc.html).
constexpr u32 kPlum = 0x4E1638;
constexpr u32 kFlesh = 0xEE95A0;
constexpr u32 kCytoplasm = 0xFFF0E6;
constexpr u32 kTissue = 0x992848;
constexpr u32 kInk = 0x2B1030;

ShapeDesc membrane_panel(Vec2 top_left, Vec2 size) {
    ShapeDesc p;
    p.center = top_left + size * 0.5f;
    p.half_size = size * 0.5f;
    p.radius = 34;
    p.fill = rgb(kFlesh);
    p.fill2 = rgb(kCytoplasm);
    p.band = 6;
    p.stroke = rgb(kPlum);
    p.stroke_width = 3.5f;
    p.rim = rgba(0xFFFFFF, 0.55f);
    p.rim_width = 2;
    p.shadow = Color{40 / 255.0f, 0, 20 / 255.0f, 0.28f};
    p.shadow_offset = Vec2{0, 5};
    p.bulge = 3;
    p.wobble_amp = 0.8f;
    p.wobble_seed = 7;
    p.decor = rgba(kFlesh, 0.5f);
    p.decor_count = 3;
    return p;
}

void ability_cell(DrawList& dl, IconLibrary& icons, TextRenderer& text, Vec2 c, u32 color, f32 level,
                  const char* icon, bool ready, const char* label) {
    ShapeDesc cell;
    cell.kind = ShapeKind::Fluid;
    cell.center = c;
    cell.half_size = Vec2{44, 44};
    cell.radius = 44;
    cell.fill = rgb(0xFBF8FF);
    cell.stroke = rgb(kPlum);
    cell.stroke_width = 3;
    cell.rim = rgb(color);
    cell.rim_width = 6;
    cell.level = level;
    cell.fill_axis = FillAxis::Vertical;
    cell.liquid = rgba(color, 0.38f);
    cell.bubble = rgba(color, 0.55f);
    cell.liquid_inset = 0;
    cell.wobble_amp = 1.2f;
    cell.wobble_seed = c.x;
    cell.shadow = rgba(0x28000F, 0.28f);
    cell.shadow_offset = Vec2{0, 4};
    dl.shape(cell);
    icons.draw(dl, icon, Rect{c - Vec2{28, 28}, c + Vec2{28, 28}}, Color{1, 1, 1, ready ? 1.0f : 0.45f});
    if (label != nullptr) {
        TextStyle t;
        t.font = FontId::FredokaSemiBold;
        t.size = 28;
        t.color = rgb(kInk);
        t.outline = kWhite;
        t.outline_width = 2.5f;
        text.draw(dl, label, t, Vec2{c.x - 60, c.y - 17}, 120, TextAlign::Center);
    }
}

void build_showcase(DrawList& dl, TextRenderer& text, IconLibrary& icons) {
    // 1. Organ integrity panel (Main.dc.html, 470x112).
    dl.shape(membrane_panel(Vec2{40, 40}, Vec2{470, 112}));
    TextStyle label;
    label.font = FontId::NunitoExtraBold;
    label.size = 13;
    label.letter_spacing = 0.1f;
    label.uppercase = true;
    label.color = rgb(0x5E4470);
    text.draw(dl, "Organ integrity", label, Vec2{66, 58});
    TextStyle pct;
    pct.font = FontId::FredokaSemiBold;
    pct.size = 40;
    pct.line_height = 1.0f;
    pct.color = rgb(kInk);
    text.draw(dl, "72%", pct, Vec2{66, 82});
    ShapeDesc bar;
    bar.kind = ShapeKind::Fluid;
    bar.center = Vec2{172 + 150, 82 + 20};
    bar.half_size = Vec2{150, 13};
    bar.radius = 12;
    bar.fill = rgb(0xF7D9D3);
    bar.stroke = rgb(kPlum);
    bar.stroke_width = 3;
    bar.level = 0.72f;
    bar.liquid = rgb(0xF08A9C);
    bar.bubble = rgba(0xFFFFFF, 0.35f);
    dl.shape(bar);

    // 2. Ability cells: cooldown as rising liquid, glyph dimmed until ready.
    ability_cell(dl, icons, text, Vec2{620, 96}, 0xF2B233, 0.35f, "ability_complement_cascade", false, "31s");
    ability_cell(dl, icons, text, Vec2{730, 96}, 0xE0559A, 1.0f, "ability_histamine_flare", true, nullptr);
    ability_cell(dl, icons, text, Vec2{840, 96}, 0xF26A3D, 0.7f, "ability_fever_response", false, "14s");
    ability_cell(dl, icons, text, Vec2{950, 96}, 0xD7334A, 0.47f, "ability_fibrin_clot", false, "40s");
    // Ready halo: a spinning dashed ring.
    ShapeDesc halo;
    halo.kind = ShapeKind::Arc;
    halo.center = Vec2{730, 96};
    halo.half_size = Vec2{54, 54};
    halo.stroke = rgba(0xE0559A, 0.8f);
    halo.stroke_width = 4;
    halo.dash_length = 14;
    halo.dash_gap = 10;
    dl.shape(halo);

    // 3. Build cards (lavender membranes) with tower icons and costs.
    const char* towers[5] = {"tower_neutrophil", "tower_cytotoxic_t", "tower_macrophage", "tower_goblet_cell",
                             "tower_fibroblast"};
    const char* costs[5] = {"70", "130", "180", "160", "120"};
    for (int i = 0; i < 5; ++i) {
        const Vec2 tl{40.0f + static_cast<f32>(i) * 150.0f, 200};
        ShapeDesc card = membrane_panel(tl, Vec2{138, 178});
        card.fill = rgb(0xA48CF0);
        card.fill2 = rgb(0xF2EDFF);
        card.radius = 30;
        card.wobble_seed = static_cast<f32>(i) * 3.0f;
        dl.shape(card);
        icons.draw(dl, towers[i], Rect{tl + Vec2{31, 22}, tl + Vec2{107, 98}});
        icons.draw(dl, "atp", Rect{tl + Vec2{34, 128}, tl + Vec2{56, 150}});
        TextStyle cost;
        cost.font = FontId::FredokaSemiBold;
        cost.size = 26;
        cost.color = rgb(kInk);
        cost.tabular = true;
        text.draw(dl, costs[i], cost, tl + Vec2{60, 122});
        ShapeDesc key = ShapeDesc::capsule(tl + Vec2{120, 20}, Vec2{13, 13});
        key.fill = rgb(0x6A4FD0);
        key.stroke = rgb(kPlum);
        key.stroke_width = 2.5f;
        dl.shape(key);
        TextStyle k;
        k.font = FontId::NunitoBlack;
        k.size = 15;
        k.color = kWhite;
        text.draw(dl, std::to_string(i + 1), k, tl + Vec2{107, 10}, 26, TextAlign::Center);
    }

    // 4. Outlined title with a hard shadow (Victory.dc.html style).
    TextStyle title;
    title.font = FontId::FredokaBold;
    title.size = 64;
    title.line_height = 1.0f;
    title.color = rgb(0xFFF0E6);
    title.outline = rgb(kPlum);
    title.outline_width = 4.5f;
    title.shadow = rgba(0x1E0010, 0.45f);
    title.shadow_offset = Vec2{0, 10};
    text.draw(dl, "Level cleared", title, Vec2{800, 210});

    // 5. A vessel curve with flowing dashes.
    Path vessel;
    vessel.svg("M820 330 C900 300 980 420 1060 380 S1180 300 1240 360");
    vessel.stroke(dl, StrokeStyle{18, rgb(kPlum)});
    vessel.stroke(dl, StrokeStyle{12, rgb(0xE3808F)});
    StrokeStyle flow{3, rgba(0xFFFFFF, 0.7f)};
    flow.dash_length = 10;
    flow.dash_gap = 12;
    vessel.stroke(dl, flow);

    // 6. Level thumbnail: lane lines clipped into a cell (stencil).
    ShapeDesc thumb;
    thumb.kind = ShapeKind::Ellipse;
    thumb.center = Vec2{1320, 470};
    thumb.half_size = Vec2{58, 58};
    thumb.wobble_amp = 2;
    thumb.wobble_seed = 11;
    ShapeDesc thumb_bg = thumb;
    thumb_bg.fill = rgb(kTissue);
    dl.shape(thumb_bg);
    dl.push_clip_shape(thumb);
    const Vec2 lane[4] = {Vec2{1220, 440}, Vec2{1305, 440}, Vec2{1320, 500}, Vec2{1440, 500}};
    dl.stroke_polyline(lane, false, StrokeStyle{22.5f, rgb(0x5E1F48)});
    dl.stroke_polyline(lane, false, StrokeStyle{19.0f, rgb(0xE3808F)});
    dl.pop_clip_shape();
    ShapeDesc thumb_rim = thumb;
    thumb_rim.stroke = rgb(kPlum);
    thumb_rim.stroke_width = 4;
    dl.shape(thumb_rim);

    // 7. A faded group: two overlapping opaque squares at 50% as one layer.
    dl.push_layer(0.5f);
    ShapeDesc sq;
    sq.center = Vec2{1500, 200};
    sq.half_size = Vec2{50, 50};
    sq.fill = rgb(0xFFFFFF);
    dl.shape(sq);
    sq.center = Vec2{1550, 230};
    dl.shape(sq);
    dl.pop_layer();

    // 8. Critical-state vignette (confined to a corner here so it does not
    // tint the pixels the test samples).
    ShapeDesc vignette;
    vignette.kind = ShapeKind::Radial;
    vignette.center = Vec2{1700, 480};
    vignette.half_size = Vec2{200, 100};
    vignette.fill = kTransparent;
    vignette.fill2 = rgba(0xFF2A4A, 0.55f);
    vignette.radial_inner = 0.55f;
    dl.shape(vignette);
}

} // namespace

TEST_CASE("gui renders the Kit showcase with the canvas's colours", "[gui][gl][render]") {
    constexpr i32 kW = 1920, kH = 600;
    platform::Window window;
    if (!platform::create_headless_gl(window, kW, kH)) {
        WARN("headless GL context unavailable in this environment; skipping");
        return;
    }
    GlBackend backend;
    INFO(backend.error());
    REQUIRE(backend.init());

    FontLibrary fonts;
    REQUIRE(fonts.load(platform::asset_path("fonts")));
    IconLibrary icons;
    REQUIRE(icons.load_dir(platform::asset_path("ui/icons")) >= 30);
    TextRenderer text(fonts);

    DrawList dl;
    dl.reset(Vec2{kW, kH}, 1.0f, 1.25f);
    build_showcase(dl, text, icons);
    const Frame f = render_frame(backend, dl, &fonts.atlas(), &icons.atlas(), kW, kH, rgb(kTissue));
    render::write_png_rgba("gui_showcase.png", f.rgba.data(), f.w, f.h);

    // Batching: panels, text, icons and curves share draws; only the stencil
    // clip and the layer split them.
    const GuiFrameStats& st = backend.stats();
    INFO("draw calls " << st.draw_calls);
    CHECK(st.draw_calls <= 10);
    CHECK(st.stencil_clips == 1);
    CHECK(st.layers == 1);

    // Background tissue shows between panels.
    INFO("bg " << describe(f.at(20, 580)));
    CHECK(near(f.at(20, 580), rgb(kTissue)));
    // The panel's cytoplasm is cream, its band flesh, its outline plum.
    INFO("cytoplasm " << describe(f.at(470, 130)));
    CHECK(near(f.at(470, 130), rgb(kCytoplasm)));
    // Walking down through the top edge meets plum outline, then flesh band,
    // then cream cytoplasm, in that order.
    i32 plum_y = -1, flesh_y = -1, cream_y = -1;
    for (i32 y = 28; y < 60; ++y) {
        const Color c = f.at(275, y);
        if (plum_y < 0 && near(c, rgb(kPlum), 0.08f)) plum_y = y;
        if (plum_y >= 0 && flesh_y < 0 && near(c, rgb(kFlesh), 0.08f)) flesh_y = y;
        if (flesh_y >= 0 && cream_y < 0 && near(c, rgb(kCytoplasm), 0.03f)) cream_y = y;
    }
    INFO("plum " << plum_y << " flesh " << flesh_y << " cream " << cream_y);
    CHECK(plum_y > 0);
    CHECK(flesh_y > plum_y);
    CHECK(cream_y > flesh_y);
    // Blood bar: liquid at the left, dry track at the far right.
    INFO("liquid " << describe(f.at(200, 102)));
    CHECK(near(f.at(200, 102), rgb(0xF08A9C), 0.12f));
    INFO("track " << describe(f.at(462, 102)));
    CHECK(near(f.at(462, 102), rgb(0xF7D9D3), 0.06f));

    // Stencil: the lane is visible inside the thumbnail and clipped outside it.
    INFO("lane inside " << describe(f.at(1300, 440)));
    CHECK(near(f.at(1300, 440), rgb(0xE3808F), 0.06f));
    INFO("lane outside " << describe(f.at(1420, 500)));
    CHECK(near(f.at(1420, 500), rgb(kTissue), 0.06f));

    // Layer: the overlap of two opaque squares faded as a group is the same
    // colour as either square alone (no double-blend seam).
    const Color alone = f.at(1470, 170);
    const Color overlap = f.at(1525, 215);
    INFO("alone " << describe(alone) << " overlap " << describe(overlap));
    CHECK(near(alone, overlap, 0.01f));
    CHECK(near(alone, mix(rgb(kTissue), kWhite, 0.5f), 0.03f));
}

TEST_CASE("Shader outline agrees with the CPU hit test", "[gui][gl][render]") {
    constexpr i32 kW = 400, kH = 300;
    platform::Window window;
    if (!platform::create_headless_gl(window, kW, kH)) {
        WARN("headless GL context unavailable in this environment; skipping");
        return;
    }
    GlBackend backend;
    REQUIRE(backend.init());

    const f32 t = 2.3f;
    ShapeDesc shapes[3];
    shapes[0].kind = ShapeKind::Box;
    shapes[0].center = Vec2{110, 150};
    shapes[0].half_size = Vec2{90, 60};
    shapes[0].radius = 30;
    shapes[0].bulge = 5;
    shapes[0].wobble_amp = 6;
    shapes[0].wobble_seed = 4;
    shapes[0].wobble_speed = 1.7f;
    shapes[1].kind = ShapeKind::Ellipse;
    shapes[1].center = Vec2{300, 100};
    shapes[1].half_size = Vec2{70, 50};
    shapes[1].wobble_amp = 5;
    shapes[1].wobble_seed = 9;
    shapes[1].rotation = 0.4f;
    shapes[2].kind = ShapeKind::Box;
    shapes[2].center = Vec2{300, 230};
    shapes[2].half_size = Vec2{60, 40};
    shapes[2].radius = 40;

    DrawList dl;
    dl.reset(Vec2{kW, kH}, 1.0f, t);
    std::vector<u32> ids;
    for (ShapeDesc& s : shapes) {
        s.fill = kWhite;
        ids.push_back(dl.shape(s));
    }
    const Frame f = render_frame(backend, dl, nullptr, nullptr, kW, kH, kBlack);

    int checked = 0, mismatched = 0;
    for (i32 y = 0; y < kH; y += 3) {
        for (i32 x = 0; x < kW; x += 3) {
            const Vec2 p{static_cast<f32>(x) + 0.5f, static_cast<f32>(y) + 0.5f};
            bool inside = false, near_edge = false;
            for (u32 id : ids) {
                const ShapeRecord& r = dl.record(id);
                if (sdf::contains(r, p, t, -1.5f)) inside = true;
                else if (sdf::contains(r, p, t, 1.5f)) near_edge = true;
            }
            if (near_edge && !inside) continue;
            ++checked;
            const bool lit = f.at(x, y).r > 0.5f;
            if (lit != inside) ++mismatched;
        }
    }
    INFO(mismatched << " of " << checked << " sample points disagree");
    CHECK(checked > 10000);
    CHECK(mismatched == 0);
}
