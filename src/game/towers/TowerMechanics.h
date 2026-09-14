// game/towers/TowerMechanics.h — the per-tower swarmer table.
//
// RATIONALE
//  - TowerStats is the tower's own economics and cadence; everything about
//    what it RELEASES -- the swarmer chassis and the per-kind payload
//    (game/config/GameConfig.h's TowerMechanics) -- lives here, in a
//    file-static store that assets/config/towers.json writes through
//    apply_tower_config().
//  - The store starts out holding compiled-in defaults that mirror the shipped
//    JSON, so a TowerSystem that was never handed a config behaves the same.
//  - swarmer_profile() is the adapter from this table to what the sim kernel
//    reads (sim::SwarmerProfile); TowerSystem registers it into the world's
//    swarmer store every time a tower fires, which is how a hot reload reaches
//    units already in flight.
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

/// The sim-side profile for one type/tier, built from the table above.
/// `tier` is 1..3; out-of-range tiers clamp.
sim::SwarmerProfile swarmer_profile(TowerType type, u8 tier);

} // namespace immune::game
