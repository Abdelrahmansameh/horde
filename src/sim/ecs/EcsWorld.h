// sim/ecs/EcsWorld.h — EnTT registry wrapper + system scheduler. FROZEN CONTRACT.
// Owner: Wave 1D.
//
// RATIONALE
// EnTT is exposed behind a thin wrapper for exactly one reason: *explicit,
// stable system ordering*. A deterministic sim needs every system to run in the
// same order every tick on every machine, which means no implicit registration
// order and no unordered iteration of a system set. Systems declare a
// SystemPhase and a sort key; the scheduler runs them in that order, always.
//
// The raw `entt::registry` is still reachable via `registry()` — this is a
// convenience layer, not an abstraction wall.
#pragma once

#include "core/Types.h"

#include <entt/entity/registry.hpp>

#include <functional>
#include <string>
#include <vector>

namespace immune { class Rng; }

namespace immune::sim {

class SimWorld;

/// Fixed intra-tick phases. Ordering across phases is guaranteed; ordering
/// within a phase is by the system's sort key, then registration order.
enum class SystemPhase : u8 {
    PreUpdate = 0,   ///< Timers, cooldown decay, telegraph advance.
    AI = 1,          ///< State machine transitions, target selection.
    Movement = 2,    ///< Flow sampling + steering for named agents.
    Combat = 3,      ///< Tower firing, damage-field submission.
    PostUpdate = 4,  ///< Death resolution, spawning, cleanup.
    Count = 5,
};

/// Systems receive the whole SimWorld so they can reach chaff, flow field, and
/// the damage system, plus the tick's dt and the sim RNG.
struct SystemContext {
    SimWorld& world;
    entt::registry& registry;
    Rng& rng;
    f32 dt;
    Tick tick;
};

using SystemFn = std::function<void(SystemContext&)>;

class EcsWorld {
public:
    entt::registry& registry() { return registry_; }
    const entt::registry& registry() const { return registry_; }

    /// Registers a system. `name` appears in profiling output; `sort_key`
    /// orders systems inside a phase (lower runs first).
    void add_system(SystemPhase phase, std::string name, i32 sort_key, SystemFn fn);

    /// Runs every registered system in phase then sort-key order.
    void tick(SystemContext& ctx);

    /// Removes all systems (level teardown).
    void clear_systems();

    /// Destroys every entity but keeps registered systems.
    void clear_entities();

    /// Converts between the opaque EntityId used across module boundaries and
    /// EnTT's handle type, so no other module needs to include EnTT.
    EntityId to_id(entt::entity e) const;
    entt::entity from_id(EntityId id) const;

    usize entity_count() const;
    /// Named agents only (entities carrying comp::NamedAgent) — the §8.6 budget
    /// of <= 200 is measured against this.
    usize named_agent_count() const;

private:
    struct SystemEntry {
        SystemPhase phase;
        i32 sort_key;
        u32 registration_index;
        std::string name;
        SystemFn fn;
    };

    entt::registry registry_;
    std::vector<SystemEntry> systems_;
    u32 next_registration_ = 0;
    bool sorted_ = true;
};

} // namespace immune::sim
