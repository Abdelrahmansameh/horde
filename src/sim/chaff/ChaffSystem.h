// sim/chaff/ChaffSystem.h — batch chaff movement kernel. FROZEN CONTRACT.
// Owner: Wave 1B. `sdf` parameter added post-Wave-2 (orchestrator amendment,
// see below) to fix a real bug, not a style change.
//
// RATIONALE (DESIGN.md §8.3, §12.3)
// Per agent, per tick, the entire "AI" is:
//     dir = flow.sample(p)                          // one bilinear field fetch
//     if dir is (0,0): dir = sdf.gradient(p)         // recovery, see below
//     v  += dir * speed
//     v  += separation(p) * k                        // 3x3 spatial-hash cell scan
//     v   = clamp_length(v, max_speed)
//     p  += v * dt
// No A*, no per-agent state machine, no virtual call. Everything else the horde
// does (replication, drift) is a family-specific variation on those
// lines, gated by a flag bit — never by a subclass.
//
// WHY THE SDF FALLBACK EXISTS (found by live-testing, not by inspection)
// `FlowField::sample()` returns (0,0) once a point is outside the baked
// walkable region — by design, per FlowField.h. Without a fallback, an agent
// nudged past the tissue-mask edge by separation/jitter permanently loses all
// directional guidance: nothing pulls it back, so it random-walks on jitter
// alone until it happens to cross the level's outer world bounds and gets
// despawned there, tens of seconds later, never having threatened the
// objective. Measured: ~95% of chaff spawned at a real portal never reached
// the goal before this fix (tests/scripts/portal_spawn_reaches_goal.json).
// `DistanceField::gradient()` — "direction of increasing clearance" — is
// exactly the recovery vector needed: it points back into the tissue from
// anywhere outside it. This is a *safety net*, not a replacement for a
// smoother wall-cost gradient in the bake itself (DESIGN.md §12.3/§14 still
// flags that as the real, remaining fluid-feel tuning target) — it guarantees
// no agent can get permanently lost, independent of how well-tuned the bake
// is.
//
// PARALLELISM & DETERMINISM
// The update splits [0, count) across JobSystem ranges. Agents read the
// *previous* tick's positions for separation and write only their own slot, so
// there is no write contention and the result does not depend on scheduling.
// Randomness (jitter, replication rolls) comes from a per-range Rng forked from
// the sim Rng by range index — never from a shared generator.
#pragma once

#include "core/Types.h"

#include <vector>

namespace immune { class JobSystem; class Rng; }

namespace immune::sim {

class ChaffBuffers;
class FlowField;
class DistanceField;
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
    f32 drift_bias = 0.0f;      ///< How much ambient drift overrides flow.
    f32 replication_rate = 0.0f;///< Viruses: expected replications per agent per second.
    f32 radius = 0.5f;          ///< Visual/collision radius, drives sprite scale.

    // ---- Fluid-feel additions (movement overhaul) --------------------------
    // The three classic boid rules minus cohesion (the flow field already
    // supplies "go the same way", so a cohesion term would just fight it and
    // clump the horde into balls). Separation above is rule one; these are the
    // rest. All three read the SAME neighbour gather -- see gather_neighbours()
    // in ChaffSystem.cpp -- so adding them costs arithmetic per neighbour, not
    // an extra spatial-hash pass.

    /// Radius over which velocities are averaged. Should be >= separation_radius
    /// (a neighbour close enough to shove you is close enough to steer you) and
    /// must stay within the 3x3 cell scan, i.e. <= spatial cell_size.
    f32 alignment_radius = 2.4f;
    /// How hard an agent steers toward its neighbours' average heading. THIS is
    /// what makes a mass read as one moving body instead of independent dots:
    /// a disturbance at the front (a wall, a tower) propagates backwards
    /// neighbour-to-neighbour as a visible wave. No agent knows about the wave.
    f32 alignment_strength = 3.0f;

    /// Neighbour count above which the local crowd counts as "packed". Below
    /// this, separation alone handles spacing and pressure contributes nothing.
    f32 pressure_threshold = 6.0f;
    /// Extra separation gain per neighbour past the threshold. Turns a jam into
    /// something that visibly builds and then releases sideways, instead of
    /// agents quietly overlapping. Paired with wall collision this is what
    /// produces DESIGN.md 4.2's splash-and-redirect.
    f32 pressure_gain = 0.12f;
    /// Ceiling on the pressure multiplier, so one pathological cell cannot
    /// launch its occupants across the level.
    f32 pressure_max = 4.0f;

    /// Fraction of the wall-normal velocity component reflected on contact.
    /// 0 = fully inelastic (slides along the wall, the dense-crowd default),
    /// 1 = a perfect bounce. Kept low: a horde is wet, not rubber.
    f32 wall_restitution = 0.15f;

    /// Fraction of the speed a wall just BLOCKED that gets redirected along the
    /// wall instead of being thrown away.
    ///
    /// Without this, a head-on impact simply deletes the agent's momentum and
    /// it stops dead against the wall -- which is why the horde read as
    /// "stopping" rather than "splashing". A real splash conserves the flow:
    /// what cannot go forward goes sideways. 0 = absorb everything (dead stop),
    /// 1 = redirect all of it into the tangent.
    f32 wall_splash = 0.75f;

    /// Hard minimum centre-to-centre spacing, as a multiple of `radius`.
    ///
    /// This backs the POSITIONAL overlap pass, which is what actually keeps
    /// agents from interpenetrating. The velocity-space separation impulse
    /// above cannot do that job on its own at high density: it is a force, and
    /// forces are clamped by max_speed and are competing with the flow field
    /// for that same budget, so however deep the overlap gets, agents can only
    /// unstack at walking pace. Projecting positions apart is unconditional and
    /// takes effect the same tick -- see the contact-relaxation pass in
    /// ChaffSystem.cpp.
    ///
    /// 2.0 means "two agents may not be closer than the sum of their radii",
    /// i.e. exactly touching. Slightly above 2 leaves a visible gap.
    f32 contact_spacing = 2.0f;
    /// How much of each detected overlap is corrected per tick, 0..1. Below 1
    /// the crowd settles over a few ticks instead of snapping, which reads as
    /// a dense fluid relaxing rather than as a rigid lattice popping apart.
    f32 contact_stiffness = 0.7f;
};

struct ChaffTuning {
    ChaffFamilyParams family[kFamilyCount];
    /// Global cap on agents spawned by replication per tick, so an unchecked
    /// viral wave degrades performance predictably instead of exploding.
    u32 max_replications_per_tick = 128;
    /// Ambient drift direction/strength applied to kDrifting agents.
    Vec2 ambient_drift{0.0f, 0.0f};

    /// Hard ceiling on how many neighbours ONE agent inspects per tick.
    ///
    /// This is the single most important number for keeping the fluid-feel
    /// rules affordable at high density. Per-neighbour cost is fixed and small,
    /// but neighbour COUNT grows with density -- without a cap, doubling the
    /// horde roughly quadruples this pass, because each of twice as many agents
    /// also sees twice as many neighbours. Capping makes the pass linear in
    /// agent count again at any density.
    ///
    /// Deterministic despite being a truncation: the spatial hash's CSR order
    /// is a pure function of agent positions, so "the first N in cell order" is
    /// the same set on every machine and at every thread count. Cutting off is
    /// also visually harmless -- the sample is already a local average, and
    /// the 24 nearest neighbours describe the same crowd as the 200 nearest.
    u32 max_neighbors_sampled = 24;
};

struct ChaffUpdateStats {
    u32 moved = 0;
    u32 replicated = 0;
    u32 despawned_at_goal = 0;
    u32 despawned_out_of_bounds = 0;
    /// Same two tallies split by family. Compaction cannot tell a leak from a
    /// kill (both arrive as kPendingKill), so this pass -- the only one that
    /// knows *why* an agent is retiring -- is where the split has to be made.
    u32 despawned_at_goal_by_family[kFamilyCount] = {};
    u32 despawned_out_of_bounds_by_family[kFamilyCount] = {};
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
    /// `sdf` is the same level's DistanceField, used only as the off-mask
    /// recovery fallback described above — a default-constructed (never
    /// baked) DistanceField is safe to pass (its sample()/gradient() both
    /// return zero), so callers that don't care about recovery behavior
    /// (most unit tests) don't need to bake one.
    ChaffUpdateStats update(ChaffBuffers& buffers,
                            const FlowField& flow,
                            const DistanceField& sdf,
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
    // ---- Per-tick scratch (Wave 1B). Sized to buffers.capacity() on first use
    // and never after: reserved-once, no-alloc-in-tick per docs/CONVENTIONS.md.
    // Private implementation detail; does not change the public contract.
    //
    // old_pos_{x,y}_: a snapshot of positions taken before this tick's movement
    // pass. Separation reads neighbours through this snapshot, never through the
    // live pos_x/pos_y arrays, because those are being written in place by
    // whichever range owns each index — reading a neighbour's *live* slot would
    // make the result depend on which range happened to run first. This is what
    // "read the previous tick's positions" (see file header) actually requires.
    std::vector<f32> old_pos_x_;
    std::vector<f32> old_pos_y_;
    /// Pre-tick velocity snapshot, for exactly the same reason as old_pos_*:
    /// alignment averages NEIGHBOURS' velocities, and those slots are being
    /// overwritten in place by whichever range owns them. Reading a neighbour's
    /// live velocity would make alignment depend on range scheduling order, and
    /// alignment is a feedback loop, so that divergence would compound rather
    /// than stay a rounding difference.
    std::vector<f32> old_vel_x_;
    std::vector<f32> old_vel_y_;
    /// Per-agent POSITIONAL correction for this tick: how far to shove this
    /// agent so it stops overlapping its neighbours. Accumulated by the same
    /// neighbour gather that feeds separation/alignment/pressure (so it costs
    /// no extra spatial-hash work) and applied during the integrate pass, where
    /// it is two more adds over contiguous memory and does not disturb
    /// vectorization.
    ///
    /// Jacobi-style: every agent computes and applies only its OWN half of each
    /// overlap, reading neighbours through the pre-tick snapshot. Both agents in
    /// a pair independently arrive at the same, opposite correction, so the pair
    /// separates without either one writing to the other's slot -- which is what
    /// keeps this parallel-safe and scheduling-independent.
    std::vector<f32> contact_push_x_;
    std::vector<f32> contact_push_y_;
    /// Per-agent effective max speed for this tick (family base, kSlowed-scaled),
    /// written by the accumulate pass and consumed by the branch-free clamp+
    /// integrate pass so that pass never has to touch flags or the tuning table.
    std::vector<f32> scratch_max_speed_;
    /// Replication rolls are deferred here instead of spawning inline: spawn()
    /// mutates the shared count/arrays, so doing it from multiple job-system
    /// ranges concurrently would race. Resolved by one deterministic serial pass
    /// in index order after the parallel step joins.
    std::vector<u8> replicate_wanted_;

    void ensure_scratch(usize capacity);

    ChaffTuning tuning_{};
    Rect bounds_{};
    Vec2 goal_{0.0f, 0.0f};
    f32 goal_radius_ = 2.0f;
};

} // namespace immune::sim
