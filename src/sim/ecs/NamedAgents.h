// sim/ecs/NamedAgents.h — the named-agent (elite/boss) tier. Owner: Wave 1D.
//
// RATIONALE (DESIGN.md §8.2)
// This is the *small half* of the sim. Chaff (~95%) lives in the SoA store and
// has no individual game logic; the ~5% named agents get real health, a real AI
// state machine, telegraphed attacks, and individual steering. Keeping the
// boundary sharp is the whole point of the tiering decision: nothing here may
// scale with chaff count, and no chaff agent may ever become an entity.
//
// WHAT THIS MODULE INSTALLS
// Five systems, one per phase, each with an explicit sort key:
//
//   PreUpdate/0   named_timers    order rebuild, timer + cooldown + telegraph advance
//   AI/0          named_ai        target selection, transition-table evaluation
//   Movement/0    named_movement  flow field + local steering + integration
//   Combat/0      named_combat    resolution of attacks whose active window opened
//   PostUpdate/0  named_cleanup   death resolution, ephemeral expiry, destruction
//
// Every one of them iterates `NamedFrame::ordered`, which is sorted by
// comp::SpawnOrder, never EnTT's storage order.
#pragma once

#include "core/Types.h"
#include "sim/ecs/AiStateMachine.h"
#include "sim/ecs/EcsWorld.h"

#include <entt/entity/registry.hpp>

#include <vector>

namespace immune::sim {

class SimWorld;

namespace named {

/// Emitted when a telegraphed attack's active window opens. Consumed by the
/// Combat system in the same tick; the buffer is scratch and never shrinks, so
/// the tick performs no allocation in steady state.
struct AttackEvent {
    EntityId source{};
    Vec2 point{0.0f, 0.0f};
    f32 radius = 0.0f;
    f32 damage = 0.0f;
    u8 kind = 0;
};

/// A telegraph currently visible in the world. Rebuilt each tick so the
/// movement system can dodge without touching the registry per candidate.
struct ActiveTelegraph {
    entt::entity owner = entt::null;
    Vec2 point{0.0f, 0.0f};
    f32 radius = 0.0f;
    f32 progress = 0.0f;   ///< 0 = just started, 1 = about to land
};

/// Per-tick scratch shared between the five systems. Lives in the registry
/// context; buffers are reused so the sim tick never allocates.
struct NamedFrame {
    std::vector<entt::entity> ordered;   ///< canonical iteration order
    std::vector<Vec2> positions;         ///< parallel to `ordered`
    std::vector<ActiveTelegraph> telegraphs;
    std::vector<AttackEvent> attacks;
    u32 next_seq = 1;                    ///< spawn sequence counter
    u32 transitions_this_tick = 0;       ///< introspection for tests
};

NamedFrame& frame(entt::registry& registry);
const NamedFrame* frame_if_any(const entt::registry& registry);

/// Registers the five named-agent systems on `world.ecs()`. Idempotent per
/// EcsWorld only in the sense that calling it twice registers them twice —
/// call once after SimWorld::init (which clears entities, not systems).
void install(SimWorld& world);

/// Spawn parameters for a named agent. Everything not set here comes from the
/// archetype table.
struct SpawnParams {
    u16 archetype = 0;
    Vec2 position{0.0f, 0.0f};
    f32 health_scale = 1.0f;
    f32 rotation = 0.0f;
};

/// Creates one named agent. Returns a null EntityId if the ECS is already at
/// the §8.6 cap of 200 named agents.
EntityId spawn(SimWorld& world, const SpawnParams& params);

/// The §8.6 named-agent budget. Spawning past this is refused, not merely
/// discouraged: the 2 ms budget is defined at <= 200.
inline constexpr usize kMaxNamedAgents = 200;

// ---------------------------------------------------------------------------
// Placeholder elite (deliverable #6)
// ---------------------------------------------------------------------------
// Wave 2C owns the real roster. This one exists to prove the layer end to end:
// it walks the flow field, has real health, cycles
// Spawning -> Advancing -> Telegraphing -> Attacking -> Advancing, drops into
// Burrowed when wounded, and dies through Dying.

/// Registers the placeholder elite archetype if absent and returns its id.
u16 placeholder_elite(entt::registry& registry);

/// Populates a world with `count` placeholder elites spread over the world
/// bounds. This is the spawner the `named200` bench scenario needs; it is
/// deterministic given the world's seed. Returns the number actually spawned.
u32 spawn_bench_population(SimWorld& world, u32 count);

/// Convenience for benches/tests: install() + placeholder_elite() +
/// spawn_bench_population() in one deterministic call.
u32 setup_bench_scenario(SimWorld& world, u32 count);

/// FNV-1a over the named-agent state, in canonical spawn order. SimWorld's
/// state_hash() covers only the chaff streams, so determinism tests for this
/// tier hash here instead.
u64 state_hash(const entt::registry& registry);

} // namespace named
} // namespace immune::sim
