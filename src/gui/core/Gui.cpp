#include "gui/core/Gui.h"

#include "core/Math.h"
#include "gui/widgets/Widgets.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iterator>

namespace immune::gui {

namespace {
/// A press becomes a drag once the pointer moves this far (logical px).
constexpr f32 kDragThreshold = 4.0f;

bool is_ancestor_or_self(const Widget* a, const Widget* w) {
    for (const Widget* x = w; x != nullptr; x = x->parent()) {
        if (x == a) return true;
    }
    return false;
}
} // namespace

Gui::Gui() {
    for (usize i = 0; i < kLayerCount; ++i) {
        layers_[i] = std::make_unique<Widget>();
        layers_[i]->layout.axis = Axis::Stack;
        layers_[i]->layout.width = Size::fill();
        layers_[i]->layout.height = Size::fill();
        layers_[i]->attach(this);
    }
}

Gui::~Gui() = default;

bool Gui::init(const Assets& assets) {
    assets_ = assets;
    if (!fonts_.load(assets.fonts_dir)) {
        error_ = fonts_.error();
        return false;
    }
    icons_.load_dir(assets.icons_dir);
    if (!assets.theme_path.empty()) {
        if (!theme_.load(assets.theme_path, &error_)) return false;
        std::ifstream in(assets.theme_path);
        theme_text_.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    // The tooltip: one shared bubble in the Tooltip layer.
    auto tip = std::make_unique<Panel>("tooltip", theme_.shape("tooltip"));
    tip->layout.position = Position::Anchored;
    tip->layout.padding = Insets::xy(14.0f, 8.0f);
    tip->layout.max_size = Vec2{360.0f, kUnbounded};
    tip->blocks_pointer = false;
    tip->visible = false;
    tooltip_label_ = &tip->emplace<Label>("text", "", theme_.text("body"));
    tooltip_label_->wrap = true;
    tooltip_ = static_cast<Panel*>(&layer(LayerId::Tooltip).add(std::move(tip)));
    error_.clear();
    return true;
}

bool Gui::init_renderer() {
    renderer_ready_ = backend_.init();
    if (!renderer_ready_) error_ = backend_.error();
    return renderer_ready_;
}

void Gui::shutdown() {
    backend_.shutdown();
    renderer_ready_ = false;
}

bool Gui::reload_theme(std::string* error) {
    if (assets_.theme_path.empty()) return false;
    return theme_.load(assets_.theme_path, error);
}

bool Gui::reload_theme_if_changed(std::string* error) {
    if (assets_.theme_path.empty()) return false;
    std::ifstream in(assets_.theme_path);
    if (!in) return false;
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (text == theme_text_) return false;
    theme_text_ = std::move(text);
    return theme_.parse(theme_text_, error);
}

void Gui::set_viewport(Vec2 framebuffer, f32 scale_override) {
    framebuffer_ = Vec2{math::max(framebuffer.x, 1.0f), math::max(framebuffer.y, 1.0f)};
    const f32 s = scale_override > 0.0f ? scale_override : framebuffer_.y / kReferenceHeight;
    if (std::fabs(s - scale_) > 1e-4f) icons_.clear_bakes();  // re-bake icons pixel-exact
    scale_ = s;
    viewport_ = framebuffer_ / scale_;
}

Widget* Gui::find(std::string_view path) {
    for (auto& l : layers_) {
        if (Widget* w = l->find(path)) return w;
    }
    return nullptr;
}

bool Gui::is_hovered(const Widget* w) const {
    return w != nullptr && hover_target_ != nullptr && is_ancestor_or_self(w, hover_target_);
}

Widget* Gui::pick(Widget& w, Vec2 p) {
    if (!w.visible) return nullptr;
    if (w.clip_children && !w.rect().contains(p)) {
        return (w.interactive || w.blocks_pointer) && w.hit_test(p) ? &w : nullptr;
    }
    const auto& kids = w.children();
    for (auto it = kids.rbegin(); it != kids.rend(); ++it) {
        if (Widget* hit = pick(**it, p)) return hit;
    }
    if ((w.interactive || w.blocks_pointer) && w.hit_test(p)) return &w;
    return nullptr;
}

void Gui::dispatch(Event e, Widget* target) {
    e.target = target;
    for (Widget* w = target; w != nullptr && !e.handled; w = w->parent()) w->on_event(e);
}

void Gui::update_tree(Widget& w, f32 dt) {
    if (!w.visible) return;
    w.update(dt);
    // Index loop: an update may add children.
    for (usize i = 0; i < w.children().size(); ++i) update_tree(*w.children()[i], dt);
}

void Gui::frame(const PointerInput& in, f32 dt) {
    time_ += dt;

    // ---- Layout ---------------------------------------------------------------
    for (auto& l : layers_) {
        l->measure(viewport_);
        l->arrange(Rect{Vec2{0.0f, 0.0f}, viewport_});
    }

    // ---- Pointer ----------------------------------------------------------------
    const bool moved = in.present && math::length_sq(in.pos - pointer_) > 0.25f;
    if (moved) forced_hover_ = nullptr;
    pointer_ = in.pos;

    Widget* picked = nullptr;
    if (forced_hover_ != nullptr) {
        picked = forced_hover_;
    } else if (in.present) {
        for (usize i = kLayerCount; i-- > 0;) {
            if ((picked = pick(*layers_[i], pointer_)) != nullptr) break;
        }
    }
    // Events go to the nearest interactive widget; a plain blocking panel
    // only stops the pointer reaching the world.
    Widget* target = picked;
    while (target != nullptr && !target->interactive) target = target->parent();

    if (target != hover_target_) {
        if (hover_target_ != nullptr) dispatch(Event{EventType::Leave, pointer_}, hover_target_);
        hover_time_ = 0.0f;
        hover_target_ = target;
        if (hover_target_ != nullptr) {
            Event enter{EventType::Enter, pointer_};
            enter.handled = true;  // Enter does not bubble
            enter.target = hover_target_;
            hover_target_->on_event(enter);
        }
    } else {
        hover_time_ += dt;
    }

    constexpr usize kLeft = 0;
    if (in.pressed[kLeft] && hover_target_ != nullptr) {
        press_target_ = hover_target_;
        press_origin_ = last_drag_ = pointer_;
        dragging_ = false;
        dispatch(Event{EventType::Down, pointer_}, press_target_);
    }
    if (press_target_ != nullptr && in.down[kLeft]) {
        if (!dragging_ && math::length(pointer_ - press_origin_) > kDragThreshold) {
            dragging_ = true;
            dispatch(Event{EventType::DragStart, pointer_}, press_target_);
        }
        if (dragging_ && math::length_sq(pointer_ - last_drag_) > 0.0f) {
            Event drag{EventType::Drag, pointer_};
            drag.delta = pointer_ - last_drag_;
            dispatch(drag, press_target_);
            last_drag_ = pointer_;
        }
    }
    if (press_target_ != nullptr && (in.released[kLeft] || !in.down[kLeft])) {
        Widget* t = press_target_;
        dispatch(Event{EventType::Up, pointer_}, t);
        if (dragging_) {
            dispatch(Event{EventType::DragEnd, pointer_}, t);
        } else if (is_hovered(t)) {
            dispatch(Event{t->enabled ? EventType::Click : EventType::Deny, pointer_}, t);
        }
        press_target_ = nullptr;
        dragging_ = false;
    }
    // Right and middle buttons: a click on release over the pressed widget,
    // no capture or drag.
    for (usize b = 1; b < 3; ++b) {
        if (in.released[b] && hover_target_ != nullptr) {
            Event e{hover_target_->enabled ? EventType::Click : EventType::Deny, pointer_};
            e.button = static_cast<PointerButton>(b);
            dispatch(e, hover_target_);
        }
    }
    if (in.wheel != 0.0f && hover_target_ != nullptr) {
        Event e{EventType::Wheel, pointer_};
        e.wheel = in.wheel;
        dispatch(e, hover_target_);
    }
    wants_pointer_ = picked != nullptr || press_target_ != nullptr;

    // ---- Animation and per-frame logic ------------------------------------------
    for (auto& l : layers_) update_tree(*l, dt);
    update_tooltip(dt);

    retired_.clear();
}

void Gui::update_tooltip(f32) {
    if (tooltip_ == nullptr) return;
    const f32 delay = theme_.number("tooltip_delay", 0.45f);
    const bool show = hover_target_ != nullptr && !hover_target_->tooltip.empty() && hover_time_ >= delay &&
                      press_target_ == nullptr;
    if (!show) {
        tooltip_->visible = false;
        return;
    }
    tooltip_label_->set_text(hover_target_->tooltip);
    tooltip_->visible = true;
    // Above-right of the pointer, flipped to stay on screen.
    const Vec2 size = tooltip_->measure(viewport_);
    Vec2 pos = pointer_ + Vec2{16.0f, -size.y - 12.0f};
    if (pos.x + size.x > viewport_.x - 8.0f) pos.x = pointer_.x - size.x - 16.0f;
    if (pos.y < 8.0f) pos.y = pointer_.y + 24.0f;
    tooltip_->layout.offset = pos;
    tooltip_->arrange(Rect{pos, pos + size});
}

void Gui::draw(DrawList& dl) {
    dl.reset(viewport_, 1.0f / scale_, time_);
    for (auto& l : layers_) l->draw(dl);
}

void Gui::render(i32 framebuffer_width, i32 framebuffer_height) {
    if (!renderer_ready_) return;
    draw(draw_list_);
    backend_.render(draw_list_, &fonts_.atlas(), &icons_.atlas(), framebuffer_width, framebuffer_height, scale_);
}

bool Gui::click(std::string_view path) {
    Widget* w = find(path);
    if (w == nullptr || !w->visible) return false;
    for (const Widget* p = w->parent(); p != nullptr; p = p->parent()) {
        if (!p->visible) return false;
    }
    Widget* prev_hover = hover_target_;
    hover_target_ = w;
    dispatch(Event{EventType::Down, w->center()}, w);
    dispatch(Event{EventType::Up, w->center()}, w);
    dispatch(Event{w->enabled ? EventType::Click : EventType::Deny, w->center()}, w);
    hover_target_ = prev_hover;
    return true;
}

bool Gui::hover(std::string_view path) {
    Widget* w = find(path);
    if (w == nullptr || !w->visible) return false;
    forced_hover_ = w;
    return true;
}

std::string Gui::dump() const {
    std::string out;
    std::function<void(const Widget&, int)> walk = [&](const Widget& w, int depth) {
        if (!w.visible) return;
        if (!w.id().empty()) {
            char buf[256];
            const Rect r = w.rect();
            std::snprintf(buf, sizeof(buf), "%*s%s  [%.0f,%.0f %.0fx%.0f]%s%s%s\n", depth * 2, "", w.path().c_str(),
                          r.min.x, r.min.y, r.size().x, r.size().y, w.interactive ? " interactive" : "",
                          w.enabled ? "" : " disabled", is_hovered(&w) ? " hovered" : "");
            out += buf;
        }
        for (const auto& c : w.children()) walk(*c, w.id().empty() ? depth : depth + 1);
    };
    static constexpr const char* kNames[kLayerCount] = {"world", "hud", "popup", "modal", "tooltip", "toast"};
    for (usize i = 0; i < kLayerCount; ++i) {
        if (layers_[i]->children().empty()) continue;
        out += std::string("# ") + kNames[i] + "\n";
        walk(*layers_[i], 0);
    }
    return out;
}

} // namespace immune::gui
