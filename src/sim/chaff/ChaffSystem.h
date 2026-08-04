// sim/chaff/ChaffSystem.h — batch chaff movement kernel. FROZEN CONTRACT.
// Owner: Wave 1B.
//
// RATIONALE (DESIGN.md §8.3)
// Per agent, per tick, the entire "AI" is:
//     v += flow.sample(p) * speed        // one bilinear field fetch
//     v += separation(p) * k             // 3x3 spatial-hash cell scan
//     v  = clamp_length(v, max_speed)
//     p += v * dt
// No A*, no per-agent state machine, no virtual call. Everything else the horde
// does (replication, clumping, drift) is a family-specific variation on those
// four lines, gated by a flag bit — never by a subclass.
//
// PARALLELISM & DETERMINISM
// The update splits [0, count) across JobSystem ranges. Agents read the
// *previous* tick's positions for separation and write only their own slot, so
// there is no write contention and the result does not depend on scheduling.
// Randomness (jitter, replication rolls) comes from a per-range Rng forked from
// the sim Rng by range index — never from a shared generator.
#pragma once

#include "core/Types.h"

namespace immune { class JobSystem; class Rng; }

namespace immune::sim {

class ChaffBuffers;
class FlowField;
class SpatialHash;

/// Per-family movement tuning. Loaded from data by Wave 2C; the layout is
/// fixed here so the movement kernel can index it with a u8 family id.
struct ChaffFamilyParams {
    f32 max_speed = 6.0f;
    f32 acceleration = 24.0f;
    f32 separation_radius = 1.2f;
    f32 separation_strength = 8.0f;
    f32 jitter = 0.4f;          ///< Random impulse magnitude; the "alive" look.
    f32 base_density = 1.0f;    ///< Spawn density/HP contribution.
    f32 drift_bias = 0.0f;      ///< Fungal spores: how much ambient drift overrides flow.
    f32 replication_rate = 0.0f;///< Viruses: expected replications per agent per second.
    f32 radius = 0.5f;          ///< Visual/collision radius, drives sprite scale.
};

struct ChaffTuning {
    ChaffFamilyParams family[kFamilyCount];
    /// Global cap on agents spawned by replication per tick, so an unchecked
    /// viral wave degrades performance predictably instead of exploding.
    u32 max_replications_per_tick = 128;
    /// Ambient drift direction/strength applied to kDrifting agents.
    Vec2 ambient_drift{0.0f, 0.0f};
};

struct ChaffUpdateStats {
    u32 moved = 0;
    u32 replicated = 0;
    u32 despawned_at_goal = 0;
    u32 despawned_out_of_bounds = 0;
};

/// Stateless-by-design: all mutable state lives in ChaffBuffers. This class
/// holds only tuning and scratch, so it can be recreated freely.
class ChaffSystem {
public:
    void set_tuning(const ChaffTuning& tuning) { tuning_ = tuning; }
    const ChaffTuning& tuning() const { return tuning_; }

    /// One fixed 60 Hz step. Order within a tick is fixed and must not change:
    ///   1. hash.rebuild(...)      (caller, profiled as spatial_hash)
    ///   2. update(...)            (profiled as chaff_update)
    ///   3. damage systems         (profiled separately)
    ///   4. buffers.compact()      (caller)
    /// `jobs` may be null for a serial update. `rng` is the sim's generator and
    /// is forked per range — it is advanced deterministically by exactly one
    /// fork call per range, so tick results do not depend on thread count.
    ChaffUpdateStats update(ChaffBuffers& buffers,
                            const FlowField& flow,
                            const SpatialHash& hash,
                            Rng& rng,
                            f32 dt,
                            JobSystem* jobs);

    /// Spawns `count` agents of `family` inside `portal_radius` of `portal`,
    /// jittered by `rng`. Returns how many were actually created (fewer at capacity).
    u32 spawn_burst(ChaffBuffers& buffers, PathogenFamily family, Vec2 portal,
                    f32 portal_radius, u32 count, Rng& rng) const;

    /// Despawn bounds. Agents leaving this rect are removed; the level loader
    /// sets it to the tissue bounds plus a margin.
    void set_world_bounds(const Rect& bounds) { bounds_ = bounds; }
    const Rect& world_bounds() const { return bounds_; }

    /// Radius around the objective at which chaff is consumed and scores damage
    /// against organ integrity.
    void set_goal(Vec2 goal, f32 radius) { goal_ = goal; goal_radius_ = radius; }

private:
    ChaffTuning tuning_{};
    Rect bounds_{};
    Vec2 goal_{0.0f, 0.0f};
    f32 goal_radius_ = 2.0f;
};

} // namespace immune::sim
