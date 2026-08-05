#include "sim/ecs/AiStateMachine.h"

namespace immune::sim {

ArchetypeRegistry& archetypes(entt::registry& registry) {
    return registry.ctx().emplace<ArchetypeRegistry>();
}

const ArchetypeRegistry* archetypes_if_any(const entt::registry& registry) {
    return registry.ctx().find<ArchetypeRegistry>();
}

namespace {

bool trigger_fires(const AiTransition& t, const AiEval& eval) {
    switch (t.trigger) {
        case AiTrigger::Never:
            return false;
        case AiTrigger::Always:
            return true;
        case AiTrigger::StateTimerAtLeast:
            return eval.brain.state_timer >= t.param;
        case AiTrigger::HealthBelowFrac:
            return eval.health.max > 0.0f && (eval.health.current / eval.health.max) < t.param;
        case AiTrigger::HealthAboveFrac:
            return eval.health.max > 0.0f && (eval.health.current / eval.health.max) > t.param;
        case AiTrigger::AbilityReady:
            return eval.brain.next_ability_cd <= 0.0f;
        case AiTrigger::AbilityNotReady:
            return eval.brain.next_ability_cd > 0.0f;
        case AiTrigger::TelegraphComplete: {
            const auto* tg = eval.registry.try_get<comp::Telegraph>(eval.self);
            return tg != nullptr && tg->elapsed >= tg->duration;
        }
        case AiTrigger::HasTargetInRange: {
            const f32 range = t.param > 0.0f ? t.param : eval.attack.range;
            return eval.has_target && eval.target_distance <= range;
        }
        case AiTrigger::NoTargetInRange: {
            const f32 range = t.param > 0.0f ? t.param : eval.attack.range;
            return !(eval.has_target && eval.target_distance <= range);
        }
        case AiTrigger::IsDead:
            return eval.health.dead();
        case AiTrigger::Custom:
            return t.custom != nullptr && t.custom(eval);
    }
    return false;
}

} // namespace

bool evaluate_transitions(const ArchetypeBehavior& behavior, const AiEval& eval, AiState& out_next) {
    // First match wins, in authored table order. Only entries whose `from`
    // matches the entity's current state are ever consulted, so ordering
    // between different `from` states never matters — only ordering among
    // entries that share a `from` state, which the archetype author controls
    // by the order they call add().
    for (u32 i = 0; i < behavior.transition_count; ++i) {
        const AiTransition& t = behavior.transitions[i];
        if (t.from != eval.brain.state) continue;
        if (trigger_fires(t, eval)) {
            out_next = t.to;
            return true;
        }
    }
    return false;
}

} // namespace immune::sim
