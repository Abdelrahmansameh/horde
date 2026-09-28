#include "gui/text/Font.h"

#include "core/Math.h"

// stb_truetype's implementation lives in this one translation unit.
// STBTT_STATIC keeps it private to this file, so it cannot collide with the
// copy ImGui compiles (the dev tools' fonts).
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4505)  // unreferenced static function
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include <stb_truetype.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#else
#pragma GCC diagnostic pop
#endif

#include <fstream>
#include <iterator>
#include <vector>

namespace immune::gui {

const char* font_file(FontId id) {
    switch (id) {
        case FontId::FredokaMedium:   return "Fredoka-Medium.ttf";
        case FontId::FredokaSemiBold: return "Fredoka-SemiBold.ttf";
        case FontId::FredokaBold:     return "Fredoka-Bold.ttf";
        case FontId::NunitoSemiBold:  return "Nunito-SemiBold.ttf";
        case FontId::NunitoBold:      return "Nunito-Bold.ttf";
        case FontId::NunitoExtraBold: return "Nunito-ExtraBold.ttf";
        case FontId::NunitoBlack:     return "Nunito-Black.ttf";
        case FontId::Count: break;
    }
    return "";
}

struct FontLibrary::Face {
    std::vector<unsigned char> data;
    stbtt_fontinfo info{};
    f32 em_scale_bake = 1.0f;   ///< Font units -> px at kBakeSize.
    f32 units_per_em_inv = 1.0f;
    i32 ascent = 0, descent = 0, line_gap = 0;  ///< Font units.
    std::unordered_map<u32, Glyph> glyphs;
    /// Kerning in font units by (a << 32 | b); looked up for every glyph pair
    /// of every layout, and stb's table walk is not cheap.
    mutable std::unordered_map<u64, i32> kerning;
};

FontLibrary::FontLibrary() = default;
FontLibrary::~FontLibrary() = default;

bool FontLibrary::load(const std::string& dir) {
    loaded_ = false;
    for (usize i = 0; i < kFontCount; ++i) {
        const std::string path = dir + "/" + font_file(static_cast<FontId>(i));
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            error_ = "cannot open font " + path;
            return false;
        }
        auto face = std::make_unique<Face>();
        face->data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        const int offset = stbtt_GetFontOffsetForIndex(face->data.data(), 0);
        if (offset < 0 || stbtt_InitFont(&face->info, face->data.data(), offset) == 0) {
            error_ = "not a TrueType font: " + path;
            return false;
        }
        face->em_scale_bake = stbtt_ScaleForMappingEmToPixels(&face->info, kBakeSize);
        face->units_per_em_inv = face->em_scale_bake / kBakeSize;
        stbtt_GetFontVMetrics(&face->info, &face->ascent, &face->descent, &face->line_gap);
        faces_[i] = std::move(face);
    }
    atlas_.create(kAtlasSize, kAtlasSize, 1);
    loaded_ = true;
    error_.clear();
    return true;
}

f32 FontLibrary::ascent(FontId f, f32 size) const {
    const Face& face = *faces_[static_cast<usize>(f)];
    return static_cast<f32>(face.ascent) * face.units_per_em_inv * size;
}

f32 FontLibrary::descent(FontId f, f32 size) const {
    const Face& face = *faces_[static_cast<usize>(f)];
    return static_cast<f32>(face.descent) * face.units_per_em_inv * size;
}

f32 FontLibrary::line_gap(FontId f, f32 size) const {
    const Face& face = *faces_[static_cast<usize>(f)];
    return static_cast<f32>(face.line_gap) * face.units_per_em_inv * size;
}

f32 FontLibrary::kern(FontId f, u32 a, u32 b, f32 size) const {
    const Face& face = *faces_[static_cast<usize>(f)];
    const u64 pair = (static_cast<u64>(a) << 32) | b;
    auto it = face.kerning.find(pair);
    if (it == face.kerning.end()) {
        it = face.kerning.emplace(pair, stbtt_GetCodepointKernAdvance(&face.info, static_cast<int>(a),
                                                                        static_cast<int>(b))).first;
    }
    return static_cast<f32>(it->second) * face.units_per_em_inv * size;
}

const Glyph& FontLibrary::glyph(FontId f, u32 codepoint) {
    Face& face = *faces_[static_cast<usize>(f)];
    if (auto it = face.glyphs.find(codepoint); it != face.glyphs.end()) return it->second;

    int index = stbtt_FindGlyphIndex(&face.info, static_cast<int>(codepoint));
    if (index == 0 && codepoint != '?' && codepoint != ' ') {
        const Glyph& fallback = glyph(f, '?');
        return face.glyphs.emplace(codepoint, fallback).first->second;
    }

    Glyph g;
    int advance = 0, lsb = 0;
    stbtt_GetGlyphHMetrics(&face.info, index, &advance, &lsb);
    g.advance = static_cast<f32>(advance) * face.em_scale_bake;

    int w = 0, h = 0, xoff = 0, yoff = 0;
    const f32 dist_scale = 128.0f / static_cast<f32>(kSpread);
    unsigned char* sdf = stbtt_GetGlyphSDF(&face.info, face.em_scale_bake, index, kSpread, 128, dist_scale,
                                           &w, &h, &xoff, &yoff);
    if (sdf != nullptr && w > 0 && h > 0) {
        i32 x = 0, y = 0;
        if (atlas_.pack(w, h, x, y)) {
            atlas_.blit(x, y, w, h, sdf);
            g.x0 = static_cast<f32>(xoff);
            g.y0 = static_cast<f32>(yoff);
            g.x1 = static_cast<f32>(xoff + w);
            g.y1 = static_cast<f32>(yoff + h);
            g.uv0 = atlas_.uv(x, y);
            g.uv1 = atlas_.uv(x + w, y + h);
            g.has_quad = true;
        }
    }
    if (sdf != nullptr) stbtt_FreeSDF(sdf, nullptr);
    return face.glyphs.emplace(codepoint, g).first->second;
}

} // namespace immune::gui
