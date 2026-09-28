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

#include <string>

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

/// A tower card in the build dock: key badge, cost, cell icon, name; greyed
/// with "N ATP short" and a progress bar when the player cannot afford it;
/// lifted with a PLACING tag while armed.
class BuildCard : public gui::Button {
public:
    BuildCard(gui::Gui& g, TowerType type, u32 slot);
    void sync(const HudTowerCard& card, u32 atp, bool armed);

    TowerType type;

private:
    gui::Gui& gui_;
    Badge* badge_ = nullptr;
    gui::Label* cost_ = nullptr;
    gui::Icon* icon_ = nullptr;
    gui::Label* name_ = nullptr;
    gui::Label* short_ = nullptr;
    gui::Meter* progress_ = nullptr;
    Tag* tag_ = nullptr;
};

/// An ability: a fluid cell filling as the cooldown recharges, the ability's
/// glyph (dim until ready), the seconds left, a pulsing halo and a wobble when
/// ready, and its keycap and short name underneath.
class AbilityCell : public gui::Widget {
public:
    AbilityCell(gui::Gui& g, game::AbilityId id, std::string key);
    void sync(const HudAbility& a, bool armed);
    void update(f32 dt) override;

    game::AbilityId ability;
    gui::Button* button = nullptr;

private:
    gui::Gui& gui_;
    gui::Ring* halo_ = nullptr;
    gui::Icon* glyph_ = nullptr;
    gui::Label* timer_ = nullptr;
    gui::Label* name_ = nullptr;
    Tag* tag_ = nullptr;
    bool ready_ = false;
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
