#include "sim/SimWorld.h"

#include "core/JobSystem.h"
#include "core/Math.h"

namespace immune::sim {

void SimWorld::init(const SimDesc& desc, JobSystem* jobs) {
    desc_ = desc;
    jobs_ = jobs;
    rng_.reseed(desc.seed);
    tick_ = 0;
    killed_total_ = 0;
    leaked_total_ = 0;
    objective_integrity_ = 100.0f;
    portals_.clear();
    last_damage_stats_ = DamageStats{};

    chaff_.reserve(desc.max_chaff);
    chaff_system_.set_tuning(desc.chaff_tuning);
    chaff_system_.set_world_bounds(desc.world_bounds);

    SpatialHashDesc shd;
    shd.bounds = desc.world_bounds;
    shd.cell_size = desc.spatial_cell_size;
    spatial_.configure(shd);

    damage_.reserve(desc.max_damage_fields);
    damage_.clear_all();

    ecs_.clear_entities();
}

void SimWorld::tick(Profiler* profiler) {
    // 1. Spatial hash rebuild.
    {
        WallClock t;
        spatial_.rebuild(chaff_.pos_x.data(), chaff_.pos_y.data(), chaff_.count(), jobs_);
        if (profiler) profiler->record(prof_key::kSpatialHash, t.elapsed_ms());
    }

    // 2. Chaff movement.
    {
        WallClock t;
        const ChaffUpdateStats chaff_stats =
            chaff_system_.update(chaff_, flow_, spatial_, rng_, kFixedDt, jobs_);
        // Each pathogen that reaches the objective chips its integrity. 1% of
        // max per leaked agent is a placeholder pending the economy pass
        // (Wave 3A) — it empties a 100-agent breach in ~1 simulated second,
        // which is enough for --sim-test to assert integrity actually moves.
        leaked_total_ += chaff_stats.despawned_at_goal;
        objective_integrity_ = math::max(
            0.0f, objective_integrity_ - static_cast<f32>(chaff_stats.despawned_at_goal));
        if (profiler) profiler->record(prof_key::kChaffUpdate, t.elapsed_ms());
    }

    // 3. ECS systems (named agents, towers).
    {
        WallClock t;
        SystemContext ctx{*this, ecs_.registry(), rng_, kFixedDt, tick_};
        ecs_.tick(ctx);
        if (profiler) profiler->record(prof_key::kEcsTick, t.elapsed_ms());
    }

    // 4. Aggregate damage.
    last_damage_stats_ = damage_.apply(chaff_, spatial_, rng_, kFixedDt);

    // 5. Compaction / kill accounting.
    killed_total_ += chaff_.compact();
    damage_.clear_transient(kFixedDt);

    // 6. Budgeted incremental flow-field rebake.
    if (flow_.has_pending_rebake()) {
        flow_.pump_rebake(tissue_, desc_.flow_rebake_budget_ms);
    }

    ++tick_;
}

void SimWorld::run_ticks(u64 n, Profiler* profiler) {
    for (u64 i = 0; i < n; ++i) tick(profiler);
}

SimSnapshot SimWorld::snapshot() const {
    SimSnapshot s;
    s.tick = tick_;
    s.chaff_count = chaff_.count();
    s.named_count = ecs_.named_agent_count();
    s.total_density = chaff_.total_density();
    s.objective_integrity = objective_integrity_;
    s.chaff_killed_total = killed_total_;
    s.chaff_leaked_total = leaked_total_;
    for (u32 f = 0; f < kFamilyCount; ++f) {
        s.chaff_by_family[f] = chaff_.family_count(static_cast<PathogenFamily>(f));
    }
    return s;
}

u64 SimWorld::state_hash() const {
    // FNV-1a over the sim-visible chaff streams plus the tick counter.
    u64 h = 1469598103934665603ULL;
    auto mix = [&h](const void* data, usize bytes) {
        const u8* p = static_cast<const u8*>(data);
        for (usize i = 0; i < bytes; ++i) {
            h ^= p[i];
            h *= 1099511628211ULL;
        }
    };
    mix(&tick_, sizeof(tick_));
    const usize n = chaff_.count();
    if (n > 0) {
        mix(chaff_.pos_x.data(), n * sizeof(f32));
        mix(chaff_.pos_y.data(), n * sizeof(f32));
        mix(chaff_.vel_x.data(), n * sizeof(f32));
        mix(chaff_.vel_y.data(), n * sizeof(f32));
        mix(chaff_.density.data(), n * sizeof(f32));
        mix(chaff_.family.data(), n * sizeof(u8));
        mix(chaff_.flags.data(), n * sizeof(u8));
    }
    mix(&killed_total_, sizeof(killed_total_));
    mix(&leaked_total_, sizeof(leaked_total_));
    return h;
}

} // namespace immune::sim
