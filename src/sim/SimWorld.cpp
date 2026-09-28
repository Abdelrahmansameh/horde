#include "sim/SimWorld.h"

#include "core/JobSystem.h"
#include "core/Math.h"
#include "sim/ecs/Components.h"

#include <algorithm>
#include <cmath>

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
    // Resolve the "empty means world_bounds" default ONCE, here, so every
    // reader below (and every reader in tick()) can use desc_.sim_bounds
    // unconditionally instead of re-deciding which rect it meant.
    if (desc_.sim_bounds.size().x <= 0.0f || desc_.sim_bounds.size().y <= 0.0f) {
        desc_.sim_bounds = desc_.world_bounds;
    }
    jobs_ = jobs;
    rng_.reseed(desc.seed);
    tick_ = 0;
    killed_total_ = 0;
    leaked_total_ = 0;
    swarmers_killed_total_ = 0;
    towers_lost_total_ = 0;
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
    chaff_system_.set_world_bounds(desc_.sim_bounds);

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
    shd.bounds = desc_.sim_bounds;
    shd.cell_size = desc.spatial_cell_size;
    spatial_.configure(shd);

    damage_.reserve(desc.max_damage_fields);
    projectiles_.reserve(desc.max_projectiles);
    swarmers_.reserve(desc.max_swarmers);
    swarmer_system_.effects().reserve(1024);
    swarmer_system_.set_collision(desc.swarmer_collision);
    {
        f32 radii[kFamilyCount];
        for (u32 f = 0; f < kFamilyCount; ++f) radii[f] = desc.chaff_tuning.family[f].radius;
        swarmer_system_.set_chaff_radii(radii, kFamilyCount);
        hostile_.set_chaff_radii(radii, kFamilyCount);
    }
    hostile_.set_tuning(desc.hostile_tuning);
    burrow_.set_tuning(desc.burrow_tuning);
    immunity_scratch_.reserve(1024);
    inflamed_zones_.clear();
    inflamed_zones_.reserve(256);
    set_immunity(desc.immunity);
    burrow_threats_.clear();
    burrow_threats_.reserve(256);
    last_burrow_stats_ = BurrowStats{};
    burrows_total_ = 0;
    scars_.reset();
    friendly_towers_.items.reserve(256);
    friendly_towers_.damage.reserve(256);
    friendly_towers_.passengers.reserve(256);
    named_targets_.items.reserve(256);
    named_targets_.damage.reserve(256);
    fluid_.reserve(desc.max_fluid_particles);
    fluid_.clear();
    fluid_system_.configure(desc_.sim_bounds, desc.fluid_tuning);
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
            0.0f, objective_integrity_ -
                      static_cast<f32>(chaff_stats.despawned_at_goal) * immunity_.leak_damage);
        if (profiler) profiler->record(prof_key::kChaffUpdate, t.elapsed_ms());
    }

    // 2b. Burrowing and slither (sim/burrow/Burrow.h). Right after movement:
    // a dive freezes the agent where this tick's step left it, and an agent
    // that finishes surfacing is back in the kernel next tick. Before every
    // damage source, so a parasite that went under this tick is already out
    // of their reach -- and before the towers' aim search in the ECS tick,
    // which reads kHidden.
    if (burrow_.active()) {
        WallClock t;
        build_burrow_threats();
        last_burrow_stats_ =
            burrow_.update(chaff_, flow_, sdf_, tissue_, spatial_, burrow_threats_, rng_, kFixedDt,
                           desc_.world_bounds);
        burrows_total_ += last_burrow_stats_.dives;
        if (profiler) profiler->record(prof_key::kBurrow, t.elapsed_ms());
    }

    // 3. ECS systems (named agents, towers).
    {
        WallClock t;
        SystemContext ctx{*this, ecs_.registry(), rng_, kFixedDt, tick_};
        ecs_.tick(ctx);
        if (profiler) profiler->record(prof_key::kEcsTick, t.elapsed_ms());
    }

    // 3b. Inflammation (sim/Immunity.h): the zones this tick's swarmers hit
    // harder in, and the reload towers standing in them get.
    inflame();

    // 4. Aggregate damage.
    last_damage_stats_ = damage_.apply(chaff_, spatial_, rng_, kFixedDt);

    // 4b. Projectiles. Must run after the ECS tick (towers fire during Combat
    // phase, so this tick's new rounds exist by now) and BEFORE the single
    // chaff compaction below -- both damage paths write density, and
    // ChaffBuffers::compact() runs exactly once per tick, after every source
    // has applied. Running it here also means a round and a field that kill the
    // same agent on the same tick both get their damage counted.
    const ProjectileStats projectile_stats =
        projectile_system_.update(projectiles_, chaff_, spatial_, tissue_, desc_.sim_bounds,
                                  rng_, kFixedDt, &combat_events_);
    // Incendiary Rounds (sim/Immunity.h): the rounds that just landed set the
    // ground alight. Timed fields, so they burn from the next tick's apply.
    ignite_impacts();

    // 4c. Swarmers. Same placement rule and the same reason as projectiles
    // above: after the ECS tick so this tick's newly released units exist,
    // and before the single chaff compaction so a swarmer's drain and a field's
    // damage on the same agent on the same tick both get counted. The kernel
    // sees named agents through a flat snapshot built here, and what it asks
    // for -- bursts, splashes, rounds -- lands in the other
    // stores straight after, so a detonation this tick is a field or
    // fluid by the time the fluid solver below runs.
    build_named_targets();
    const SwarmerStats swarmer_stats =
        swarmer_system_.update(swarmers_, chaff_, spatial_, named_targets_, &sdf_, &flow_, desc_.sim_bounds,
                               rng_, kFixedDt, &combat_events_);
    apply_swarmer_effects();

    // 4c'. Slow expiry. The mucus film (below) is the only thing that grants
    // chaff_flags::kSlowed, and this is the only place the bit goes away. It
    // runs BEFORE the fluid refresh, so an agent still standing in mucus
    // never sees its clock hit zero: the refresh tops it back up this tick.
    chaff_.expire_slows(kFixedDt);

    // 4c''. The horde fights back (sim/hostile/HostileAttacks.h): viruses
    // latch onto towers and swarmers and feed, bacteria burn whatever stands
    // in their aura. After the swarmer update so passengers are planted on
    // this tick's positions and a unit killed here has had its last say;
    // before compaction so a virus that died while latched retires in the
    // same pass as everything else. Tower damage lands on the ECS straight
    // after, the way swarmer hits on named agents do.
    {
        WallClock t;
        build_friendly_towers();
        const HostileStats hostile_stats = hostile_.update(
            chaff_, spatial_, swarmers_, friendly_towers_, tissue_, kFixedDt, &combat_events_);
        swarmers_killed_total_ += hostile_stats.swarmers_killed;
        apply_hostile_effects();
        // Scars the pass just emptied come down now, in the same tick, so
        // the tissue is handed back before the flow pump below and the
        // passengers riding them are let go on the next pass (their host
        // id no longer resolves). Towers are torn down by the game layer
        // instead (game/towers), because it owns the placed-tower list.
        scars_.upkeep(*this, kFixedDt, &combat_events_);
        if (profiler) profiler->record(prof_key::kHostile, t.elapsed_ms());
    }

    // 4d. Fluid. Same placement rule and the same reason again -- after the ECS
    // tick so this tick's freshly emitted jet exists, and before the single
    // chaff compaction so the mucus film's damage lands in the same accounting
    // pass as everything else. It runs LAST of the three on purpose: the solver
    // reads the chaff spatial hash for crowd braking, and running it after the
    // other two means a round or a granule that already killed an agent this
    // tick has not yet moved that agent's slot out from under the grid.
    const FluidStats fluid_stats =
        fluid_system_.update(fluid_, chaff_, spatial_, sdf_, desc_.sim_bounds,
                             kFixedDt, &combat_events_);
    // Named agents use the same wet coverage as chaff. Refresh while standing
    // in mucus, then let the ordinary slowed upkeep expire it after leaving.
    entt::registry& registry = ecs_.registry();
    auto fluid_targets = registry.view<const comp::NamedAgent, const comp::Transform,
                                       const comp::Health>();
    for (auto e : fluid_targets) {
        if (fluid_targets.get<const comp::Health>(e).dead()) continue;
        f32 duration = 0.0f;
        f32 factor = 1.0f;
        const u8 family = static_cast<u8>(fluid_targets.get<const comp::NamedAgent>(e).family);
        const Vec2 pos = fluid_targets.get<const comp::Transform>(e).position;
        if (!fluid_system_.slow_at(pos, family, duration, factor)) continue;
        if (auto* sl = registry.try_get<comp::Slowed>(e)) {
            sl->remaining = math::max(sl->remaining, duration);
            sl->factor = math::min(sl->factor, factor);
        } else {
            registry.emplace<comp::Slowed>(e, comp::Slowed{duration, factor, {}});
        }
    }

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
    // 4d'. Inflammatory Scarring: after every other source, so a scar only
    // burns what the rest of the board left standing, and before the
    // accounting below so its kills pay like any other.
    const f32 scar_contact_removed = apply_scar_contact();

    last_damage_stats_.density_removed +=
        projectile_stats.density_removed + swarmer_stats.density_removed +
        fluid_stats.density_removed + scar_contact_removed;

    // 4e'. Anaphylactic Shock. Every damage source for the tick has decided
    // who dies, and nothing has been compacted yet, so a dying agent still has
    // its position and its slow to pass on.
    spread_contagion();

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
    s.chaff_latched = hostile_.last_stats().latched;
    s.swarmers_killed_total = swarmers_killed_total_;
    s.towers_lost_total = towers_lost_total_;
    s.scars_live = scars_.stats().live;
    s.scars_built_total = scars_.stats().built_total;
    s.scars_lost_total = scars_.stats().lost_total;
    s.chaff_burrowed = last_burrow_stats_.burrowed;
    s.burrows_total = burrows_total_;
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
        mix(chaff_.wander_x.data(), n * sizeof(f32));
        mix(chaff_.wander_y.data(), n * sizeof(f32));
        mix(chaff_.density.data(), n * sizeof(f32));
        mix(chaff_.family.data(), n * sizeof(u8));
        mix(chaff_.flags.data(), n * sizeof(u8));
        // The slow is a timer now, not just a bit: two worlds whose agents are
        // slowed for different remaining seconds diverge on the next tick.
        mix(chaff_.slow_remaining.data(), n * sizeof(f32));
        mix(chaff_.slow_factor.data(), n * sizeof(f32));
        // Burrowing is a timer-driven state machine with a chosen exit: two
        // worlds that disagree on any of it put a parasite somewhere else.
        mix(chaff_.burrow_state.data(), n * sizeof(u8));
        mix(chaff_.burrow_timer.data(), n * sizeof(f32));
        mix(chaff_.burrow_target_x.data(), n * sizeof(f32));
        mix(chaff_.burrow_target_y.data(), n * sizeof(f32));
    }
    // Swarmers steer, choose and kill, so where they are and what they hold
    // is gameplay state; positions plus targets are enough to catch a
    // divergence, everything else is derived from those next tick.
    const usize sn = swarmers_.count();
    if (sn > 0) {
        mix(swarmers_.pos_x.data(), sn * sizeof(f32));
        mix(swarmers_.pos_y.data(), sn * sizeof(f32));
        mix(swarmers_.target_generation.data(), sn * sizeof(u32));
        mix(swarmers_.life.data(), sn * sizeof(f32));
        mix(swarmers_.group.data(), sn * sizeof(u32));
        // Health decides when a unit dissolves, and the horde is what spends
        // it: two worlds whose units have been bitten differently diverge the
        // tick one of them dies.
        mix(swarmers_.health.data(), sn * sizeof(f32));
        // An ArborGrabber's arm cycles and captive handles decide which
        // agents are currently removed from the flow and when they are
        // swallowed. Hash only initialized fields, never struct padding.
        for (usize i = 0; i < sn; ++i) {
            if (swarmers_.profile_of(i).kind != SwarmerKind::ArborGrabber) continue;
            const ArborGrabberState& arbor = swarmers_.arbor_grabber[i];
            mix(&arbor.heading, sizeof(arbor.heading));
            for (const ArborArmState& arm : arbor.arms) {
                mix(&arm.heading, sizeof(arm.heading));
                const u8 phase = static_cast<u8>(arm.phase);
                mix(&phase, sizeof(phase));
                mix(&arm.time, sizeof(arm.time));
                mix(&arm.reach, sizeof(arm.reach));
                mix(&arm.grip, sizeof(arm.grip));
                mix(&arm.captive.named.value, sizeof(arm.captive.named.value));
                mix(&arm.captive.chaff_count, sizeof(arm.captive.chaff_count));
                for (u32 c = 0; c < arm.captive.chaff_count && c < kArborMaxCaptives; ++c) {
                    mix(&arm.captive.chaff[c].handle.index,
                        sizeof(arm.captive.chaff[c].handle.index));
                    mix(&arm.captive.chaff[c].handle.generation,
                        sizeof(arm.captive.chaff[c].handle.generation));
                    mix(&arm.captive.chaff[c].offset, sizeof(arm.captive.chaff[c].offset));
                }
            }
        }
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
    mix(&swarmers_killed_total_, sizeof(swarmers_killed_total_));
    mix(&towers_lost_total_, sizeof(towers_lost_total_));
    return h;
}

void SimWorld::build_burrow_threats() {
    burrow_threats_.clear();
    const entt::registry& registry = ecs_.registry();
    auto view = registry.view<const comp::Tower, const comp::Transform, const comp::Health>();
    for (auto e : view) {
        if (view.get<const comp::Health>(e).dead()) continue;
        BurrowThreat t;
        t.position = view.get<const comp::Transform>(e).position;
        // comp::Tower::range is the swarmers' aggro radius (game/towers):
        // the ground this tower actually defends.
        t.radius = view.get<const comp::Tower>(e).range;
        burrow_threats_.push_back(t);
    }
    // entt view order is storage order, which depends on creation/destruction
    // history rather than on anything in the sim state. The scoring only sums
    // over the list, so order cannot change a result -- but keep it canonical
    // anyway so nothing downstream can come to depend on it.
    std::sort(burrow_threats_.begin(), burrow_threats_.end(),
              [](const BurrowThreat& a, const BurrowThreat& b) {
                  if (a.position.x != b.position.x) return a.position.x < b.position.x;
                  if (a.position.y != b.position.y) return a.position.y < b.position.y;
                  return a.radius < b.radius;
              });
}

void SimWorld::build_friendly_towers() {
    friendly_towers_.clear();
    const entt::registry& registry = ecs_.registry();
    auto view = registry.view<const comp::Tower, const comp::Transform, const comp::Health>();
    for (auto e : view) {
        const comp::Health& hp = view.get<const comp::Health>(e);
        // Already at zero: waiting for the game layer to tear it down. Not a
        // host any more -- its passengers drop off this tick -- and not worth
        // burning further.
        if (hp.dead()) continue;
        FriendlyTower t;
        t.id = ecs_.to_id(e);
        t.position = view.get<const comp::Transform>(e).position;
        const comp::Tower& tw = view.get<const comp::Tower>(e);
        t.type = tw.type;
        t.visual_id = tw.tier;
        // Body radius from the sprite, which is the footprint drawn at
        // diameter 2 * footprint_radius (game/towers): half its size.
        if (const auto* sp = registry.try_get<comp::Sprite>(e)) t.radius = math::max(sp->size * 0.5f, 0.0f);
        t.health = hp.current;
        friendly_towers_.add(t);
    }
    // Scars (sim/scar) are hosts too, as bars: the pass measures to their
    // faces and latches along them. `radius` is the bounding radius so the
    // walk around the centre still reaches every part of the wall.
    auto scars = registry.view<const comp::Scar, const comp::Transform, const comp::Health>();
    for (auto e : scars) {
        const comp::Health& hp = scars.get<const comp::Health>(e);
        if (hp.dead()) continue;
        const comp::Scar& sc = scars.get<const comp::Scar>(e);
        const comp::Transform& tf = scars.get<const comp::Transform>(e);
        FriendlyTower t;
        t.id = ecs_.to_id(e);
        t.position = tf.position;
        t.rotation = tf.rotation;
        t.half_extents = sc.half_extents;
        t.radius = math::length(sc.half_extents);
        t.health = hp.current;
        t.type = sc.source;
        t.visual_id = sc.visual_id;
        friendly_towers_.add(t);
    }
    friendly_towers_.sort();
}

void SimWorld::apply_hostile_effects() {
    entt::registry& registry = ecs_.registry();
    for (usize k = 0; k < friendly_towers_.items.size(); ++k) {
        const f32 amount = friendly_towers_.damage[k];
        if (amount <= 0.0f) continue;
        const entt::entity e = ecs_.from_id(friendly_towers_.items[k].id);
        if (!registry.valid(e) || !registry.all_of<comp::Health>(e)) continue;
        comp::Health& hp = registry.get<comp::Health>(e);
        const bool was_alive = !hp.dead();
        hp.current -= amount;
        // Scars are in the list too; their losses are the scar system's count.
        if (was_alive && hp.dead() && registry.all_of<comp::Tower>(e)) ++towers_lost_total_;
    }
}

void SimWorld::build_named_targets() {
    named_targets_.clear();
    const entt::registry& registry = ecs_.registry();
    auto view = registry.view<const comp::NamedAgent, const comp::Transform, const comp::Health>();
    for (auto e : view) {
        const comp::Health& hp = view.get<const comp::Health>(e);
        if (hp.dead()) continue;
        // Burrowed is invisible to every swarmer. This is the one place that
        // rule is enforced for named agents; chaff enforce it through kHidden
        // in the kernel's own targetable() test.
        if (const auto* brain = registry.try_get<comp::AiBrain>(e)) {
            if (brain->state == comp::AiState::Burrowed) continue;
        }
        NamedTarget t;
        t.id = ecs_.to_id(e);
        t.position = view.get<const comp::Transform>(e).position;
        if (const auto* v = registry.try_get<comp::Velocity>(e)) t.velocity = v->value;
        // Body radius from the sprite, which is what the player sees as the
        // agent's edge; half its size is the radius.
        if (const auto* sp = registry.try_get<comp::Sprite>(e)) t.radius = math::max(sp->size * 0.5f, 0.0f);
        t.armor = hp.armor;
        t.health = hp.current;
        if (const auto* mk = registry.try_get<comp::Marked>(e)) t.damage_multiplier = mk->damage_multiplier;
        if (registry.all_of<comp::Slowed>(e)) t.damage_multiplier *= immunity_.slowed_damage_mult;
        t.family = static_cast<u8>(view.get<const comp::NamedAgent>(e).family);
        named_targets_.add(t);
    }
    named_targets_.sort();
}

void SimWorld::apply_swarmer_effects() {
    entt::registry& registry = ecs_.registry();
    SwarmerEffects& fx = swarmer_system_.effects();
    DamageAttribution* attribution = damage_.attribution();

    // ---- Named-agent hit points the kernel accumulated (latch drains, shooter
    // rounds). Already armor-adjusted, marked-multiplied and attributed; just
    // land them.
    for (usize k = 0; k < named_targets_.items.size(); ++k) {
        const f32 amount = named_targets_.damage[k];
        if (amount <= 0.0f) continue;
        const entt::entity e = ecs_.from_id(named_targets_.items[k].id);
        if (!registry.valid(e) || !registry.all_of<comp::Health>(e)) continue;
        registry.get<comp::Health>(e).current -= amount * immunity_.named_damage_mult;
    }

    // ---- Bursts: a timed Circle field for the chaff (the damage system's
    // job from here), plus a direct hit on every named agent under it.
    for (const SwarmerBurst& b : fx.bursts) {
        DamageField field;
        field.shape = FieldShape::Circle;
        field.origin = b.origin;
        field.radius = b.radius;
        // `damage` is the total a centred agent takes over the burst; the
        // field wants a rate.
        field.kill_rate = b.damage / math::max(b.seconds, kFixedDt);
        field.falloff = b.falloff;
        field.family_mask = b.family_mask;
        field.marked_multiplier = chaff_flags::kMarkedDamageMultiplier;
        field.lifetime = math::max(b.seconds, kFixedDt);
        field.owner = b.owner;
        damage_.submit(field);

        if (b.named_damage <= 0.0f) continue;
        auto view = registry.view<const comp::NamedAgent, const comp::Transform, comp::Health>();
        for (auto e : view) {
            comp::Health& hp = view.get<comp::Health>(e);
            if (hp.dead()) continue;
            const comp::NamedAgent& na = view.get<const comp::NamedAgent>(e);
            if ((b.family_mask & static_cast<u8>(1u << static_cast<u8>(na.family))) == 0) continue;
            const Vec2 pos = view.get<const comp::Transform>(e).position;
            const f32 d = math::length(pos - b.origin);
            if (d > b.radius) continue;
            const f32 t = math::saturate(d / math::max(b.radius, math::kEpsilon));
            const f32 fall = b.falloff <= 0.0f ? 1.0f : std::pow(1.0f - t, math::max(b.falloff, 0.1f));
            f32 amount = math::max(0.0f, b.named_damage * fall - hp.armor);
            if (const auto* mk = registry.try_get<comp::Marked>(e)) amount *= mk->damage_multiplier;
            if (registry.all_of<comp::Slowed>(e)) amount *= immunity_.slowed_damage_mult;
            amount *= immunity_.named_damage_mult;
            if (amount <= 0.0f) continue;
            const bool was_alive = !hp.dead();
            hp.current -= amount;
            if (attribution && b.owner.valid()) {
                attribution->record_named(b.owner, amount, was_alive && hp.dead());
            }
        }
    }

    // ---- Splashes: real fluid whose coverage applies a timed slow.
    for (const SwarmerSplash& s : fx.splashes) {
        FluidJetParams jet;
        jet.lifetime = s.lifetime;
        jet.damage_per_second = 0.0f;
        jet.slow_duration = s.slow_duration;
        jet.slow_factor = s.slow_factor;
        jet.family_mask = s.family_mask;
        jet.owner = s.owner;
        jet.visual_id = s.visual_id;
        jet.burst_id = static_cast<u16>(s.seed & 0xFFFFu);
        jet.seed = s.seed;
        fluid_system_.splash(fluid_, jet, s.origin, s.radius, s.speed, s.droplets);

    }

    // ---- Builds: a Fibroblast's builder reached its site. The scar system
    // decides what that means -- a new wall carved out of the tissue, a
    // reinforcement of one already there, or nothing (it would seal the
    // lane) -- with the wall's numbers read off the unit's profile.
    for (const SwarmerBuild& b : fx.builds) {
        ScarDesc d = scar_desc_from_profile(swarmers_.profile_at(b.profile));
        d.center = b.origin;
        d.owner = b.owner;
        d.source = b.source;
        d.visual_id = b.visual_id;
        scars_.build(*this, d, nullptr, &combat_events_);
    }

    // ---- Rounds: straight into the projectile store. They integrate from
    // the next tick, which is the same one-tick latency a tower-fired round
    // always had.
    for (const SwarmerShot& sh : fx.shots) {
        ProjectileSpawnParams round;
        round.position = sh.origin;
        round.velocity = sh.velocity;
        round.damage = sh.damage;
        round.lifetime = sh.lifetime;
        round.hit_radius = sh.hit_radius;
        round.family_mask = sh.family_mask;
        round.owner = sh.owner;
        round.visual_id = sh.visual_id;
        if (immunity_.incendiary_radius > 0.0f) round.flags |= projectile_flags::kIncendiary;
        projectiles_.spawn(round);
    }

    // ---- Heals: Phagocytic Sustain. Onto the releasing tower's integrity,
    // never past its max, and never onto one already emptied -- a tower at
    // zero is waiting for the game layer to tear it down, not to be revived.
    for (const SwarmerHeal& h : fx.heals) {
        const entt::entity e = ecs_.from_id(h.owner);
        if (!registry.valid(e) || !registry.all_of<comp::Tower, comp::Health>(e)) continue;
        comp::Health& hp = registry.get<comp::Health>(e);
        if (hp.dead()) continue;
        hp.current = math::min(hp.max, hp.current + h.amount);
    }

    fx.clear();
}

// ---------------------------------------------------------------------------
// Strengthen Immunity capstones with no unit to hang them on (sim/Immunity.h).
// ---------------------------------------------------------------------------

void SimWorld::set_immunity(const ImmunityTuning& t) {
    immunity_ = t;
    chaff_.set_slowed_damage_multiplier(immunity_.slowed_damage_mult);
    swarmer_system_.set_inflamed_zones(&inflamed_zones_, immunity_.inflammation_damage_mult);
    projectile_system_.set_max_impact_log(
        immunity_.incendiary_radius > 0.0f ? immunity_.max_incendiary_per_tick : 0u);
}

void SimWorld::ignite_impacts() {
    if (immunity_.incendiary_radius <= 0.0f) return;
    const f32 seconds = math::max(immunity_.incendiary_seconds, kFixedDt);
    for (const ProjectileImpactRecord& r : projectile_system_.impact_log()) {
        DamageField field;
        field.shape = FieldShape::Circle;
        field.origin = r.position;
        field.radius = immunity_.incendiary_radius;
        field.kill_rate = immunity_.incendiary_damage / seconds;
        field.falloff = 0.5f;
        field.marked_multiplier = chaff_flags::kMarkedDamageMultiplier;
        field.lifetime = seconds;
        field.owner = r.owner;
        damage_.submit(field);
    }
}

f32 SimWorld::apply_scar_contact() {
    if (immunity_.scar_contact_rate <= 0.0f || chaff_.count() == 0) return 0.0f;
    const f32 reach = math::max(immunity_.scar_contact_reach, 0.0f);
    const f32 bite = immunity_.scar_contact_rate * kFixedDt;
    const usize indexed = math::min(spatial_.indexed_count(), chaff_.count());
    f32 removed_total = 0.0f;

    // Walked through friendly_towers_ (rebuilt this tick, sorted by id) rather
    // than an entt view, so which scar reaches a shared agent first is a
    // function of the sim state alone and not of storage order.
    const entt::registry& registry = ecs_.registry();
    for (usize k = 0; k < friendly_towers_.items.size(); ++k) {
        const entt::entity e = ecs_.from_id(friendly_towers_.items[k].id);
        if (!registry.valid(e)) continue;
        const auto* sc = registry.try_get<comp::Scar>(e);
        const auto* tf = registry.try_get<comp::Transform>(e);
        const auto* hp = registry.try_get<comp::Health>(e);
        if (sc == nullptr || tf == nullptr || hp == nullptr || hp->dead()) continue;
        const Vec2 he = sc->half_extents + Vec2{reach, reach};
        const f32 c = std::cos(tf->rotation);
        const f32 s = std::sin(tf->rotation);

        immunity_scratch_.clear();
        spatial_.query_circle(tf->position, math::length(he), immunity_scratch_);
        for (const u32 idx : immunity_scratch_) {
            if (idx >= indexed) continue;
            const u8 f = chaff_.flags[idx];
            if ((f & chaff_flags::kAlive) == 0 || (f & chaff_flags::kPendingKill) != 0) continue;
            if ((f & chaff_flags::kHidden) != 0) continue;
            // Into the bar's frame: x along it, y across it.
            const f32 dx = chaff_.pos_x[idx] - tf->position.x;
            const f32 dy = chaff_.pos_y[idx] - tf->position.y;
            const f32 lx = dx * c + dy * s;
            const f32 ly = -dx * s + dy * c;
            if (std::fabs(lx) > he.x || std::fabs(ly) > he.y) continue;
            const f32 before = chaff_.density[idx];
            chaff_.apply_density_loss(idx, bite);
            removed_total += before - chaff_.density[idx];
        }
    }
    return removed_total;
}

void SimWorld::spread_contagion() {
    if (immunity_.contagion_radius <= 0.0f) return;
    const usize n = chaff_.count();
    const usize indexed = math::min(spatial_.indexed_count(), n);
    const f32 r2 = immunity_.contagion_radius * immunity_.contagion_radius;
    u32 spread = 0;
    for (usize i = 0; i < n && spread < immunity_.max_contagion_per_tick; ++i) {
        const u8 f = chaff_.flags[i];
        // Killed -- not leaked: see raise_chaff_deaths for why density is the
        // tell -- while carrying a slow.
        if ((f & chaff_flags::kPendingKill) == 0 || (f & chaff_flags::kSlowed) == 0) continue;
        if (chaff_.density[i] > 0.0f) continue;
        ++spread;

        const f32 ax = chaff_.pos_x[i];
        const f32 ay = chaff_.pos_y[i];
        const f32 factor = chaff_.slow_factor[i];
        immunity_scratch_.clear();
        spatial_.query_circle(Vec2{ax, ay}, immunity_.contagion_radius, immunity_scratch_);
        for (const u32 j : immunity_scratch_) {
            if (j >= indexed || j == i) continue;
            const u8 g = chaff_.flags[j];
            if ((g & chaff_flags::kAlive) == 0 || (g & chaff_flags::kPendingKill) != 0) continue;
            const f32 dx = chaff_.pos_x[j] - ax;
            const f32 dy = chaff_.pos_y[j] - ay;
            if (dx * dx + dy * dy > r2) continue;
            if ((g & chaff_flags::kSlowed) != 0) {
                chaff_.slow_remaining[j] =
                    math::max(chaff_.slow_remaining[j], immunity_.contagion_seconds);
                chaff_.slow_factor[j] = math::min(chaff_.slow_factor[j], factor);
            } else {
                chaff_.flags[j] = static_cast<u8>(g | chaff_flags::kSlowed);
                chaff_.slow_remaining[j] = immunity_.contagion_seconds;
                chaff_.slow_factor[j] = factor;
            }
        }
    }
}

void SimWorld::inflame() {
    inflamed_zones_.clear();
    if (immunity_.inflammation_radius <= 0.0f) return;
    entt::registry& registry = ecs_.registry();
    auto scars = registry.view<const comp::Scar, const comp::Transform, const comp::Health>();
    for (auto e : scars) {
        if (scars.get<const comp::Health>(e).dead()) continue;
        InflamedZone z;
        z.center = scars.get<const comp::Transform>(e).position;
        // Out past the bar's ends, so a long wall inflames along its length.
        z.radius = scars.get<const comp::Scar>(e).half_extents.x + immunity_.inflammation_radius;
        inflamed_zones_.push_back(z);
    }
    // Storage order is creation history, not sim state; the kernel only asks
    // "inside any?", but keep the list canonical all the same.
    std::sort(inflamed_zones_.begin(), inflamed_zones_.end(),
              [](const InflamedZone& a, const InflamedZone& b) {
                  if (a.center.x != b.center.x) return a.center.x < b.center.x;
                  if (a.center.y != b.center.y) return a.center.y < b.center.y;
                  return a.radius < b.radius;
              });
    if (inflamed_zones_.empty() || immunity_.inflammation_reload_mult <= 1.0f) return;

    // Reload: a tower standing in inflamed tissue runs its cooldown this much
    // faster. The tower's own system already took one dt off this tick.
    const f32 extra = (immunity_.inflammation_reload_mult - 1.0f) * kFixedDt;
    auto towers = registry.view<comp::Tower, const comp::Transform>();
    for (auto e : towers) {
        const Vec2 p = towers.get<const comp::Transform>(e).position;
        for (const InflamedZone& z : inflamed_zones_) {
            if (math::length_sq(p - z.center) > z.radius * z.radius) continue;
            comp::Tower& tw = towers.get<comp::Tower>(e);
            tw.cooldown = math::max(0.0f, tw.cooldown - extra);
            break;
        }
    }
}

} // namespace immune::sim
