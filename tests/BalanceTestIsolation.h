#pragma once

#include "game/config/GameConfig.h"
#include "game/abilities/AbilityConfigApply.h"
#include "game/enemies/EnemyConfigApply.h"
#include "game/enemies/EnemyRoster.h"
#include "game/towers/TowerMechanics.h"
#include "game/towers/TowerSystem.h"

namespace immune::test {
// Balance fixtures deliberately install shipped configs and experimental tree
// ranks. These tables are process-wide; later configless engine tests expect
// executable defaults even when a REQUIRE aborts an experiment early.
struct BalanceConfigIsolation {
    const game::GameConfig bootstrap = game::default_game_config();
    ~BalanceConfigIsolation() {
        game::TowerSystem towers;
        game::EnemyRoster enemies;
        game::ActiveAbilitySystem abilities;
        game::apply_tower_config(towers, bootstrap.towers);
        game::apply_enemy_config(enemies, bootstrap.enemies);
        game::apply_ability_config(abilities, bootstrap.abilities);
    }
};
} // namespace immune::test
