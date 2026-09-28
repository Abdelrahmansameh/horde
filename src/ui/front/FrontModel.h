// ui/front/FrontModel.h — what the out-of-match screens show, as plain data.
//
// Filled by app/UiBridge (make_front_model) from the level list, the save and
// the run that just ended; the screens never read LevelDef, MetaProgression
// or the state machine. Tests build these by hand.
#pragma once

#include "core/Types.h"
#include "ui/Menu.h"

#include <string>
#include <vector>

namespace immune::ui {

/// A level's lanes and obstacles, normalized for a level-select thumbnail:
/// coordinates are in "thumb units" where the thumbnail square is 0..1 on
/// both axes (content may overhang, the cell clips it).
struct LevelThumb {
    struct Lane {
        std::vector<Vec2> points;
        f32 width = 0.1f;  ///< Lumen diameter in thumb units.
    };
    struct Obstacle {
        std::vector<Vec2> outline;  ///< Closed polygon (may be concave).
    };
    std::vector<Lane> lanes;
    std::vector<Obstacle> obstacles;
    std::vector<Vec2> spawns;
};

/// One campaign level on the vessel map.
struct CampaignLevel {
    u32 number = 0;            ///< 1-based position in the campaign.
    std::string name;          ///< Display name ("First Bend").
    usize level_index = 0;     ///< Index into app's level list (MenuResult::level_index).
    bool cleared = false;
    bool locked = true;        ///< The previous level is not cleared yet.
    LevelThumb thumb;
};

/// Campaign gating: level `i` (0-based) is open when it is the first one or
/// the one before it has been cleared.
inline bool campaign_unlocked(const std::vector<CampaignLevel>& levels, usize i) {
    return i == 0 || (i < levels.size() && levels[i - 1].cleared);
}

/// The first open, uncleared level: where a returning player picks up. Falls
/// back to the last open level when everything open is cleared.
inline usize campaign_frontier(const std::vector<CampaignLevel>& levels) {
    usize last_open = 0;
    for (usize i = 0; i < levels.size(); ++i) {
        if (levels[i].locked) break;
        last_open = i;
        if (!levels[i].cleared) return i;
    }
    return last_open;
}

struct FrontModel {
    std::vector<CampaignLevel> campaign;

    // ---- Results ----
    RunSummary run;
    bool playtest = false;           ///< An editor playtest: Restart / Back to Editor.
    std::string level_name;          ///< The level that just ended (or is paused).
    /// Campaign position of that level; -1 when it is not a campaign level.
    i32 campaign_slot = -1;
    /// This run unlocked the next campaign level (a first clear).
    bool unlocked_next = false;
};

} // namespace immune::ui
