#include "app/GameState.h"

namespace immune::app {

void GameStateMachine::request(GameStateId next) { pending_ = next; }

bool GameStateMachine::apply_pending() {
    if (pending_ == current_) return false;
    previous_ = current_;
    current_ = pending_;
    just_entered_ = true;
    return true;
}

const char* GameStateMachine::name(GameStateId id) {
    switch (id) {
        case GameStateId::Boot:          return "Boot";
        case GameStateId::MainMenu:      return "MainMenu";
        case GameStateId::LevelSelect:   return "LevelSelect";
        case GameStateId::Loadout:       return "Loadout";
        case GameStateId::InLevel:       return "InLevel";
        case GameStateId::Paused:        return "Paused";
        case GameStateId::Editor:        return "Editor";
        case GameStateId::LevelComplete: return "LevelComplete";
        case GameStateId::LevelFailed:   return "LevelFailed";
        case GameStateId::Quitting:      return "Quitting";
        default:                         return "Unknown";
    }
}

} // namespace immune::app
