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
            AbilityTuning{90.0f, 6.0f, 120.0f, 1.5f, 3.0f, 7.0f, 1.5f};
        seed.ability[static_cast<u32>(AbilityId::HistamineFlare)] =
            AbilityTuning{45.0f, 14.0f, 30.0f, 1.5f, 3.0f, 7.0f, 1.5f};
        // Fever spends no field at all; only the relief figure matters.
        seed.ability[static_cast<u32>(AbilityId::FeverResponse)] =
            AbilityTuning{60.0f, 20.0f, 40.0f, 1.5f, 3.0f, 7.0f, 1.5f};
        // Clot: no field either. field_duration is how long the bar stands;
        // the two barrier figures are its half-size. 14 x 3 world units sits
        // across a little over half of a shipped level's 23-wide lane, so
        // the horde is squeezed, never sealed, and the flow field has a gap to
        // route through on at least one side.
        seed.ability[static_cast<u32>(AbilityId::FibrinClot)] =
            AbilityTuning{75.0f, 0.0f, 0.0f, 8.0f, 3.0f, 7.0f, 1.5f};
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
