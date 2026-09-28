// app/UiBridge.h — reads the live game into the HUD's plain model.
//
// The only place that knows both sides: game/sim state on one, ui::HudModel
// on the other. HudScreen never touches SimWorld, TowerSystem or the wave
// director; everything it shows arrives through here, once per frame.
#pragma once

#include "core/Types.h"
#include "game/gym/GymCommands.h"
#include "ui/hud/HudModel.h"

#include <string>
#include <vector>

namespace immune::sim { class SimWorld; }
namespace immune::game {
class Economy;
class WaveDirector;
class TowerSystem;
class ActiveAbilitySystem;
struct LevelDef;
}

namespace immune::ui { class HudScreen; }
namespace immune::gui { class Gui; }

namespace immune::app {

struct HudSources {
    const sim::SimWorld* world = nullptr;
    const game::Economy* economy = nullptr;
    const game::WaveDirector* waves = nullptr;
    const game::TowerSystem* towers = nullptr;
    const game::ActiveAbilitySystem* abilities = nullptr;
    const game::LevelDef* level = nullptr;  ///< Spawn points; may be null.
    f32 time_scale = 1.0f;
};

/// Fills a HudModel. `screen` supplies the armed tower so the placement
/// preview can be validated at `world_cursor`.
ui::HudModel make_hud_model(const HudSources& src, const ui::HudScreen& screen, Vec2 world_cursor);

/// What the gym's `ui` command drives.
struct UiDriver {
    gui::Gui* gui = nullptr;
    ui::HudScreen* hud = nullptr;
    const ui::HudModel* model = nullptr;
    /// Screenshot mode: the pointer the UI frames use, logical px. Null in
    /// interactive play, where the real mouse is the pointer.
    Vec2* pointer = nullptr;
};

/// `ui dump | click <path> | hover <path> | pointer <x> <y> | select <n> |
/// cancel` -- tokens as the gym tokenized them, tokens[0] == "ui".
game::GymResult run_ui_command(const UiDriver& d, const std::vector<std::string>& tokens);

} // namespace immune::app
