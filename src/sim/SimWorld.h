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
//   5. chaff compact + despawn accounting
//   6. flow-field incremental rebake pump (budgeted)
//   7. tick counter advance
#pragma once

#include "core/Profiler.h"
#include "core/Rng.h"
#include "core/Types.h"
#include "sim/chaff/ChaffBuffers.h"
#include "sim/chaff/ChaffSystem.h"
#include "sim/damage/DamageField.h"
#include "sim/ecs/EcsWorld.h"
#include "sim/flowfield/FlowField.h"
#include "sim/spatial/SpatialHash.h"

#include <string>

namespace immune { class JobSystem; }

namespace immune::sim {

struct SimDesc {
    u64 seed = 0x1234'5678'9abc'def0ULL;
    usize max_chaff = 16384;
    usize max_damage_fields = 512;
    Rect world_bounds{Vec2{0.0f, 0.0f}, Vec2{256.0f, 144.0f}};
    f32 spatial_cell_size = 4.0f;
    /// Milliseconds per frame the flow field may spend on incremental rebakes.
    f64 flow_rebake_budget_ms = 0.5;
    ChaffTuning chaff_tuning{};
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

    Rng& rng() { return rng_; }
    Tick tick_index() const { return tick_; }
    const SimDesc& desc() const { return desc_; }

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
    EcsWorld ecs_;

    u64 killed_total_ = 0;
    u64 leaked_total_ = 0;
    f32 objective_integrity_ = 100.0f;
};

} // namespace immune::sim
