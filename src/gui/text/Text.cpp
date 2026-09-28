#include "gui/text/Text.h"

#include "core/Math.h"
#include "gui/draw/DrawList.h"

#include <string>

namespace immune::gui {

u32 utf8_next(std::string_view s, usize& i) {
    const auto b0 = static_cast<unsigned char>(s[i]);
    auto cont = [&](usize k) {
        return i + k < s.size() && (static_cast<unsigned char>(s[i + k]) & 0xC0) == 0x80;
    };
    auto byte = [&](usize k) { return static_cast<u32>(static_cast<unsigned char>(s[i + k]) & 0x3F); };
    if (b0 < 0x80) { i += 1; return b0; }
    if ((b0 & 0xE0) == 0xC0 && cont(1)) {
        const u32 cp = ((b0 & 0x1Fu) << 6) | byte(1);
        i += 2;
        return cp;
    }
    if ((b0 & 0xF0) == 0xE0 && cont(1) && cont(2)) {
        const u32 cp = ((b0 & 0x0Fu) << 12) | (byte(1) << 6) | byte(2);
        i += 3;
        return cp;
    }
    if ((b0 & 0xF8) == 0xF0 && cont(1) && cont(2) && cont(3)) {
        const u32 cp = ((b0 & 0x07u) << 18) | (byte(1) << 12) | (byte(2) << 6) | byte(3);
        i += 4;
        return cp;
    }
    i += 1;
    return 0xFFFD;
}

namespace {

u32 upper(u32 cp) {
    if (cp >= 'a' && cp <= 'z') return cp - 32;
    // Latin-1 lowercase letters (except ß and ÿ, which have no Latin-1 capital).
    if (cp >= 0xE0 && cp <= 0xFE && cp != 0xF7) return cp - 32;
    return cp;
}

bool is_digit(u32 cp) { return cp >= '0' && cp <= '9'; }

} // namespace

f32 TextRenderer::advance(const TextStyle& s, u32 cp, u32 next) {
    const f32 k = s.size / FontLibrary::kBakeSize;
    f32 adv = 0.0f;
    if (s.tabular && is_digit(cp)) {
        for (u32 d = '0'; d <= '9'; ++d) adv = math::max(adv, fonts_.glyph(s.font, d).advance);
    } else {
        adv = fonts_.glyph(s.font, cp).advance;
    }
    adv *= k;
    if (next != 0 && !(s.tabular && is_digit(cp) && is_digit(next))) {
        adv += fonts_.kern(s.font, cp, next, s.size);
    }
    return adv + s.letter_spacing * s.size;
}

f32 TextRenderer::line_width(std::string_view text, usize b, usize e, const TextStyle& s) {
    f32 w = 0.0f;
    usize i = b;
    bool any = false;
    while (i < e) {
        u32 cp = utf8_next(text, i);
        if (s.uppercase) cp = upper(cp);
        u32 next = 0;
        if (i < e) {
            usize j = i;
            next = utf8_next(text, j);
            if (s.uppercase) next = upper(next);
        }
        w += advance(s, cp, next);
        any = true;
    }
    // CSS letter-spacing trails the last glyph too; drop it so centring is true.
    if (any) w -= s.letter_spacing * s.size;
    return w;
}

TextLayout TextRenderer::layout(std::string_view text, const TextStyle& style, f32 max_width) {
    TextLayout out;
    out.line_height = style.size * style.line_height;
    const f32 asc = fonts_.ascent(style.font, style.size);
    const f32 desc = fonts_.descent(style.font, style.size);
    out.baseline = (out.line_height - (asc - desc)) * 0.5f + asc;

    usize line_start = 0;
    while (line_start <= text.size()) {
        usize nl = text.find('\n', line_start);
        if (nl == std::string_view::npos) nl = text.size();
        if (max_width <= 0.0f) {
            out.lines.push_back(TextLine{line_start, nl, line_width(text, line_start, nl, style)});
        } else {
            // Greedy wrap at spaces.
            usize b = line_start;
            while (true) {
                usize e = b;
                usize last_break = std::string_view::npos;
                f32 w = 0.0f;
                while (e < nl) {
                    usize j = e;
                    const u32 cp = utf8_next(text, j);
                    const f32 nw = line_width(text, b, j, style);
                    if (nw > max_width && e > b) {
                        if (last_break != std::string_view::npos) e = last_break;
                        break;
                    }
                    if (cp == ' ') last_break = e;
                    w = nw;
                    e = j;
                }
                if (e >= nl) {
                    out.lines.push_back(TextLine{b, nl, line_width(text, b, nl, style)});
                    break;
                }
                out.lines.push_back(TextLine{b, e, line_width(text, b, e, style)});
                (void)w;
                b = e;
                while (b < nl && text[b] == ' ') ++b;
                if (b >= nl) break;
            }
        }
        if (nl >= text.size()) break;
        line_start = nl + 1;
    }
    for (const TextLine& l : out.lines) out.size.x = math::max(out.size.x, l.width);
    out.size.y = out.line_height * static_cast<f32>(out.lines.size());
    return out;
}

void TextRenderer::draw_run(DrawList& dl, std::string_view text, usize b, usize e, const TextStyle& s,
                            Vec2 pen, Color color, u32 style_record) {
    const f32 k = s.size / FontLibrary::kBakeSize;
    usize i = b;
    while (i < e) {
        u32 cp = utf8_next(text, i);
        if (s.uppercase) cp = upper(cp);
        u32 next = 0;
        if (i < e) {
            usize j = i;
            next = utf8_next(text, j);
            if (s.uppercase) next = upper(next);
        }
        const Glyph& g = fonts_.glyph(s.font, cp);
        if (g.has_quad) {
            f32 x = pen.x;
            if (s.tabular && is_digit(cp)) {
                // Centre each digit in the tabular cell.
                f32 cell = 0.0f;
                for (u32 d = '0'; d <= '9'; ++d) cell = math::max(cell, fonts_.glyph(s.font, d).advance);
                x += (cell - g.advance) * 0.5f * k;
            }
            dl.glyph(Rect{Vec2{x + g.x0 * k, pen.y + g.y0 * k}, Vec2{x + g.x1 * k, pen.y + g.y1 * k}}, g.uv0,
                     g.uv1, color, style_record);
        }
        pen.x += advance(s, cp, next);
    }
}

void TextRenderer::draw(DrawList& dl, std::string_view text, const TextStyle& style, Vec2 pos, f32 box_width,
                        TextAlign align, bool wrap) {
    if (!fonts_.loaded() || text.empty()) return;
    const TextLayout lay = layout(text, style, wrap ? box_width : 0.0f);
    const f32 frame = box_width > 0.0f ? box_width : lay.size.x;

    auto emit = [&](Vec2 offset, Color fill, Color outline, f32 softness) {
        TextStyleRecord rec;
        rec.outline = outline;
        rec.outline_width = style.outline_width;
        rec.softness = softness;
        const u32 id = dl.text_style(rec);
        for (usize li = 0; li < lay.lines.size(); ++li) {
            const TextLine& l = lay.lines[li];
            f32 x = pos.x;
            if (align == TextAlign::Center) x += (frame - l.width) * 0.5f;
            else if (align == TextAlign::Right) x += frame - l.width;
            const Vec2 pen{x + offset.x,
                           pos.y + lay.baseline + lay.line_height * static_cast<f32>(li) + offset.y};
            draw_run(dl, text, l.begin, l.end, style, pen, fill, id);
        }
    };

    if (style.shadow.a > 0.0f) {
        const Color sc = style.shadow;
        emit(style.shadow_offset, sc, style.outline_width > 0.0f ? sc : kTransparent, style.shadow_blur);
    }
    emit(Vec2{0.0f, 0.0f}, style.color, style.outline, 0.0f);
}

void TextRenderer::draw_ellipsized(DrawList& dl, std::string_view text, const TextStyle& style, Vec2 pos,
                                   f32 max_width, TextAlign align) {
    if (line_width(text, 0, text.size(), style) <= max_width) {
        draw(dl, text, style, pos, max_width, align, false);
        return;
    }
    static constexpr std::string_view kEllipsis = "\xE2\x80\xA6";
    // Cut at code-point boundaries until "prefix…" fits.
    std::vector<usize> cuts;
    for (usize i = 0; i < text.size();) {
        utf8_next(text, i);
        cuts.push_back(i);
    }
    std::string shortened;
    for (usize n = cuts.size(); n-- > 0;) {
        shortened.assign(text.substr(0, cuts[n]));
        while (!shortened.empty() && shortened.back() == ' ') shortened.pop_back();
        shortened += kEllipsis;
        if (line_width(shortened, 0, shortened.size(), style) <= max_width) break;
        if (n == 0) shortened = std::string(kEllipsis);
    }
    draw(dl, shortened, style, pos, max_width, align, false);
}

} // namespace immune::gui
