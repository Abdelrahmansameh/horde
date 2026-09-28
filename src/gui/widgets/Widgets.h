// gui/widgets/Widgets.h — the generic widget library.
//
// Game-agnostic building blocks; the IMMUNE screens (src/ui) compose them into
// build cards, ability cells, the tower popup and so on. Shapes and text
// styles normally come from the Theme by name so the look lives in
// assets/config/ui_theme.json.
#pragma once

#include "core/Types.h"
#include "gui/Color.h"
#include "gui/anim/Anim.h"
#include "gui/core/Event.h"
#include "gui/core/Widget.h"
#include "gui/draw/Shape.h"
#include "gui/text/Text.h"

#include <functional>
#include <string>

namespace immune::gui {

/// A container painted with an SDF shape (a membrane panel, a pill, a cell).
/// Blocks the pointer from the world by default, and hit tests against the
/// drawn outline (wobble included), not its rect.
class Panel : public Widget {
public:
    explicit Panel(std::string id = {}, const ShapeDesc& shape = {});

    ShapeDesc shape;
    /// Shape rect relative to the widget rect: positive shrinks, negative grows.
    Insets shape_inset{};

    /// The shape as it will be drawn this frame (geometry filled in).
    ShapeDesc resolved_shape() const { return resolve(shape); }
    /// `base`'s style with this widget's geometry.
    ShapeDesc resolve(const ShapeDesc& base) const;

    void draw_self(DrawList& dl) override;
    bool hit_test(Vec2 p) const override;
};

/// Text. Auto-sizes to its content; with `wrap` it wraps to the width its
/// parent offers; with `ellipsize` it shortens to fit instead.
class Label : public Widget {
public:
    explicit Label(std::string id = {}, std::string text = {}, const TextStyle& style = {});

    void set_text(std::string t) { text_ = std::move(t); }
    const std::string& text() const { return text_; }
    TextStyle style;
    TextAlign align = TextAlign::Left;
    bool wrap = false;
    bool ellipsize = false;

    Vec2 content_size(Vec2 available) override;
    void draw_self(DrawList& dl) override;

private:
    std::string text_;
};

/// An icon from the IconLibrary, fitted into the widget's rect.
class Icon : public Widget {
public:
    explicit Icon(std::string id = {}, std::string name = {}, f32 size = 32.0f);

    std::string name;
    Color tint = kWhite;

    void draw_self(DrawList& dl) override;
};

/// A Panel that can be clicked: springs up on hover, squashes on press,
/// shakes and plays Deny when clicked while disabled.
class Button : public Panel {
public:
    explicit Button(std::string id = {}, const ShapeDesc& shape = {});

    std::function<void()> on_click;
    std::function<void()> on_deny;
    std::function<void()> on_right_click;
    /// Shape used while `enabled` is false (defaults to `shape`, dimmed).
    bool has_disabled_shape = false;
    ShapeDesc disabled_shape;
    f32 hover_scale = 1.04f;
    f32 press_scale = 0.95f;

    void on_event(Event& e) override;
    void update(f32 dt) override;
    void draw_self(DrawList& dl) override;

    /// Starts the deny shake (also triggered by a disabled click).
    void shake();

private:
    Spring scale_{1.0f};
    Spring shake_{0.0f, 900.0f, 18.0f};
};

/// A fluid-filled bar or cell whose level eases to its target.
class Meter : public Panel {
public:
    explicit Meter(std::string id = {}, const ShapeDesc& shape = {});

    /// Moves the liquid to `level` (0..1) over `seconds`.
    void set_level(f32 level, f32 seconds = 0.35f);
    f32 level() const { return level_.value(); }

    void update(f32 dt) override;
    void draw_self(DrawList& dl) override;

private:
    Tween level_{0.0f};
};

/// A progress ring or spinning dashed halo (ShapeKind::Arc).
class Ring : public Widget {
public:
    explicit Ring(std::string id = {}, const ShapeDesc& shape = {});

    ShapeDesc shape;
    /// 0..1 of the circle, clockwise from 12 o'clock. 1 = full ring.
    f32 progress = 1.0f;
    /// Radians per second the dashes travel (spin); 0 = still.
    f32 spin_speed = 0.0f;

    void draw_self(DrawList& dl) override;
};

/// Invisible flexible space.
class Spacer : public Widget {
public:
    explicit Spacer(f32 grow = 1.0f) { layout.grow = grow; }
};

} // namespace immune::gui
