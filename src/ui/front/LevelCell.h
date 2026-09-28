// ui/front/LevelCell.h — one level on the campaign vessel map.
//
// Follows the canvas's Levels artboard: a lavender-rimmed cell (grey when
// locked) with the level's lanes drawn inside it, clipped to the membrane
// through the stencil; the level number in a badge on its shoulder; a check
// when cleared; a padlock under a dark veil when locked; a pulsing gold halo
// on the level the player should play next; and a spinning dashed white ring
// on the selected one. The name sits below in a plum pill.
#pragma once

#include "gui/widgets/Widgets.h"
#include "ui/front/FrontModel.h"

namespace immune::ui {

/// The cell itself (the name pill is a sibling built by the screen). Its
/// geometry is the canvas's 136-unit cell viewBox drawn into 156 px.
class LevelCell : public gui::Button {
public:
    static constexpr f32 kSize = 156.0f;

    LevelCell(gui::Gui& g, std::string id, const CampaignLevel& level);

    void set_level(const CampaignLevel& level);
    const CampaignLevel& level() const { return level_; }

    bool selected = false;
    /// The next level to play: gold halo.
    bool frontier = false;

    void draw_self(gui::DrawList& dl) override;

private:
    /// Canvas units (cell viewBox is -68..68) to px.
    static constexpr f32 kUnit = kSize / 136.0f;
    gui::ShapeDesc membrane(f32 radius_units) const;
    void draw_thumb(gui::DrawList& dl, Vec2 c) const;

    gui::Gui& gui_;
    CampaignLevel level_;
    std::string number_;
};

} // namespace immune::ui
