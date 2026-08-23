// ui/GymPanel.h — the gym level's control window. NEW MODULE.
//
// RATIONALE
// game/gym/GymCommands.h owns the command language; this file is only its front
// end. The split is deliberate: the executor must stay linkable into headless
// builds (--sim-test, unit tests) that have no ImGui, no window, and no fonts,
// so every line of ImGui lives here and nothing here knows what a command
// *does*.
//
// WIDGETS, NOT A PROMPT
// Every control in this window builds a command string and runs it through
// gym_execute(), and the log at the bottom echoes the exact line it ran. So the
// panel is not a second API that can drift from the text one -- it is a typist
// for it, and using it teaches the language it types. The input box is still
// there for anything the widgets do not cover.
//
// SCOPED TO THE GYM
// set_level("gym") opens the window automatically; any other level leaves it
// closed until the toggle key is pressed. A debug panel that opens itself on
// every level is a debug panel people turn off and forget exists, and the gym
// level's whole purpose is that this window is open.
//
// It deliberately does NOT go through ui::Intent: intents are the player's
// auditable path into the sim (Hud.h's rationale), and a debug panel
// masquerading as a player would make that record lie.
#pragma once

#include "core/Types.h"

#include <string>
#include <vector>

namespace immune::game { struct GymContext; }

namespace immune::ui {

/// Tabbed control panel over the gym command language, plus a command log and
/// an input line. Toggled with backtick or F2 (F1 belongs to the debug
/// overlay).
///
/// Must be built inside an ImGui frame (between Hud::begin_frame and
/// Hud::render), like every other window in ui/.
class GymPanel {
public:
    /// Draws the panel if visible and runs whatever the user asked for. Handles
    /// its own toggle key, so call this every frame regardless of visibility.
    void build(game::GymContext& ctx);

    /// Runs one command line and appends it, and its result, to the log --
    /// exactly as if it had been typed. Every widget goes through here, which
    /// is what keeps the panel and the text language the same thing.
    bool execute(game::GymContext& ctx, const std::string& line);

    /// Tells the panel which level is loaded. The gym level opens it; anything
    /// else leaves the current visibility alone (so a panel you opened by hand
    /// on another level stays open).
    void set_level(const std::string& level_name);

    /// Forces a tab to the front on the next build(): 0 Horde, 1 Defense,
    /// 2 Waves, 3 World. Lets app/ (or a test) land on the right tab instead of
    /// whichever one was last clicked.
    void select_tab(i32 index) { force_tab_ = index; }

    bool visible() const { return visible_; }
    void set_visible(bool v) { visible_ = v; }
    void toggle() { visible_ = !visible_; }

    /// Appends a line the user did not type. `ok == false` uses the error colour.
    void print(const std::string& text, bool ok = true);
    void clear_log();

private:
    struct Line {
        std::string text;
        u8 kind = 0;   ///< 0 = output, 1 = echo of a command, 2 = error.
    };

    /// "cursor" / "p_lymph" / "objective" / "12.0,34.0" — the shared target
    /// selector at the top of the window, as the command language spells it.
    std::string target_arg() const;
    /// target_arg() with the " at " keyword in front, which is how every
    /// command except `cam` takes a point. One control aims all of them.
    std::string target_suffix() const;

    void draw_target_bar(game::GymContext& ctx);
    void draw_horde_tab(game::GymContext& ctx);
    void draw_defense_tab(game::GymContext& ctx);
    void draw_waves_tab(game::GymContext& ctx);
    void draw_world_tab(game::GymContext& ctx);
    void draw_log(game::GymContext& ctx);

    std::vector<Line> lines_;
    std::vector<std::string> history_;
    i32 history_pos_ = -1;          ///< -1 = editing a fresh line.
    char input_[512] = {};
    bool visible_ = false;
    bool focus_input_ = false;
    bool scroll_to_bottom_ = false;
    bool greeted_ = false;
    i32 force_tab_ = -1;            ///< Consumed by the next build(); -1 = leave it alone.

    // ---- Widget state ------------------------------------------------------
    i32 target_mode_ = 0;           ///< 0 cursor, 1 spawn point, 2 objective, 3 custom.
    i32 target_spawn_point_ = 0;
    std::string target_spawn_point_id_;   ///< Resolved from the live spawn point list.
    f32 target_xy_[2] = {0.0f, 0.0f};

    i32 family_ = 0;
    i32 spawn_count_ = 300;
    i32 elite_ = 0;
    i32 flood_count_ = 900;

    i32 tower_ = 0;
    i32 tower_tier_ = 1;
    i32 atp_amount_ = 5000;

    f32 field_radius_ = 18.0f;
    f32 field_rate_ = 60.0f;
    f32 field_duration_ = 2.0f;
    i32 vfx_ = 0;
    i32 step_ticks_ = 60;
    f32 cam_height_ = 0.0f;         ///< 0 = leave the camera's own height alone.
    bool overlay_debug_ = false;
    bool overlay_threat_ = true;
};

} // namespace immune::ui
