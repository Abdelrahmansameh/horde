// gui/text/Text.h — laying out and drawing styled text.
//
// Styles mirror the CSS the design canvas uses, so a label can be copied from
// an artboard field by field: font-size is the em size, letter-spacing is in
// em, text-transform: uppercase, font-variant-numeric: tabular-nums, and
// line-height as a multiplier with CSS half-leading.
//
// Outline: `outline_width` is the VISIBLE width outside the glyph. The canvas
// writes `-webkit-text-stroke: 9px` with `paint-order: stroke fill`, which
// paints half the stroke under the fill, so that is outline_width = 4.5.
#pragma once

#include "core/Types.h"
#include "gui/Color.h"
#include "gui/text/Font.h"

#include <string_view>
#include <vector>

namespace immune::gui {

class DrawList;

enum class TextAlign : u8 { Left, Center, Right };

struct TextStyle {
    FontId font = FontId::NunitoBold;
    f32 size = 16.0f;
    Color color = kBlack;
    f32 letter_spacing = 0.0f;   ///< In em (CSS "0.1em" = 0.1).
    bool uppercase = false;
    bool tabular = false;        ///< Every digit takes the widest digit's advance.
    f32 line_height = 1.2f;      ///< Multiplier of size.

    Color outline = kTransparent;
    f32 outline_width = 0.0f;

    /// Hard (blur 0) or soft offset shadow, drawn under the text and its outline.
    Color shadow = kTransparent;
    Vec2 shadow_offset{0.0f, 0.0f};
    f32 shadow_blur = 0.0f;
};

struct TextLine {
    usize begin = 0, end = 0;  ///< Byte range in the source string.
    f32 width = 0.0f;
};

struct TextLayout {
    std::vector<TextLine> lines;
    Vec2 size{0.0f, 0.0f};    ///< Box: widest line x (lines * line height).
    f32 line_height = 0.0f;
    f32 baseline = 0.0f;      ///< First baseline from the top of the box.
};

/// Decodes one UTF-8 code point at `i`, advancing it. Malformed bytes decode
/// as U+FFFD one byte at a time.
u32 utf8_next(std::string_view s, usize& i);

class TextRenderer {
public:
    explicit TextRenderer(FontLibrary& fonts) : fonts_(fonts) {}

    /// Lays out `text`. `max_width` > 0 wraps at spaces (a word longer than
    /// the line breaks mid-word); `max_width` <= 0 is one line per '\n'.
    TextLayout layout(std::string_view text, const TextStyle& style, f32 max_width = 0.0f);

    Vec2 measure(std::string_view text, const TextStyle& style, f32 max_width = 0.0f) {
        return layout(text, style, max_width).size;
    }

    /// Draws `text` in the box whose top-left is `pos`; `box_width` sets the
    /// alignment frame (and wrap width when `wrap`).
    void draw(DrawList& dl, std::string_view text, const TextStyle& style, Vec2 pos,
              f32 box_width = 0.0f, TextAlign align = TextAlign::Left, bool wrap = false);

    /// Single line, shortened with "…" to fit `max_width`.
    void draw_ellipsized(DrawList& dl, std::string_view text, const TextStyle& style, Vec2 pos,
                         f32 max_width, TextAlign align = TextAlign::Left);

    FontLibrary& fonts() { return fonts_; }

private:
    f32 advance(const TextStyle& s, u32 cp, u32 next);
    f32 line_width(std::string_view text, usize b, usize e, const TextStyle& s);
    void draw_run(DrawList& dl, std::string_view text, usize b, usize e, const TextStyle& s, Vec2 pen,
                  Color color, u32 style_record);

    FontLibrary& fonts_;
};

} // namespace immune::gui
