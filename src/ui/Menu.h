// ui/Menu.h — front-end screens: main menu and level select.
//
// WHY THIS IS NOT PART OF Hud
// Hud draws the in-level HUD and needs the whole live game (sim, economy,
// waves, towers, abilities, camera). The menus need none of that — they run
// when there is no level loaded at all. Folding them into Hud::build() would
// mean handing it a half-constructed world and having it guess which half is
// valid.
//
// WHY IT DOES NOT KNOW ABOUT GameStateId
// GameStateId lives in app/, and ui/ must not depend on app/ — that would
// invert the dependency direction the rest of this layer follows (the same
// reason GameStateId was deliberately kept out of Hud::build()'s signature).
// So Menu reports what the player *clicked*, as a MenuAction, and app/ decides
// what that means for the state machine. Menu owns no game state and no
// transition logic; it is a pure input/output screen.
#pragma once

#include "core/Types.h"

#include <string>
#include <vector>

namespace immune::platform { class InputState; }

namespace immune::ui {

/// One selectable level, discovered by scanning the levels directory. Built by
/// app/ (which owns level loading); Menu only displays what it is handed.
struct LevelEntry {
    std::string path;         ///< Full path, passed straight back to the loader.
    std::string display_name; ///< LevelDef::display_name, else name, else the filename.
    std::string region;       ///< LevelDef::region, for grouping and subtitle.
    u32 lane_count = 0;       ///< Distinct vessel lane ids; 0 when unknown.
    i32 difficulty = 0;       ///< LevelDef::difficulty (1-10); 0 = unrated, not shown.
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
};

struct MenuResult {
    MenuAction action = MenuAction::None;
    usize level_index = 0;
};

class Menu {
public:
    /// Draws the title screen. Assumes an ImGui frame is already open — Hud
    /// owns begin_frame()/render(), and these screens draw inside that same
    /// frame rather than starting a competing one.
    MenuResult build_main_menu(i32 screen_width, i32 screen_height);

    /// Draws the level list. `levels` may be empty, which is shown as an
    /// explicit "no levels found" message rather than an empty window — a
    /// missing asset root is otherwise indistinguishable from a broken build.
    MenuResult build_level_select(const std::vector<LevelEntry>& levels,
                                  i32 screen_width, i32 screen_height);

    /// Draws the level failed screen with restart and back buttons.
    ///
    /// `playtest` marks a run launched from the level editor: the way out is
    /// back to the document being tested, not to the main menu, and the level
    /// on offer to restart is that same document.
    MenuResult build_level_failed_screen(i32 screen_width, i32 screen_height,
                                         bool playtest = false);

    /// Draws the level complete screen with continue and back buttons. A
    /// playtest also gets Restart here: the run ending is not a reason to have
    /// to walk back through the editor to test the same waves again.
    MenuResult build_level_complete_screen(i32 screen_width, i32 screen_height,
                                           bool playtest = false);

    /// Draws the in-level pause menu: resume, restart, or return to the main
    /// menu. Drawn as an overlay over a frozen (but still rendered) game frame.
    MenuResult build_pause_menu(i32 screen_width, i32 screen_height);

private:
    /// Survives across frames so the list keeps its highlight between clicks.
    int selected_ = 0;
};

} // namespace immune::ui
