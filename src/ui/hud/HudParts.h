// ui/hud/HudParts.h — the in-match HUD's composite widgets.
//
// Each part is built from gui widgets and styled from the theme, and each has
// a sync() that takes the plain HudModel data it shows. Shapes, sizes and
// colours follow the design canvas's HUD artboards (docs/ui-concepts/canvas:
// Main, Placing, Inspect, Prep, Critical).
#pragma once

#include "gui/anim/Anim.h"
#include "gui/widgets/Widgets.h"
#include "ui/hud/HudModel.h"

#include <functional>
#include <string>
#include <vector>

namespace immune::gui { class Gui; class Theme; }

namespace immune::ui {

using gui::Color;
using gui::kTransparent;
using gui::kWhite;

/// A keyboard key ("Q", "LMB", "Space") as a white keycap.
class KeyCap : public gui::Panel {
public:
    KeyCap(gui::Gui& g, std::string id, std::string key);
};

/// A small filled pill with a label: the hotkey number badge on a card.
class Badge : public gui::Panel {
public:
    Badge(gui::Gui& g, std::string id, std::string text);
    gui::Label* label = nullptr;
};

/// "PLACING" / "AIMING" tag floating above a card or cell.
class Tag : public gui::Panel {
public:
    Tag(gui::Gui& g, std::string id, std::string text);
};

enum class DockEdge : u8 { None, Left, Right, Top, Bottom };

/// A panel the player can move by dragging it (or anything inside it: a drag
/// that starts on a card moves the panel instead of clicking the card). Moves
/// its layout offset, and keeps itself on screen. A docked panel slides behind
/// an edge at its original size, leaving an interactive strip exposed.
class DragPanel : public gui::Panel {
public:
    DragPanel(std::string id, const gui::ShapeDesc& shape);
    void on_event(gui::Event& e) override;
    void before_layout(f32 dt) override;
    void update(f32 dt) override;
    static constexpr f32 kPeek = 72.0f;
    bool dockable = false;
    std::function<void(DockEdge)> on_dock_changed;
    std::vector<gui::Widget*> dock_obstacles;
    DockEdge dock_edge() const { return edge_; }
    bool dragging() const { return dragging_; }
    f32 reveal() const { return math::saturate(reveal_.value()); }

protected:
    /// Called as the panel takes or leaves an edge, before it is placed
    /// there, so a subclass can reshape itself for that edge.
    virtual void dock_changed(DockEdge) {}

private:
    Rect layout_rect();
    void move_to(Vec2 position);
    void clamp_on_screen();
    /// `settle` avoids every obstacle; otherwise a dragged rail shoves docked
    /// panels along their edges instead of stepping around them.
    void pin_to_edge(bool settle = false);
    void dock(DockEdge edge);
    /// `release` also takes an edge the panel is resting on, whatever the
    /// final movement was.
    DockEdge nearest_edge(Vec2 movement, f32 projection = 0.0f, bool release = false);
    /// Limits a dragged move so docked panels are only pushed along their
    /// rails, never pressed into their edge, and only while they have room.
    Vec2 limit_push(Rect from, Vec2 delta);
    void record_motion();
    /// The panel's own velocity over the last few frames; zero once it has
    /// stood still that long, even while the pointer keeps pushing.
    Vec2 motion_velocity();
    f32 slide_distance();
    DockEdge edge_ = DockEdge::None;
    DockEdge undocked_from_ = DockEdge::None;
    gui::Spring reveal_{1.0f, 500.0f, 44.0f};
    gui::Spring entry_{0.0f, 500.0f, 44.0f};
    Vec2 entry_offset_{0.0f, 0.0f};
    // Where the panel itself has been during the drag, newest last, to read
    // its own velocity rather than the pointer's.
    struct MotionSample {
        f32 time;
        Vec2 pos;
    };
    std::vector<MotionSample> motion_;
    f32 press_reveal_target_ = 1.0f;
    f32 dock_along_ = 0.0f;
    f32 undock_pull_ = 0.0f;
    bool dragging_ = false;
    bool input_layout_changed_ = false;
    bool newly_docked_ = false;
};

/// Stable full-menu dimensions; the same cards slide between their full rows
/// and an icon strip along the drawer's exposed border.
class BuildDock : public DragPanel {
public:
    using DragPanel::DragPanel;
    Vec2 content_size(Vec2 available) override;
protected:
    void arrange_children(Rect inner) override;
};

/// The ability panel: a row of ability cells that moves and docks like the
/// build dock. A side edge stands it on end so its cells run along that edge;
/// docked anywhere, each cell shrinks into the exposed strip.
class AbilityBar : public DragPanel {
public:
    AbilityBar(std::string id, const gui::ShapeDesc& shape);
protected:
    void arrange_children(Rect inner) override;
    void dock_changed(DockEdge edge) override;
};

/// A tower card in the build dock: key badge, cost, cell icon, name; greyed
/// when the player cannot afford it; lifted while armed.
class BuildCard : public gui::Button {
public:
    BuildCard(gui::Gui& g, TowerType type, u32 slot);
    void sync(const HudTowerCard& card, u32 atp, bool armed);
    void on_event(gui::Event& e) override;
    void update(f32 dt) override;
    void set_compact(bool compact);
    void set_reveal(f32 reveal);
    bool compact() const { return compact_; }

    TowerType type;

protected:
    void arrange_children(Rect inner) override;

private:
    void sync_availability_style();
    gui::Gui& gui_;
    Badge* badge_ = nullptr;
    gui::Label* cost_ = nullptr;
    gui::Icon* icon_ = nullptr;
    gui::Label* name_ = nullptr;
    gui::Label* short_ = nullptr;
    gui::Widget* names_ = nullptr;
    gui::Widget* price_ = nullptr;
    HudTowerCard card_{};
    u32 atp_ = 0;
    bool armed_ = false;
    bool compact_ = false;
    f32 reveal_ = 1.0f;
    gui::Tween deny_tint_{0.0f};
};

/// An ability: a fluid cell filling as the cooldown recharges, the ability's
/// glyph (dim until ready), the seconds left, a pulsing halo and a wobble when
/// ready, and its keycap and short name underneath.
class AbilityCell : public gui::Widget {
public:
    AbilityCell(gui::Gui& g, game::AbilityId id, std::string key);
    void sync(const HudAbility& a, bool armed);
    void update(f32 dt) override;
    /// 1 is the full cell; below that it shrinks toward a docked icon and its
    /// keycap and name fade out.
    void set_reveal(f32 reveal);

    game::AbilityId ability;
    gui::Button* button = nullptr;

protected:
    void arrange_children(Rect inner) override;

private:
    gui::Gui& gui_;
    gui::Widget* cell_ = nullptr;
    gui::Widget* keys_ = nullptr;
    gui::Ring* halo_ = nullptr;
    gui::Icon* glyph_ = nullptr;
    gui::Label* timer_ = nullptr;
    gui::Label* name_ = nullptr;
    Tag* tag_ = nullptr;
    bool ready_ = false;
    f32 reveal_ = 1.0f;
};

/// One family row of the next-wave panel: icon, name, share bar, count.
class WaveRow : public gui::Widget {
public:
    WaveRow(gui::Gui& g, std::string id);
    void sync(const HudFamilyCount& f, u32 total);

private:
    gui::Gui& gui_;
    gui::Icon* icon_ = nullptr;
    gui::Label* name_ = nullptr;
    gui::Meter* bar_ = nullptr;
    gui::Label* count_ = nullptr;
};

// ---- World overlays -----------------------------------------------------------

/// A count-free enemy entrance, with chevrons following the lane into the map.
class SpawnMarker : public gui::Panel {
public:
    SpawnMarker(gui::Gui& g, std::string id);
    Vec2 direction{1.0f, 0.0f}; ///< Projected inward direction, in screen space.
    bool preparing = false;
    void draw_self(gui::DrawList& dl) override;

private:
    gui::Gui& gui_;
};

/// The path out to the organ, sharing the entrance arrow's visual language.
/// Set direction to the projected outward lane direction.
class OrganMarker : public SpawnMarker {
public:
    OrganMarker(gui::Gui& g, std::string id);
};

/// A world-space circle (a tower's reach, the placement ring) drawn through
/// the camera projection, so the tilted view shows it as the ellipse it is.
class RangeRing : public gui::Widget {
public:
    explicit RangeRing(std::string id = {});
    Vec2 world{0.0f, 0.0f};
    f32 radius = 1.0f;
    Color stroke = kWhite;
    f32 stroke_width = 3.0f;
    f32 dash = 2.0f;     ///< 0 = solid
    f32 gap = 8.0f;
    Color fill = kTransparent;
    /// Dash travel speed in px/s (the canvas's `flow`).
    f32 flow = 0.0f;
    /// Aim reticle: small dots riding the ring.
    u32 dots = 0;
    void draw_self(gui::DrawList& dl) override;
};

/// The selected tower's popup: name, integrity, Sell with its refund, joined
/// to the tower by a curved tail.
class TowerPopup : public gui::Panel {
public:
    TowerPopup(gui::Gui& g, std::string id);
    void sync(const HudTower& t);
    void draw_self(gui::DrawList& dl) override;

    gui::Button* sell = nullptr;

private:
    gui::Gui& gui_;
    gui::Label* name_ = nullptr;
    gui::Meter* health_ = nullptr;
    gui::Label* health_text_ = nullptr;
    gui::Label* refund_ = nullptr;
};

/// The hint pill above the dock while a cursor is armed.
class HintPill : public gui::Panel {
public:
    HintPill(gui::Gui& g, std::string id);
    /// `verb` is "Place" or "Cast"; the dot takes `accent`.
    void sync(const std::string& verb, Color accent);

private:
    gui::Label* verb_ = nullptr;
    gui::Panel* dot_ = nullptr;
};

/// Health colour ramp shared by the popup and the pips.
Color health_color(const gui::Theme& theme, f32 frac);

/// "0:18" for a prep countdown.
std::string format_clock(f32 seconds);

} // namespace immune::ui
