// tests/test_gui_text.cpp — fonts and text layout against the real shipped
// .ttf files (assets/fonts), no GL needed: glyphs bake into a CPU atlas.
#include "gui/draw/DrawList.h"
#include "gui/text/Text.h"
#include "platform/FileIO.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace immune;
using namespace immune::gui;
using Catch::Approx;

namespace {
FontLibrary& fonts() {
    static FontLibrary lib;
    static bool once = [] { return lib.load(platform::asset_path("fonts")); }();
    (void)once;
    return lib;
}
} // namespace

TEST_CASE("Every UI font loads from assets/fonts", "[gui][text]") {
    FontLibrary lib;
    INFO(lib.error());
    REQUIRE(lib.load(platform::asset_path("fonts")));
    for (usize i = 0; i < kFontCount; ++i) {
        const auto f = static_cast<FontId>(i);
        CHECK(lib.ascent(f, 40) > 20.0f);
        CHECK(lib.descent(f, 40) < 0.0f);
    }
    FontLibrary missing;
    CHECK_FALSE(missing.load("/nonexistent/fonts"));
    CHECK_FALSE(missing.error().empty());
}

TEST_CASE("Glyphs bake on first use into the SDF atlas", "[gui][text]") {
    FontLibrary& lib = fonts();
    REQUIRE(lib.loaded());
    lib.atlas().clear_dirty();
    const Glyph& a = lib.glyph(FontId::FredokaSemiBold, 'A');
    REQUIRE(a.has_quad);
    CHECK(a.advance > 10.0f);
    CHECK_FALSE(lib.atlas().dirty().empty());
    // The glyph's centre is inside the letter: SDF above the on-edge value.
    // (Space has no quad but still advances.)
    const Glyph& sp = lib.glyph(FontId::FredokaSemiBold, ' ');
    CHECK_FALSE(sp.has_quad);
    CHECK(sp.advance > 0.0f);
    // Unknown code points fall back to '?' rather than vanishing.
    const Glyph& q = lib.glyph(FontId::NunitoBold, 0x4E2D);
    CHECK(q.has_quad);
}

TEST_CASE("UTF-8 decodes multi-byte and malformed input", "[gui][text]") {
    const std::string s = "a\xC3\x97\xE2\x80\xA6\xF0\x9F\x98\x80\xFF";
    usize i = 0;
    CHECK(utf8_next(s, i) == 'a');
    CHECK(utf8_next(s, i) == 0xD7);     // ×
    CHECK(utf8_next(s, i) == 0x2026);   // …
    CHECK(utf8_next(s, i) == 0x1F600);
    CHECK(utf8_next(s, i) == 0xFFFD);
    CHECK(i == s.size());
}

TEST_CASE("Text measures like CSS: size, letter-spacing, uppercase, tabular", "[gui][text]") {
    TextRenderer text(fonts());
    TextStyle st;
    st.font = FontId::NunitoExtraBold;
    st.size = 13;
    const f32 w = text.measure("Organ integrity", st).x;
    CHECK(w > 60.0f);
    CHECK(w < 130.0f);

    TextStyle big = st;
    big.size = 26;
    CHECK(text.measure("Organ integrity", big).x == Approx(w * 2.0f).epsilon(0.02));

    TextStyle spaced = st;
    spaced.letter_spacing = 0.1f;  // 14 gaps between 15 glyphs
    CHECK(text.measure("Organ integrity", spaced).x == Approx(w + 14 * 1.3f).epsilon(0.01));

    TextStyle caps = st;
    caps.uppercase = true;
    CHECK(text.measure("Organ integrity", caps).x > w);
    CHECK(text.measure("organ", caps).x == Approx(text.measure("ORGAN", st).x));

    TextStyle tab;
    tab.font = FontId::FredokaSemiBold;
    tab.size = 40;
    tab.tabular = true;
    CHECK(text.measure("111", tab).x == Approx(text.measure("888", tab).x));
    tab.tabular = false;
    CHECK(text.measure("111", tab).x < text.measure("888", tab).x);

    // Line box: CSS line-height multiplies the size.
    st.line_height = 1.5f;
    CHECK(text.measure("A", st).y == Approx(19.5f));
}

TEST_CASE("Text wraps at spaces and honours explicit newlines", "[gui][text]") {
    TextRenderer text(fonts());
    TextStyle st;
    st.size = 16;
    const TextLayout one = text.layout("Needs 6 points in Neutrophil", st);
    REQUIRE(one.lines.size() == 1);
    const TextLayout wrapped = text.layout("Needs 6 points in Neutrophil", st, one.size.x * 0.55f);
    CHECK(wrapped.lines.size() >= 2);
    for (const TextLine& l : wrapped.lines) CHECK(l.width <= one.size.x * 0.55f + 0.01f);
    const TextLayout nl = text.layout("Level\ncleared", st);
    CHECK(nl.lines.size() == 2);
    CHECK(nl.size.y == Approx(2 * 16 * 1.2f));
}

TEST_CASE("Drawing text emits glyph quads, and shadows and outlines add a pass", "[gui][text]") {
    TextRenderer text(fonts());
    DrawList dl;
    dl.reset(Vec2{1920, 1080}, 1.0f, 0.0f);
    TextStyle st;
    st.size = 40;
    text.draw(dl, "72%", st, Vec2{0, 0});
    CHECK(dl.vertices().size() == 3 * 4);
    CHECK(dl.records().size() == 1);  // one style record

    DrawList shadowed;
    shadowed.reset(Vec2{1920, 1080}, 1.0f, 0.0f);
    st.shadow = rgb(0x4E1638);
    st.shadow_offset = Vec2{0, 3};
    st.outline = kWhite;
    st.outline_width = 2.5f;
    text.draw(shadowed, "72%", st, Vec2{0, 0});
    CHECK(shadowed.vertices().size() == 2 * 3 * 4);
    CHECK(shadowed.records().size() == 2);
    // The shadow pass is offset 3 px down from the fill pass.
    CHECK(shadowed.vertices()[0].pos.y - shadowed.vertices()[12].pos.y == Approx(3.0f));

    // Centred text sits in the middle of its frame.
    DrawList centred;
    centred.reset(Vec2{1920, 1080}, 1.0f, 0.0f);
    TextStyle c;
    c.size = 20;
    const f32 w = text.measure("Play", c).x;
    text.draw(centred, "Play", c, Vec2{100, 0}, 200.0f, TextAlign::Center);
    // Glyph quads carry SDF padding on both sides, so compare centres.
    f32 min_x = 1e9f, max_x = -1e9f;
    for (const Vertex& v : centred.vertices()) {
        min_x = math::min(min_x, v.pos.x);
        max_x = math::max(max_x, v.pos.x);
    }
    CHECK((min_x + max_x) * 0.5f == Approx(200.0f).margin(2.0f));
    CHECK(max_x - min_x > w);
}

TEST_CASE("Ellipsized text fits its width", "[gui][text]") {
    TextRenderer text(fonts());
    TextStyle st;
    st.size = 16;
    DrawList dl;
    dl.reset(Vec2{1920, 1080}, 1.0f, 0.0f);
    text.draw_ellipsized(dl, "Efficient Clearance of the whole lymphatic system", st, Vec2{0, 0}, 120.0f);
    f32 max_x = 0.0f;
    for (const Vertex& v : dl.vertices()) max_x = math::max(max_x, v.pos.x);
    CHECK(max_x <= 122.0f);
    CHECK(dl.vertices().size() > 4);
}
