// tests/test_gui_icons.cpp — the SVG icon set and its atlas bakes, no GL.
#include "gui/draw/DrawList.h"
#include "gui/icons/IconLibrary.h"
#include "platform/FileIO.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace immune;
using namespace immune::gui;

TEST_CASE("Every extracted icon parses into drawable shapes", "[gui][icons]") {
    IconLibrary icons;
    const usize n = icons.load_dir(platform::asset_path("ui/icons"));
    for (const std::string& e : icons.errors()) WARN(e);
    CHECK(icons.errors().empty());
    REQUIRE(n >= 30);
    for (const std::string& name : icons.names()) {
        INFO(name);
        CHECK(icons.shape_count(name) > 0);
    }
    // The ones the HUD and tree depend on by name.
    for (const char* name : {"tower_neutrophil", "tower_cytotoxic_t", "tower_macrophage", "tower_goblet_cell",
                             "tower_fibroblast", "ability_complement_cascade", "ability_histamine_flare",
                             "ability_fever_response", "ability_fibrin_clot", "pathogen_virus",
                             "pathogen_bacteria", "pathogen_parasite", "marker_elite", "atp", "memory_cell",
                             "antibody", "glyph_pause", "glyph_menu", "glyph_back", "glyph_play"}) {
        INFO(name);
        CHECK(icons.has(name));
    }
}

TEST_CASE("Icons bake once per size, premultiplied, into the atlas", "[gui][icons]") {
    IconLibrary icons;
    REQUIRE(icons.load_svg("dot", R"(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64"
        viewBox="0 0 64 64"><circle cx="32" cy="32" r="20" fill="#FF0000" fill-opacity="0.5"/></svg>)"));
    icons.atlas().clear_dirty();
    const IconSprite& a = icons.sprite("dot", 32);
    REQUIRE(a.valid);
    CHECK_FALSE(icons.atlas().dirty().empty());
    icons.atlas().clear_dirty();
    const IconSprite& b = icons.sprite("dot", 32);
    CHECK(&a == &b);
    CHECK(icons.atlas().dirty().empty());  // cached: nothing new to upload
    const IconSprite& c = icons.sprite("dot", 64);
    CHECK(c.valid);
    CHECK(c.uv1.x - c.uv0.x > b.uv1.x - b.uv0.x);

    // Premultiplied: no texel has more colour than alpha, and the disc's
    // centre is half-transparent red.
    const Atlas& at = icons.atlas();
    bool premul = true;
    for (i32 i = 0; i < at.width() * at.height(); ++i) {
        const u8* p = at.pixels() + static_cast<usize>(i) * 4;
        if (p[0] > p[3] || p[1] > p[3] || p[2] > p[3]) premul = false;
    }
    CHECK(premul);
    const i32 cx = static_cast<i32>((a.uv0.x + a.uv1.x) * 0.5f * static_cast<f32>(at.width()));
    const i32 cy = static_cast<i32>((a.uv0.y + a.uv1.y) * 0.5f * static_cast<f32>(at.height()));
    const u8* centre = at.pixels() + (static_cast<usize>(cy) * static_cast<usize>(at.width()) + static_cast<usize>(cx)) * 4;
    CHECK(centre[3] > 110);
    CHECK(centre[3] < 145);
    CHECK(centre[0] == centre[3]);
    CHECK(centre[1] == 0);

    // Unknown names draw nothing rather than failing.
    CHECK_FALSE(icons.sprite("nope", 32).valid);
    CHECK_FALSE(icons.load_svg("empty", "<svg xmlns=\"http://www.w3.org/2000/svg\"></svg>"));
}

TEST_CASE("Drawing an icon maps its viewBox onto the rect plus the bake padding", "[gui][icons]") {
    IconLibrary icons;
    REQUIRE(icons.load_svg("sq", R"(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64"
        viewBox="0 0 64 64"><rect width="64" height="64" fill="#fff"/></svg>)"));
    DrawList dl;
    dl.reset(Vec2{800, 600}, 1.0f, 0.0f);
    icons.draw(dl, "sq", Rect{Vec2{100, 100}, Vec2{164, 164}});
    REQUIRE(dl.vertices().size() == 4);
    const f32 pad = 64.0f * IconLibrary::kPadFraction;
    CHECK(dl.vertices()[0].pos.x == 100.0f - pad);
    CHECK(dl.vertices()[2].pos.y == 164.0f + pad);
}
