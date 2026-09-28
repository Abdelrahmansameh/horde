// gui/widgets/Builders.h — terse helpers for building widget trees in code.
//
//     Widget& col = column(panel, 6.0f);
//     text(col, "title", "Organ integrity", theme.text("label"));
//     Widget& r = row(col, 14.0f);
//     icon(r, "atp", "atp", 22.0f);
#pragma once

#include "gui/core/Widget.h"
#include "gui/widgets/Widgets.h"

#include <string>

namespace immune::gui {

inline Widget& row(Widget& parent, f32 gap, Align align = Align::Center, std::string id = {}) {
    Widget& w = parent.emplace<Widget>(std::move(id));
    w.layout.axis = Axis::Row;
    w.layout.gap = gap;
    w.layout.align = align;
    return w;
}

inline Widget& column(Widget& parent, f32 gap, Align align = Align::Start, std::string id = {}) {
    Widget& w = parent.emplace<Widget>(std::move(id));
    w.layout.axis = Axis::Column;
    w.layout.gap = gap;
    w.layout.align = align;
    return w;
}

inline Widget& stack(Widget& parent, Align align = Align::Center, std::string id = {}) {
    Widget& w = parent.emplace<Widget>(std::move(id));
    w.layout.axis = Axis::Stack;
    w.layout.align = align;
    return w;
}

inline Label& text(Widget& parent, std::string id, std::string value, const TextStyle& style,
                   TextAlign align = TextAlign::Left) {
    Label& l = parent.emplace<Label>(std::move(id), std::move(value), style);
    l.align = align;
    return l;
}

inline Icon& icon(Widget& parent, std::string id, std::string name, f32 size) {
    return parent.emplace<Icon>(std::move(id), std::move(name), size);
}

inline Icon& icon(Widget& parent, std::string id, std::string name, Vec2 size) {
    Icon& i = parent.emplace<Icon>(std::move(id), std::move(name), size.x);
    i.layout.height = Size::px(size.y);
    return i;
}

inline void fixed_size(Widget& w, f32 width, f32 height) {
    w.layout.width = Size::px(width);
    w.layout.height = Size::px(height);
}

/// Pins `w` to a point of its parent: `anchor` and `pivot` in 0..1.
inline void anchor(Widget& w, Vec2 anchor, Vec2 pivot, Vec2 offset = {0.0f, 0.0f}) {
    w.layout.position = Position::Anchored;
    w.layout.anchor = anchor;
    w.layout.pivot = pivot;
    w.layout.offset = offset;
}

} // namespace immune::gui
