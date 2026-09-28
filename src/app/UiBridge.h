// app/UiBridge.h — reads the live game into the HUD's plain model.
//
// The only place that knows both sides: game/sim state on one, ui::HudModel
// on the other. HudScreen never touches SimWorld, TowerSystem or the wave
// director; everything it shows arrives through here, once per frame.
#pragma once

#include "core/Types.h"
#include "game/gym/GymCommands.h"
#include "ui/front/FrontModel.h"
#include "ui/hud/HudModel.h"

#include <string>
#include <vector>

namespace immune::sim { class SimWorld; }
namespace immune::game {
class MetaProgression;
struct MetaConfig;
class Economy;
class WaveDirector;
class TowerSystem;
class ActiveAbilitySystem;
struct LevelDef;
}

namespace immune::ui { class HudScreen; class FrontEnd; enum class FrontScreen : u8; }
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

/// A level's lanes, obstacles and spawns normalized into a level-select
/// thumbnail (y flipped: the world is y-up, the thumbnail y-down).
ui::LevelThumb make_level_thumb(const game::LevelDef& def);

/// True for the player's campaign files: campaign_NN_*.json.
bool is_campaign_level(const std::string& path);

/// The campaign, in file-name order, from the discovered level list and the
/// matching LevelDefs (`defs[i]` for `levels[i]`). Cleared/locked flags are
/// filled by refresh_campaign.
std::vector<ui::CampaignLevel> make_campaign(const std::vector<ui::LevelEntry>& levels,
                                             const std::vector<game::LevelDef>& defs);

/// Re-reads each level's cleared flag from `levels` (kept current from the
/// save) and applies the unlock rule (ui::campaign_unlocked).
void refresh_campaign(std::vector<ui::CampaignLevel>& campaign, const std::vector<ui::LevelEntry>& levels);

/// The Strengthen Immunity tree as the screen shows it: every node's level,
/// state (from MetaProgression::check_purchase), next price and missing
/// prerequisite; the wallet; branch points; respec.
ui::TreeModel make_tree_model(const game::MetaProgression& meta, const game::MetaConfig& cfg);

/// What the gym's `ui` command drives.
struct UiDriver {
    gui::Gui* gui = nullptr;
    ui::HudScreen* hud = nullptr;
    const ui::HudModel* model = nullptr;
    /// Screenshot mode: the pointer the UI frames use, logical px. Null in
    /// interactive play, where the real mouse is the pointer.
    Vec2* pointer = nullptr;
    /// The out-of-match screens (level select's `ui level <n>`). May be null.
    ui::FrontEnd* front = nullptr;
    /// The player UI scale (`ui scale <f>`). May be null.
    f32* ui_scale = nullptr;
    /// Screenshot mode: which front-end screen to show (`ui screen <name>`).
    /// Null in interactive play, where the state machine decides.
    ui::FrontScreen* screen = nullptr;
};

/// Screenshot mode's front-end data: the campaign found on disk (cleared up
/// to `current_level_path`'s slot), `level` as the level just played, and a
/// sample first-clear payout for the results screens.
ui::FrontModel make_screenshot_front_model(const game::LevelDef& level, const std::string& current_level_path);

/// `ui dump | click <path> | hover <path> | pointer <x> <y> | select <n> |
/// cancel | level <n> | screen <name> | scale <f>` -- tokens as the gym tokenized them, tokens[0] == "ui".
game::GymResult run_ui_command(const UiDriver& d, const std::vector<std::string>& tokens);

} // namespace immune::app
