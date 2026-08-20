// game/abilities/AbilityConfigApply.h — applies abilities.json.
//
// ActiveAbilities.h is a frozen contract, so the config reaches the system the
// same way the tower mechanics do: a file-static store this header exposes,
// which load_defaults() reads. No method is added and no signature changes.
#pragma once

#include "game/config/GameConfig.h"

namespace immune::game {

/// Installs `cfg` as the live ability tuning. `system.load_defaults()` is
/// called for you, so the change takes effect immediately.
void apply_ability_config(ActiveAbilitySystem& system, const AbilityConfig& cfg);

/// The live tuning. Holds the DESIGN.md §5.6 values until a config is applied,
/// so an ActiveAbilitySystem that never sees a file behaves as it always did.
const AbilityConfig& ability_config();

} // namespace immune::game
