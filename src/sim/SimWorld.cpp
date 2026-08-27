#include "sim/SimWorld.h"

#include "core/JobSystem.h"
#include "core/Math.h"

namespace immune::sim {
namespace {

/// Raises one ChaffDeath event per agent this tick's damage actually killed.
///
/// WHY DENSITY IS THE DISCRIMINATOR. Compaction is the only pass that sees
/// every retirement, and by then it cannot tell them apart — a leaked agent and
/// a killed one both arrive carrying nothing but kPendingKill. But only ONE of
/// the two ways that bit gets set also zeroes density:
/// ChaffBuffers::apply_density_loss(). Pass C's goal/out-of-bounds despawns
/// call kill() directly and leave density standing, and every damage source in
/// the sim skips an agent that is already flagged (see the kPendingKill early-
/// outs in DamageField.cpp, Projectiles.cpp, Swarmers.cpp and Fluid.cpp), so a
/// leaked agent can never have its density zeroed afterwards either. That makes
/// `pending kill AND density <= 0` exactly "the player killed this" — which is
/// the distinction that matters here, because an agent that walked into the
/// organ should not be rewarded with a death pop.
///
/// PURE OUTPUT. Reads sim state, writes only the sink. Detaching the sink or
/// dropping every event cannot move state_hash() by a bit, which is the
/// standing contract for this channel (sim/CombatEvents.h).
void raise_chaff_deaths(const ChaffBuffers& chaff, const ChaffTuning& tuning, usize budget,
                        CombatEventSink& events) {
    usize raised = 0;
    const usize n = chaff.count();
    for (usize i = 0; i < n && raised < budget; ++i) {
        if ((chaff.flags[i] & chaff_flags::kPendingKill) == 0) continue;
        if (chaff.density[i] > 0.0f) continue;

        const u32 f = chaff.family[i];
        const Vec2 velocity{chaff.vel_x[i], chaff.vel_y[i]};
        const f32 speed = math::length(velocity);

        CombatEvent e;
        e.type = CombatEventType::ChaffDeath;
        e.origin = Vec2{chaff.pos_x[i], chaff.pos_y[i]};
        e.direction = speed > math::kEpsilon ? velocity / speed : Vec2{1.0f, 0.0f};
        e.magnitude = speed;
        // The agent's own body radius, so the burst is sized by what died
        // rather than by a constant the art has to keep matched to silhouettes.
        e.radius = f < kFamilyCount ? tuning.family[f].radius : 0.5f;
        e.target_family =
            f < kFamilyCount ? static_cast<PathogenFamily>(f) : PathogenFamily::Count;
        e.source = TowerType::Count;   // see CombatEventType::ChaffDeath
        events.push(e);
        ++raised;
    }
    // Over budget, this keeps the first `budget` deaths in slot order and drops
    // the rest. Slot order is not spatial — compact()'s swap-remove shuffles it
    // continuously — so the survivors are scattered across the kill zone rather
    // than clustered at one end of it, which is the only property that matters
    // when the alternative is showing none of them.
}

} // namespace

void SimWorld::init(const SimDesc& desc, JobSystem* jobs) {
    desc_ = desc;
    jobs_ = jobs;
    rng_.reseed(desc.seed);
    tick_ = 0;
    killed_total_ = 0;
    leaked_total_ = 0;
    for (u32 f = 0; f < kFamilyCount; ++f) {
        killed_by_family_[f] = 0;
        leaked_by_family_[f] = 0;
        despawned_by_family_[f] = 0;
    }
    objective_integrity_ = 100.0f;
    spawn_points_.clear();
    // Overwritten wholesale by LevelLoader::instantiate(), but cleared here too
    // so a load that fails partway cannot leave the previous level's buildable
    // area standing over the new one's geometry.
    placement_zones_.clear();
    last_damage_stats_ = DamageStats{};

    chaff_.reserve(desc.max_chaff);
    chaff_system_.set_tuning(desc.chaff_tuning);
    chaff_system_.set_world_bounds(desc.world_bounds);

    // Squad tuning is bounded HERE because this is the only place that sees
    // both the separation radii and the spatial cell size. gather_neighbours()
    // scans 3x3 cells, so a foreign separation radius past one cell width does
    // not error -- it silently stops seeing the neighbours it is supposed to
    // repel, which would read as squads intermittently failing to hold apart.
    {
        SquadTuning st = desc.squad_tuning;
        f32 widest_sep = 0.0f;
        for (u32 f = 0; f < kFamilyCount; ++f)
            widest_sep = math::max(widest_sep, desc.chaff_tuning.family[f].separation_radius);
        if (widest_sep > math::kEpsilon && st.foreign_radius_mult * widest_sep >
                                               desc.spatial_cell_size) {
            st.foreign_radius_mult = desc.spatial_cell_size / widest_sep;
        }
        squads_.set_tuning(st);
        squads_.clear();
    }

    SpatialHashDesc shd;
    shd.bounds = desc.world_bounds;
    shd.cell_size = desc.spatial_cell_size;
    spatial_.configure(shd);

    damage_.reserve(desc.max_damage_fields);
    projectiles_.reserve(desc.max_projectiles);
    swarmers_.reserve(desc.max_swarmers);
    fluid_.reserve(desc.max_fluid_particles);
    fluid_.clear();
    fluid_system_.configure(desc.world_bounds, desc.fluid_tuning);
    combat_events_.reserve(desc.max_combat_events);
    damage_.clear_all();

    // Systems and context variables go too, not just entities: init() is "this
    // world is now a different level", and a re-used world that kept its
    // systems would run a second copy of every one its caller re-registers.
    // Callers register their per-level systems after init(), which is the order
    // every call site already uses.
    ecs_.reset();
}

void SimWorld::tick(Profiler* profiler) {
    // Retirements this tick that were NOT the player's doing. Filled by the
    // chaff pass below and consumed at compaction, where the only thing known
    // about a retiring agent is that it is retiring.
    u64 not_killed_this_tick[kFamilyCount] = {};

    // 1. Spatial hash rebuild.
    {
        WallClock t;
        spatial_.rebuild(chaff_.pos_x.data(), chaff_.pos_y.data(), chaff_.count(), jobs_);
        if (profiler) profiler->record(prof_key::kSpatialHash, t.elapsed_ms());
    }

    // 1b. Squad centroids and anchors. Must run BEFORE chaff movement: the
    // centroids are accumulated from the same positions pass A reads as
    // old_pos_*, so anchor and agent agree on where the squad is this tick.
    {
        WallClock t;
        squads_.update(chaff_, kFixedDt);
        if (profiler) profiler->record(prof_key::kSquadUpdate, t.elapsed_ms());
    }

    // 2. Chaff movement.
    {
        WallClock t;
        const ChaffUpdateStats chaff_stats =
            chaff_system_.update(chaff_, flow_, sdf_, tissue_, spatial_, squads_, rng_,
                                 kFixedDt, jobs_);
        // Each pathogen that reaches the objective chips its integrity. 1% of
        // max per leaked agent is a placeholder pending the economy pass
        // (Wave 3A) — it empties a 100-agent breach in ~1 simulated second,
        // which is enough for --sim-test to assert integrity actually moves.
        leaked_total_ += chaff_stats.despawned_at_goal;
        for (u32 f = 0; f < kFamilyCount; ++f) {
            leaked_by_family_[f] += chaff_stats.despawned_at_goal_by_family[f];
            despawned_by_family_[f] += chaff_stats.despawned_out_of_bounds_by_family[f];
            not_killed_this_tick[f] = chaff_stats.despawned_at_goal_by_family[f] +
                                      chaff_stats.despawned_out_of_bounds_by_family[f];
        }
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

    // 4b. Projectiles. Must run after the ECS tick (towers fire during Combat
    // phase, so this tick's new rounds exist by now) and BEFORE the single
    // chaff compaction below -- both damage paths write density, and
    // ChaffBuffers::compact() runs exactly once per tick, after every source
    // has applied. Running it here also means a round and a field that kill the
    // same agent on the same tick both get their damage counted.
    const ProjectileStats projectile_stats =
        projectile_system_.update(projectiles_, chaff_, spatial_, desc_.world_bounds,
                                  rng_, kFixedDt, &combat_events_);

    // 4c. Swarmers. Same placement rule and the same reason as projectiles
    // above: after the ECS tick so this tick's newly released granules exist,
    // and before the single chaff compaction so a swarmer's drain and a field's
    // damage on the same agent on the same tick both get counted.
    const SwarmerStats swarmer_stats =
        swarmer_system_.update(swarmers_, chaff_, spatial_, desc_.world_bounds,
                               rng_, kFixedDt, &combat_events_);

    // 4d. Fluid. Same placement rule and the same reason again -- after the ECS
    // tick so this tick's freshly emitted jet exists, and before the single
    // chaff compaction so the mucus film's damage lands in the same accounting
    // pass as everything else. It runs LAST of the three on purpose: the solver
    // reads the chaff spatial hash for crowd braking, and running it after the
    // other two means a round or a granule that already killed an agent this
    // tick has not yet moved that agent's slot out from under the grid.
    const FluidStats fluid_stats =
        fluid_system_.update(fluid_, chaff_, spatial_, sdf_, desc_.world_bounds,
                             kFixedDt, &combat_events_);

    // 4e. Kill accounting for the tick. DamageField.h's ACCOUNTING rule is
    // "removed density is attributed to the economy", and the economy reads it
    // from last_damage_stats_ -- but damage_.apply() above only knows about
    // FIELDS. The other three sources above thin real chaff too, and a Gunner
    // publishes no field at all (TowerSystem.cpp), so leaving their stats on
    // the floor means the only projectile tower in the roster earns nothing
    // for its kills. Fold them in here, where every source for the tick has
    // run and none has been compacted away yet.
    //
    // Aggregate density only: the per-family split stays field-exclusive
    // because rounds, granules and fluid do not track which family they thinned,
    // and inventing a split would make the HUD's colour-coded feed lie. Nothing
    // reads density_removed_by_family off this snapshot today.
    last_damage_stats_.density_removed +=
        projectile_stats.density_removed + swarmer_stats.density_removed +
        fluid_stats.density_removed;

    // 4f. Death VFX events. This is the LAST thing before compaction and it has
    // to be: every damage source for the tick has now decided who is dying, and
    // the next statement swap-removes them, taking the positions the burst has
    // to be drawn at with it. Purely cosmetic and purely an output — see
    // raise_chaff_deaths() above.
    raise_chaff_deaths(chaff_, chaff_system_.tuning(), desc_.max_chaff_death_events,
                       combat_events_);

    // 5. Compaction / kill accounting.
    // Compaction sees every retirement; the leak/out-of-bounds tallies above
    // are the part of it that was not the player's doing, so subtracting them
    // leaves exactly "killed by damage" per family. chaff_killed_total keeps
    // its historical meaning (all retirements) so no existing assertion moves.
    {
        u32 retired_by_family[kFamilyCount] = {};
        killed_total_ += chaff_.compact(retired_by_family);
        for (u32 f = 0; f < kFamilyCount; ++f) {
            const u64 not_ours = not_killed_this_tick[f];
            const u64 retired = retired_by_family[f];
            killed_by_family_[f] += retired > not_ours ? retired - not_ours : 0;
        }
    }
    damage_.clear_transient(kFixedDt);

    // Every damage source for this tick has now run, so the per-owner sink can
    // close the tick out. Null in normal play (sim/Attribution.h).
    if (DamageAttribution* attribution = damage_.attribution()) attribution->mark_tick();

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
    s.active_squads = squads_.active_count();
    s.chaff_leaked_total = leaked_total_;
    for (u32 f = 0; f < kFamilyCount; ++f) {
        s.chaff_by_family[f] = chaff_.family_count(static_cast<PathogenFamily>(f));
        s.chaff_spawned_by_family[f] = chaff_.spawned_by_family()[f];
        s.chaff_killed_by_family[f] = killed_by_family_[f];
        s.chaff_leaked_by_family[f] = leaked_by_family_[f];
        s.chaff_despawned_by_family[f] = despawned_by_family_[f];
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
    // The fluid is gameplay state -- it damages, it slows, and where it lands
    // decides both -- so it belongs in the hash. Positions only: velocity and
    // density are recomputed from them every substep, so hashing those too
    // would cost three streams to detect nothing extra.
    const usize fn = fluid_.count();
    if (fn > 0) {
        mix(fluid_.pos_x.data(), fn * sizeof(f32));
        mix(fluid_.pos_y.data(), fn * sizeof(f32));
    }
    // Squad anchors steer agents, so a determinism assertion that ignored them
    // would not be checking the squad layer at all. state_hash() is folded in
    // rather than mixing the squad array directly, so slot churn (retire/reuse)
    // cannot alias into a false mismatch.
    const u64 squad_h = squads_.state_hash();
    mix(&squad_h, sizeof(squad_h));
    mix(&killed_total_, sizeof(killed_total_));
    mix(&leaked_total_, sizeof(leaked_total_));
    return h;
}

} // namespace immune::sim
