// ui/Menu.h — the front end's contract with app/: MenuAction, MenuResult,
// LevelEntry, RunSummary; and the ImGui Strengthen Immunity screen.
//
// The screens themselves are ui/front/FrontEnd (gui framework). The HUD
// (ui/hud) shows the live level; the front end needs none of that.
//
// WHY IT DOES NOT KNOW ABOUT GameStateId
// GameStateId lives in app/, and ui/ must not depend on app/ — that would
// invert the dependency direction the rest of this layer follows (the same
// reason the HUD reports ui::Intent rather than touching the state machine).
// So the screens report what the player *clicked*, as a MenuAction, and app/
// decides what that means for the state machine. The screens own no game state and
// no transition logic; they are pure input/output.
#pragma once

#include "core/Types.h"

#include <string>
#include <vector>

namespace immune::game { class MetaProgression; struct MetaConfig; }

namespace immune::ui {

/// One selectable level, discovered by scanning the levels directory. Built by
/// app/ (which owns level loading); the screens only display what they are handed.
struct LevelEntry {
    std::string path;         ///< Full path, passed straight back to the loader.
    std::string display_name; ///< LevelDef::display_name, else name, else the filename.
    std::string region;       ///< LevelDef::region, for grouping and subtitle.
    u32 lane_count = 0;       ///< Distinct vessel lane ids; 0 when unknown.
    i32 difficulty = 0;       ///< LevelDef::difficulty (1-10); 0 = unrated, not shown.
    /// Cleared at least once (game/meta). An uncleared level still has its
    /// first-clear Antibody to pay, which the list says out loud.
    bool cleared = false;
    /// LevelDef::name: the id campaign progress records a clear under.
    std::string level_id;
};

/// What the run that just ended paid, for the results screens. Plain data so
/// the screens do not have to know how it was computed (app/ does).
struct RunSummary {
    bool valid = false;        ///< False: nothing to show (e.g. no run yet).
    /// Sandboxed run (editor playtest, the gym, --sandbox): no payout at all.
    bool sandbox = false;
    u32 waves_cleared = 0;
    u32 memory_cells = 0;
    u32 antibodies = 0;
    bool first_clear = false;
};

enum class MenuAction : u8 {
    None = 0,        ///< Nothing clicked this frame.
    OpenLevelSelect,
    OpenEditor,      ///< Open the in-game level editor (docs/LEVEL_EDITOR.md).
    StartLevel,      ///< `level_index` names the chosen entry.
    RestartLevel,    ///< Restart the current level.
    BackToEditor,    ///< Leave a playtest and return to the level editor.
    Resume,          ///< Close the pause menu and continue the current level.
    Back,
    Quit,
    OpenImmunityTree, ///< The Strengthen Immunity screen (game/meta).
    PurchaseNode,     ///< Buy the next level of `node` (a game::TreeNode).
    Respec,           ///< Refund the whole tree (minus the fee).
};

struct MenuResult {
    MenuAction action = MenuAction::None;
    usize level_index = 0;
    /// PurchaseNode payload: a game::TreeNode, as its integer value.
    u32 node = 0;
};

/// The Strengthen Immunity screen, still on ImGui. The rest of the front end
/// (main menu, level select, pause, results) is ui/front/FrontEnd on the gui
/// framework; this moves there in phase 5 of docs/UI_FRAMEWORK.md.
class Menu {
public:
    /// The Strengthen Immunity tree (PROGRESSION.md): both currencies, every
    /// branch drawn as a column of nodes off one vessel, each node showing its
    /// level, its price and -- when it cannot be bought -- why not. Reports a
    /// click as PurchaseNode; buying is app/'s job, through MetaProgression,
    /// so this screen can never disagree with the rules it displays. Assumes
    /// DevUi's ImGui frame is open.
    MenuResult build_immunity_tree(const game::MetaProgression& meta,
                                   const game::MetaConfig& cfg, i32 screen_width,
                                   i32 screen_height);
};

} // namespace immune::ui
