// sim/projectile/Projectiles.h — SoA projectile store + integration/collision.
// FROZEN CONTRACT. Owner: Wave 6A.
//
// WHY THIS EXISTS (and why it does not contradict DamageField.h)
// DamageField.h states, correctly, that chaff is never hit individually by the
// *aggregate* damage path. That remains true and remains the load-bearing
// performance decision: area weapons (timed bursts, Cryo's signal
// cone, Tesla's chain, Laser's beam, Blade's rotor) all still publish a
// DamageField and never test pairs.
//
// The Gunner is the deliberate exception. Its whole identity is a visible
// stream of individual rounds that fly, land, and pop — a field centred on the
// target reads as a glow, not as gunfire. So projectiles are a real, separate,
// mass-simulated layer:
//
//   - SoA and capacity-bounded, exactly like ChaffBuffers. Never per-projectile
//     virtual dispatch, never a heap allocation in flight, never an ECS entity
//     per round (comp::Ephemeral exists for a handful of named spawns; it is
//     the wrong tool for thousands of bullets a second).
//   - Agent collision is APPROXIMATE BY DESIGN (explicit user decision). A
//     round asks the spatial hash for its own cell and damages one agent found
//     there. Wall collision is swept through the tissue grid so a fast round
//     cannot tunnel through a thin obstacle between ticks. There is still no
//     nearest-of-all-agent-candidates search.
//   - Cost therefore scales with live round count and is independent of chaff
//     count: one hash cell lookup plus the few tissue cells crossed per round
//     per tick, with no pair tests.
//
// DETERMINISM
// This runs inside the fixed 60 Hz sim tick and contributes to state_hash().
// Same seed + same thread count => identical results. Take an Rng& for spread;
// never read a clock; never accumulate shared floats across threads.
//
// This layer is SIMULATION. The cosmetic tracers, muzzle flashes, and impact
// pops that make a Gunner burst look like a Gunner burst are NOT here — they
// live in vfx/Particles.h, driven by vfx/CombatEvents.h. Deleting every
// particle in the game must not change one bit of state_hash().
#pragma once

#include "core/Types.h"
#include "sim/Attribution.h"

#include <vector>

namespace immune { class Rng; }

namespace immune::sim {

class ChaffBuffers;
class SpatialHash;
class CombatEventSink;
class TissueMask;

namespace projectile_flags {
inline constexpr u8 kAlive       = 1u << 0; ///< Slot occupied.
inline constexpr u8 kPendingKill = 1u << 1; ///< Retire in the next compact().
/// Passes through its first victim and keeps flying (pierce upgrades). Without
/// this a round retires on its first hit.
inline constexpr u8 kPiercing    = 1u << 2;
} // namespace projectile_flags

struct ProjectileSpawnParams {
    Vec2 position{0.0f, 0.0f};
    /// World units per second. The system integrates position by this directly;
    /// it does not renormalize, so callers control speed via this vector.
    Vec2 velocity{0.0f, 0.0f};
    /// Density removed from the single agent this round hits. Not a per-second
    /// rate — a projectile applies its damage once, on impact.
    f32 damage = 1.0f;
    /// Seconds before the round self-retires if it never hits anything. Keeps
    /// strays from living forever and bounds the live set.
    f32 lifetime = 1.5f;
    /// Impact test radius. Kept small; this is a cell-local proximity check,
    /// not a physical body.
    f32 hit_radius = 0.5f;
    /// Bitmask of PathogenFamily bits this round may damage. 0xFF = all.
    u8 family_mask = 0xFF;
    u8 flags = 0;
    /// Owning tower, for kill attribution back to the economy.
    EntityId owner{};
    /// Purely cosmetic: lets the VFX layer pick a tracer look per tower tier
    /// without the sim knowing anything about rendering.
    u16 visual_id = 0;
};

/// The projectile store. One instance per sim world.
///
/// INVARIANTS (asserted in debug, checked by --sim-test):
///   P1. Every index in [0, count) has projectile_flags::kAlive set.
///   P2. Every stream reports size() == capacity and identical size to every other.
///   P3. count <= capacity at all times; spawn() never grows a stream.
class ProjectileBuffers {
public:
    // Parallel SoA streams. Public by design, same rationale as ChaffBuffers:
    // the integration loop, the collision loop, and the renderer's instance
    // upload all walk them directly. Treat as read-only outside sim/projectile.
    std::vector<f32> pos_x;
    std::vector<f32> pos_y;
    std::vector<f32> vel_x;
    std::vector<f32> vel_y;
    std::vector<f32> damage;
    std::vector<f32> life;        ///< Seconds remaining; <= 0 retires the round.
    std::vector<f32> hit_radius;
    std::vector<u8>  family_mask;
    std::vector<u8>  flags;
    std::vector<u16> visual_id;
    std::vector<EntityId> owner;

    /// Reserves every stream. Call once at level load. Sized generously — the
    /// Gunner is expected to keep thousands of rounds live at high tiers.
    void reserve(usize max_projectiles);

    usize count() const { return count_; }
    usize capacity() const { return capacity_; }
    bool full() const { return count_ >= capacity_; }

    /// Appends one round. Silently drops (returns false) when full: a missing
    /// tracer is invisible, a reallocation mid-tick is not acceptable.
    bool spawn(const ProjectileSpawnParams& params);

    /// Flags a round for removal. Removal happens in compact(), so indices stay
    /// stable within a tick.
    void kill(usize index);

    /// Swap-removes every kPendingKill round. Returns the number removed.
    usize compact();

    void clear();

private:
    void assert_invariants() const;

    usize count_ = 0;
    usize capacity_ = 0;
};

struct ProjectileStats {
    u32 live = 0;
    u32 spawned_this_tick = 0;
    u32 impacts = 0;          ///< Rounds that hit an agent.
    u32 wall_impacts = 0;     ///< Rounds destroyed by tissue/obstacle walls.
    u32 expired = 0;          ///< Rounds that timed out or left the world.
    f32 density_removed = 0.0f;
};

class ProjectileSystem {
public:
    /// One tick: integrate, sweep through the tissue grid for walls, test each
    /// surviving round against its own spatial-hash cell, apply damage on
    /// impact, retire spent/expired rounds, compact.
    ///
    /// `events` may be null. When present, every impact and expiry is reported
    /// so the VFX layer can draw pops and streaks; the sim's own behaviour must
    /// be byte-identical whether or not a sink is attached.
    ///
    /// Does NOT compact the chaff store — the caller runs ChaffBuffers::compact()
    /// once, after every damage source (fields and projectiles) has applied.
    ProjectileStats update(ProjectileBuffers& projectiles,
                           ChaffBuffers& chaff,
                           const SpatialHash& hash,
                           const TissueMask& tissue,
                           const Rect& world_bounds,
                           Rng& rng,
                           f32 dt,
                           CombatEventSink* events);

    const ProjectileStats& last_stats() const { return last_; }

    /// Attaches (or detaches, with null) the per-owner accounting sink, exactly
    /// as DamageSystem does -- the Gunner is the one tower whose entire output
    /// arrives through this path, so a sink that skipped it would rank it at
    /// zero. Null by default; see sim/Attribution.h.
    void set_attribution(DamageAttribution* sink) { attribution_ = sink; }
    DamageAttribution* attribution() const { return attribution_; }

private:
    ProjectileStats last_{};
    DamageAttribution* attribution_ = nullptr;
};

} // namespace immune::sim
