// game/abilities/ActiveAbilities.h — player-triggered emergency abilities.
// FROZEN CONTRACT. Owner: Wave 4C. New module (DESIGN.md §5.6).
//
// RATIONALE
// Three cooldown-gated, player-triggered, screen-changing tools, deliberately
// separate from towers: no placement, no per-target selection beyond an
// optional world-space point. They exist to rescue a moment a pure
// tower-and-economy response can't handle -- an amplifier or a one-time
// bailout, never a substitute for having placed towers well. Long cooldowns
// (multiple waves, not multiple seconds) are load-bearing: choosing *when* to
// spend one is meant to be a real decision.
//
// Complement Cascade Burst and Histamine Flare both resolve through the
// existing, already-implemented DamageField/DamageSystem contract (sim/damage)
// -- this module never touches chaff or towers directly, it only submits
// fields, exactly like a tower's Combat-phase system does. Fever Response is
// the one ability that reaches into the ECS directly (see .cpp) rather than
// through a damage field, since "buff every tower" has no damage-field
// equivalent; it does so by reading/writing the already-public comp::Tower
// component, the same access pattern the renderer and HUD already use.
#pragma once

#include "core/Types.h"

namespace immune::sim { class SimWorld; }

namespace immune::game {

enum class AbilityId : u8 {
    ComplementCascadeBurst = 0,
    HistamineFlare = 1,
    FeverResponse = 2,
    Count = 3,
};

inline constexpr u32 kAbilityCount = static_cast<u32>(AbilityId::Count);

/// Static tuning for one ability. Numeric balance is intentionally rough
/// (DESIGN.md's own closing note) -- the shape is the contract, not these values.
struct AbilityDef {
    const char* name = "";
    f32 cooldown_seconds = 60.0f;
    /// Complement/Histamine: the DamageField's radius and kill_rate.
    f32 radius = 20.0f;
    f32 kill_rate = 40.0f;
    /// Histamine only: how long the nova field persists (Complement resolves
    /// as an instant chain, per DamageField.h's Chain shape semantics).
    f32 field_duration = 1.5f;
    /// Fever only: seconds shaved off every currently-placed tower's *current*
    /// cooldown on cast -- an instant burst of responsiveness rather than a
    /// sustained rate multiplier, chosen deliberately to avoid needing to
    /// track-and-revert a temporary buff across ticks (see .cpp rationale).
    f32 fever_cooldown_relief = 3.0f;
};

/// Player-facing snapshot for the HUD ability bar.
struct AbilityStatus {
    bool ready = false;
    f32 cooldown_remaining = 0.0f;
    f32 cooldown_total = 0.0f;
};

class ActiveAbilitySystem {
public:
    /// Populates the DESIGN.md §5.6 defaults. Call once after construction.
    void load_defaults();

    /// One fixed 60 Hz step: decrements cooldown timers. Deterministic (no
    /// RNG, no branching on wall-clock time) despite living outside SimWorld --
    /// it only advances a per-ability f32 by dt, so replay/--sim-test
    /// determinism is unaffected by where this is called from.
    void tick(f32 dt);

    bool ready(AbilityId id) const;
    AbilityStatus status(AbilityId id) const;

    /// Casts the ability if ready; no-ops (returns false) otherwise. Complement
    /// Cascade Burst and Histamine Flare use `target_point` (world space);
    /// Fever Response ignores it.
    bool cast(sim::SimWorld& world, AbilityId id, Vec2 target_point);

    const AbilityDef& def(AbilityId id) const { return defs_[static_cast<u32>(id)]; }

private:
    AbilityDef defs_[kAbilityCount]{};
    f32 cooldown_remaining_[kAbilityCount] = {};
};

const char* ability_name(AbilityId id);

} // namespace immune::game
