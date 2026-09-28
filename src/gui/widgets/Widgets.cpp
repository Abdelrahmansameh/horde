#include "gui/widgets/Widgets.h"

#include "core/Math.h"
#include "gui/core/Gui.h"
#include "gui/draw/DrawList.h"
#include "gui/draw/ShapeSdf.h"

#include <cmath>
#include <functional>

namespace immune::gui {

namespace {
/// Different panels wobble differently: seed from the id unless one is set.
f32 seed_from(const std::string& id) {
    return static_cast<f32>(std::hash<std::string>{}(id) % 997u) + 1.0f;
}
} // namespace

// ---- Panel ------------------------------------------------------------------------

Panel::Panel(std::string id, const ShapeDesc& s) : Widget(std::move(id)), shape(s) {
    blocks_pointer = true;
    if (shape.wobble_seed == 0.0f) shape.wobble_seed = seed_from(this->id());
}

ShapeDesc Panel::resolve(const ShapeDesc& base) const {
    ShapeDesc d = base;
    const Rect r = rect();
    const Rect s{Vec2{r.min.x + shape_inset.left, r.min.y + shape_inset.top},
                 Vec2{r.max.x - shape_inset.right, r.max.y - shape_inset.bottom}};
    d.center = s.center();
    d.half_size = Vec2{math::max(s.size().x, 0.0f), math::max(s.size().y, 0.0f)} * 0.5f;
    if (d.kind == ShapeKind::Arc) d.half_size = Vec2{math::min(d.half_size.x, d.half_size.y)};
    return d;
}

void Panel::draw_self(DrawList& dl) { dl.shape(resolved_shape()); }

bool Panel::hit_test(Vec2 p) const {
    const ShapeDesc d = resolved_shape();
    const f32 t = gui() != nullptr ? gui()->time() : 0.0f;
    return sdf::contains(make_record(d), p, t, d.stroke_width * 0.5f);
}

// ---- Label ------------------------------------------------------------------------

Label::Label(std::string id, std::string text, const TextStyle& s)
    : Widget(std::move(id)), style(s), text_(std::move(text)) {}

Vec2 Label::content_size(Vec2 available) {
    if (gui() == nullptr || !gui()->fonts().loaded() || text_.empty()) {
        return Vec2{0.0f, style.size * style.line_height};
    }
    const f32 wrap_width = wrap && !std::isinf(available.x) ? available.x : 0.0f;
    if (wrap_width != measured_wrap_ || text_ != measured_text_ || !(style == measured_style_)) {
        measured_size_ = gui()->text().measure(text_, style, wrap_width);
        measured_text_ = text_;
        measured_style_ = style;
        measured_wrap_ = wrap_width;
    }
    Vec2 s = measured_size_;
    if (ellipsize && !std::isinf(available.x)) s.x = math::min(s.x, available.x);
    return s;
}

void Label::draw_self(DrawList& dl) {
    if (gui() == nullptr || text_.empty()) return;
    const Rect r = rect();
    if (ellipsize) {
        gui()->text().draw_ellipsized(dl, text_, style, r.min, r.size().x, align);
    } else {
        gui()->text().draw(dl, text_, style, r.min, r.size().x, align, wrap);
    }
}

// ---- Icon --------------------------------------------------------------------------

Icon::Icon(std::string id, std::string n, f32 size) : Widget(std::move(id)), name(std::move(n)) {
    layout.width = Size::px(size);
    layout.height = Size::px(size);
}

void Icon::draw_self(DrawList& dl) {
    if (gui() == nullptr || name.empty()) return;
    gui()->icons().draw(dl, name, rect(), tint, gui()->scale(), grayscale);
}

// ---- Button -------------------------------------------------------------------------

Button::Button(std::string id, const ShapeDesc& s) : Panel(std::move(id), s) { interactive = true; }

void Button::shake() { shake_.kick(420.0f); }

void Button::on_event(Event& e) {
    Gui* g = gui();
    switch (e.type) {
        case EventType::Enter:
            if (enabled && g != nullptr) g->play(UiSound::Hover);
            break;
        case EventType::Click:
            if (e.button == PointerButton::Right) {
                if (on_right_click) on_right_click();
                e.handled = true;
                break;
            }
            if (g != nullptr) g->play(UiSound::Click);
            if (on_click) on_click();
            e.handled = true;
            break;
        case EventType::Deny:
            if (e.button != PointerButton::Left) break;
            shake();
            if (g != nullptr) g->play(UiSound::Deny);
            if (on_deny) on_deny();
            e.handled = true;
            break;
        default:
            break;
    }
}

void Button::update(f32 dt) {
    f32 target = 1.0f;
    if (enabled && hovered()) target = hover_scale;
    if (enabled && pressed()) target = press_scale;
    scale_.set_target(target);
    scale_.update(dt);
    shake_.update(dt);
    nudge_x_.set_target(nudge.x);
    nudge_y_.set_target(nudge.y);
    nudge_x_.update(dt);
    nudge_y_.update(dt);
    anim_transform = Affine2::translate(Vec2{nudge_x_.value() + shake_.value() * 0.05f, nudge_y_.value()}) *
                     Affine2::scale(scale_.value());
}

void Button::draw_self(DrawList& dl) {
    if (enabled || !has_disabled_shape) {
        Panel::draw_self(dl);
        return;
    }
    ShapeDesc d = disabled_shape;
    d.wobble_seed = shape.wobble_seed;
    dl.shape(resolve(d));
}

// ---- Meter ----------------------------------------------------------------------------

Meter::Meter(std::string id, const ShapeDesc& s) : Panel(std::move(id), s) {
    shape.kind = ShapeKind::Fluid;
    level_.snap(math::saturate(s.level));
}

void Meter::set_level(f32 level, f32 seconds) {
    level = math::saturate(level);
    if (std::fabs(level - level_.target()) < 1e-4f) return;
    if (seconds <= 0.0f) level_.snap(level);
    else level_.start(level, seconds, Ease::OutCubic);
}

void Meter::update(f32 dt) { level_.update(dt); }

void Meter::draw_self(DrawList& dl) {
    ShapeDesc d = resolved_shape();
    d.level = level_.value();
    dl.shape(d);
}

// ---- Ring --------------------------------------------------------------------------------

Ring::Ring(std::string id, const ShapeDesc& s) : Widget(std::move(id)), shape(s) { shape.kind = ShapeKind::Arc; }

void Ring::draw_self(DrawList& dl) {
    ShapeDesc d = shape;
    const Rect r = rect();
    d.center = r.center();
    // Radius to the stroke's centreline, so the ring fits inside the rect.
    const f32 radius = math::min(r.size().x, r.size().y) * 0.5f - d.stroke_width * 0.5f;
    d.half_size = Vec2{radius, radius};
    d.arc_start = -math::kPi * 0.5f;
    d.arc_sweep = math::saturate(progress) * math::kTwoPi;
    if (spin_speed != 0.0f && gui() != nullptr) d.dash_offset -= gui()->time() * spin_speed * radius;
    if (d.arc_sweep <= 1e-4f) return;
    dl.shape(d);
}

} // namespace immune::gui
