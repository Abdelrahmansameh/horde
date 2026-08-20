// game/towers/TowerMechanics.h — the per-role mechanism table.
//
// RATIONALE
//  - TowerSystem.h is a frozen contract and TowerStats cannot grow, but every
//    role has knobs that are not stats: the mortar's burst duty cycle, the
//    T-cell's granule release rate, the gunner's muzzle velocity and spread.
//    Those used to be file-static constexpr tables in TowerSystem.cpp, which
//    made them the one part of a tower nobody could tune without a rebuild.
//  - They now live in a file-static store that assets/config/towers.json
//    writes through apply_tower_config(). This header is new and NOT frozen,
//    so exposing them costs no change to any frozen file.
//  - The store starts out holding exactly the values the constants held, so a
//    TowerSystem that was never handed a config behaves as it always did.
#pragma once

#include "game/config/GameConfig.h"

namespace immune::game {

/// Applies a loaded tower config: stats go through TowerSystem::set_stats(),
/// mechanics and globals into the file-static store this header exposes.
/// Safe to call at any time, including mid-run for a hot reload.
void apply_tower_config(TowerSystem& towers, const TowerConfig& cfg);

/// Mechanism parameters for one type/tier. `tier` is 1..3; out-of-range tiers
/// clamp, matching TowerSystem::stats().
const TowerMechanics& tower_mechanics(TowerType type, u8 tier);

/// Shape-id base, refund fraction and the placement search window.
const TowerGlobals& tower_globals();

} // namespace immune::game
