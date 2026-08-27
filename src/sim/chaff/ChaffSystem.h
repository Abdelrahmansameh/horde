// sim/chaff/ChaffSystem.h — batch chaff movement kernel. FROZEN CONTRACT.
// Owner: Wave 1B. `sdf` parameter added post-Wave-2 (orchestrator amendment,
// see below) to fix a real bug, not a style change. `squads` parameter added
// later on the same footing -- see WHY THE SQUAD PARAMETER EXISTS below.
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
// objective. Measured: ~95% of chaff spawned at a real spawn point never
// reached the goal before this fix (tests/scripts/spawn_point_reaches_goal.json).
// `DistanceField::gradient()` — "direction of increasing clearance" — is
// exactly the recovery vector needed: it points back into the tissue from
// anywhere outside it. This is a *safety net*, not a replacement for a
// smoother wall-cost gradient in the bake itself (DESIGN.md §12.3/§14 still
// flags that as the real, remaining fluid-feel tuning target) — it guarantees
// no agent can get permanently lost, independent of how well-tuned the bake
// is.
//
// WHY THE SQUAD PARAMETER EXISTS (readability, DESIGN.md §4.2/§9)
// Every agent samples the same level-wide flow field, so the whole horde
// converges on one shortest path and arrives as a single undifferentiated mass.
// sim/squad/Squads.h partitions it into squads that each follow their own
// authored path, which costs this kernel exactly two things:
//   - one direction blend in pass A, weighted ZERO while an agent is inside its
//     own squad radius, so packed interiors keep the pre-squad behaviour
//     unchanged and the fluid feel is not traded away for the grouping;
//   - one u16 compare per neighbour in gather_neighbours(), which widens and
//     strengthens separation against agents from a DIFFERENT squad. That
//     asymmetry is the entire mechanism by which squads stay visually distinct
//     instead of merging the moment they touch.
// Both collapse to the original kernel when squads are disabled or the level
// authored no paths, and an agent carrying kNoSquad is unaffected either way.
//
// PARALLELISM & DETERMINISM
// The update splits [0, count) across JobSystem ranges. Agents read the
// *previous* tick's positions for separation and write only their own slot, so
// there is no write contention and the result does not depend on scheduling.
// Randomness (jitter, replication rolls) comes from a per-range Rng forked from
// the sim Rng by range index — never from a shared generator. The fork stream
// also mixes in one draw taken from the sim Rng at the top of update(), because
// fork() is const and does not advance its parent: without that draw every tick
// re-derived the identical per-range streams, which turned jitter into a
// constant per-agent force and replication into a fixed set of always-breeding
// slots. One draw, before the split, so the result is still independent of
// thread count.
#pragma once

#include "core/Types.h"
#include "sim/squad/Squads.h"

#include <cmath>
#include <vector>

namespace immune { class JobSystem; class Rng; }

namespace immune::sim {

class ChaffBuffers;
class FlowField;
class DistanceField;
class TissueMask;
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
    ///
    /// Left at exactly 2.0 deliberately, and 2.2 was tried: this number is the
    /// GEOMETRIC constraint -- bodies may not interpenetrate -- and the gap a
    /// comfortable crowd stands at is `crowd_relief`'s job, expressed as a
    /// density with an equilibrium instead of as a distance. Raising it only
    /// changes behaviour where relief has already lost, i.e. in a jam, where it
    /// buys no gap (there is no room for one) and spends the difference
    /// pressing the crowd harder into the lane walls: it measurably raised the
    /// worst upstream shove in tests/test_squads.cpp by the same 0.5 units/s
    /// that full-strength contact does, for nothing visible in return.
    f32 contact_spacing = 2.0f;
    /// How much of each detected overlap is corrected per tick, 0..1.
    ///
    /// 1.0 is the exactly-resolving value, not an aggressive one: each agent
    /// takes HALF of each overlap and the neighbour independently takes the
    /// other half, so a lone pair at stiffness 1 lands exactly at spacing. The
    /// old 0.7 was under-relaxation -- it left 30% of every overlap standing
    /// each tick, which compounds through a jam that is being re-compressed by
    /// the flow field every tick and never actually converges.
    ///
    /// "A lone pair" is load-bearing in that sentence: an agent overlapping n
    /// neighbours at once takes the AVERAGE of the n corrections, not their
    /// sum, so 1.0 stays the exactly-resolving value at any density instead of
    /// becoming an n-fold over-relaxation that oscillates. That averaging is
    /// what makes this number mean the same thing in a jam as it does for two
    /// agents alone -- see NeighbourSample::contact_push in ChaffSystem.cpp.
    f32 contact_stiffness = 1.0f;

    /// Positional relief for an over-packed agent, as a multiple of `radius`
    /// per tick. 0 disables it.
    ///
    /// Contact answers "am I inside someone" and stops the moment bodies are
    /// merely touching. Nothing then makes a touching crowd USE the empty lane
    /// beside it: expansion past contact distance is left to the separation
    /// force, and force is rationed by max_speed, which the flow field has
    /// already spent driving everyone toward the goal. So the horde travels as
    /// a clot -- correctly un-overlapped, and much denser than the space it is
    /// standing in.
    ///
    /// This is the term that spends free space. Like contact it is a
    /// DISPLACEMENT, so it is not rationed by max_speed and a crowd can open up
    /// far faster than it can walk; unlike contact it acts out to
    /// alignment_radius, so it keeps pushing after bodies separate.
    ///
    /// It expands toward the density `pressure_threshold` calls comfortable and
    /// fades to nothing there, so it cannot boil a settled crowd apart -- and
    /// where the room to reach that density does not exist (a narrow lane, a
    /// jam against a wall) contact still governs and the crowd simply packs in.
    /// That is what makes a horde fill whatever it is given: the target is a
    /// DENSITY, and the geometry decides whether it is reachable.
    ///
    /// At 0.7 a virus relieves up to 0.54 units/tick -- about 32 units/second,
    /// roughly twice its own top speed, which is the point: a crowd should be
    /// able to open up faster than it can walk. It is still bounded by the same
    /// per-tick displacement cap that keeps the crowd from stepping over a lane
    /// wall, and that cap is what makes the returns above ~0.7 shallow (a
    /// 400-agent clump settles at mean radius 10.0 at 0.7, measured in
    /// tests/test_chaff_system.cpp).
    ///
    /// That ceiling is what an agent on the crowd's FACE gets. Deeper in, the
    /// figure scales down with how one-sided the neighbourhood is, so a fully
    /// enclosed agent -- one with no free space to move into -- relieves at
    /// essentially zero however packed it is. Without that, the term spent its
    /// whole budget every tick on a direction that was pure sampling noise, and
    /// the interior of a jam boiled. See the crowd-relief block in
    /// ChaffSystem.cpp.
    f32 crowd_relief = 0.7f;
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
    ///
    /// `squads` supplies the per-squad anchors pass A steers toward. A
    /// default-constructed (no paths, no squads) registry is safe to pass and
    /// reproduces the pre-squad kernel exactly, so tests that do not care about
    /// grouping do not need to build one.
    ///
    /// `mask` is the walkability authority, and is passed SEPARATELY from `sdf`
    /// because the two disagree by design. sim::block_rect() marks a new
    /// tower's footprint non-walkable in the mask and the flow field is re-baked
    /// to route around it, but the DistanceField is deliberately never re-baked:
    /// tower placement validation reads it for "is there clearance here", so
    /// folding towers into it would make every tower block its own neighbours.
    /// The consequence is that the SDF-based wall response cannot see towers at
    /// all, and a dense enough crowd pushes agents straight through one. The
    /// mask is what closes that. A default-constructed (empty) mask is safe to
    /// pass and simply skips the check.
    ChaffUpdateStats update(ChaffBuffers& buffers,
                            const FlowField& flow,
                            const DistanceField& sdf,
                            const TissueMask& mask,
                            const SpatialHash& hash,
                            const SquadRegistry& squads,
                            Rng& rng,
                            f32 dt,
                            JobSystem* jobs);

    /// Spawns `count` agents of `family` inside `spawn_point_radius` of
    /// `spawn_pos`, jittered by `rng`. Returns how many were actually created
    /// (fewer at capacity).
    ///
    /// `squad_id` is stamped onto every agent in the burst; kNoSquad (the
    /// default) produces ungrouped chaff that steers exactly as it did before
    /// the squad layer existed, which is what the gym and the headless CLI
    /// modes want unless they explicitly ask for a squad.
    u32 spawn_burst(ChaffBuffers& buffers, PathogenFamily family, Vec2 spawn_pos,
                    f32 spawn_point_radius, u32 count, Rng& rng,
                    u16 squad_id = kNoSquad) const;

    /// Despawn bounds. Agents leaving this rect are removed; the level loader
    /// sets it to the tissue bounds plus a margin.
    void set_world_bounds(const Rect& bounds) { bounds_ = bounds; }
    const Rect& world_bounds() const { return bounds_; }

    /// The objective's footprint: chaff inside it is consumed and scores damage
    /// against organ integrity. An oriented rectangle, matching
    /// game::ObjectivePoint exactly -- `half_extents` are along the objective's
    /// own axes and `rotation` is in RADIANS. A zero or negative extent on
    /// either axis disables goal despawn entirely.
    ///
    /// The trig is resolved here rather than per agent per tick: this is called
    /// once at load, the despawn test runs over every agent every tick.
    void set_goal(Vec2 goal, Vec2 half_extents, f32 rotation = 0.0f) {
        goal_ = goal;
        goal_half_ = half_extents;
        goal_rotation_ = rotation;
        goal_cos_ = std::cos(-rotation);
        goal_sin_ = std::sin(-rotation);
    }

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
    ///
    /// Being Jacobi is also why the value stored here is the MEAN of an agent's
    /// overlap corrections rather than their sum: every projection is computed
    /// against the same pre-tick snapshot, as if it were the only one, so
    /// applying n of them together overshoots n-fold and oscillates. The
    /// averaging is where that is done and why -- NeighbourSample::contact_push
    /// in ChaffSystem.cpp carries the argument and the measurements.
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
    /// Per-squad count of daughters spawned so far in the CURRENT tick, indexed
    /// by squad id. Sized to the registry (not to agent capacity) and rebuilt in
    /// the replication resolve, so it is not part of ensure_scratch's
    /// reserved-once set. Exists because SquadRegistry::member_count is a
    /// top-of-tick snapshot: without it every daughter in a tick tests the same
    /// stale count against max_squad_size and they all pass together.
    std::vector<u32> squad_growth_;

    void ensure_scratch(usize capacity);

    ChaffTuning tuning_{};
    Rect bounds_{};
    Vec2 goal_{0.0f, 0.0f};
    Vec2 goal_half_{2.0f, 2.0f};   ///< Rectangle half-extents; see set_goal().
    f32 goal_rotation_ = 0.0f;     ///< Radians, as handed to set_goal().
    /// cos/sin of -goal_rotation_, precomputed by set_goal().
    f32 goal_cos_ = 1.0f;
    f32 goal_sin_ = 0.0f;
};

} // namespace immune::sim
