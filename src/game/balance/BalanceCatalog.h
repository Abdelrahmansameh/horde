#pragma once

#include "config/Json.h"
#include "game/config/GameConfig.h"

namespace immune::game::balance {

/// Discover the loaded game's real tree, prices, capabilities and complete
/// addressable tuning surface. Pure: no files, globals, simulation or saves.
config::Json describe_catalog(const GameConfig& config);

} // namespace immune::game::balance
