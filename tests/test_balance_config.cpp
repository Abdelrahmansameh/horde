#include "BalanceTestIsolation.h"
#include "game/config/GameConfig.h"
#include "game/towers/TowerSystem.h"
#include "game/towers/TowerMechanics.h"
#include "game/enemies/EnemyRoster.h"
#include "game/enemies/EnemyConfigApply.h"
#include "game/abilities/AbilityConfigApply.h"
#include <catch2/catch_test_macros.hpp>

using namespace immune;
using namespace immune::game;

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation, "experiments cannot replace the executable bootstrap with live upgraded tuning", "[balance][config]") {
    const auto before = default_game_config();
    auto variant = before;
    variant.towers.mechanics[0].shooter.round_damage += 123;
    variant.towers.mechanics[1].arbor_grabber.arm_count = 4;
    variant.enemies.families[0].behavior.base_density += 17;
    variant.abilities.ability[0].cooldown_seconds += 29;
    TowerSystem towers;
    EnemyRoster enemies;
    ActiveAbilitySystem abilities;
    apply_tower_config(towers,variant.towers);
    apply_enemy_config(enemies,variant.enemies);
    apply_ability_config(abilities,variant.abilities);
    CHECK(tower_mechanics(TowerType::Neutrophil).shooter.round_damage == variant.towers.mechanics[0].shooter.round_damage);
    CHECK(enemy_config().families[0].behavior.base_density == variant.enemies.families[0].behavior.base_density);
    CHECK(dump_game_config(default_game_config()) == dump_game_config(before));
    apply_tower_config(towers,before.towers);
    apply_enemy_config(enemies,before.enemies);
    apply_ability_config(abilities,before.abilities);
}
