// Wave 0: the scheduler is real (ordering is a determinism guarantee).
// The systems themselves are owned by Waves 1D / 2B / 2C.
#include "sim/ecs/EcsWorld.h"

#include "sim/ecs/Components.h"

#include <algorithm>

namespace immune::sim {

void EcsWorld::add_system(SystemPhase phase, std::string name, i32 sort_key, SystemFn fn) {
    systems_.push_back(SystemEntry{phase, sort_key, next_registration_++, std::move(name), std::move(fn)});
    sorted_ = false;
}

void EcsWorld::tick(SystemContext& ctx) {
    if (!sorted_) {
        std::sort(systems_.begin(), systems_.end(), [](const SystemEntry& a, const SystemEntry& b) {
            if (a.phase != b.phase) return a.phase < b.phase;
            if (a.sort_key != b.sort_key) return a.sort_key < b.sort_key;
            return a.registration_index < b.registration_index;
        });
        sorted_ = true;
    }
    for (const auto& s : systems_) {
        if (s.fn) s.fn(ctx);
    }
}

void EcsWorld::clear_systems() {
    systems_.clear();
    next_registration_ = 0;
    sorted_ = true;
}

void EcsWorld::clear_entities() { registry_.clear(); }

void EcsWorld::reset() {
    // Assignment rather than clear(): entt::registry::clear() destroys entities
    // and components but leaves the context variables alone, and those are
    // exactly where the per-world "already installed" guards live.
    registry_ = entt::registry{};
    clear_systems();
}

EntityId EcsWorld::to_id(entt::entity e) const {
    if (e == entt::null) return EntityId{};
    return EntityId{static_cast<u32>(entt::to_integral(e)) + 1u};
}

entt::entity EcsWorld::from_id(EntityId id) const {
    if (!id.valid()) return entt::null;
    return static_cast<entt::entity>(id.value - 1u);
}

usize EcsWorld::entity_count() const {
    usize n = 0;
    registry_.view<entt::entity>().each([&n](auto) { ++n; });
    return n;
}

usize EcsWorld::named_agent_count() const {
    return registry_.view<const comp::NamedAgent>().size();
}

} // namespace immune::sim
