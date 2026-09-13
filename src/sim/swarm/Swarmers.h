// sim/swarm/Swarmers.h — SoA swarmer store + seek/attach/drain integration.
// Owner: the Cytotoxic T redesign.
//
// WHAT A SWARMER IS
// One lytic granule released by a Cytotoxic T cell. It flies out of the cell's
// synapse face, picks a pathogen, latches onto it, and drains it until
// either the host dies — at which point the swarmer detaches and finds another
// — or the swarmer's own lifetime runs out. The tower keeps releasing more the
// whole time, so what the player reads is not a beam or a blast but a small
// standing CLOUD of the tower's own units eating into the horde: a horde
// answering a horde.
//
// WHY THIS IS NOT A DamageField, AND NOT A ProjectileBuffer
// DamageField.h is right that area weapons must never test pairs, and the
// Cytotoxic T used to be one: a Chain field, one disc of damage, drawn as a
// glowing circle. That visual was the problem — a disc communicates "an area is
// being hurt", and nothing about it says the tower is a cell that kills other
// cells one at a time.
//
// Projectiles.h is the closer relative and still the wrong home. A round is
// FIRE-AND-FORGET: it flies straight, hits at most one agent, and its whole
// cost model rests on never searching for a target (see that file's collision
// note, which is still correct and still applies to the Gunner). A swarmer is
// the opposite on every axis — it steers, it chooses, it persists on a victim
// across ticks, and it re-chooses when that victim dies. Bolting that onto
// ProjectileSystem would have quietly repealed the one design rule that file
// exists to state.
//
// So this is its own layer, with its own cost model, stated plainly:
//
//   - An ATTACHED swarmer costs one handle resolve and one density write per
//     tick. No search at all. This is the common case by a wide margin, because
//     a swarmer spends most of its life latched.
//   - An UNATTACHED swarmer costs one spatial-hash circle query. That is the
//     expensive path, and it runs only on the ticks where a swarmer has just
//     spawned or has just lost its host.
//
// Cost therefore scales with live swarmer count and with how often hosts die,
// and is independent of total chaff count. It is emphatically NOT free the way
// an aggregate field is; it is a deliberate trade of cycles for the read.
//
// IDENTITY
// Targets are held as ChaffHandle, not as a raw index: chaff compaction swaps
// live agents down over dead slots every tick, so a stored index would silently
// re-point a swarmer at whichever unlucky agent got moved into that slot. The
// handle is exactly what ChaffBuffers::resolve() exists for, and this is the
// one system in the game that genuinely needs it.
//
// DETERMINISM
// This runs inside the fixed 60 Hz tick and contributes to state_hash(). Same
// seed and same thread count give identical results. Target choice breaks ties
// by ascending chaff index. The wander that gives the cloud its life is driven
// by a PER-SWARMER seed advanced locally, NOT by the shared sim Rng — drawing
// from the shared stream here would make every downstream system's numbers
// depend on how many swarmers happened to be alive, which is a far nastier
// coupling than it looks.
//
// This layer is SIMULATION. The glow, the trail, and the death pop are not
// here; they live in the renderer and vfx/Particles.h.
#pragma once

#include "core/Types.h"
#include "sim/Attribution.h"
#include "sim/chaff/ChaffBuffers.h"

#include <vector>

namespace immune { class Rng; }

namespace immune::sim {

class SpatialHash;
class CombatEventSink;

namespace swarmer_flags {
inline constexpr u8 kAlive       = 1u << 0; ///< Slot occupied.
inline constexpr u8 kPendingKill = 1u << 1; ///< Retire in the next compact().
inline constexpr u8 kAttached    = 1u << 2; ///< Currently latched onto its target.
} // namespace swarmer_flags

struct SwarmerSpawnParams {
    Vec2 position{0.0f, 0.0f};
    /// Initial launch velocity. Steering takes over from the first tick; this
    /// only decides which way the granule leaves the synapse.
    Vec2 velocity{0.0f, 0.0f};
    /// Density drained per SECOND while attached. Unlike a projectile's
    /// one-shot `damage`, a swarmer is a damage-over-time source.
    f32 damage_per_second = 6.0f;
    /// Seconds before the swarmer dissolves regardless of what it is doing.
    /// This is what makes the cloud a standing population rather than an
    /// ever-growing one: spawn rate against lifetime sets the equilibrium size.
    f32 lifetime = 3.0f;
    /// How close the swarmer must get before it latches on.
    f32 attach_radius = 0.55f;
    /// How far it will look for a new host when it has none.
    f32 search_radius = 9.0f;
    f32 speed = 14.0f;
    u8 family_mask = 0xFF;
    /// Owning tower, for kill attribution back to the economy.
    EntityId owner{};
    /// Cosmetic tier selector, passed through to the renderer untouched.
    u16 visual_id = 0;
    /// Seeds this swarmer's private wander stream. Give each one a different
    /// value or the whole cloud flies in perfect formation.
    u32 seed = 0;
};

/// The swarmer store. One instance per sim world.
///
/// INVARIANTS:
///   S1. Every index in [0, count) has swarmer_flags::kAlive set.
///   S2. Every stream reports size() == capacity and identical size to the rest.
///   S3. count <= capacity at all times; spawn() never grows a stream.
class SwarmerBuffers {
public:
    // Parallel SoA streams. Public by design, same rationale as ChaffBuffers
    // and ProjectileBuffers: the integration loop and the renderer's instance
    // upload walk them directly. Treat as read-only outside sim/swarm.
    std::vector<f32> pos_x;
    std::vector<f32> pos_y;
    std::vector<f32> vel_x;
    std::vector<f32> vel_y;
    std::vector<f32> dps;
    std::vector<f32> life;          ///< Seconds remaining; <= 0 dissolves it.
    std::vector<f32> attach_radius;
    std::vector<f32> search_radius;
    std::vector<f32> speed;
    std::vector<u32> target_index;      ///< ChaffHandle halves, kept as two
    std::vector<u32> target_generation; ///< streams so the SoA stays scalar.
    std::vector<u8>  family_mask;
    std::vector<u8>  flags;
    std::vector<u16> visual_id;
    std::vector<u32> seed;
    std::vector<EntityId> owner;

    /// Reserves every stream. Call once at level load.
    void reserve(usize max_swarmers);

    usize count() const { return count_; }
    usize capacity() const { return capacity_; }
    bool full() const { return count_ >= capacity_; }

    /// Appends one swarmer. Silently drops (returns false) when full — a
    /// missing granule is invisible in a cloud, a mid-tick reallocation is not
    /// acceptable.
    bool spawn(const SwarmerSpawnParams& params);

    /// Flags a swarmer for removal. Removal happens in compact(), so indices
    /// stay stable within a tick.
    void kill(usize index);

    /// Swap-removes every kPendingKill swarmer. Returns the number removed.
    usize compact();

    void clear();

    ChaffHandle target(usize i) const {
        return ChaffHandle{target_index[i], target_generation[i]};
    }

private:
    usize count_ = 0;
    usize capacity_ = 0;
};

struct SwarmerStats {
    u32 live = 0;
    u32 spawned_this_tick = 0;
    u32 attached = 0;          ///< Latched onto a host this tick.
    u32 searching = 0;         ///< Hostless, paid for a spatial query.
    u32 expired = 0;           ///< Dissolved on lifetime.
    u32 hosts_finished = 0;    ///< Targets that died under a swarmer.
    f32 density_removed = 0.0f;
};

class SwarmerSystem {
public:
    /// One tick: resolve targets, acquire new ones for the hostless, steer,
    /// integrate, drain attached hosts, retire the expired, compact.
    ///
    /// `events` may be null. When present, latches and host deaths are reported
    /// so the VFX layer can pop; the sim's behaviour must be byte-identical
    /// whether or not a sink is attached.
    ///
    /// Does NOT compact the chaff store — the caller runs ChaffBuffers::compact()
    /// once, after every damage source has applied.
    SwarmerStats update(SwarmerBuffers& swarmers,
                        ChaffBuffers& chaff,
                        const SpatialHash& hash,
                        const Rect& world_bounds,
                        Rng& rng,
                        f32 dt,
                        CombatEventSink* events);

    const SwarmerStats& last_stats() const { return last_; }

    /// Per-owner accounting sink, null by default. The NK Cell's blades are a
    /// damage path of their own, so a sink that skipped them would rank that
    /// tower at zero. See sim/Attribution.h.
    void set_attribution(DamageAttribution* sink) { attribution_ = sink; }

private:
    SwarmerStats last_{};
    DamageAttribution* attribution_ = nullptr;
    /// Scratch for the hostless swarmers' hash queries. A member so the vector
    /// is allocated once and reused, never per-swarmer inside the tick.
    std::vector<u32> scratch_;
};

} // namespace immune::sim
