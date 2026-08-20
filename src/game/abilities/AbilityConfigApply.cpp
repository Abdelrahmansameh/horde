// game/abilities/AbilityConfigApply.cpp
#include "game/abilities/AbilityConfigApply.h"

namespace immune::game {
namespace {

/// DESIGN.md §5.6. The design doc calls these numbers explicitly provisional
/// pending a balance pass -- which is precisely the case abilities.json exists
/// to serve, so they are a seed rather than a constant now.
AbilityConfig& mutable_ability_config() {
    static AbilityConfig cfg = [] {
        AbilityConfig seed;
        // Cascade: per-link radius; Chain resolves multiple links. High rate
        // over a short lifetime so it reads as instant.
        seed.ability[static_cast<u32>(AbilityId::ComplementCascadeBurst)] =
            AbilityTuning{90.0f, 6.0f, 120.0f, 1.5f, 3.0f};
        seed.ability[static_cast<u32>(AbilityId::HistamineFlare)] =
            AbilityTuning{45.0f, 14.0f, 30.0f, 1.5f, 3.0f};
        // Fever spends no field at all; only the relief figure matters.
        seed.ability[static_cast<u32>(AbilityId::FeverResponse)] =
            AbilityTuning{60.0f, 20.0f, 40.0f, 1.5f, 3.0f};
        return seed;
    }();
    return cfg;
}

} // namespace

const AbilityConfig& ability_config() { return mutable_ability_config(); }

void apply_ability_config(ActiveAbilitySystem& system, const AbilityConfig& cfg) {
    mutable_ability_config() = cfg;
    system.load_defaults();
}

} // namespace immune::game
