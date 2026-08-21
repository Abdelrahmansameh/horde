// sim/ecs/AiStateMachine.h — data-driven behaviour tables for named agents.
// Owner: Wave 1D. Additive to Components.h (which stays frozen and untouched).
//
// RATIONALE (DESIGN.md §8.2, §6)
// Named agents are the ~5% tier that can afford real per-entity logic. That
// logic still must not become a class hierarchy: no virtuals, no per-entity heap
// allocation, no dynamic dispatch in the tick. So behaviour is *data*: an
// archetype owns a fixed-size transition table plus tuning, and the AI system is
// one loop that evaluates that table.
//
// Wave 2C extends this by REGISTERING archetypes — it never edits this file.
// Exotic conditions that the
// AiTrigger enum cannot express hook in as plain function pointers
// (AiTrigger::Custom / on_enter / on_exit / local_steer), which keeps the hot
// loop free of indirect calls except where an archetype explicitly asks for one.
//
// DETERMINISM
// Transitions are evaluated in table order, first match wins. The table order is
// authored, not sorted by anything runtime-dependent, so behaviour never depends
// on registration timing or on EnTT storage order.
#pragma once

#include "core/Types.h"
#include "sim/ecs/Components.h"

#include <entt/entity/registry.hpp>

namespace immune { class Rng; }

namespace immune::sim {

class SimWorld;

/// comp::AiState (Components.h) is used constantly outside the comp::
/// namespace by this module; alias it in so callers can write the bare name.
using AiState = comp::AiState;

// ---------------------------------------------------------------------------
// Additional components (Wave 1D owned; kept out of the frozen Components.h)
// ---------------------------------------------------------------------------
namespace comp {

/// Monotonic spawn sequence. This is the *canonical iteration order* for every
/// named-agent system: EnTT's storage order changes when entities are destroyed
/// (swap-and-pop), so nothing observable may depend on it.
struct SpawnOrder {
    u32 seq = 0;
};

/// Per-entity deterministic RNG seed. Derived once at spawn from the sim RNG and
/// never re-read from shared state, so a system may draw random numbers without
/// advancing (and therefore without ordering-coupling) the world generator.
struct NamedSeed {
    u64 seed = 0;
};

/// The telegraphed-attack primitive: wind-up -> active -> recovery.
/// `windup` is the readable telegraph window (DESIGN.md §2 readability pillar);
/// `active` is the frame band where the hit actually lands; `recovery` is folded
/// into AiBrain::next_ability_cd when the agent leaves Attacking.
struct Attack {
    f32 windup = 0.8f;     ///< seconds of readable tell before the hit
    f32 active = 0.2f;     ///< seconds the hit is live
    f32 recovery = 0.4f;   ///< seconds of vulnerability afterwards
    f32 range = 10.0f;     ///< how close a target must be to commit
    f32 radius = 3.0f;     ///< blast radius at the telegraphed point
    f32 damage = 12.0f;
    u8 kind = 0;           ///< renderer picks the telegraph VFX by this
};

/// Steering weights for the local layer that sits on top of the shared flow
/// field (DESIGN.md §8.3: "named agents can layer additional local steering").
struct Steering {
    f32 flow_weight = 1.0f;
    f32 separation_weight = 1.4f;
    f32 separation_radius = 2.0f;
    f32 dodge_weight = 3.0f;      ///< avoidance of other agents' telegraph zones
    f32 wall_weight = 1.0f;       ///< vessel-hugging via the clearance field
    f32 wall_clearance = 1.5f;    ///< below this clearance, push toward open tissue
    f32 jitter = 0.15f;           ///< deterministic per-entity wander
    f32 accel = 14.0f;            ///< world units / s^2
};

} // namespace comp

// ---------------------------------------------------------------------------
// Transition conditions
// ---------------------------------------------------------------------------

/// What makes a transition fire. Anything expressible here costs a switch, not
/// an indirect call.
enum class AiTrigger : u8 {
    Never = 0,
    Always,               ///< unconditional (use as a table terminator)
    StateTimerAtLeast,    ///< param = seconds in the current state
    HealthBelowFrac,      ///< param = fraction of max health (0..1)
    HealthAboveFrac,      ///< param = fraction of max health (0..1)
    AbilityReady,         ///< AiBrain::next_ability_cd <= 0
    AbilityNotReady,
    TelegraphComplete,    ///< Telegraph::elapsed >= Telegraph::duration
    HasTargetInRange,     ///< param = range override; <= 0 uses Attack::range
    NoTargetInRange,
    IsDead,               ///< Health::dead()
    Custom,               ///< AiTransition::custom decides
};

/// Everything a trigger or hook may read. Read-only by contract: a predicate
/// that mutates the world would make evaluation order observable.
struct AiEval {
    const entt::registry& registry;
    entt::entity self;
    const comp::AiBrain& brain;
    const comp::Transform& transform;
    const comp::Health& health;
    const comp::Attack& attack;
    Vec2 target_position{0.0f, 0.0f};
    f32 target_distance = 0.0f;   ///< infinity when there is no target
    bool has_target = false;
    f32 dt = 0.0f;
    Tick tick = 0;
};

using AiPredicate = bool (*)(const AiEval&);
using AiStateHook = void (*)(entt::registry&, entt::entity, AiState from, AiState to);

struct AiTransition {
    AiState from = AiState::Advancing;
    AiState to = AiState::Advancing;
    AiTrigger trigger = AiTrigger::Never;
    f32 param = 0.0f;
    AiPredicate custom = nullptr;   ///< only consulted for AiTrigger::Custom
};

/// Context handed to an archetype's optional local-steering hook.
struct AiSteerContext {
    const SimWorld& world;
    const entt::registry& registry;
    entt::entity self;
    Vec2 position{0.0f, 0.0f};
    Vec2 velocity{0.0f, 0.0f};
    AiState state = AiState::Advancing;
    f32 dt = 0.0f;
    Rng& rng;
};

using AiSteerHook = void (*)(const AiSteerContext&, Vec2& accum);

// ---------------------------------------------------------------------------
// Archetype behaviour table
// ---------------------------------------------------------------------------

/// One named-agent species, expressed entirely as data. Trivially copyable and
/// fixed size: no allocation, no ownership, safe to hold by value in a table.
struct ArchetypeBehavior {
    static constexpr u32 kMaxTransitions = 24;

    const char* name = "unnamed";
    PathogenFamily family = PathogenFamily::Virus;
    u8 tier = 1;                  ///< 1 = elite, 2 = boss

    f32 max_health = 300.0f;
    f32 armor = 0.0f;
    f32 max_speed = 3.0f;
    f32 ability_cooldown = 3.0f;  ///< seconds between telegraphed attacks
    f32 death_fade = 0.5f;        ///< seconds spent in Dying before removal

    /// Movement speed multiplier per state. Indexed by AiState; this is how
    /// "commits to the wind-up" and "burrowed agents do not move" are expressed
    /// without branching per archetype.
    f32 speed_scale[7] = {0.0f, 1.0f, 0.15f, 0.0f, 0.0f, 1.4f, 0.0f};

    comp::Attack attack{};
    comp::Steering steering{};

    f32 sprite_size = 1.6f;
    Vec4 tint{1.0f, 1.0f, 1.0f, 1.0f};
    u16 atlas_index = 0;

    AiTransition transitions[kMaxTransitions]{};
    u8 transition_count = 0;

    // Optional hooks. Null by default; an archetype that needs behaviour the
    // AiTrigger enum cannot express fills these in.
    AiStateHook on_enter = nullptr;
    AiStateHook on_exit = nullptr;
    AiSteerHook local_steer = nullptr;

    /// Appends a transition. Returns false (and drops it) when the table is
    /// full rather than allocating — a full table is an authoring bug.
    bool add(AiState from, AiState to, AiTrigger trigger, f32 param = 0.0f,
             AiPredicate custom = nullptr) {
        if (transition_count >= kMaxTransitions) return false;
        transitions[transition_count++] = AiTransition{from, to, trigger, param, custom};
        return true;
    }
};

/// Fixed-capacity archetype table, stored in the registry context so it has the
/// same lifetime as the world and needs no global state.
struct ArchetypeRegistry {
    static constexpr u32 kMaxArchetypes = 64;
    ArchetypeBehavior entries[kMaxArchetypes]{};
    u16 count = 0;

    /// Registers `b` and returns its archetype id (what comp::NamedAgent stores).
    /// Returns 0 if the table is full.
    u16 add(const ArchetypeBehavior& b) {
        if (count >= kMaxArchetypes) return 0;
        entries[count] = b;
        return count++;
    }

    const ArchetypeBehavior& get(u16 id) const {
        return entries[id < count ? id : 0];
    }
};

/// Fetches (creating on first use) the archetype table for a registry.
ArchetypeRegistry& archetypes(entt::registry& registry);
const ArchetypeRegistry* archetypes_if_any(const entt::registry& registry);

/// Evaluates one archetype's transition table for one entity.
/// Returns true and writes `out_next` when a transition fires.
bool evaluate_transitions(const ArchetypeBehavior& behavior, const AiEval& eval,
                          AiState& out_next);

} // namespace immune::sim
