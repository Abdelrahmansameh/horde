#pragma once

#include "game/config/GameConfig.h"
#include "game/meta/MetaProgression.h"
#include "sim/SimWorld.h"

namespace immune::game {
class TowerSystem;
class EnemyRoster;
class Economy;
class ActiveAbilitySystem;
class WaveDirector;
struct LevelDef;

// Shared by interactive play and balance runs. Always fold into a fresh copy.
struct RunTuning {
    GameConfig config;
    TreeEffects tree;
};
RunTuning make_run_tuning(const GameConfig& base, const MetaProgression& meta,
                          bool sandbox = false);
sim::SimDesc make_level_sim_desc(const GameConfig& base, const EnemyRoster& enemies,
                                 const LevelDef& level, u64 seed);
void apply_run_tuning(const GameConfig& base, const RunTuning& run,
                      const LevelDef& level, sim::SimWorld& world,
                      TowerSystem& towers, EnemyRoster& enemies, Economy& economy,
                      ActiveAbilitySystem& abilities, bool sandbox = false);
RunResult collect_run_result(const sim::SimWorld& world, const WaveDirector& waves,
                             bool won);
} // namespace immune::game
