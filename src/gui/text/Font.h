// gui/text/Font.h — the UI's typefaces, baked lazily as signed distance fields.
//
// The design (docs/ui-concepts/canvas) uses Fredoka 500/600/700 for numbers and
// titles and Nunito 600/700/800/900 for labels. stb_truetype cannot read a
// variable font's axes, so each weight ships as its own static file in
// assets/fonts/ (built by tools/build_fonts.py).
//
// WHY SDF
// One bake per glyph serves every size from the 13 px minimum to 72 px titles,
// and the shader draws the canvas's heavy text outlines (up to a 9 px
// -webkit-text-stroke) and hard offset shadows from the same texel, with no
// second bake. kSpread is sized for that 9 px outline at the largest title.
#pragma once

#include "core/Types.h"
#include "gui/Atlas.h"

#include <array>
#include <memory>
#include <string>
#include <unordered_map>

namespace immune::gui {

enum class FontId : u8 {
    FredokaMedium = 0,
    FredokaSemiBold,
    FredokaBold,
    NunitoSemiBold,
    NunitoBold,
    NunitoExtraBold,
    NunitoBlack,
    Count
};
inline constexpr usize kFontCount = static_cast<usize>(FontId::Count);

/// File name (under assets/fonts/) for each FontId.
const char* font_file(FontId id);

struct Glyph {
    /// Quad relative to the pen position on the baseline, in px at the BAKE
    /// size (scale by size / kBakeSize). Empty glyphs (space) have no quad.
    f32 x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    Vec2 uv0{0, 0}, uv1{0, 0};
    f32 advance = 0;   ///< At the bake size.
    bool has_quad = false;
};

class FontLibrary {
public:
    static constexpr f32 kBakeSize = 48.0f;     ///< Em size glyphs are baked at.
    static constexpr i32 kSpread = 12;          ///< SDF range, atlas px each side.
    static constexpr i32 kAtlasSize = 2048;

    FontLibrary();
    ~FontLibrary();
    FontLibrary(const FontLibrary&) = delete;
    FontLibrary& operator=(const FontLibrary&) = delete;

    /// Loads every FontId from `dir` (normally platform::asset_path("fonts")).
    /// False if any file is missing or unreadable; error() says which.
    bool load(const std::string& dir);
    bool loaded() const { return loaded_; }
    const std::string& error() const { return error_; }

    /// Vertical metrics for `size` px (CSS font-size = em size).
    f32 ascent(FontId f, f32 size) const;
    f32 descent(FontId f, f32 size) const;  ///< Negative (below baseline).
    f32 line_gap(FontId f, f32 size) const;

    /// Glyph for a code point, baking it on first use. Missing code points map
    /// to '?'. Never null once loaded.
    const Glyph& glyph(FontId f, u32 codepoint);
    /// Kerning adjustment between two code points, px at `size`.
    f32 kern(FontId f, u32 a, u32 b, f32 size) const;

    Atlas& atlas() { return atlas_; }
    const Atlas& atlas() const { return atlas_; }

private:
    struct Face;
    std::array<std::unique_ptr<Face>, kFontCount> faces_;
    Atlas atlas_;
    bool loaded_ = false;
    std::string error_;
};

} // namespace immune::gui
