// gui/core/Widget.h — the retained widget tree.
//
// Screens build their widgets once and then only change properties (a label's
// text, a bar's level) as the game state changes; the tree keeps layout,
// hover/press state and animation between frames, which is what makes the
// springy hover, the deny shake, the sloshing liquid and the transitions cheap
// to write.
//
// Every frame Gui runs: layout (measure + arrange) → input dispatch →
// update(dt) → draw. Layout is recomputed every frame: a HUD is a few hundred
// widgets, and a fresh layout is simpler than dirty tracking and always right.
#pragma once

#include "core/Types.h"
#include "gui/core/Layout.h"
#include "gui/draw/Affine2.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace immune::gui {

class DrawList;
class Gui;
struct Event;

class Widget {
public:
    explicit Widget(std::string id = {}) : id_(std::move(id)) {}
    virtual ~Widget() = default;
    Widget(const Widget&) = delete;
    Widget& operator=(const Widget&) = delete;

    // ---- Tree -------------------------------------------------------------
    Widget& add(std::unique_ptr<Widget> child);
    template <typename T, typename... Args>
    T& emplace(Args&&... args) {
        return static_cast<T&>(add(std::make_unique<T>(std::forward<Args>(args)...)));
    }
    /// Detaches and destroys a child. Safe during dispatch: destruction is
    /// deferred to the end of the frame by Gui.
    void remove(Widget& child);
    void clear_children();

    Widget* parent() const { return parent_; }
    const std::vector<std::unique_ptr<Widget>>& children() const { return children_; }
    const std::string& id() const { return id_; }
    /// Ids of this widget and its ancestors joined by '/', skipping empty ids:
    /// "hud/dock/neutrophil". This is what tests and the ui.click gym command
    /// address widgets by.
    std::string path() const;
    /// Descendant by relative path ("dock/neutrophil"). Null if absent.
    Widget* find(std::string_view relative_path);
    Gui* gui() const { return gui_; }

    // ---- Layout -----------------------------------------------------------
    LayoutParams layout;
    Rect rect() const { return rect_; }
    Vec2 size() const { return rect_.size(); }
    Vec2 center() const { return rect_.center(); }

    /// Outer size this widget wants given the space its parent offers.
    Vec2 measure(Vec2 available);
    /// Places this widget at `r` and lays its children out inside it.
    void arrange(Rect r);

    // ---- State ------------------------------------------------------------
    bool visible = true;
    /// Disabled widgets still hover (tooltips, "why not") but a click on one
    /// is a Deny, not a Click.
    bool enabled = true;
    /// Receives pointer events and blocks them from reaching the game world.
    bool interactive = false;
    /// Blocks the pointer from the world without being interactive itself (a
    /// panel's background).
    bool blocks_pointer = false;
    /// False: the pointer passes through this whole subtree (a screen that is
    /// fading out still draws but takes no clicks).
    bool accepts_pointer = true;
    /// Children are clipped to this widget's rect.
    bool clip_children = false;
    f32 opacity = 1.0f;
    /// Fade this subtree as one picture: while `opacity` < 1 it is drawn into
    /// an offscreen layer and composited once, so overlapping parts (a button
    /// on its panel) do not show through each other. Screen transitions.
    bool group_opacity = false;
    /// Extra transform about the widget's centre: animations (beat, wobble,
    /// press squash) that must not disturb layout.
    Affine2 anim_transform{};
    std::string tooltip;

    bool hovered() const;
    bool pressed() const;

    // ---- Hooks for subclasses ----------------------------------------------
    /// Size of the content (excluding padding) for Auto sizing. Default:
    /// the flow children laid out along `layout.axis`.
    virtual Vec2 content_size(Vec2 available);
    virtual void update(f32 dt);
    /// Draws this widget (not its children).
    virtual void draw_self(DrawList&) {}
    virtual void on_event(Event&) {}
    /// Pointer inside? Default: inside rect(). Shaped widgets test their shape.
    virtual bool hit_test(Vec2 p) const { return rect_.contains(p); }
    /// World-anchored children need a projection; Gui supplies it.
    virtual void draw(DrawList& dl);

protected:
    /// Lays out children inside `inner` (rect minus padding).
    virtual void arrange_children(Rect inner);
    Vec2 measured() const { return measured_; }

private:
    friend class Gui;
    void attach(Gui* gui);

    std::string id_;
    Widget* parent_ = nullptr;
    Gui* gui_ = nullptr;
    std::vector<std::unique_ptr<Widget>> children_;
    Rect rect_{};
    Vec2 measured_{0.0f, 0.0f};
};

} // namespace immune::gui
