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
//   1b. squad centroids + anchors  [prof: squad_update]
//   2. chaff update                [prof: chaff_update]
//   3. ECS systems                 [prof: ecs_tick]
//   4. damage fields apply
//   4b. projectiles, swarmers, fluid
//   4f. chaff death events (must be after every damage source and before
//       compaction — the only window where a dead agent still has a position)
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
#include "sim/squad/Squads.h"

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
    /// Cap on CombatEventType::ChaffDeath events raised in ONE tick.
    ///
    /// Chaff deaths are the only combat event whose rate is set by how badly
    /// the player is winning rather than by how many towers are firing: a wave
    /// breaking against a finished defence retires hundreds of agents on a
    /// single tick. Uncapped, one such tick fills the whole shared sink and
    /// every muzzle flash and impact for that frame is dropped instead —
    /// the screen goes quiet exactly when it should be loudest. Capping trades
    /// some of the pops (which are individually indistinguishable in a crowd
    /// that size) for keeping everything else.
    usize max_chaff_death_events = 512;
    /// The PLAY area: what the camera clamps to and what the level is framed
    /// by. Nothing in the tick culls against it -- see sim_bounds.
    Rect world_bounds{Vec2{0.0f, 0.0f}, Vec2{256.0f, 144.0f}};
    /// The rect the simulation actually covers: the spatial hash, the fluid
    /// grid, and every out-of-bounds retirement test (chaff, projectiles,
    /// swarmers) use THIS. Levels are allowed to put a spawn point outside the
    /// play area so the horde walks in from off-screen, and everything that
    /// touches those agents has to reach that far (game::level_sim_bounds()).
    ///
    /// Empty means "same as world_bounds", which is what a caller that has no
    /// level -- a test, the gym, headless -- wants; init() resolves it once so
    /// the tick never has to ask which of the two it meant.
    Rect sim_bounds{};
    f32 spatial_cell_size = 4.0f;
    /// Milliseconds per frame the flow field may spend on incremental rebakes.
    f64 flow_rebake_budget_ms = 0.5;
    /// World-space radius of the flow field's direction-smoothing pass, applied
    /// when LevelLoader::instantiate() bakes the field. See
    /// sim::FlowFieldBakeDesc::smoothing_radius.
    f32 flow_smoothing_radius = 2.0f;
    /// How much more a cell costs to cross when it is hard against a vessel
    /// wall, versus one clear of it. DEFAULT OFF -- see the measurement below.
    ///
    /// WHAT IT IS FOR. A shortest-path field takes corners infinitely tight: at
    /// the free end of a septum every route to the goal touches that one tip,
    /// so the field is a fan converging on a point and the horde beelines to it
    /// rather than sweeping round the bend. Charging for wall proximity bows
    /// the path off the corner, because scraping it is expensive.
    ///
    /// WHY IT SHIPS OFF ANYWAY. Clearance cannot tell the tip of a septum from
    /// the side of a straight lane -- both are just "near a wall" -- so the same
    /// term that opens a hairpin also tilts every lane's field toward its own
    /// centreline. Measured on capillary_switchback and capillary_2: the
    /// best-case setting bought ~8 world units of standoff at the hairpin, and
    /// cost capillary_2 its straight-lane flatness (mean cross-lane component
    /// 0.22 -> 0.36, i.e. most of the way back to the pre-eikonal field) while
    /// making the field measurably less smooth (mean neighbour disagreement
    /// 0.32 -> 1.17 degrees). That is a bad trade, so it is opt-in: turn it up
    /// on a level whose hairpins matter more than its straights.
    f32 flow_wall_cost = 0.0f;
    /// Clearance, in world units, at which flow_wall_cost has fully decayed.
    /// Roughly the radius of the arc a turn will take, so it wants to be on the
    /// order of half a lane width, not a couple of cells.
    f32 flow_wall_falloff = 34.0f;
    /// Shape of that decay. This is the knob that separates "take corners wide"
    /// from "funnel every lane onto its own centreline", and it matters more
    /// than either value above.
    ///
    /// The penalty is gain * (1 - clearance/falloff)^exponent. At exponent 2 the
    /// cost gradient is still substantial halfway across a lumen, so a straight
    /// lane develops a herringbone pointing at its axis -- agents near either
    /// wall cut steeply inward and the overlay shows a chevron. Raising the
    /// exponent concentrates the whole penalty into a thin boundary layer at
    /// the lining, which is enough to stop a path grazing a septum tip while
    /// leaving the open middle of a vessel almost perfectly flat.
    f32 flow_wall_exponent = 12.0f;

    ChaffTuning chaff_tuning{};
    /// Squad grouping (sim/squad/Squads.h). Defaults are live: a world built
    /// with a plain SimDesc gets squads as soon as a level installs paths, and
    /// gets the pre-squad kernel until then.
    SquadTuning squad_tuning{};
};

/// One vessel spawn point, captured from the level at load time by
/// LevelLoader::instantiate(). Read-only after that: WaveDirector::tick()
/// resolves each wave's SpawnEntry::spawn_point_id against this list to know where
/// to place new agents, since SimWorld -- not LevelDef, which doesn't survive
/// past load -- is the only thing a running tick can reach.
struct SpawnPointRuntime {
    std::string id;
    Vec2 position{0.0f, 0.0f};
    f32 radius = 3.0f;
    /// Effective lane this spawn point feeds, already resolved through
    /// LevelLoader::resolve_spawn_point_lane_id() -- never the raw authored
    /// hint, so a tick never has to guess. WaveDirector uses it to pick which
    /// of the level's squad paths a new squad from this spawn point is
    /// assigned.
    std::string lane_id;
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
    /// Live squads (sim/squad/Squads.h). Exposed so --sim-test can assert that
    /// a real level's waves actually group -- an aggregate the HUD and the
    /// balance report can read without walking the registry.
    u32 active_squads = 0;
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
    /// The level's squad paths and live squads. LevelLoader::instantiate()
    /// installs the paths; WaveDirector opens squads against it at spawn time.
    SquadRegistry& squads() { return squads_; }
    const SquadRegistry& squads() const { return squads_; }

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

    /// Spawn points for the current level. Set once by
    /// LevelLoader::instantiate(); empty for the CLI headless modes that never
    /// load a real level.
    const std::vector<SpawnPointRuntime>& spawn_points() const { return spawn_points_; }
    void set_spawn_points(std::vector<SpawnPointRuntime> spawn_points) { spawn_points_ = std::move(spawn_points); }

    /// Buildable rectangles from the level (game/level/Level.h's
    /// `placement_zones`). EMPTY MEANS "ANYWHERE", which is what every level
    /// that authors none has always meant.
    ///
    /// Threaded here for the same reason spawn points are: LevelDef does not
    /// survive instantiate(), and TowerSystem::validate() is the only thing
    /// that needs to answer "may a tower go here". Before this, the field was
    /// authorable, validated, drawn by the editor, read by the balance bot --
    /// and enforced by nothing, because PlacementResult::OutsidePlacementZone
    /// had no data source to be returned from.
    const std::vector<Rect>& placement_zones() const { return placement_zones_; }
    void set_placement_zones(std::vector<Rect> zones) { placement_zones_ = std::move(zones); }

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

    /// Kill accounting for the most recently completed tick. Economy reads
    /// density_removed from this to credit kill income -- aggregate damage
    /// means only sim knows a kill happened, so nothing else may infer it by
    /// diffing agent counts (DamageField.h's own rationale).
    ///
    /// `density_removed` covers EVERY chaff damage source in the tick: fields,
    /// projectiles, swarmers and fluid (see tick() step 4e). The remaining
    /// members are the DamageSystem's own and describe fields only -- including
    /// density_removed_by_family, because the other three sources do not track
    /// which family they thinned.
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
    SquadRegistry squads_;
    DamageSystem damage_;
    ProjectileBuffers projectiles_;
    ProjectileSystem projectile_system_;
    SwarmerBuffers swarmers_;
    SwarmerSystem swarmer_system_;
    FluidBuffers fluid_;
    FluidSystem fluid_system_;
    CombatEventSink combat_events_;
    EcsWorld ecs_;

    std::vector<SpawnPointRuntime> spawn_points_;
    std::vector<Rect> placement_zones_;
    DamageStats last_damage_stats_{};

    u64 killed_total_ = 0;
    u64 leaked_total_ = 0;
    u64 killed_by_family_[kFamilyCount] = {};
    u64 leaked_by_family_[kFamilyCount] = {};
    u64 despawned_by_family_[kFamilyCount] = {};
    f32 objective_integrity_ = 100.0f;
};

} // namespace immune::sim
