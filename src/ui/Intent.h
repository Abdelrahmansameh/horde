// ui/Intent.h — what the player asked for, as data.
//
// The UI emits *intents*, it does not mutate the sim. app/ translates intents
// into sim commands so that every state change goes through one auditable path
// (which is also how --sim-test scripts drive the game).
#pragma once

#include "core/Types.h"
#include "game/abilities/ActiveAbilities.h"

namespace immune::ui {

/// What the player asked for this frame. Consumed by app/, not by ui/.
enum class IntentKind : u8 {
    None = 0,
    PlaceTower,
    SelectTower,
    /// Retired: towers have no in-run tiers (PROGRESSION.md §7). Kept so the
    /// enum's numbering is stable; nothing emits it and app/ ignores it.
    UpgradeTower,
    SellTower,
    CastAbility,     ///< A DESIGN.md §5.6 active ability (game/abilities).
    SetTimeScale,
    StartWaveEarly,
    OpenMenu,        ///< The HUD's menu button: the pause menu.
    QuitToMenu,
};

struct Intent {
    IntentKind kind = IntentKind::None;
    TowerType tower_type = TowerType::Macrophage;
    Vec2 world_position{0.0f, 0.0f};
    EntityId entity{};
    f32 value = 0.0f;   ///< SetTimeScale payload.
    game::AbilityId ability_id = game::AbilityId::ComplementCascadeBurst; ///< CastAbility payload.
};

} // namespace immune::ui
