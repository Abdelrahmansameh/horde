// game/abilities/ActiveAbilities.cpp — implementation of the frozen
// ActiveAbilities.h contract. Owner: Wave 4C.
#include "game/abilities/ActiveAbilities.h"

#include "game/abilities/AbilityConfigApply.h"

#include "core/Math.h"
#include "sim/SimWorld.h"
#include "sim/damage/DamageField.h"
#include "sim/ecs/Components.h"
#include "sim/ecs/EcsWorld.h"

namespace immune::game {

void ActiveAbilitySystem::load_defaults() {
    // Names are identity, not tuning, so they stay here; every number comes
    // from assets/config/abilities.json (or, until one is applied, from the
    // DESIGN.md §5.6 values seeded in AbilityConfigApply.cpp).
    static const char* kNames[kAbilityCount] = {
        "Complement Cascade Burst",
        "Histamine Flare",
        "Fever Response",
    };
    const AbilityConfig& cfg = ability_config();
    for (u32 i = 0; i < kAbilityCount; ++i) {
        AbilityDef& d = defs_[i];
        const AbilityTuning& t = cfg.ability[i];
        d.name = kNames[i];
        d.cooldown_seconds = t.cooldown_seconds;
        d.radius = t.radius;
        d.kill_rate = t.kill_rate;
        d.field_duration = t.field_duration;
        d.fever_cooldown_relief = t.fever_cooldown_relief;
    }
    for (f32& r : cooldown_remaining_) r = 0.0f;
}

void ActiveAbilitySystem::tick(f32 dt) {
    for (f32& r : cooldown_remaining_) {
        if (r > 0.0f) r = math::max(0.0f, r - dt);
    }
}

bool ActiveAbilitySystem::ready(AbilityId id) const {
    return cooldown_remaining_[static_cast<u32>(id)] <= 0.0f;
}

AbilityStatus ActiveAbilitySystem::status(AbilityId id) const {
    const u32 i = static_cast<u32>(id);
    AbilityStatus s;
    s.cooldown_remaining = cooldown_remaining_[i];
    s.cooldown_total = defs_[i].cooldown_seconds;
    s.ready = s.cooldown_remaining <= 0.0f;
    return s;
}

bool ActiveAbilitySystem::cast(sim::SimWorld& world, AbilityId id, Vec2 target_point) {
    if (!ready(id)) return false;
    const u32 i = static_cast<u32>(id);
    const AbilityDef& d = defs_[i];

    switch (id) {
        case AbilityId::ComplementCascadeBurst: {
            // Resolves through the existing Chain-shape damage-field contract
            // -- the same jump-through-dense-clusters resolution a Complement
            // Cascade tower uses, just triggered once, globally, at a point
            // rather than continuously from a placed tower. A short positive
            // lifetime (not <=0) marks this as one-shot: DamageSystem drops it
            // after clear_transient() rather than expecting a per-tick refresh.
            sim::DamageField field;
            field.shape = sim::FieldShape::Chain;
            field.origin = target_point;
            field.radius = d.radius;
            field.kill_rate = d.kill_rate;
            field.lifetime = 0.1f;
            world.damage().submit(field);
            break;
        }
        case AbilityId::HistamineFlare: {
            sim::DamageField field;
            field.shape = sim::FieldShape::Circle;
            field.origin = target_point;
            field.radius = d.radius;
            field.kill_rate = d.kill_rate;
            field.falloff = 1.0f; // soft-edged nova, not a hard cutoff
            field.lifetime = d.field_duration;
            world.damage().submit(field);
            break;
        }
        case AbilityId::FeverResponse: {
            // No damage-field equivalent exists for "buff every tower" -- this
            // is the one ability that reaches into the ECS directly. An
            // instant cooldown-relief burst (read/write the already-public
            // comp::Tower.cooldown) was chosen deliberately over a sustained
            // rate multiplier: it needs no revert bookkeeping across ticks,
            // so there's no risk of a buff window silently outliving its
            // intended duration if something goes wrong elsewhere.
            auto view = world.ecs().registry().view<sim::comp::Tower>();
            for (auto e : view) {
                sim::comp::Tower& t = view.get<sim::comp::Tower>(e);
                t.cooldown = math::max(0.0f, t.cooldown - d.fever_cooldown_relief);
            }
            break;
        }
        case AbilityId::Count:
            return false;
    }

    cooldown_remaining_[i] = d.cooldown_seconds;
    return true;
}

const char* ability_name(AbilityId id) {
    switch (id) {
        case AbilityId::ComplementCascadeBurst: return "Complement Cascade Burst";
        case AbilityId::HistamineFlare: return "Histamine Flare";
        case AbilityId::FeverResponse: return "Fever Response";
        case AbilityId::Count: break;
    }
    return "?";
}

} // namespace immune::game
