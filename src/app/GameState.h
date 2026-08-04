// app/GameState.h — top-level state machine. FROZEN CONTRACT.
//
// Only one state is "live" at a time and transitions are explicit, so the
// headless modes can construct exactly the state they need (InLevel) without
// dragging menus, audio devices, or a window along.
#pragma once

#include "core/Types.h"

#include <string>

namespace immune::app {

enum class GameStateId : u8 {
    Boot = 0,        ///< Subsystem init.
    MainMenu,
    LevelSelect,
    Loadout,         ///< Antibody-memory pre-run choice.
    InLevel,         ///< The only state that ticks SimWorld.
    Paused,
    LevelComplete,
    LevelFailed,
    Quitting,
};

/// Reason a level ended, for the results screen and for --sim-test assertions.
enum class LevelOutcome : u8 { InProgress = 0, Cleared, ObjectiveDestroyed, Aborted };

class GameStateMachine {
public:
    GameStateId current() const { return current_; }
    GameStateId previous() const { return previous_; }

    /// Requests a transition, applied at the top of the next frame so a state
    /// never destroys itself mid-update.
    void request(GameStateId next);
    bool has_pending() const { return pending_ != current_; }

    /// Applies any pending transition. Returns true if the state changed.
    bool apply_pending();

    /// True only on the first frame after entering the current state.
    bool just_entered() const { return just_entered_; }
    void clear_just_entered() { just_entered_ = false; }

    LevelOutcome outcome() const { return outcome_; }
    void set_outcome(LevelOutcome o) { outcome_ = o; }

    const std::string& current_level_id() const { return level_id_; }
    void set_current_level_id(std::string id) { level_id_ = std::move(id); }

    /// True while the sim should advance (InLevel and not paused).
    bool sim_running() const { return current_ == GameStateId::InLevel; }

    static const char* name(GameStateId id);

private:
    GameStateId current_ = GameStateId::Boot;
    GameStateId previous_ = GameStateId::Boot;
    GameStateId pending_ = GameStateId::Boot;
    LevelOutcome outcome_ = LevelOutcome::InProgress;
    bool just_entered_ = true;
    std::string level_id_;
};

} // namespace immune::app
