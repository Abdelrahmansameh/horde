// game/enemies/EnemyRoster.cpp — implementation. Owner: Wave 2C.
// EnemyRoster.h (declared contract) is frozen; see its header for rationale.
//
// DESIGN RULE (DESIGN.md §8.2, enforced here structurally)
// No per-family subclass, ever. Chaff behaviour is FamilyDef data + chaff flag
// bits consumed by ChaffSystem's one shared kernel (sim/chaff/ChaffSystem.cpp,
// Wave 1B). Only the three named elites below get real per-archetype ECS logic,
// and even that logic is *data* (an ArchetypeBehavior transition table plus a
// couple of small registered systems) rather than a class hierarchy.
#include "game/enemies/EnemyRoster.h"

#include "core/Math.h"
#include "render/ChaffBatcher.h"
#include "render/Renderer.h"
#include "sim/SimWorld.h"
#include "sim/chaff/ChaffBuffers.h"
#include "sim/chaff/ChaffSystem.h"
#include "sim/damage/DamageField.h"
#include "sim/ecs/AiStateMachine.h"
#include "sim/ecs/EcsWorld.h"
#include "sim/ecs/NamedAgents.h"
#include "sim/spatial/SpatialHash.h"

#include <entt/entity/registry.hpp>

#include <vector>

namespace immune::game {

namespace {

// ---------------------------------------------------------------------------
// Chaff tuning derivation helpers
// ---------------------------------------------------------------------------

/// Movement feel by speed tier (DESIGN.md §6's "tempo = speed tier" rule,
/// applied to the physics side rather than the render tempo table). A single
/// small table, not a per-family special case, so every family's tuning comes
/// from exactly two inputs: its speed tier and its behaviour flags.
struct SpeedProfile {
    f32 max_speed;
    f32 acceleration;
    f32 jitter;
};

SpeedProfile speed_profile(SpeedTier tier) {
    switch (tier) {
        case SpeedTier::Slow:    return SpeedProfile{2.5f, 9.0f, 0.12f};
        case SpeedTier::Normal:  return SpeedProfile{4.5f, 16.0f, 0.30f};
        case SpeedTier::Fast:    return SpeedProfile{7.5f, 28.0f, 0.45f};
        // Erratic reads through jitter, not raw top speed: sudden, unpredictable
        // impulses rather than a flat-out sprint (DESIGN.md's Parasite tempo).
        case SpeedTier::Erratic: return SpeedProfile{5.5f, 20.0f, 1.00f};
    }
    return SpeedProfile{4.0f, 16.0f, 0.30f};
}

// ---------------------------------------------------------------------------
// Per-world elite registration state, cached in the ECS registry's context so
// spawn_elite()/register_systems() are idempotent no matter how many times
// App.cpp calls them across level reloads (SimWorld::init() clears entities
// but not systems or context vars — see sim/SimWorld.cpp). Mirrors the
// PlaceholderEliteState pattern in sim/ecs/NamedAgents.cpp.
// ---------------------------------------------------------------------------

struct RosterState {
    bool installed = false;
    u16 parasite_archetype = 0;
    u16 biofilm_archetype = 0;
    u16 tumor_archetype = 0;
};

RosterState& roster_state(entt::registry& registry) {
    return registry.ctx().emplace<RosterState>();
}

// ---------------------------------------------------------------------------
// Archetype behaviour builders. All data, no subclassing (DESIGN.md §8.2):
// each elite is one ArchetypeBehavior value assembled from its EliteDef plus a
// hand-authored transition table.
// ---------------------------------------------------------------------------

sim::ArchetypeBehavior base_behavior_from_elite(const EliteDef& e) {
    sim::ArchetypeBehavior b{};
    b.name = e.name;
    b.family = e.family;
    // ThreatTier::Elite == 1, ThreatTier::Boss == 2 — exactly what
    // ArchetypeBehavior::tier documents ("1 = elite, 2 = boss").
    b.tier = static_cast<u8>(e.tier);
    b.max_health = e.max_health;
    b.armor = e.armor;
    b.max_speed = e.speed;
    b.ability_cooldown = e.ability_cooldown;
    b.death_fade = 0.6f;
    b.sprite_size = e.sprite_size;
    b.tint = render::family_color(e.family);
    b.atlas_index = 0;
    b.attack = sim::comp::Attack{
        /*windup*/ e.telegraph_duration, /*active*/ 0.2f, /*recovery*/ 0.4f,
        /*range*/ 10.0f, /*radius*/ 3.0f, /*damage*/ 10.0f, /*kind*/ 0};
    b.steering = sim::comp::Steering{};
    return b;
}

/// Parasite mid-tier elite (DESIGN.md §6: "Burrows/hides periodically;
/// punishes single-target-only defenses"). Extends Wave 1D's placeholder
/// pattern, but the burrow trigger is *periodic* (a state timer on Advancing)
/// rather than health-gated: the point of this archetype is that no single
/// sustained-damage tower can just camp it down, since it goes untargetable
/// on a fixed cadence regardless of how much damage it has taken.
sim::ArchetypeBehavior make_parasite_behavior(const EliteDef& e) {
    sim::ArchetypeBehavior b = base_behavior_from_elite(e);
    // speed_scale defaults (Burrowed=0, Telegraphing=0.15, Attacking=0,
    // Fleeing=1.4) already fit; Advancing needs the full 1.0 explicitly since
    // ArchetypeBehavior's default table already sets it, kept here for clarity.
    b.speed_scale[static_cast<u8>(sim::AiState::Advancing)] = 1.0f;

    b.add(sim::AiState::Spawning, sim::AiState::Advancing, sim::AiTrigger::StateTimerAtLeast, 0.25f);

    b.add(sim::AiState::Advancing, sim::AiState::Dying, sim::AiTrigger::IsDead);
    // Periodic hide: burrows every 3.5s of advancing, independent of health.
    b.add(sim::AiState::Advancing, sim::AiState::Burrowed, sim::AiTrigger::StateTimerAtLeast, 3.5f);
    b.add(sim::AiState::Advancing, sim::AiState::Telegraphing, sim::AiTrigger::AbilityReady);

    b.add(sim::AiState::Telegraphing, sim::AiState::Dying, sim::AiTrigger::IsDead);
    b.add(sim::AiState::Telegraphing, sim::AiState::Attacking, sim::AiTrigger::TelegraphComplete);

    b.add(sim::AiState::Attacking, sim::AiState::Dying, sim::AiTrigger::IsDead);
    b.add(sim::AiState::Attacking, sim::AiState::Advancing, sim::AiTrigger::StateTimerAtLeast, b.attack.active);

    b.add(sim::AiState::Burrowed, sim::AiState::Dying, sim::AiTrigger::IsDead);
    // Nimble resurface: shorter than the placeholder's wound-triggered hide —
    // this is an offense/defense tempo tool, not a last-resort escape.
    b.add(sim::AiState::Burrowed, sim::AiState::Advancing, sim::AiTrigger::StateTimerAtLeast, 1.8f);
    return b;
}

/// Bacteria "biofilm colony" elite (DESIGN.md §6: "occasional elite" alongside
/// the Bacteria chaff family). Tankier and slower than the parasite; its
/// telegraphed "attack" is not a hit on the objective (damage left at 0) but a
/// cohesion pulse: biofilm_pulse_system (below) reacts to its AttackEvent by
/// flagging nearby chaff Bacteria as chaff_flags::kClumped, i.e. it is the
/// system that manufactures the "occasional elite variant of the clumping
/// mechanic" the frozen header calls for.
sim::ArchetypeBehavior make_biofilm_behavior(const EliteDef& e) {
    sim::ArchetypeBehavior b = base_behavior_from_elite(e);
    b.attack.radius = 6.0f;   // cohesion pulse reach
    b.attack.damage = 0.0f;   // pure utility pulse, not an objective hit
    b.attack.range = 0.0f;    // unused: the pulse doesn't need a locked target

    b.add(sim::AiState::Spawning, sim::AiState::Advancing, sim::AiTrigger::StateTimerAtLeast, 0.4f);
    b.add(sim::AiState::Advancing, sim::AiState::Dying, sim::AiTrigger::IsDead);
    b.add(sim::AiState::Advancing, sim::AiState::Telegraphing, sim::AiTrigger::AbilityReady);
    b.add(sim::AiState::Telegraphing, sim::AiState::Dying, sim::AiTrigger::IsDead);
    b.add(sim::AiState::Telegraphing, sim::AiState::Attacking, sim::AiTrigger::TelegraphComplete);
    b.add(sim::AiState::Attacking, sim::AiState::Dying, sim::AiTrigger::IsDead);
    b.add(sim::AiState::Attacking, sim::AiState::Advancing, sim::AiTrigger::StateTimerAtLeast, b.attack.active);
    return b;
}

/// Cancer Cell tumour boss/objective enemy (DESIGN.md §6: "Not a horde unit —
/// a slow-growing tumor mass that expands from within a defended zone if
/// ignored"). Deliberately minimal state table: it is not a telegraphed
/// attacker, so all it needs is a way out of Spawning and a way into Dying.
/// Its actual threat is comp::TumorMass::radius, grown by tumor_growth_system.
sim::ArchetypeBehavior make_tumor_behavior(const EliteDef& e) {
    sim::ArchetypeBehavior b = base_behavior_from_elite(e);
    b.add(sim::AiState::Spawning, sim::AiState::Advancing, sim::AiTrigger::StateTimerAtLeast, 0.5f);
    b.add(sim::AiState::Advancing, sim::AiState::Dying, sim::AiTrigger::IsDead);
    return b;
}

// ---------------------------------------------------------------------------
// Registered systems
// ---------------------------------------------------------------------------

/// PostUpdate: grows every comp::TumorMass toward its max_radius and clamps
/// there. O(named-agent-count), no allocation, no branching per unrelated
/// entity type (EnTT's view already filters to TumorMass carriers only).
void tumor_growth_system(sim::SystemContext& ctx) {
    auto view = ctx.registry.view<sim::comp::TumorMass>();
    for (auto e : view) {
        sim::comp::TumorMass& t = view.get<sim::comp::TumorMass>(e);
        if (t.radius < t.max_radius) {
            t.radius = math::min(t.max_radius, t.radius + t.growth_rate * ctx.dt);
        }
    }
}

/// Combat phase, after named_combat: inspects this tick's AttackEvents (the
/// same buffer named_combat consumes for objective damage) and, for any event
/// whose source is a biofilm colony, flags nearby chaff Bacteria as
/// chaff_flags::kClumped via a spatial-hash circle query.
///
/// This writes directly into ChaffBuffers::flags from outside sim/chaff.
/// ChaffBuffers.h asks external code to "treat as read-only", which is about
/// not touching layout/count/compaction invariants (I1-I4) — a single flag-bit
/// OR on a live slot changes none of those. EnemyRoster.h's own rationale for
/// chaff_flags::kClumped names this exact pattern ("a system that flags nearby
/// same-family Bacteria as clumped") as the extension point Wave 2C owns, so
/// this is read as the intended use, not a violation of the SoA contract.
void biofilm_pulse_system(sim::SystemContext& ctx) {
    const RosterState* state = ctx.registry.ctx().find<RosterState>();
    if (state == nullptr) return;

    const sim::named::NamedFrame* fr = sim::named::frame_if_any(ctx.registry);
    if (fr == nullptr || fr->attacks.empty()) return;

    sim::ChaffBuffers& chaff = ctx.world.chaff();
    const sim::SpatialHash& hash = ctx.world.spatial();

    // Reused across calls, cleared (not freed) each time: this allocates only
    // while growing toward its steady-state capacity, never in steady state
    // (docs/CONVENTIONS.md §3 — no allocation in the hot path). A biofilm
    // colony's pulse cadence is seconds apart and there are at most a handful
    // of colonies alive at once (named-agent budget is 200), so this is cheap
    // regardless.
    static std::vector<u32> scratch;

    for (const auto& ev : fr->attacks) {
        const entt::entity source = ctx.world.ecs().from_id(ev.source);
        if (!ctx.registry.valid(source)) continue;
        const auto* named = ctx.registry.try_get<sim::comp::NamedAgent>(source);
        if (named == nullptr || named->archetype != state->biofilm_archetype) continue;

        scratch.clear();
        hash.query_circle(ev.point, ev.radius, scratch);
        const f32 r2 = ev.radius * ev.radius;
        for (u32 idx : scratch) {
            if (idx >= chaff.count()) continue;
            if (static_cast<PathogenFamily>(chaff.family[idx]) != PathogenFamily::Bacteria) continue;
            const f32 dx = chaff.pos_x[idx] - ev.point.x;
            const f32 dy = chaff.pos_y[idx] - ev.point.y;
            if (dx * dx + dy * dy > r2) continue;
            chaff.flags[idx] |= sim::chaff_flags::kClumped;
        }
    }
}

// ---------------------------------------------------------------------------
// Idempotent world install, shared by register_systems() and spawn_elite().
// ---------------------------------------------------------------------------

void ensure_roster_installed(const EnemyRoster& roster, sim::SimWorld& world) {
    entt::registry& registry = world.ecs().registry();
    RosterState& state = roster_state(registry);
    if (state.installed) return;

    // The named-agent layer (Wave 1D) is built but nothing in app/ wires it
    // up; enemies are what own "the named-agent tier exists", so this is
    // where it gets installed.
    sim::named::install(world);

    sim::ArchetypeRegistry& table = sim::archetypes(registry);
    if (const EliteDef* d = roster.find_elite("parasite_burrower")) {
        state.parasite_archetype = table.add(make_parasite_behavior(*d));
    }
    if (const EliteDef* d = roster.find_elite("biofilm_colony")) {
        state.biofilm_archetype = table.add(make_biofilm_behavior(*d));
    }
    if (const EliteDef* d = roster.find_elite("tumor_mass")) {
        state.tumor_archetype = table.add(make_tumor_behavior(*d));
    }

    world.ecs().add_system(sim::SystemPhase::PostUpdate, "tumor_growth", 10, &tumor_growth_system);
    world.ecs().add_system(sim::SystemPhase::Combat, "biofilm_colony_pulse", 10, &biofilm_pulse_system);

    state.installed = true;
}

} // namespace

// ---------------------------------------------------------------------------
// EnemyRoster
// ---------------------------------------------------------------------------

void EnemyRoster::load_defaults() {
    auto set = [this](PathogenFamily f, const char* name, SpeedTier speed, f32 density) {
        FamilyDef& d = families_[static_cast<u32>(f)];
        d.family = f;
        d.name = name;
        d.color = render::family_color(f);
        d.speed_tier = speed;
        d.base_density = density;
    };
    // DESIGN.md §6 table. Names mirror the sim-test schema's family strings
    // (docs/AGENT_BRIEF.md): "virus" "bacteria" "fungal_spore" "parasite"
    // "cancer_cell" "allergen".
    set(PathogenFamily::Virus, "virus", SpeedTier::Fast, 0.6f);
    set(PathogenFamily::Bacteria, "bacteria", SpeedTier::Normal, 1.8f);
    set(PathogenFamily::FungalSpore, "fungal_spore", SpeedTier::Slow, 1.2f);
    set(PathogenFamily::Parasite, "parasite", SpeedTier::Erratic, 2.5f);
    set(PathogenFamily::CancerCell, "cancer_cell", SpeedTier::Slow, 8.0f);
    set(PathogenFamily::Allergen, "allergen", SpeedTier::Normal, 1.0f);

    // Behaviour switches, straight off the frozen header's table: Virus
    // replicates, Bacteria clumps, FungalSpore drifts + leaves a hazard,
    // Parasite can hide. CancerCell and Allergen get none — per the header,
    // neither is a chaff-swarm family (boss mass / wave modifier respectively).
    families_[static_cast<u32>(PathogenFamily::Virus)].replicates = true;
    families_[static_cast<u32>(PathogenFamily::Bacteria)].clumps = true;
    families_[static_cast<u32>(PathogenFamily::FungalSpore)].drifts = true;
    families_[static_cast<u32>(PathogenFamily::FungalSpore)].leaves_hazard = true;
    families_[static_cast<u32>(PathogenFamily::Parasite)].can_hide = true;

    elites_.clear();
    elites_.push_back(EliteDef{
        .id = 1, .name = "parasite_burrower", .family = PathogenFamily::Parasite,
        .tier = ThreatTier::Elite, .max_health = 420.0f, .armor = 1.5f,
        .speed = 3.4f, .sprite_size = 1.7f, .ability_cooldown = 5.0f,
        .telegraph_duration = 0.6f, .atp_bounty = 70});
    elites_.push_back(EliteDef{
        .id = 2, .name = "biofilm_colony", .family = PathogenFamily::Bacteria,
        .tier = ThreatTier::Elite, .max_health = 950.0f, .armor = 5.0f,
        .speed = 1.1f, .sprite_size = 2.1f, .ability_cooldown = 4.5f,
        .telegraph_duration = 0.5f, .atp_bounty = 100});
    elites_.push_back(EliteDef{
        .id = 3, .name = "tumor_mass", .family = PathogenFamily::CancerCell,
        .tier = ThreatTier::Boss, .max_health = 5000.0f, .armor = 8.0f,
        .speed = 0.0f, .sprite_size = 3.2f, .ability_cooldown = 30.0f,
        .telegraph_duration = 1.0f, .atp_bounty = 800});
}

const FamilyDef& EnemyRoster::family(PathogenFamily f) const {
    const u32 i = static_cast<u32>(f) < kFamilyCount ? static_cast<u32>(f) : 0u;
    return families_[i];
}

const EliteDef* EnemyRoster::find_elite(std::string_view name) const {
    for (const auto& e : elites_) {
        if (name == e.name) return &e;
    }
    return nullptr;
}

void EnemyRoster::apply_to_tuning(sim::ChaffTuning& tuning) const {
    for (u32 i = 0; i < kFamilyCount; ++i) {
        const FamilyDef& d = families_[i];
        sim::ChaffFamilyParams& p = tuning.family[i];
        const SpeedProfile sp = speed_profile(d.speed_tier);

        p.max_speed = sp.max_speed;
        p.acceleration = sp.acceleration;
        p.jitter = sp.jitter;
        p.base_density = d.base_density;

        // Physical footprint mirrors the renderer's silhouette table
        // (render::family_visual, Wave 1C) so a chaff agent's collision size
        // can never disagree with what's drawn — the same "single source of
        // truth" reasoning FamilyDef::color's doc comment states for colour,
        // just applied to size instead of tint.
        const f32 silhouette = render::family_visual(d.family).silhouette;
        p.radius = silhouette * 0.5f;
        p.separation_radius = p.radius * 2.4f;
        // Denser/tankier families push harder apart so they read as distinct
        // mass rather than an overlapping pile. chaff_flags::kClumped
        // (set per-instance, e.g. by the biofilm elite's pulse) overrides this
        // to zero regardless — see ChaffSystem::update's accumulate pass.
        p.separation_strength = math::min(6.0f + d.base_density * 1.5f, 12.0f);

        // Family-specific mechanics, straight off the flag bits: only
        // FungalSpore drifts, only Virus replicates.
        p.drift_bias = d.drifts ? 0.75f : 0.0f;
        p.replication_rate = d.replicates ? 0.05f : 0.0f;
    }

    // Nothing else currently owns a world "wind" direction; give kDrifting
    // fungal spores somewhere to drift rather than standing still with only
    // jitter to move them. A future level/weather system can override this
    // per level — SimDesc::chaff_tuning is copied into ChaffSystem at
    // SimWorld::init(), so whichever call sets it last wins.
    tuning.ambient_drift = Vec2{0.5f, 0.28f};
}

EntityId EnemyRoster::spawn_elite(sim::SimWorld& world, u16 elite_id, Vec2 pos) const {
    const EliteDef* def = nullptr;
    for (const auto& e : elites_) {
        if (e.id == elite_id) { def = &e; break; }
    }
    if (def == nullptr) return EntityId{};

    ensure_roster_installed(*this, world);
    entt::registry& registry = world.ecs().registry();
    const RosterState* state = registry.ctx().find<RosterState>();
    if (state == nullptr) return EntityId{};

    u16 archetype_id = 0;
    if (def->name == std::string_view("parasite_burrower")) archetype_id = state->parasite_archetype;
    else if (def->name == std::string_view("biofilm_colony")) archetype_id = state->biofilm_archetype;
    else if (def->name == std::string_view("tumor_mass")) archetype_id = state->tumor_archetype;
    else return EntityId{};

    sim::named::SpawnParams sp;
    sp.archetype = archetype_id;
    sp.position = pos;
    sp.health_scale = 1.0f;
    sp.rotation = 0.0f;
    const EntityId id = sim::named::spawn(world, sp);
    if (!id.valid()) return id;

    // comp::Health/comp::NamedAgent/comp::Sprite are already attached by
    // named::spawn() straight from the archetype table above, which was built
    // from this EliteDef — so they already agree with it by construction.
    // TumorMass is the one extra component the shared named-agent spawn path
    // knows nothing about.
    if (def->family == PathogenFamily::CancerCell) {
        const entt::entity raw = world.ecs().from_id(id);
        sim::comp::TumorMass tumor;
        tumor.growth_rate = 0.12f;   // slow: DESIGN.md's "expands if ignored"
        tumor.radius = 1.0f;
        tumor.max_radius = 10.0f;
        registry.emplace<sim::comp::TumorMass>(raw, tumor);
    }
    return id;
}

void EnemyRoster::register_systems(sim::SimWorld& world) {
    ensure_roster_installed(*this, world);
}

// ---------------------------------------------------------------------------
// Allergen wave modifier (DESIGN.md §6: "Curveball wave type; triggers a
// friendly 'overreaction' risk/reward mechanic (temporary self-damage
// field)"). Allergen is explicitly NOT a spawnable elite and not really a
// chaff-swarm family — EnemyRoster.h's own rationale calls it a wave
// modifier — so it has no natural slot in the frozen class's public surface
// (no new method can be added to a frozen header without an orchestrator
// broadcast). This free function is the minimal, scoped primitive the brief
// asks for: it submits exactly the friendly-fire-flagged DamageField the
// mechanic needs. Full orchestration (when a wave rolls this curveball, how
// long the overreaction lasts, HUD warning) is Wave 3A's territory and is
// deliberately not attempted here.
//
// Not declared in EnemyRoster.h. External linkage (no anonymous namespace) so
// a future translation unit can forward-declare this exact signature and call
// it without needing a header change today; tests/test_enemies_allergen.cpp
// does exactly that to prove it out.
void allergen_overreaction(sim::SimWorld& world, Vec2 origin, f32 radius, f32 kill_rate, f32 duration) {
    sim::DamageField field;
    field.shape = sim::FieldShape::Circle;
    field.origin = origin;
    field.radius = radius;
    field.kill_rate = kill_rate;
    field.falloff = 0.0f;
    field.family_mask = 0xFFu;
    field.marked_multiplier = 1.0f;
    field.lifetime = duration;
    field.friendly_fire = true;
    world.damage().submit(field);
}

} // namespace immune::game
