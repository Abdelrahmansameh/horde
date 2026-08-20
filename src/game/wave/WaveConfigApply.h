// game/wave/WaveConfigApply.h — applies waves.json to the generator.
//
// WaveDirector.h is a frozen contract, so the config reaches generate() the
// same way the tower and ability tuning reaches theirs: a file-static store
// this header exposes. generate()'s signature is unchanged.
//
// This covers only the PROCEDURAL table. A level that authors its own `waves`
// block still overrides it outright -- that path has always been data-driven,
// and this fills the gap for the eleven shipped levels that do not.
#pragma once

#include "game/config/GameConfig.h"

namespace immune::game {

/// Installs `cfg` as the live wave-generator tuning.
void apply_wave_config(const WaveConfig& cfg);

/// The live tuning. Holds the five region curves compiled into
/// WaveDirector.cpp until a config is applied.
const WaveConfig& wave_config();

} // namespace immune::game
