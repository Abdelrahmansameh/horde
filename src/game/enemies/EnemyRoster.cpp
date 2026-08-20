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

#include "game/enemies/EnemyConfigApply.h"

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
#include "sim/flowfield/FlowField.h"
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

/// The four tiers, now read from assets/config/enemies.json. Erratic
/// deliberately reads through jitter rather than raw top speed: sudden,
/// unpredictable impulses rather than a flat-out sprint (DESIGN.md's Parasite
/// tempo).
SpeedProfile speed_profile(SpeedTier tier) {
    const SpeedProfileParams& p = enemy_speed_profile(tier);
    return SpeedProfile{p.max_speed, p.acceleration, p.jitter};
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
    u16 abscess_hulk_archetype = 0;
    u16 spore_colossus_archetype = 0;
    /// Shared cooldown gate for fungal_hazard_system (below), so one dense
    /// cluster of eroding fungal spores can't spawn a hazard field every
    /// single tick it stays in a damage field's radius.
    f32 fungal_hazard_cooldown = 0.0f;
};

RosterState& roster_state(entt::registry& registry) {
    return registry.ctx().emplace<RosterState>();
}

// ---------------------------------------------------------------------------
// Per-entity state for the deliverables this wave adds. Plain EnTT component
// types, deliberately NOT added to the frozen sim/ecs/Components.h -- EnTT
// stores any trivially-manageable type as a component without it needing to
// be declared there; that file's rationale is about the *sim-visible*
// component set other modules consume, not an exhaustive list of every
// component that may ever exist. These two are private to this .cpp.
// ---------------------------------------------------------------------------

/// Tracks how many of the tumour's fixed growth-stage integrity breaches have
/// already fired, so a tumour sitting exactly on a stage boundary for many
/// ticks (or one that regresses -- it never does today, but the check is
/// written to be safe regardless) breaches an objective exactly once per
/// newly-crossed stage, not once per tick. See tumor_breach_system.
struct TumorBreachState {
    u32 stage_reached = 0;
};

/// Tracks the tissue-cost obstruction a lane-blocking boss currently has
/// carved into the world, so boss_obstruction_system can restore the exact
/// prior costs before re-carving at a new position (mirrors
/// game/towers/TowerSystem.cpp's TowerRecord snapshot/restore pattern for
/// tower footprints -- same idiom, applied to a moving named agent instead
/// of a static placement).
struct BossObstructionState {
    bool applied = false;
    Vec2 applied_at{0.0f, 0.0f};
    IVec2 cell_min{0, 0};
    IVec2 cell_dims{0, 0};
    std::vector<f32> saved_cost;
};

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

/// Abscess Hulk: the organ-chamber finale boss (DESIGN.md §6.4's "at least two
/// simultaneous pressures" framework, deliberately separate from tumor_mass --
/// DESIGN.md §6.2 frames the cancer tumour's objective breach as "independent
/// of the lane horde", i.e. explicitly NOT the lane-obstruction archetype
/// §6.4 asks for). This archetype supplies:
///
///   Pressure 1, lane-obstructing presence: boss_obstruction_system (below)
///     keeps a circle of raised TissueMask traversal cost centred on the
///     hulk as it slowly advances, restoring the previous circle first. The
///     shared flow field (the SAME one chaff already samples, DESIGN.md
///     §12.3) routes around/piles up against the expensive region on its own
///     -- no chaff-side change needed, which is why this is safe to add from
///     game/enemies without touching sim/chaff.
///   Pressure 2, periodic AoE punishing over-concentration: the telegraphed
///     attack below already damages any comp::Objective in blast radius
///     through the existing generic named-agent attack path
///     (sim/ecs/NamedAgents.cpp's system_named_combat, which every elite's
///     attack already goes through); boss_pulse_system (below) adds the
///     "punishes *concentration*" teeth by scaling that hit with how many
///     towers the player stacked inside the blast.
///   Pressure 3, ambient horde: intentionally NOT this archetype's job --
///     DESIGN.md §6.4's third bullet wants the boss fought "not in isolation
///     from the horde-management game the rest of the level trained", which
///     is a level/wave-table concern (5C's assets/levels content), not
///     something a single archetype can supply on its own.
sim::ArchetypeBehavior make_abscess_hulk_behavior(const EliteDef& e) {
    sim::ArchetypeBehavior b = base_behavior_from_elite(e);
    b.attack.radius = 14.0f;   // big enough to catch a concentrated cluster
    b.attack.damage = 25.0f;   // base objective hit; boss_pulse_system adds more per stacked tower
    b.attack.range = 0.0f;     // unused: the pulse doesn't lock onto a single target

    b.add(sim::AiState::Spawning, sim::AiState::Advancing, sim::AiTrigger::StateTimerAtLeast, 0.6f);
    b.add(sim::AiState::Advancing, sim::AiState::Dying, sim::AiTrigger::IsDead);
    b.add(sim::AiState::Advancing, sim::AiState::Telegraphing, sim::AiTrigger::AbilityReady);
    b.add(sim::AiState::Telegraphing, sim::AiState::Dying, sim::AiTrigger::IsDead);
    b.add(sim::AiState::Telegraphing, sim::AiState::Attacking, sim::AiTrigger::TelegraphComplete);
    b.add(sim::AiState::Attacking, sim::AiState::Dying, sim::AiTrigger::IsDead);
    b.add(sim::AiState::Attacking, sim::AiState::Advancing, sim::AiTrigger::StateTimerAtLeast, b.attack.active);
    return b;
}

/// Spore Colossus: a FungalSpore elite for mucosal (floodplain) finales.
/// DESIGN.md §6.3's elite framework asks for the gap to be named before the
/// elite is built: chaff-tier fungal spores already "leave a lingering
/// hazard zone on death" (§6.2), but at chaff density that's easy to ignore
/// -- a handful of hazard patches in a wide floodplain lane barely register.
/// This elite asks "can your AoE/field damage keep pace with a single big
/// spore repeatedly reseeding a wide lane with hazard clouds, or do they
/// overlap faster than you can clear them" -- a pure single-target/Precision
/// answer to it (kill it fast) still works, but a slow single-target kill
/// lets multiple bursts stack, which is the intended lesson for a region
/// whose design role is explicitly "AoE/field towers" (DESIGN.md §4.4).
/// spore_burst_system (below) is the named-agent amplification of
/// fungal_hazard_system's chaff-tier approximation of the same mechanic.
sim::ArchetypeBehavior make_spore_colossus_behavior(const EliteDef& e) {
    sim::ArchetypeBehavior b = base_behavior_from_elite(e);
    b.attack.radius = 6.0f;
    b.attack.damage = 6.0f;
    b.attack.range = 0.0f;

    b.add(sim::AiState::Spawning, sim::AiState::Advancing, sim::AiTrigger::StateTimerAtLeast, 0.4f);
    b.add(sim::AiState::Advancing, sim::AiState::Dying, sim::AiTrigger::IsDead);
    b.add(sim::AiState::Advancing, sim::AiState::Telegraphing, sim::AiTrigger::AbilityReady);
    b.add(sim::AiState::Telegraphing, sim::AiState::Dying, sim::AiTrigger::IsDead);
    b.add(sim::AiState::Telegraphing, sim::AiState::Attacking, sim::AiTrigger::TelegraphComplete);
    b.add(sim::AiState::Attacking, sim::AiState::Dying, sim::AiTrigger::IsDead);
    b.add(sim::AiState::Attacking, sim::AiState::Advancing, sim::AiTrigger::StateTimerAtLeast, b.attack.active);
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

/// How many fixed-size integrity breaches a tumour can inflict over its full
/// growth from 0 to max_radius (DESIGN.md §6.2: "breaches the objective
/// directly at a fixed integrity cost per growth stage").
constexpr u32 kTumorBreachStages = 4;
constexpr f32 kTumorBreachDamagePerStage = 15.0f;
/// Extra reach beyond the tumour's own (still-growing) radius that still
/// counts as "the objective it's embedded in" -- a tumour planted right next
/// to, but not literally overlapping, the objective structure should still
/// threaten it once it's grown enough to matter.
constexpr f32 kTumorBreachReachPad = 4.0f;

/// PostUpdate, after tumor_growth: the tumour's actual objective-breach
/// consequence (deliverable #3). Deliberately independent of any lane horde
/// state or damage-field mechanics -- DESIGN.md §6.2 is explicit that this
/// threat "punishes ignoring a spot", not "losing a lane", so the only inputs
/// are the tumour's own grown radius and nearby comp::Objective entities.
/// Fires exactly once per newly-crossed growth stage (monotonic
/// TumorBreachState::stage_reached), never continuously per tick, matching
/// "a fixed integrity cost per growth stage" rather than a DPS drain.
void tumor_breach_system(sim::SystemContext& ctx) {
    auto tumors = ctx.registry.view<sim::comp::TumorMass, TumorBreachState, sim::comp::Transform>();
    if (tumors.begin() == tumors.end()) return;   // common case: no tumour alive, skip the objective scan entirely

    auto objectives = ctx.registry.view<sim::comp::Objective, sim::comp::Transform>();
    for (auto e : tumors) {
        const sim::comp::TumorMass& tumor = tumors.get<sim::comp::TumorMass>(e);
        TumorBreachState& breach = tumors.get<TumorBreachState>(e);
        const sim::comp::Transform& xf = tumors.get<sim::comp::Transform>(e);

        const f32 frac = tumor.max_radius > math::kEpsilon
                            ? math::saturate(tumor.radius / tumor.max_radius) : 0.0f;
        // 1e-4 guards against a stage boundary landing exactly on a float
        // that rounds down by an epsilon (e.g. frac == 0.25 - 1ulp).
        const u32 stage = static_cast<u32>(frac * static_cast<f32>(kTumorBreachStages) + 1e-4f);
        if (stage <= breach.stage_reached) continue;

        const u32 newly_crossed = stage - breach.stage_reached;
        breach.stage_reached = stage;
        const f32 damage = kTumorBreachDamagePerStage * static_cast<f32>(newly_crossed);

        for (auto obj_e : objectives) {
            sim::comp::Objective& obj = objectives.get<sim::comp::Objective>(obj_e);
            const sim::comp::Transform& obj_t = objectives.get<sim::comp::Transform>(obj_e);
            if (math::length(obj_t.position - xf.position) > tumor.radius + obj.radius + kTumorBreachReachPad) continue;
            obj.integrity = math::max(0.0f, obj.integrity - damage);
        }
    }
}

// ---------------------------------------------------------------------------
// Abscess Hulk boss: lane-obstructing presence (tissue-cost carving).
// ---------------------------------------------------------------------------

constexpr f32 kBossObstructionRadius = 6.0f;
/// Multiplies whatever cost was already there (vessel width, an existing NET,
/// etc.) rather than overwriting it, so this composes with other cost sources
/// instead of silently erasing them. "Expensive", not impassable: the flow
/// field routes around high-cost cells (DESIGN.md §12.3) without ever
/// reporting them unreachable, so the hulk itself never traps its own path.
constexpr f32 kBossObstructionCostMul = 5.0f;

/// Restores every cell this boss had raised the cost of, then marks that
/// region dirty so the incremental rebake (already pumped every tick by
/// SimWorld::tick(), step 6) picks the reversion up on its own.
void clear_boss_obstruction(sim::TissueMask& mask, sim::FlowField& flow, BossObstructionState& st) {
    if (!st.applied) return;
    for (i32 y = 0; y < st.cell_dims.y; ++y) {
        for (i32 x = 0; x < st.cell_dims.x; ++x) {
            const i32 wx = st.cell_min.x + x;
            const i32 wy = st.cell_min.y + y;
            if (!mask.in_range(wx, wy)) continue;
            const usize idx = static_cast<usize>(y) * static_cast<usize>(st.cell_dims.x) + static_cast<usize>(x);
            mask.set_cost(wx, wy, st.saved_cost[idx]);
        }
    }
    const Vec2 lo = mask.cell_to_world(st.cell_min.x, st.cell_min.y);
    const Vec2 hi = mask.cell_to_world(st.cell_min.x + st.cell_dims.x, st.cell_min.y + st.cell_dims.y);
    flow.mark_dirty(Rect{lo, hi});
    st.applied = false;
}

/// Snapshots the current cost of every cell in a `radius`-circle around `pos`,
/// then multiplies it by `cost_mul` for cells actually inside the circle
/// (the bounding box is quantized to cells; only the exact circle test gets
/// the multiplier, matching TissueRaster.h's own disc-stamp convention).
void apply_boss_obstruction(sim::TissueMask& mask, sim::FlowField& flow, BossObstructionState& st,
                            Vec2 pos, f32 radius, f32 cost_mul) {
    const IVec2 c0 = mask.world_to_cell(pos - Vec2{radius, radius});
    const IVec2 c1 = mask.world_to_cell(pos + Vec2{radius, radius});
    const i32 max_x = math::max(mask.width() - 1, 0);
    const i32 max_y = math::max(mask.height() - 1, 0);
    const i32 x0 = math::clamp(math::min(c0.x, c1.x), 0, max_x);
    const i32 x1 = math::clamp(math::max(c0.x, c1.x), 0, max_x);
    const i32 y0 = math::clamp(math::min(c0.y, c1.y), 0, max_y);
    const i32 y1 = math::clamp(math::max(c0.y, c1.y), 0, max_y);
    if (x1 < x0 || y1 < y0) return;

    st.cell_min = IVec2{x0, y0};
    st.cell_dims = IVec2{x1 - x0 + 1, y1 - y0 + 1};
    const usize n = static_cast<usize>(st.cell_dims.x) * static_cast<usize>(st.cell_dims.y);
    st.saved_cost.assign(n, 1.0f);   // steady-state: same dims every call, so this never reallocates after warmup

    const f32 r2 = radius * radius;
    for (i32 y = y0; y <= y1; ++y) {
        for (i32 x = x0; x <= x1; ++x) {
            const usize idx = static_cast<usize>(y - y0) * static_cast<usize>(st.cell_dims.x) + static_cast<usize>(x - x0);
            st.saved_cost[idx] = mask.cost(x, y);
            if (math::length_sq(mask.cell_to_world(x, y) - pos) <= r2) {
                mask.set_cost(x, y, mask.cost(x, y) * cost_mul);
            }
        }
    }
    flow.mark_dirty(Rect{mask.cell_to_world(x0, y0), mask.cell_to_world(x1 + 1, y1 + 1)});
    st.applied = true;
    st.applied_at = pos;
}

/// PostUpdate: keeps the obstruction circle centred on every
/// BossObstructionState-carrying boss (currently only abscess_hulk). Skips
/// entirely when no level's tissue mask is loaded (bare SimWorld in unit
/// tests/benches), mirroring TowerSystem.cpp's own
/// `mask.width() <= 0` guard. Re-carves only after the boss has moved at
/// least half a cell, so a slow-moving boss doesn't force a rebake every
/// single tick.
void boss_obstruction_system(sim::SystemContext& ctx) {
    sim::TissueMask& mask = ctx.world.tissue();
    if (mask.width() <= 0 || mask.height() <= 0) return;
    sim::FlowField& flow = ctx.world.flow();
    const f32 move_eps = math::max(0.25f, mask.cell_size() * 0.5f);

    auto view = ctx.registry.view<sim::comp::AiBrain, sim::comp::Transform, BossObstructionState>();
    for (auto e : view) {
        const sim::comp::AiBrain& brain = view.get<sim::comp::AiBrain>(e);
        const sim::comp::Transform& xf = view.get<sim::comp::Transform>(e);
        BossObstructionState& st = view.get<BossObstructionState>(e);

        if (brain.state == sim::AiState::Dying || brain.state == sim::AiState::Spawning) {
            clear_boss_obstruction(mask, flow, st);
            continue;
        }
        if (st.applied && math::length_sq(xf.position - st.applied_at) < move_eps * move_eps) continue;
        clear_boss_obstruction(mask, flow, st);
        apply_boss_obstruction(mask, flow, st, xf.position, kBossObstructionRadius, kBossObstructionCostMul);
    }
}

// ---------------------------------------------------------------------------
// Abscess Hulk boss: periodic AoE that punishes over-concentration.
// ---------------------------------------------------------------------------

/// Extra objective damage per tower caught in the blast, on top of the
/// archetype's own base Attack::damage (already applied generically by
/// sim/ecs/NamedAgents.cpp's system_named_combat for every named agent's
/// attack). This is what makes the pulse specifically punish *concentration*
/// rather than just being a bigger flat hit: a lightly-defended blast radius
/// takes only the base hit, a heavily-stacked one takes much more.
constexpr f32 kBossPulseDamagePerTower = 6.0f;

/// Combat phase, after tower firing and the biofilm pulse: inspects this
/// tick's AttackEvents for ones sourced from the abscess_hulk archetype and
/// adds the per-tower-in-blast scaling. Same idiom as biofilm_pulse_system
/// above -- react to the generic AttackEvent stream, keyed by archetype id.
void boss_pulse_system(sim::SystemContext& ctx) {
    const RosterState* state = ctx.registry.ctx().find<RosterState>();
    if (state == nullptr) return;
    const sim::named::NamedFrame* fr = sim::named::frame_if_any(ctx.registry);
    if (fr == nullptr || fr->attacks.empty()) return;

    auto towers = ctx.registry.view<sim::comp::Tower, sim::comp::Transform>();
    auto objectives = ctx.registry.view<sim::comp::Objective, sim::comp::Transform>();

    for (const auto& ev : fr->attacks) {
        const entt::entity source = ctx.world.ecs().from_id(ev.source);
        if (!ctx.registry.valid(source)) continue;
        const auto* named = ctx.registry.try_get<sim::comp::NamedAgent>(source);
        if (named == nullptr || named->archetype != state->abscess_hulk_archetype) continue;

        u32 towers_in_blast = 0;
        for (auto t : towers) {
            if (math::length(towers.get<sim::comp::Transform>(t).position - ev.point) <= ev.radius) {
                ++towers_in_blast;
            }
        }
        if (towers_in_blast == 0) continue;

        const f32 bonus = kBossPulseDamagePerTower * static_cast<f32>(towers_in_blast);
        for (auto obj_e : objectives) {
            sim::comp::Objective& obj = objectives.get<sim::comp::Objective>(obj_e);
            const sim::comp::Transform& obj_t = objectives.get<sim::comp::Transform>(obj_e);
            if (math::length(obj_t.position - ev.point) > ev.radius + obj.radius) continue;
            obj.integrity = math::max(0.0f, obj.integrity - bonus);
        }
    }
}

// ---------------------------------------------------------------------------
// Fungal hazard-on-death (deliverable #1) and its Spore Colossus amplification.
// ---------------------------------------------------------------------------

constexpr u8 kFungalSporeBit = static_cast<u8>(1u << static_cast<u8>(PathogenFamily::FungalSpore));

/// Submits a residual toxic-cloud DamageField -- the "lingering hazard zone"
/// DESIGN.md §6.2 describes for a fungal spore death. Shared by
/// fungal_hazard_system (chaff-tier approximation) and spore_burst_system
/// (the Spore Colossus elite's amplification of the same mechanic).
/// `family_mask` deliberately excludes FungalSpore itself: the hazard is "a
/// residual patch other chaff also suffers passing through" per §6.2, not a
/// second helping of damage to spores already dying in the field that seeded
/// it -- and, just as importantly, this is what keeps fungal_hazard_system
/// from ever re-triggering on its own output (see that system's comment).
void submit_toxic_hazard(sim::SimWorld& world, Vec2 origin, f32 radius, f32 kill_rate, f32 duration) {
    sim::DamageField hazard;
    hazard.shape = sim::FieldShape::Circle;
    hazard.origin = origin;
    hazard.radius = radius;
    hazard.kill_rate = kill_rate;
    hazard.falloff = 1.0f;
    hazard.family_mask = static_cast<u8>(0xFFu & ~kFungalSporeBit);
    hazard.marked_multiplier = 1.0f;
    hazard.lifetime = duration;
    hazard.owner = EntityId{};      // environmental hazard, not tower-attributed
    hazard.friendly_fire = false;
    world.damage().submit(hazard);
}

Vec2 field_footprint_center(const sim::DamageField& f) {
    return f.shape == sim::FieldShape::Rect ? f.rect.center() : f.origin;
}

constexpr f32 kFungalHazardRadius = 4.0f;
constexpr f32 kFungalHazardKillRate = 2.5f;
constexpr f32 kFungalHazardDuration = 3.0f;
/// Throttle, not randomness: DESIGN.md's own suggested pragmatic
/// implementation ("N% of the time...") is one valid reading, but a
/// deterministic cooldown gate reaches the same goal (bounded hazard-field
/// spawn rate regardless of how many eroding fields overlap fungal mass at
/// once) without needing an Rng draw at all, which is the simpler way to
/// satisfy docs/CONVENTIONS.md §4's determinism rules -- nothing here is
/// random, so there is nothing that could desync.
constexpr f32 kFungalHazardCooldown = 1.5f;

/// PostUpdate: the chaff-tier approximation of "fungal spore leaves a hazard
/// on death" (deliverable #1). See this wave's report for why an exact
/// per-death hook does not exist today (ChaffBuffers::apply_density_loss has
/// no death callback, and DamageStats carries no per-kill position). The
/// approximation implemented here: once per cooldown window, scan this
/// tick's already-submitted (Combat-phase) damage fields for one that (a) is
/// a real, non-friendly-fire, family-matching-FungalSpore erosion field and
/// (b) currently overlaps *live* FungalSpore mass (checked via
/// DamageSystem::measure_density with the mask narrowed to FungalSpore only,
/// a read-only query, not a guess) -- i.e. fungal spores are about to take
/// damage there this tick, which is the closest observable proxy this module
/// has to "fungal spores are dying here" without a new sim-layer hook.
void fungal_hazard_system(sim::SystemContext& ctx) {
    RosterState& state = roster_state(ctx.registry);
    if (state.fungal_hazard_cooldown > 0.0f) {
        state.fungal_hazard_cooldown = math::max(0.0f, state.fungal_hazard_cooldown - ctx.dt);
        return;
    }

    // Find a candidate first, without mutating DamageSystem's field list
    // mid-iteration: submit_toxic_hazard() below calls DamageSystem::submit(),
    // which push_back()s into the very vector fields() returns a reference to
    // -- doing that while a range-for still holds iterators into it would be
    // exactly the kind of iterator invalidation docs/CONVENTIONS.md's "no
    // allocation in the hot path" rule is protecting against, so the read and
    // the mutation are kept in two separate passes on purpose.
    bool found = false;
    Vec2 hazard_at{0.0f, 0.0f};
    for (const auto& f : ctx.world.damage().fields()) {
        if (f.friendly_fire || f.kill_rate <= 0.0f) continue;
        if ((f.family_mask & kFungalSporeBit) == 0) continue;

        sim::DamageField probe = f;
        probe.family_mask = kFungalSporeBit;
        const f32 fungal_mass_here =
            ctx.world.damage().measure_density(ctx.world.chaff(), ctx.world.spatial(), probe);
        if (fungal_mass_here <= 0.0f) continue;

        hazard_at = field_footprint_center(f);
        found = true;
        break;   // one hazard per tick is plenty; the cooldown bounds the rest
    }
    if (!found) return;

    submit_toxic_hazard(ctx.world, hazard_at, kFungalHazardRadius, kFungalHazardKillRate, kFungalHazardDuration);
    state.fungal_hazard_cooldown = kFungalHazardCooldown;
}

constexpr f32 kSporeBurstRadius = 5.0f;
constexpr f32 kSporeBurstKillRate = 3.0f;
constexpr f32 kSporeBurstDuration = 4.0f;

/// Combat phase: the Spore Colossus's own telegraphed attack already damages
/// nearby objectives generically (system_named_combat); this adds the
/// "leaves a hazard cloud where it bursts" flavour on top, at the point its
/// attack landed. Same archetype-id-keyed AttackEvent idiom as
/// biofilm_pulse_system/boss_pulse_system.
void spore_burst_system(sim::SystemContext& ctx) {
    const RosterState* state = ctx.registry.ctx().find<RosterState>();
    if (state == nullptr) return;
    const sim::named::NamedFrame* fr = sim::named::frame_if_any(ctx.registry);
    if (fr == nullptr || fr->attacks.empty()) return;

    for (const auto& ev : fr->attacks) {
        const entt::entity source = ctx.world.ecs().from_id(ev.source);
        if (!ctx.registry.valid(source)) continue;
        const auto* named = ctx.registry.try_get<sim::comp::NamedAgent>(source);
        if (named == nullptr || named->archetype != state->spore_colossus_archetype) continue;
        submit_toxic_hazard(ctx.world, ev.point, kSporeBurstRadius, kSporeBurstKillRate, kSporeBurstDuration);
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
    if (const EliteDef* d = roster.find_elite("abscess_hulk")) {
        state.abscess_hulk_archetype = table.add(make_abscess_hulk_behavior(*d));
    }
    if (const EliteDef* d = roster.find_elite("spore_colossus")) {
        state.spore_colossus_archetype = table.add(make_spore_colossus_behavior(*d));
    }

    world.ecs().add_system(sim::SystemPhase::PostUpdate, "tumor_growth", 10, &tumor_growth_system);
    world.ecs().add_system(sim::SystemPhase::PostUpdate, "tumor_breach", 11, &tumor_breach_system);
    world.ecs().add_system(sim::SystemPhase::PostUpdate, "boss_obstruction", 12, &boss_obstruction_system);
    world.ecs().add_system(sim::SystemPhase::PostUpdate, "fungal_hazard", 20, &fungal_hazard_system);
    world.ecs().add_system(sim::SystemPhase::Combat, "biofilm_colony_pulse", 10, &biofilm_pulse_system);
    world.ecs().add_system(sim::SystemPhase::Combat, "abscess_hulk_pulse", 11, &boss_pulse_system);
    world.ecs().add_system(sim::SystemPhase::Combat, "spore_colossus_burst", 12, &spore_burst_system);

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
        .speed = 7.65f, .sprite_size = 6.12f, .ability_cooldown = 5.0f,
        .telegraph_duration = 0.6f, .atp_bounty = 70});
    elites_.push_back(EliteDef{
        .id = 2, .name = "biofilm_colony", .family = PathogenFamily::Bacteria,
        .tier = ThreatTier::Elite, .max_health = 950.0f, .armor = 5.0f,
        .speed = 2.475f, .sprite_size = 7.56f, .ability_cooldown = 4.5f,
        .telegraph_duration = 0.5f, .atp_bounty = 100});
    elites_.push_back(EliteDef{
        .id = 3, .name = "tumor_mass", .family = PathogenFamily::CancerCell,
        .tier = ThreatTier::Boss, .max_health = 5000.0f, .armor = 8.0f,
        .speed = 0.0f, .sprite_size = 11.52f, .ability_cooldown = 30.0f,
        .telegraph_duration = 1.0f, .atp_bounty = 800});
    // Organ-chamber finale boss (DESIGN.md §6.4's two-simultaneous-pressures
    // framework). Biggest silhouette in the roster, per the readability rule
    // (colour = family, silhouette = threat tier): bigger than tumor_mass's
    // 3.2, which is itself already the largest elite before this one.
    elites_.push_back(EliteDef{
        .id = 4, .name = "abscess_hulk", .family = PathogenFamily::Bacteria,
        .tier = ThreatTier::Boss, .max_health = 15000.0f, .armor = 12.0f,
        .speed = 2.025f, .sprite_size = 13.68f, .ability_cooldown = 9.0f,
        .telegraph_duration = 1.3f, .atp_bounty = 1800});
    // Mucosal (floodplain) region elite: see make_spore_colossus_behavior's
    // doc comment for the defensive gap it tests.
    elites_.push_back(EliteDef{
        .id = 5, .name = "spore_colossus", .family = PathogenFamily::FungalSpore,
        .tier = ThreatTier::Elite, .max_health = 1400.0f, .armor = 3.0f,
        .speed = 3.6f, .sprite_size = 8.64f, .ability_cooldown = 5.5f,
        .telegraph_duration = 0.7f, .atp_bounty = 220});
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
    const EnemyConfig& cfg = enemy_config();
    for (u32 i = 0; i < kFamilyCount; ++i) {
        const FamilyDef& d = families_[i];
        const FamilyChaffParams& fc = cfg.families[i].chaff;
        sim::ChaffFamilyParams& p = tuning.family[i];
        const SpeedProfile sp = speed_profile(d.speed_tier);

        p.max_speed = sp.max_speed;
        p.acceleration = sp.acceleration;
        p.jitter = sp.jitter;
        p.base_density = d.base_density;

        // Physical footprint mirrors the renderer's silhouette so a chaff
        // agent's collision size can never disagree with what is drawn -- the
        // same "single source of truth" reasoning FamilyDef::color's comment
        // states for colour, applied to size instead. Both multipliers are
        // authorable now, so the relationship stays visible and adjustable
        // rather than buried in this function.
        const f32 silhouette = render::family_visual(d.family).silhouette;
        p.radius = silhouette * fc.radius_from_silhouette;
        p.separation_radius = p.radius * fc.separation_radius_mul;
        // Until a config is applied, the three values enemies.json now owns are
        // still DERIVED here, exactly as they always were. Without this a bare
        // EnemyRoster -- a unit test, and more importantly default_game_config()
        // generating the shipped files -- would read the empty seed and produce
        // a virus that does not replicate and a spore that does not drift.
        const bool from_config = enemy_config_applied();
        p.separation_strength = from_config
                                    ? fc.separation_strength
                                    : math::min(6.0f + d.base_density * 1.5f, 12.0f);

        // The fluid-feel block. Every one of these was unreachable from any
        // data path before the config existed: struct defaults nothing wrote.
        p.alignment_radius = fc.alignment_radius;
        p.alignment_strength = fc.alignment_strength;
        p.pressure_threshold = fc.pressure_threshold;
        p.pressure_gain = fc.pressure_gain;
        p.pressure_max = fc.pressure_max;
        p.wall_restitution = fc.wall_restitution;
        p.wall_splash = fc.wall_splash;
        p.contact_spacing = fc.contact_spacing;
        p.contact_stiffness = fc.contact_stiffness;

        p.drift_bias = from_config ? fc.drift_bias : (d.drifts ? 0.75f : 0.0f);
        p.replication_rate = from_config ? fc.replication_rate : (d.replicates ? 0.2f : 0.0f);
    }

    // NOTE: ambient_drift is deliberately NOT set here any more. This function
    // used to stamp a hardcoded Vec2{0.5, 0.28} over the whole tuning, which
    // silently overrode LevelDef::ambient_drift -- a field the level schema has
    // parsed and then discarded since it was written. sim.json now supplies the
    // default and the level file overrides it, both applied by the caller.
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
    else if (def->name == std::string_view("abscess_hulk")) archetype_id = state->abscess_hulk_archetype;
    else if (def->name == std::string_view("spore_colossus")) archetype_id = state->spore_colossus_archetype;
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
    // TumorMass/TumorBreachState/BossObstructionState are the extra
    // components the shared named-agent spawn path knows nothing about.
    const entt::entity raw = world.ecs().from_id(id);
    if (def->family == PathogenFamily::CancerCell) {
        sim::comp::TumorMass tumor;
        tumor.growth_rate = 0.12f;   // slow: DESIGN.md's "expands if ignored"
        tumor.radius = 1.0f;
        tumor.max_radius = 10.0f;
        registry.emplace<sim::comp::TumorMass>(raw, tumor);
        registry.emplace<TumorBreachState>(raw, TumorBreachState{});
    } else if (def->name == std::string_view("abscess_hulk")) {
        registry.emplace<BossObstructionState>(raw, BossObstructionState{});
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

// ---------------------------------------------------------------------------
// Config application (game/enemies/EnemyConfigApply.h)
// ---------------------------------------------------------------------------

namespace {

/// The live tuning apply_to_tuning() and speed_profile() read.
///
/// It starts out holding exactly the values this file used to hardcode, so a
/// roster that never sees enemies.json behaves as it always did -- and so
/// default_game_config() can read the shipped defaults back out of here
/// instead of restating them.
EnemyConfig& mutable_enemy_config() {
    static EnemyConfig cfg = [] {
        EnemyConfig seed;
        seed.speed_tiers[static_cast<u32>(SpeedTier::Slow)] =
            SpeedProfileParams{5.625f, 20.25f, 0.12f};
        seed.speed_tiers[static_cast<u32>(SpeedTier::Normal)] =
            SpeedProfileParams{10.125f, 36.0f, 0.30f};
        seed.speed_tiers[static_cast<u32>(SpeedTier::Fast)] =
            SpeedProfileParams{16.875f, 63.0f, 0.45f};
        seed.speed_tiers[static_cast<u32>(SpeedTier::Erratic)] =
            SpeedProfileParams{12.375f, 45.0f, 1.00f};
        return seed;
    }();
    return cfg;
}

bool g_enemy_config_applied = false;

} // namespace

const EnemyConfig& enemy_config() { return mutable_enemy_config(); }

bool enemy_config_applied() { return g_enemy_config_applied; }

const SpeedProfileParams& enemy_speed_profile(SpeedTier tier) {
    const u32 i = static_cast<u32>(tier);
    return mutable_enemy_config().speed_tiers[i < 4u ? i : 1u];
}

void apply_enemy_config(EnemyRoster& roster, const EnemyConfig& cfg) {
    mutable_enemy_config() = cfg;
    g_enemy_config_applied = true;

    // render/ cannot see game/, so the family look has to be pushed down.
    // Do this BEFORE load_defaults(), which reads render::family_color back
    // out into FamilyDef::color.
    for (u32 i = 0; i < kFamilyCount; ++i) {
        const auto family = static_cast<PathogenFamily>(i);
        const FamilyVisualParams& v = cfg.families[i].visual;
        render::set_family_visual(family, render::FamilyVisual{v.silhouette, v.tempo, v.wobble});
        render::set_family_color(family, v.color);
    }

    roster.load_defaults();

    for (u32 i = 0; i < kFamilyCount; ++i) {
        const FamilyConfig& fc = cfg.families[i];
        FamilyDef& d = roster.families_[i];
        d.speed_tier = fc.speed_tier;
        d.base_density = fc.behavior.base_density;
        d.replicates = fc.behavior.replicates;
        d.clumps = fc.behavior.clumps;
        d.drifts = fc.behavior.drifts;
        d.can_hide = fc.behavior.can_hide;
        d.leaves_hazard = fc.behavior.leaves_hazard;
    }

    // Elites are matched by id, not by position: reordering the JSON array
    // must not silently reassign one archetype's health to another. An id the
    // roster does not know is ignored rather than fatal -- the behaviour code
    // for it would not exist either.
    for (const EliteConfig& ec : cfg.elites) {
        for (EliteDef& def : roster.elites_) {
            if (def.id != ec.id) continue;
            def.max_health = ec.stats.max_health;
            def.armor = ec.stats.armor;
            def.speed = ec.stats.speed;
            def.sprite_size = ec.stats.sprite_size;
            def.ability_cooldown = ec.stats.ability_cooldown;
            def.telegraph_duration = ec.stats.telegraph_duration;
            def.atp_bounty = ec.stats.atp_bounty;
            break;
        }
    }
}

} // namespace immune::game
