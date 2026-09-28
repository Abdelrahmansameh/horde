// gui/core/Layout.h — layout parameters every widget carries.
//
// A small flexbox: a widget lays its FLOW children out in a row, a column or
// stacked on top of each other, with padding, gap, alignment on the cross axis
// and justification on the main axis; children can grow into spare space. A
// child can instead be ANCHORED to a point of its parent (a screen corner, the
// centre) or to a WORLD point projected through the game camera every frame
// (the tower popup, a marker over a lane).
//
// Sizes are logical pixels at the 1920x1080 reference the canvas is drawn at.
#pragma once

#include "core/Types.h"

#include <limits>

namespace immune::gui {

enum class Axis : u8 {
    Row,     ///< Children left to right.
    Column,  ///< Children top to bottom.
    Stack,   ///< Children on top of each other, each aligned in the whole box.
};

enum class Align : u8 { Start, Center, End, Stretch };
enum class Justify : u8 { Start, Center, End, SpaceBetween, SpaceAround };

struct Insets {
    f32 left = 0.0f, top = 0.0f, right = 0.0f, bottom = 0.0f;
    static constexpr Insets all(f32 v) { return Insets{v, v, v, v}; }
    static constexpr Insets xy(f32 x, f32 y) { return Insets{x, y, x, y}; }
    constexpr f32 horizontal() const { return left + right; }
    constexpr f32 vertical() const { return top + bottom; }
};

enum class SizeMode : u8 {
    Auto,     ///< Fit the content.
    Fixed,    ///< `value` px.
    Fill,     ///< All the space the parent offers on this axis.
    Percent,  ///< `value` (0..1) of the parent's space.
};

struct Size {
    SizeMode mode = SizeMode::Auto;
    f32 value = 0.0f;
    static constexpr Size fit() { return Size{SizeMode::Auto, 0.0f}; }
    static constexpr Size px(f32 v) { return Size{SizeMode::Fixed, v}; }
    static constexpr Size fill() { return Size{SizeMode::Fill, 0.0f}; }
    static constexpr Size pct(f32 v) { return Size{SizeMode::Percent, v}; }
};

enum class Position : u8 {
    Flow,      ///< Laid out by the parent's axis.
    Anchored,  ///< `anchor` of the parent's rect, minus `pivot` of own size, plus `offset`.
    World,     ///< The projected world point, minus `pivot` of own size, plus `offset`.
};

inline constexpr f32 kUnbounded = std::numeric_limits<f32>::infinity();

struct LayoutParams {
    // ---- As a child ----
    Size width{};
    Size height{};
    Vec2 min_size{0.0f, 0.0f};
    Vec2 max_size{kUnbounded, kUnbounded};
    /// Share of the parent's spare main-axis space (0 = none).
    f32 grow = 0.0f;
    /// Overrides the parent's cross-axis `align` for this child.
    bool has_align_self = false;
    Align align_self = Align::Start;
    Position position = Position::Flow;
    Vec2 anchor{0.0f, 0.0f};  ///< 0..1 of the parent rect (Anchored).
    Vec2 pivot{0.0f, 0.0f};   ///< 0..1 of own size (Anchored, World).
    Vec2 offset{0.0f, 0.0f};
    Vec2 world{0.0f, 0.0f};   ///< World-space point (World).

    // ---- As a container ----
    Axis axis = Axis::Column;
    Align align = Align::Start;
    Justify justify = Justify::Start;
    f32 gap = 0.0f;
    Insets padding{};
};

} // namespace immune::gui
