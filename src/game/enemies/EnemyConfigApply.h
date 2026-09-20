// game/enemies/EnemyConfigApply.h — applies enemies.json to the roster.
//
// EnemyRoster.h is a frozen contract and exposes no setter, so this is a free
// function rather than a method: the class's public surface is unchanged and
// the only edit to the frozen header is a friend declaration.
//
// It also pushes each family's silhouette/tempo/wobble/colour down into
// render/, which cannot see game/ and therefore cannot read the config itself.
// That is the one direction data has to travel to make enemy SIZE tunable at
// all -- the renderer draws from the silhouette and the sim derives its
// collision radius from that same number.
#pragma once

#include "game/config/GameConfig.h"

namespace immune::game {

class EnemyRoster;

/// Applies `cfg` to `roster` and to render's family tables. Safe to call at
/// any time, including mid-run for a hot reload.
void apply_enemy_config(EnemyRoster& roster, const EnemyConfig& cfg);

/// Speed profile for one tier, from the live config.
const SpeedProfileParams& enemy_speed_profile(SpeedTier tier);

/// True once apply_enemy_config() has run. Before that the roster still
/// DERIVES separation strength, drift bias and replication rate the way it
/// always did, so a roster that never sees a config file is unchanged -- and
/// so default_game_config() reads real values rather than an empty seed.
bool enemy_config_applied();

/// The live enemy config. Holds the values compiled into EnemyRoster.cpp until
/// apply_enemy_config() replaces them.
const EnemyConfig& enemy_config();

/// The hostile pass's tuning (sim/hostile/HostileAttacks.h), joined from the
/// two files that own it: each family's latch and aura from the live enemy
/// config, the switch and the caps from sim.json's `hostile` block. This is
/// what every SimDesc built by the game should carry; a SimDesc built without
/// it ships the pass off.
sim::HostileTuning hostile_tuning(const HostileGlobals& globals);

} // namespace immune::game
