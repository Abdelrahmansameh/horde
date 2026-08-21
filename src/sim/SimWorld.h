// sim/SimWorld.h — the whole deterministic simulation, in one object.
// FROZEN CONTRACT. Owner: Wave 0 (orchestration), Waves 1-2 (the subsystems).
//
// RATIONALE
// One object owns every piece of sim state and defines the *canonical tick
// order*. Everything a tick touches is reachable from here and nothing else is.
// That is what makes `--bench`, `--sim-test`, and `--screenshot` able to run the
// real game with no window, no input, and no wall clock: they construct a
// SimWorld, call tick() N times, and read the result.
//
// DETERMINISM CONTRACT
//   - Given the same SimDesc (level + seed + tuning), tick N is bit-identical
//     across runs, machines, thread counts, and build configs.
//   - Nothing inside tick() may read wall-clock time, call rand(), touch input,
//     or depend on iteration order of an unordered container.
//   - All randomness flows from `rng()`, seeded from SimDesc::seed.
//
// TICK ORDER (fixed; changing it is a contract change)
//   1. spatial hash rebuild        [prof: spatial_hash]
//   2. chaff update                [prof: chaff_update]
//   3. ECS systems                 [prof: ecs_tick]
//   4. damage fields apply
//   4b. projectiles, swarmers, fluid
//   5. chaff compact + despawn accounting
//   6. flow-field incremental rebake pump (budgeted)
//   7. tick counter advance
#pragma once

#include "core/Profiler.h"
#include "core/Rng.h"
#include "core/Types.h"
#include "sim/CombatEvents.h"
#include "sim/chaff/ChaffBuffers.h"
#include "sim/chaff/ChaffSystem.h"
#include "sim/damage/DamageField.h"
#include "sim/ecs/EcsWorld.h"
#include "sim/flowfield/FlowField.h"
#include "sim/fluid/Fluid.h"
#include "sim/projectile/Projectiles.h"
#include "sim/swarm/Swarmers.h"
#include "sim/spatial/SpatialHash.h"

#include <string>
#include <vector>

namespace immune { class JobSystem; }

namespace immune::sim {

struct SimDesc {
    u64 seed = 0x1234'5678'9abc'def0ULL;
    usize max_chaff = 16384;
    usize max_damage_fields = 512;
    /// Live Gunner rounds. Generous: a maxed Gunner is meant to read as a
    /// continuous stream, and a dropped round is invisible but a reallocation
    /// mid-tick is forbidden outright.
    usize max_projectiles = 8192;
    /// Live Cytotoxic T swarmers. Spawn rate against lifetime sets the standing
    /// cloud size (see sim/swarm/Swarmers.h); this only has to be above the
    /// equilibrium a full board of maxed T-cells reaches.
    usize max_swarmers = 24576;
    /// Live Goblet Cell fluid particles (sim/fluid/Fluid.h). Emission rate is
    /// derived from nozzle geometry rather than authored, so the standing
    /// population is a firm number -- roughly 380 per firing tower at tier 3.
    /// This clears a board densely packed with them, with headroom for the
    /// puddles they leave behind.
    usize max_fluid_particles = 8192;
    /// Solver knobs for that fluid. Lives on SimDesc, not on the tower table,
    /// because there is exactly one solver per world and every jet shares it.
    FluidTuning fluid_tuning{};
    /// Per-tick combat-event capacity. Sized well above the expected rate so
    /// the VFX layer never starves during a heavy volley; overflow is counted,
    /// not silent.
    usize max_combat_events = 8192;
    Rect world_bounds{Vec2{0.0f, 0.0f}, Vec2{256.0f, 144.0f}};
    f32 spatial_cell_size = 4.0f;
    /// Milliseconds per frame the flow field may spend on incremental rebakes.
    f64 flow_rebake_budget_ms = 0.5;
    ChaffTuning chaff_tuning{};
};

/// One vessel spawn point, captured from the level at load time by
/// LevelLoader::instantiate(). Read-only after that: WaveDirector::tick()
/// resolves each wave's SpawnEntry::portal_id against this list to know where
/// to place new agents, since SimWorld -- not LevelDef, which doesn't survive
/// past load -- is the only thing a running tick can reach.
struct SpawnPortalRuntime {
    std::string id;
    Vec2 position{0.0f, 0.0f};
    f32 radius = 3.0f;
};

/// Aggregate counters exposed to --sim-test invariants and the HUD.
struct SimSnapshot {
    Tick tick = 0;
    u64 chaff_count = 0;
    u64 named_count = 0;
    f32 total_density = 0.0f;
    f32 objective_integrity = 100.0f;
    u64 chaff_killed_total = 0;
    u64 chaff_leaked_total = 0;   ///< Reached the objective.
    u32 chaff_by_family[kFamilyCount] = {};

    // Per-family lifetime tallies. Additive to this struct (Wave "balance
    // harness"): the three aggregate counters above cannot answer "which
    // pathogen is the one getting through", which is the first question any
    // balance pass asks. `killed` here means killed by damage specifically --
    // leaks and out-of-bounds despawns are broken out rather than folded in,
    // unlike chaff_killed_total, which has always counted all three together.
    u64 chaff_spawned_by_family[kFamilyCount] = {};
    u64 chaff_killed_by_family[kFamilyCount] = {};
    u64 chaff_leaked_by_family[kFamilyCount] = {};
    u64 chaff_despawned_by_family[kFamilyCount] = {};   ///< Left the world bounds.
};

class SimWorld {
public:
    SimWorld() = default;

    /// Allocates all buffers. Safe to call again to reset for a new level.
    void init(const SimDesc& desc, JobSystem* jobs);

    /// Advances exactly one 60 Hz step. `profiler` may be null.
    void tick(Profiler* profiler = nullptr);

    /// Runs `n` ticks. Convenience for headless modes.
    void run_ticks(u64 n, Profiler* profiler = nullptr);

    SimSnapshot snapshot() const;

    // ---- Subsystem access -------------------------------------------------
    ChaffBuffers& chaff() { return chaff_; }
    const ChaffBuffers& chaff() const { return chaff_; }
    ChaffSystem& chaff_system() { return chaff_system_; }
    SpatialHash& spatial() { return spatial_; }
    const SpatialHash& spatial() const { return spatial_; }
    FlowField& flow() { return flow_; }
    const FlowField& flow() const { return flow_; }
    TissueMask& tissue() { return tissue_; }
    const TissueMask& tissue() const { return tissue_; }
    /// Clearance field baked alongside the tissue mask. Tower placement
    /// validation, vessel-hugging steering, and the tissue render pass read it.
    DistanceField& sdf() { return sdf_; }
    const DistanceField& sdf() const { return sdf_; }
    DamageSystem& damage() { return damage_; }
    EcsWorld& ecs() { return ecs_; }
    const EcsWorld& ecs() const { return ecs_; }

    /// The Gunner's live rounds. Towers push straight into this store; the
    /// system has no spawn entry point of its own.
    ProjectileBuffers& projectiles() { return projectiles_; }
    const ProjectileBuffers& projectiles() const { return projectiles_; }
    ProjectileSystem& projectile_system() { return projectile_system_; }

    /// The Cytotoxic T's live swarmers. Towers push straight into this store,
    /// same arrangement as projectiles.
    SwarmerBuffers& swarmers() { return swarmers_; }
    const SwarmerBuffers& swarmers() const { return swarmers_; }
    SwarmerSystem& swarmer_system() { return swarmer_system_; }

    /// The Goblet Cell's live fluid. Unlike projectiles and swarmers, towers do
    /// NOT push into this store directly -- they go through
    /// FluidSystem::emit(), because the emission COUNT is derived from nozzle
    /// geometry and rest spacing, both of which only the system knows. A tower
    /// that spawned particles itself would inevitably over-fill the nozzle and
    /// detonate its own jet; see the emit() comment for why.
    FluidBuffers& fluid() { return fluid_; }
    const FluidBuffers& fluid() const { return fluid_; }
    FluidSystem& fluid_system() { return fluid_system_; }
    const FluidSystem& fluid_system() const { return fluid_system_; }

    /// Instantaneous combat happenings raised during the tick, drained once per
    /// frame by the VFX layer. This is an OUTPUT of the tick: nothing in the
    /// sim ever reads it back, so a full sink cannot perturb state_hash().
    CombatEventSink& combat_events() { return combat_events_; }
    const CombatEventSink& combat_events() const { return combat_events_; }

    Rng& rng() { return rng_; }
    Tick tick_index() const { return tick_; }
    const SimDesc& desc() const { return desc_; }

    /// Spawn portals for the current level. Set once by
    /// LevelLoader::instantiate(); empty for the CLI headless modes that never
    /// load a real level.
    const std::vector<SpawnPortalRuntime>& portals() const { return portals_; }
    void set_portals(std::vector<SpawnPortalRuntime> portals) { portals_ = std::move(portals); }

    /// Overwrites the objective's remaining integrity, clamped at zero.
    ///
    /// Nothing in the sim ever *raises* this -- tick() only subtracts the chaff
    /// that reached the goal -- so this is not a gameplay path and no system
    /// inside sim/ calls it. It exists for the gym's "objective invulnerable"
    /// toggle (game/gym), which re-applies it after each tick so a leak still
    /// counts as a leak while the run cannot end. Safe for determinism:
    /// objective_integrity_ is not part of state_hash(), and nothing in the
    /// tick reads it back.
    void set_objective_integrity(f32 value) {
        objective_integrity_ = value < 0.0f ? 0.0f : value;
    }

    /// DamageStats from the most recently completed tick. Economy reads
    /// density_removed from this to credit kill income -- aggregate damage
    /// means only sim/damage knows a kill happened, so nothing else may infer
    /// it by diffing agent counts (DamageField.h's own rationale).
    const DamageStats& last_damage_stats() const { return last_damage_stats_; }

    /// Hash of the sim state, for --sim-test determinism assertions:
    /// two runs of the same SimDesc must produce the same value at every tick.
    u64 state_hash() const;

private:
    SimDesc desc_{};
    JobSystem* jobs_ = nullptr;
    Rng rng_{};
    Tick tick_ = 0;

    TissueMask tissue_;
    DistanceField sdf_;
    FlowField flow_;
    SpatialHash spatial_;
    ChaffBuffers chaff_;
    ChaffSystem chaff_system_;
    DamageSystem damage_;
    ProjectileBuffers projectiles_;
    ProjectileSystem projectile_system_;
    SwarmerBuffers swarmers_;
    SwarmerSystem swarmer_system_;
    FluidBuffers fluid_;
    FluidSystem fluid_system_;
    CombatEventSink combat_events_;
    EcsWorld ecs_;

    std::vector<SpawnPortalRuntime> portals_;
    DamageStats last_damage_stats_{};

    u64 killed_total_ = 0;
    u64 leaked_total_ = 0;
    u64 killed_by_family_[kFamilyCount] = {};
    u64 leaked_by_family_[kFamilyCount] = {};
    u64 despawned_by_family_[kFamilyCount] = {};
    f32 objective_integrity_ = 100.0f;
};

} // namespace immune::sim
