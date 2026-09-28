// ui/front/FrontEnd.h — the out-of-match screens, built on the gui framework.
//
// Main menu, campaign level select, pause, and the results of a run (level
// cleared / level failed, with the editor-playtest variants), following the
// design canvas's Menu, Levels, Victory and Defeat artboards.
//
// Like the HUD, these screens never touch the state machine: a click becomes
// a ui::MenuResult (Menu.h), taken by app/ each frame, which decides what it
// means. app/ also decides which screen shows (from GameStateId) and hands in
// a FrontModel; ui/ never sees app types.
//
// Frame order (app/): show(screen, model) → gui.frame() → take_result().
//
// Switching screens crossfades: the old screen fades out while the new one
// buds in (fades up from 97% scale). Each fades as a group through an
// offscreen layer, so a button does not show its panel through itself.
#pragma once

#include "core/Types.h"
#include "ui/Menu.h"
#include "ui/front/FrontModel.h"

#include <vector>

namespace immune::gui { class Gui; class Widget; }

namespace immune::ui {

class LevelCell;

enum class FrontScreen : u8 {
    None = 0,     ///< Nothing (in a live match, in the editor).
    MainMenu,
    LevelSelect,
    Pause,
    Victory,
    Defeat,
};

const char* front_screen_name(FrontScreen s);

class FrontEnd {
public:
    explicit FrontEnd(gui::Gui& gui);
    ~FrontEnd();
    FrontEnd(const FrontEnd&) = delete;
    FrontEnd& operator=(const FrontEnd&) = delete;

    /// Shows `s` built from `m`. Call every frame: a new screen crossfades in,
    /// the same screen is rebuilt in place only when the data it shows
    /// changed. None fades the current screen out.
    void show(FrontScreen s, const FrontModel& m);
    FrontScreen current() const { return current_; }

    /// What the player clicked since the last call (None if nothing).
    MenuResult take_result();

    /// Level select: the selected campaign position, or -1.
    i32 selected_level() const { return selected_; }
    /// Selects campaign position `pos` (false when locked or out of range).
    bool select_level(usize pos);

    /// Skips running transitions (tests, screenshots).
    void finish_transitions();

private:
    class ScreenRoot;

    void build(ScreenRoot& root, FrontScreen s, const FrontModel& m);
    void build_main(gui::Widget& root);
    void build_levels(gui::Widget& root, const FrontModel& m);
    void build_pause(gui::Widget& root, const FrontModel& m);
    void build_results(gui::Widget& root, const FrontModel& m, bool victory);
    void sync_selection();
    void emit(MenuResult r);
    static u64 signature(FrontScreen s, const FrontModel& m);

    gui::Gui& gui_;
    gui::Widget* layer_root_ = nullptr;
    ScreenRoot* live_ = nullptr;
    std::vector<ScreenRoot*> leaving_;
    FrontScreen current_ = FrontScreen::None;
    u64 signature_ = 0;
    MenuResult pending_{};

    // Level select state.
    std::vector<CampaignLevel> campaign_;
    std::vector<LevelCell*> cells_;
    gui::Widget* play_button_ = nullptr;
    i32 selected_ = -1;
};

} // namespace immune::ui
