// tests/test_gui_draw.cpp — the gui draw layer without a GL context: shape
// records, the CPU outline mirror used for hit testing, DrawList command
// batching and state stacks, stroke tessellation, and SVG path parsing.
#include "gui/Color.h"
#include "gui/draw/DrawList.h"
#include "gui/draw/ShapeSdf.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace immune;
using namespace immune::gui;
using Catch::Approx;

TEST_CASE("Colours pack premultiplied and parse from hex", "[gui][draw]") {
    CHECK(pack_premul(rgb(0xFF0000)) == 0xFF0000FFu);
    // Half-transparent white: every channel halves.
    const u32 half = pack_premul(Color{1, 1, 1, 0.5f});
    CHECK((half & 0xFF) == 128);
    CHECK((half >> 24) == 128);

    Color c;
    REQUIRE(parse_hex_color("#4E1638", c));
    CHECK(c.r == Approx(0x4E / 255.0f));
    CHECK(c.a == 1.0f);
    REQUIRE(parse_hex_color("A48CF080", c));
    CHECK(c.a == Approx(128.0f / 255.0f));
    REQUIRE(parse_hex_color("#fff", c));
    CHECK(c.g == 1.0f);
    CHECK_FALSE(parse_hex_color("#12345", c));
    CHECK_FALSE(parse_hex_color("zzzzzz", c));
}

TEST_CASE("Box and ellipse outline distances are exact on simple cases", "[gui][draw]") {
    CHECK(sdf::box(Vec2{0, 0}, Vec2{10, 5}, 0) == Approx(-5));
    CHECK(sdf::box(Vec2{15, 0}, Vec2{10, 5}, 0) == Approx(5));
    CHECK(sdf::box(Vec2{10, 5}, Vec2{10, 5}, 5) > 0.0f);  // rounded corner cut
    CHECK(sdf::ellipse(Vec2{20, 0}, Vec2{10, 10}) == Approx(10).margin(1e-3));
    CHECK(sdf::ellipse(Vec2{0, 0}, Vec2{10, 4}) < 0.0f);
}

TEST_CASE("Shape records carry kind, geometry and derived wobble lobes", "[gui][draw]") {
    ShapeDesc d;
    d.kind = ShapeKind::Box;
    d.center = Vec2{100, 50};
    d.half_size = Vec2{200, 40};
    d.radius = 20;
    d.wobble_amp = 2;
    d.wobble_wavelength = 90;
    const ShapeRecord r = make_record(d);
    CHECK(r.shadow[3] == 0.0f);
    CHECK(r.rect[2] == 200.0f);
    // Lobes scale with the perimeter so long panels wobble at the same pitch.
    const f32 perimeter = shape_perimeter(ShapeKind::Box, d.half_size, d.radius);
    CHECK(r.wobble[2] == Approx(std::round(perimeter / 90.0f)));
    CHECK(r.dash[3] == Approx(perimeter));

    d.half_size = Vec2{10, 10};
    CHECK(make_record(d).wobble[2] >= 2.0f);  // never fewer than two lobes
}

TEST_CASE("Membrane hit test follows the wobbling outline and is deterministic", "[gui][draw]") {
    ShapeDesc d;
    d.center = Vec2{0, 0};
    d.half_size = Vec2{100, 50};
    d.radius = 30;
    d.wobble_amp = 6;
    d.wobble_seed = 3;
    d.wobble_speed = 1.5f;
    const ShapeRecord r = make_record(d);

    const f32 t = 0.7f;
    // Same seed, same time, same outline.
    CHECK(sdf::membrane_offset(r, Vec2{100, 0}, t) == sdf::membrane_offset(make_record(d), Vec2{100, 0}, t));
    // It moves with time.
    CHECK(sdf::membrane_offset(r, Vec2{100, 0}, t) != sdf::membrane_offset(r, Vec2{100, 0}, t + 0.5f));

    // The right edge sits at 100 + offset; points just either side of it
    // classify accordingly.
    const f32 off = sdf::membrane_offset(r, Vec2{100, 0}, t);
    CHECK(sdf::contains(r, Vec2{100 + off - 0.5f, 0}, t));
    CHECK_FALSE(sdf::contains(r, Vec2{100 + off + 0.5f, 0}, t));
    CHECK(sdf::contains(r, Vec2{100 + off + 0.5f, 0}, t, 1.0f));  // slop widens it

    // Rotation is honoured: a point on the rotated long axis is inside.
    ShapeDesc rot = d;
    rot.wobble_amp = 0;
    rot.rotation = math::kPi * 0.5f;
    const ShapeRecord rr = make_record(rot);
    CHECK(sdf::contains(rr, Vec2{0, 90}, 0));
    CHECK_FALSE(sdf::contains(rr, Vec2{90, 0}, 0));
}

TEST_CASE("Bulge bows the edge midpoints outward and leaves corners", "[gui][draw]") {
    ShapeDesc d;
    d.half_size = Vec2{100, 50};
    d.bulge = 4;
    const ShapeRecord r = make_record(d);
    CHECK(sdf::membrane_offset(r, Vec2{0, -50}, 0) == Approx(4.0f));
    CHECK(sdf::membrane_offset(r, Vec2{100, 50}, 0) == Approx(0.0f).margin(1e-5));
    CHECK(sdf::contains(r, Vec2{0, -53}, 0));
}

TEST_CASE("DrawList batches everything into one draw until clip state changes", "[gui][draw]") {
    DrawList dl;
    dl.reset(Vec2{1920, 1080}, 1.0f, 0.0f);
    ShapeDesc s;
    s.center = Vec2{100, 100};
    s.half_size = Vec2{50, 20};
    s.fill = kWhite;
    dl.shape(s);
    dl.image(Rect{Vec2{0, 0}, Vec2{10, 10}}, Vec2{0, 0}, Vec2{1, 1}, kWhite);
    const Vec2 line[2] = {Vec2{0, 0}, Vec2{100, 0}};
    dl.stroke_polyline(line, false, StrokeStyle{});
    REQUIRE(dl.commands().size() == 1);
    CHECK(dl.commands()[0].kind == CmdKind::Draw);
    CHECK(dl.commands()[0].index_count == dl.indices().size());
    CHECK(dl.records().size() == 1);

    dl.push_clip_rect(Rect{Vec2{10, 10}, Vec2{200, 200}});
    dl.shape(s);
    dl.pop_clip_rect();
    dl.shape(s);
    REQUIRE(dl.commands().size() == 3);
    CHECK(dl.commands()[1].clip.min.x == 10.0f);
    CHECK(dl.commands()[2].clip.max.x == 1920.0f);

    // Every index points at a real vertex.
    for (u32 i : dl.indices()) CHECK(i < dl.vertices().size());
}

TEST_CASE("Clip rects nest by intersection and follow the transform", "[gui][draw]") {
    DrawList dl;
    dl.reset(Vec2{800, 600}, 1.0f, 0.0f);
    dl.push_transform(Affine2::translate(Vec2{100, 50}));
    dl.push_clip_rect(Rect{Vec2{0, 0}, Vec2{300, 300}});
    CHECK(dl.clip_rect().min.x == 100.0f);
    CHECK(dl.clip_rect().min.y == 50.0f);
    dl.push_clip_rect(Rect{Vec2{200, 200}, Vec2{1000, 1000}});
    CHECK(dl.clip_rect().min.x == 300.0f);
    CHECK(dl.clip_rect().max.x == 400.0f);
    dl.pop_clip_rect();
    dl.pop_clip_rect();
    dl.pop_transform();
    CHECK(dl.clip_rect().max.x == 800.0f);
}

TEST_CASE("Shapes are recorded in target space under the transform", "[gui][draw]") {
    DrawList dl;
    dl.reset(Vec2{800, 600}, 1.0f, 0.0f);
    dl.push_transform(Affine2::translate(Vec2{10, 20}) * Affine2::scale(2.0f));
    ShapeDesc s;
    s.center = Vec2{5, 5};
    s.half_size = Vec2{4, 3};
    s.stroke_width = 1.5f;
    s.stroke = kBlack;
    const u32 id = dl.shape(s);
    const ShapeRecord& r = dl.record(id);
    CHECK(r.rect[0] == 20.0f);
    CHECK(r.rect[1] == 30.0f);
    CHECK(r.rect[2] == 8.0f);
    CHECK(r.geom[1] == 3.0f);
}

TEST_CASE("Stencil clips and layers emit balanced commands", "[gui][draw]") {
    DrawList dl;
    dl.reset(Vec2{400, 300}, 1.0f, 0.0f);
    ShapeDesc cell;
    cell.kind = ShapeKind::Ellipse;
    cell.center = Vec2{200, 150};
    cell.half_size = Vec2{60, 60};

    dl.push_layer(0.5f);
    dl.push_clip_shape(cell);
    const Vec2 lane[3] = {Vec2{0, 150}, Vec2{200, 100}, Vec2{400, 150}};
    dl.stroke_polyline(lane, false, StrokeStyle{12.0f, kBlack});
    dl.pop_clip_shape();
    dl.pop_layer();

    const auto& c = dl.commands();
    REQUIRE(c.size() == 5);
    CHECK(c[0].kind == CmdKind::LayerBegin);
    CHECK(c[0].layer_depth == 1);
    CHECK(c[1].kind == CmdKind::StencilPush);
    CHECK(c[1].stencil_depth == 0);
    CHECK(c[2].kind == CmdKind::Draw);
    CHECK(c[2].stencil_depth == 1);
    CHECK(c[3].kind == CmdKind::StencilPop);
    CHECK(c[3].first_index == c[1].first_index);  // the same clip quad, removed again
    CHECK(c[4].kind == CmdKind::LayerEnd);
    CHECK(c[4].layer_depth == 1);
    CHECK(c[4].stencil_depth == 0);
    CHECK(dl.max_layer_depth() == 1);

    // After the pops, a new draw is back at the root state.
    ShapeDesc s;
    s.half_size = Vec2{5, 5};
    s.fill = kWhite;
    dl.shape(s);
    CHECK(dl.commands().back().kind == CmdKind::Draw);
    CHECK(dl.commands().back().stencil_depth == 0);
    CHECK(dl.commands().back().layer_depth == 0);
}

TEST_CASE("Strokes carry an anti-aliasing fringe that fades to transparent", "[gui][draw]") {
    DrawList dl;
    dl.reset(Vec2{400, 300}, 1.0f, 0.0f);
    const Vec2 pts[3] = {Vec2{10, 10}, Vec2{100, 10}, Vec2{100, 100}};
    dl.stroke_polyline(pts, false, StrokeStyle{6.0f, kBlack, LineCap::Butt, LineJoin::Miter});
    usize edge = 0, core = 0;
    f32 max_dist = 0.0f;
    for (const Vertex& v : dl.vertices()) {
        if (v.color == 0) ++edge; else ++core;
        // Vertices of the horizontal segment sit within half width + fringe.
        if (v.pos.x < 90.0f) max_dist = math::max(max_dist, std::fabs(v.pos.y - 10.0f));
    }
    CHECK(edge > 0);
    CHECK(core > 0);
    CHECK(max_dist == Approx(3.5f));
}

TEST_CASE("Dashed strokes split into separate dashes", "[gui][draw]") {
    DrawList solid, dashed;
    solid.reset(Vec2{400, 300}, 1.0f, 0.0f);
    dashed.reset(Vec2{400, 300}, 1.0f, 0.0f);
    const Vec2 pts[2] = {Vec2{0, 0}, Vec2{100, 0}};
    StrokeStyle st{4.0f, kBlack, LineCap::Butt, LineJoin::Miter};
    solid.stroke_polyline(pts, false, st);
    st.dash_length = 10;
    st.dash_gap = 10;
    dashed.stroke_polyline(pts, false, st);
    // 5 dashes of 2 stations each vs one run of 2 stations.
    CHECK(dashed.vertices().size() == solid.vertices().size() * 5);

    // Moving the offset shifts where the first dash starts.
    DrawList shifted;
    shifted.reset(Vec2{400, 300}, 1.0f, 0.0f);
    st.dash_offset = 5;
    shifted.stroke_polyline(pts, false, st);
    f32 min_x = 1e9f;
    for (const Vertex& v : shifted.vertices()) min_x = math::min(min_x, v.pos.x);
    CHECK(min_x == Approx(0.0f).margin(1e-4));  // a partial dash still starts at 0
    CHECK(shifted.vertices().size() == solid.vertices().size() * 6);
}

TEST_CASE("Path parses SVG data, flattens curves and reflects smooth controls", "[gui][draw]") {
    Path p;
    REQUIRE(p.svg("M0 0 L10 0 L10 10 Z"));
    REQUIRE(p.contours().size() == 1);
    CHECK(p.contours()[0].closed);
    CHECK(p.contours()[0].points.size() == 3);
    CHECK(p.length() == Approx(10 + 10 + std::sqrt(200.0f)));

    Path rel;
    REQUIRE(rel.svg("m5 5 h10 v10 l-10 0z"));
    CHECK(rel.contours()[0].points[1] == Vec2{15, 5});
    CHECK(rel.contours()[0].points[2] == Vec2{15, 15});

    // A quarter-circle-ish cubic flattens within tolerance of its true arc.
    Path c(0.1f);
    REQUIRE(c.svg("M100 0 C100 55.23 55.23 100 0 100"));
    for (const Vec2& q : c.contours()[0].points) CHECK(math::length(q) == Approx(100.0f).margin(0.2f));

    // Compact canvas style: implicit repeats and signs as separators.
    Path compact;
    REQUIRE(compact.svg("M22 0c6-3 7 4 12 1s6 3 10 0"));
    CHECK(compact.contours()[0].points.back() == Vec2{44, 1});

    Path bad;
    CHECK_FALSE(bad.svg("M0 0 A 5 5 0 0 1 10 10"));
    CHECK_FALSE(bad.svg("Z 3"));
}

TEST_CASE("Affine2 composes, inverts and pivots", "[gui][draw]") {
    const Affine2 m = Affine2::translate(Vec2{5, 7}) * Affine2::rotate(0.3f) * Affine2::scale(2.0f);
    const Vec2 p{3, -4};
    const Vec2 back = m.inverse().apply(m.apply(p));
    CHECK(back.x == Approx(p.x).margin(1e-4));
    CHECK(back.y == Approx(p.y).margin(1e-4));
    CHECK(m.uniform_scale() == Approx(2.0f));
    CHECK(m.rotation() == Approx(0.3f));
    const Affine2 spin = Affine2::about(Vec2{10, 10}, Affine2::rotate(math::kPi));
    const Vec2 q = spin.apply(Vec2{10, 10});
    CHECK(q.x == Approx(10.0f));
    CHECK(q.y == Approx(10.0f));
}
