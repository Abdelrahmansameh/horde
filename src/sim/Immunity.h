// sim/Immunity.h — the player's permanent, world-wide combat modifiers.
//
// RATIONALE (PROGRESSION.md §5/§6)
// The Strengthen Immunity tree (game/meta/ImmunityTree.h) buys permanent
// upgrades between runs. Almost all of them are just different numbers in
// towers.json / economy.json / abilities.json, and the game layer folds them
// into the config it hands every other system -- the sim never hears about
// them. What is left over is the handful of purchases that are NOT a number
// any existing system already reads: a rule about what happens when a round
// lands, when a slowed agent dies, when something presses against a scar, how
// hard an elite is hit, how much a leak costs. Those live here, as one flat
// record the game layer sets on the world at level load.
//
// The per-unit capstones (a Cytotoxic T latcher's kill pulse, a Macrophage
// healing its tower) are NOT here: they belong to the unit that does them and
// ride its sim::SwarmerProfile instead, the same way every other thing a
// swarmer does on contact does. This struct is only for effects with no unit
// to hang them on.
//
// Every default is "off" / "no change", so a world that never has this set --
// every test, every bench, every headless mode -- is exactly the world it was
// before the tree existed.
#pragma once

#include "core/Types.h"

namespace immune::sim {

/// A disc of inflamed tissue around a standing scar (Inflammation, below).
/// SimWorld rebuilds the list every tick from the live scars.
struct InflamedZone {
    Vec2 center{0.0f, 0.0f};
    f32 radius = 0.0f;
};

struct ImmunityTuning {
    /// Multiplier on every hit point a named agent (elite/boss) loses to the
    /// player's swarmers and bursts. Elite Response.
    f32 named_damage_mult = 1.0f;

    /// Objective integrity lost per agent that reaches the goal. The shipped
    /// config's sim.globals.objective_damage_per_leak, scaled by Homeostasis.
    f32 leak_damage = 1.0f;

    /// Incendiary Rounds (Neutrophil capstone). Every round that lands leaves
    /// a burning patch: a timed damage field of this radius that takes
    /// `incendiary_damage` density off an agent at its centre over
    /// `incendiary_seconds`. Radius 0 is off.
    f32 incendiary_radius = 0.0f;
    f32 incendiary_damage = 0.0f;
    f32 incendiary_seconds = 1.0f;
    /// Patches laid per tick at most. A full Neutrophil board lands hundreds
    /// of rounds a second; past this the extra rounds simply do not ignite,
    /// which keeps the damage-field budget for everything else.
    u32 max_incendiary_per_tick = 48;

    /// Anaphylactic Shock (Goblet Cell capstone). A slowed agent that dies
    /// passes its slow on to every chaff agent within this radius, for this
    /// many seconds, at the dying agent's own slow factor. Radius 0 is off.
    f32 contagion_radius = 0.0f;
    f32 contagion_seconds = 2.0f;
    /// Deaths that spread per tick at most, for the same reason as above.
    u32 max_contagion_per_tick = 64;

    /// Inflammatory Scarring (Fibroblast capstone). Every standing scar burns
    /// chaff pressed against it: density per second, out to `scar_contact_reach`
    /// beyond the bar's faces. Rate 0 is off.
    f32 scar_contact_rate = 0.0f;
    f32 scar_contact_reach = 0.8f;

    /// Weakening Mucus (Goblet Cell line). A slowed agent takes this much
    /// more from every damage source while the slow lasts -- chaff through
    /// ChaffBuffers::apply_density_loss, named agents through the named-damage
    /// paths. 1 is off.
    f32 slowed_damage_mult = 1.0f;

    /// Inflammation (Fibroblast line). Every standing scar inflames the tissue
    /// out to `inflammation_radius` past its ends: the player's swarmers inside
    /// deal `inflammation_damage_mult` times their damage, and shooters inside
    /// reload `inflammation_reload_mult` times as fast. Radius 0 is off.
    f32 inflammation_radius = 0.0f;
    f32 inflammation_damage_mult = 1.0f;
    f32 inflammation_reload_mult = 1.0f;

    /// True when anything here differs from "off" (tests, debug readouts).
    bool any_active() const {
        return incendiary_radius > 0.0f || contagion_radius > 0.0f || scar_contact_rate > 0.0f ||
               slowed_damage_mult != 1.0f || inflammation_radius > 0.0f;
    }
};

} // namespace immune::sim
