#include "gui/core/Widget.h"

#include "core/Math.h"
#include "gui/core/Gui.h"
#include "gui/draw/DrawList.h"

#include <cmath>

namespace immune::gui {

namespace {

f32 main_of(Vec2 v, Axis a) { return a == Axis::Row ? v.x : v.y; }
f32 cross_of(Vec2 v, Axis a) { return a == Axis::Row ? v.y : v.x; }
Vec2 make(Axis a, f32 main, f32 cross) { return a == Axis::Row ? Vec2{main, cross} : Vec2{cross, main}; }
const Size& main_size(const LayoutParams& l, Axis a) { return a == Axis::Row ? l.width : l.height; }
const Size& cross_size(const LayoutParams& l, Axis a) { return a == Axis::Row ? l.height : l.width; }

f32 minus(f32 v, f32 d) { return std::isinf(v) ? v : math::max(v - d, 0.0f); }

/// Space a child's own size spec leaves for its content on one axis.
f32 inner_available(const Size& s, f32 available, f32 padding) {
    switch (s.mode) {
        case SizeMode::Fixed: return math::max(s.value - padding, 0.0f);
        case SizeMode::Percent: return std::isinf(available) ? available : math::max(available * s.value - padding, 0.0f);
        case SizeMode::Fill:
        case SizeMode::Auto: return minus(available, padding);
    }
    return available;
}

f32 resolve(const Size& s, f32 available, f32 content) {
    switch (s.mode) {
        case SizeMode::Auto: return content;
        case SizeMode::Fixed: return s.value;
        case SizeMode::Fill: return std::isinf(available) ? content : available;
        case SizeMode::Percent: return std::isinf(available) ? content : available * s.value;
    }
    return content;
}

f32 align_offset(Align a, f32 space, f32 size) {
    switch (a) {
        case Align::Center: return (space - size) * 0.5f;
        case Align::End: return space - size;
        case Align::Start:
        case Align::Stretch: return 0.0f;
    }
    return 0.0f;
}

} // namespace

// ---- Tree ------------------------------------------------------------------------

Widget& Widget::add(std::unique_ptr<Widget> child) {
    Widget& ref = *child;
    child->parent_ = this;
    child->attach(gui_);
    children_.push_back(std::move(child));
    return ref;
}

void Widget::attach(Gui* gui) {
    gui_ = gui;
    for (auto& c : children_) c->attach(gui);
}

void Widget::remove(Widget& child) {
    for (auto it = children_.begin(); it != children_.end(); ++it) {
        if (it->get() != &child) continue;
        std::unique_ptr<Widget> owned = std::move(*it);
        children_.erase(it);
        owned->parent_ = nullptr;
        if (gui_ != nullptr) gui_->retire(std::move(owned));
        return;
    }
}

void Widget::clear_children() {
    while (!children_.empty()) remove(*children_.back());
}

std::string Widget::path() const {
    std::string out;
    for (const Widget* w = this; w != nullptr; w = w->parent_) {
        if (w->id_.empty()) continue;
        out = out.empty() ? w->id_ : w->id_ + "/" + out;
    }
    return out;
}

Widget* Widget::find(std::string_view relative_path) {
    if (relative_path.empty()) return this;
    const usize slash = relative_path.find('/');
    const std::string_view head = relative_path.substr(0, slash);
    const std::string_view rest = slash == std::string_view::npos ? std::string_view{} : relative_path.substr(slash + 1);
    for (auto& c : children_) {
        if (c->id_ == head) {
            if (Widget* w = c->find(rest)) return w;
        } else if (c->id_.empty()) {
            // Anonymous containers are transparent to paths.
            if (Widget* w = c->find(relative_path)) return w;
        }
    }
    return nullptr;
}

bool Widget::hovered() const { return gui_ != nullptr && gui_->is_hovered(this); }
bool Widget::pressed() const { return gui_ != nullptr && gui_->pressed_widget() == this; }

// ---- Layout ------------------------------------------------------------------------

Vec2 Widget::measure(Vec2 available) {
    const Insets& pad = layout.padding;
    const Vec2 inner{inner_available(layout.width, available.x, pad.horizontal()),
                     inner_available(layout.height, available.y, pad.vertical())};
    const bool need_content = layout.width.mode == SizeMode::Auto || layout.height.mode == SizeMode::Auto ||
                              (layout.width.mode == SizeMode::Fill && std::isinf(available.x)) ||
                              (layout.height.mode == SizeMode::Fill && std::isinf(available.y));
    const Vec2 content = need_content ? content_size(inner) : Vec2{0.0f, 0.0f};
    Vec2 s{resolve(layout.width, available.x, content.x + pad.horizontal()),
           resolve(layout.height, available.y, content.y + pad.vertical())};
    s.x = math::clamp(s.x, layout.min_size.x, layout.max_size.x);
    s.y = math::clamp(s.y, layout.min_size.y, layout.max_size.y);
    measured_ = s;
    return s;
}

Vec2 Widget::content_size(Vec2 available) {
    const Axis axis = layout.axis;
    Vec2 total{0.0f, 0.0f};
    usize n = 0;
    for (auto& c : children_) {
        if (!c->visible || c->layout.position != Position::Flow) continue;
        if (axis == Axis::Stack) {
            const Vec2 s = c->measure(available);
            total.x = math::max(total.x, s.x);
            total.y = math::max(total.y, s.y);
        } else {
            // Along the main axis a child sizes to its content; across it the
            // container's width is on offer (so text can wrap to it).
            const Vec2 offer = make(axis, kUnbounded, cross_of(available, axis));
            const Vec2 s = c->measure(offer);
            total = make(axis, main_of(total, axis) + main_of(s, axis),
                         math::max(cross_of(total, axis), cross_of(s, axis)));
        }
        ++n;
    }
    if (axis != Axis::Stack && n > 1) {
        total = make(axis, main_of(total, axis) + layout.gap * static_cast<f32>(n - 1), cross_of(total, axis));
    }
    return total;
}

void Widget::arrange(Rect r) {
    rect_ = r;
    const Insets& pad = layout.padding;
    Rect inner{Vec2{r.min.x + pad.left, r.min.y + pad.top}, Vec2{r.max.x - pad.right, r.max.y - pad.bottom}};
    inner.max.x = math::max(inner.max.x, inner.min.x);
    inner.max.y = math::max(inner.max.y, inner.min.y);
    arrange_children(inner);
}

void Widget::arrange_children(Rect inner) {
    const Vec2 space = inner.size();
    const Axis axis = layout.axis;

    std::vector<Widget*> flow;
    for (auto& c : children_) {
        if (c->visible && c->layout.position == Position::Flow) flow.push_back(c.get());
    }

    auto cross_align = [&](const Widget& c) { return c.layout.has_align_self ? c.layout.align_self : layout.align; };

    if (axis == Axis::Stack) {
        for (Widget* c : flow) {
            Vec2 s = c->measure(space);
            const Align a = cross_align(*c);
            if (c->layout.width.mode == SizeMode::Fill || a == Align::Stretch) s.x = space.x;
            if (c->layout.height.mode == SizeMode::Fill || a == Align::Stretch) s.y = space.y;
            const Vec2 pos{inner.min.x + align_offset(a, space.x, s.x), inner.min.y + align_offset(a, space.y, s.y)};
            c->arrange(Rect{pos, pos + s});
        }
    } else if (!flow.empty()) {
        const usize n = flow.size();
        std::vector<Vec2> sizes(n);
        std::vector<f32> main(n), grow(n);
        f32 total = layout.gap * static_cast<f32>(n - 1);
        f32 grow_sum = 0.0f;
        for (usize i = 0; i < n; ++i) {
            Widget* c = flow[i];
            sizes[i] = c->measure(make(axis, main_of(space, axis), cross_of(space, axis)));
            const bool fill_main = main_size(c->layout, axis).mode == SizeMode::Fill;
            main[i] = fill_main ? 0.0f : main_of(sizes[i], axis);
            grow[i] = c->layout.grow > 0.0f ? c->layout.grow : (fill_main ? 1.0f : 0.0f);
            total += main[i];
            grow_sum += grow[i];
        }
        f32 free = main_of(space, axis) - total;
        if (free > 0.0f && grow_sum > 0.0f) {
            for (usize i = 0; i < n; ++i) main[i] += free * grow[i] / grow_sum;
            free = 0.0f;
        }
        f32 cursor = 0.0f, between = layout.gap;
        if (free > 0.0f) {
            switch (layout.justify) {
                case Justify::Start: break;
                case Justify::Center: cursor = free * 0.5f; break;
                case Justify::End: cursor = free; break;
                case Justify::SpaceBetween:
                    if (n > 1) between += free / static_cast<f32>(n - 1);
                    break;
                case Justify::SpaceAround:
                    between += free / static_cast<f32>(n);
                    cursor = free / static_cast<f32>(n) * 0.5f;
                    break;
            }
        }
        for (usize i = 0; i < n; ++i) {
            Widget* c = flow[i];
            const Align a = cross_align(*c);
            f32 cross = cross_of(sizes[i], axis);
            if (a == Align::Stretch || cross_size(c->layout, axis).mode == SizeMode::Fill) cross = cross_of(space, axis);
            const f32 cross_pos = align_offset(a, cross_of(space, axis), cross);
            const Vec2 pos = inner.min + make(axis, cursor, cross_pos);
            c->arrange(Rect{pos, pos + make(axis, main[i], cross)});
            cursor += main[i] + between;
        }
    }

    // Anchored and world-anchored children, relative to this widget's rect.
    for (auto& c : children_) {
        if (!c->visible || c->layout.position == Position::Flow) continue;
        const Vec2 s = c->measure(rect_.size());
        Vec2 at;
        if (c->layout.position == Position::Anchored) {
            at = rect_.min + Vec2{c->layout.anchor.x * rect_.size().x, c->layout.anchor.y * rect_.size().y};
        } else {
            at = gui_ != nullptr ? gui_->project(c->layout.world) : Vec2{-1e6f, -1e6f};
        }
        const Vec2 pos = at - Vec2{c->layout.pivot.x * s.x, c->layout.pivot.y * s.y} + c->layout.offset;
        c->arrange(Rect{pos, pos + s});
    }
}

// ---- Frame ----------------------------------------------------------------------------

void Widget::update(f32) {}

void Widget::draw(DrawList& dl) {
    if (!visible || opacity <= 0.0f) return;
    const bool xf = !anim_transform.is_identity();
    if (xf) dl.push_transform(Affine2::about(center(), anim_transform));
    const bool faded = opacity < 1.0f;
    if (faded) dl.push_alpha(opacity);
    draw_self(dl);
    if (clip_children) dl.push_clip_rect(rect_);
    for (auto& c : children_) c->draw(dl);
    if (clip_children) dl.pop_clip_rect();
    if (faded) dl.pop_alpha();
    if (xf) dl.pop_transform();
}

} // namespace immune::gui
