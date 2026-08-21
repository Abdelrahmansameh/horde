#include "sim/ecs/NamedAgents.h"

#include "core/Math.h"
#include "core/Rng.h"
#include "sim/SimWorld.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace immune::sim::named {

NamedFrame& frame(entt::registry& registry) {
    return registry.ctx().emplace<NamedFrame>();
}

const NamedFrame* frame_if_any(const entt::registry& registry) {
    return registry.ctx().find<NamedFrame>();
}

namespace {

/// Fallback used only if a NamedAgent somehow references an archetype id
/// before any archetype table exists. Static so looking it up never copies
/// the (fairly large, fixed-size) ArchetypeBehavior struct.
const ArchetypeBehavior kFallbackBehavior{};

const ArchetypeBehavior& behavior_for(const ArchetypeRegistry* table, u16 archetype_id) {
    return table ? table->get(archetype_id) : kFallbackBehavior;
}

/// Deterministic per-entity, per-tick wander impulse. Draws from a scoped Rng
/// forked off the entity's spawn seed and the current tick — never the shared
/// world generator — so it costs nothing in ordering and never depends on
/// which entities happen to run before it in the loop.
Vec2 wander_impulse(u64 entity_seed, Tick tick, f32 magnitude) {
    if (magnitude <= 0.0f) return Vec2{0.0f, 0.0f};
    Rng r(entity_seed, static_cast<u64>(tick) + 1u);
    return r.unit_disc() * magnitude;
}

// ---------------------------------------------------------------------------
// PreUpdate: order rebuild, timer / cooldown / telegraph advance.
// ---------------------------------------------------------------------------
void system_named_timers(SystemContext& ctx) {
    entt::registry& registry = ctx.registry;
    NamedFrame& fr = frame(registry);

    fr.ordered.clear();
    fr.positions.clear();
    fr.attacks.clear(); // last tick's events were consumed by Combat already.

    auto view = registry.view<comp::AiBrain, comp::Transform, comp::SpawnOrder>();
    fr.ordered.reserve(view.size_hint());
    for (auto e : view) fr.ordered.push_back(e);

    // Canonical order: spawn sequence, never EnTT storage order.
    std::sort(fr.ordered.begin(), fr.ordered.end(), [&](entt::entity a, entt::entity b) {
        return registry.get<comp::SpawnOrder>(a).seq < registry.get<comp::SpawnOrder>(b).seq;
    });

    fr.positions.reserve(fr.ordered.size());
    for (entt::entity e : fr.ordered) {
        fr.positions.push_back(registry.get<comp::Transform>(e).position);

        comp::AiBrain& brain = registry.get<comp::AiBrain>(e);
        brain.state_timer += ctx.dt;
        if (brain.next_ability_cd > 0.0f) {
            brain.next_ability_cd = math::max(0.0f, brain.next_ability_cd - ctx.dt);
        }
        if (auto* tg = registry.try_get<comp::Telegraph>(e)) {
            tg->elapsed = math::min(tg->duration, tg->elapsed + ctx.dt);
        }
    }
}

// ---------------------------------------------------------------------------
// AI: transition-table evaluation, telegraph lifecycle, attack-event emission.
// ---------------------------------------------------------------------------
void system_named_ai(SystemContext& ctx) {
    entt::registry& registry = ctx.registry;
    NamedFrame& fr = frame(registry);
    const ArchetypeRegistry* table = archetypes_if_any(registry);
    fr.transitions_this_tick = 0;
    fr.telegraphs.clear();

    for (entt::entity e : fr.ordered) {
        const comp::NamedAgent& named = registry.get<comp::NamedAgent>(e);
        const ArchetypeBehavior& behavior = behavior_for(table, named.archetype);

        comp::AiBrain& brain = registry.get<comp::AiBrain>(e);
        const comp::Transform& transform = registry.get<comp::Transform>(e);
        const comp::Health& health = registry.get<comp::Health>(e);
        const comp::Attack& attack = registry.get<comp::Attack>(e);

        AiEval eval{registry, e, brain, transform, health, attack,
                    /*target_position*/ transform.position,
                    /*target_distance*/ std::numeric_limits<f32>::infinity(),
                    /*has_target*/ false,
                    ctx.dt, ctx.tick};

        AiState next{};
        if (evaluate_transitions(behavior, eval, next) && next != brain.state) {
            const AiState prev = brain.state;
            if (behavior.on_exit) behavior.on_exit(registry, e, prev, next);

            if (next == AiState::Telegraphing) {
                comp::Telegraph tg{};
                tg.duration = attack.windup;
                tg.elapsed = 0.0f;
                tg.target_point = transform.position;
                tg.radius = attack.radius;
                tg.kind = attack.kind;
                registry.emplace_or_replace<comp::Telegraph>(e, tg);
            } else if (next == AiState::Attacking) {
                // The wind-up just completed: the active window opens this
                // tick. Resolve it once, here, rather than re-testing a timer
                // window every tick of Attacking.
                Vec2 point = transform.position;
                f32 radius = attack.radius;
                if (const auto* tg = registry.try_get<comp::Telegraph>(e)) {
                    point = tg->target_point;
                    radius = tg->radius;
                }
                fr.attacks.push_back(AttackEvent{ctx.world.ecs().to_id(e), point, radius,
                                                 attack.damage, attack.kind});
            } else if (prev == AiState::Attacking) {
                // Leaving Attacking: pay the recovery cooldown and clear the
                // now-resolved telegraph.
                brain.next_ability_cd = behavior.ability_cooldown;
                registry.remove<comp::Telegraph>(e);
            }

            brain.state = next;
            brain.state_timer = 0.0f;
            if (behavior.on_enter) behavior.on_enter(registry, e, prev, next);
            ++fr.transitions_this_tick;
        }

        if (brain.state == AiState::Telegraphing) {
            if (const auto* tg = registry.try_get<comp::Telegraph>(e)) {
                fr.telegraphs.push_back(ActiveTelegraph{e, tg->target_point, tg->radius, tg->progress()});
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Movement: shared flow-field sampling + per-entity local steering layered on
// top (DESIGN.md §8.3). O(n) over named agents plus an O(n^2) separation pass
// that is cheap at n <= 200 (<= 40,000 scalar ops) and never touches chaff.
// ---------------------------------------------------------------------------
void system_named_movement(SystemContext& ctx) {
    entt::registry& registry = ctx.registry;
    NamedFrame& fr = frame(registry);
    const ArchetypeRegistry* table = archetypes_if_any(registry);
    const SimWorld& world = ctx.world;

    for (usize i = 0; i < fr.ordered.size(); ++i) {
        const entt::entity e = fr.ordered[i];
        const comp::NamedAgent& named = registry.get<comp::NamedAgent>(e);
        const ArchetypeBehavior& behavior = behavior_for(table, named.archetype);
        const comp::AiBrain& brain = registry.get<comp::AiBrain>(e);
        const comp::Steering& steer = registry.get<comp::Steering>(e);
        const comp::NamedSeed* seed = registry.try_get<comp::NamedSeed>(e);

        comp::Transform& transform = registry.get<comp::Transform>(e);
        comp::Velocity& velocity = registry.get<comp::Velocity>(e);

        const Vec2 pos = fr.positions[i];
        const u8 state_index = static_cast<u8>(brain.state);
        const f32 speed_scale = state_index < 7 ? behavior.speed_scale[state_index] : 0.0f;

        Vec2 accel{0.0f, 0.0f};

        // 1. Shared flow field (the same field chaff samples).
        accel += world.flow().sample(pos) * steer.flow_weight;

        // 2. Separation from other named agents. n is small (<= 200) so a
        // direct pairwise scan is cheap and avoids building a second spatial
        // structure just for a handful of elites.
        if (steer.separation_weight > 0.0f && steer.separation_radius > 0.0f) {
            Vec2 push{0.0f, 0.0f};
            const f32 r2 = steer.separation_radius * steer.separation_radius;
            for (usize j = 0; j < fr.positions.size(); ++j) {
                if (j == i) continue;
                const Vec2 d = pos - fr.positions[j];
                const f32 d2 = math::length_sq(d);
                if (d2 > r2 || d2 < math::kEpsilon) continue;
                const f32 dist = std::sqrt(d2);
                push += (d / dist) * (steer.separation_radius - dist);
            }
            accel += push * steer.separation_weight;
        }

        // 3. Dodge other agents' telegraphed attacks.
        if (steer.dodge_weight > 0.0f) {
            for (const ActiveTelegraph& tg : fr.telegraphs) {
                if (tg.owner == e) continue;
                const Vec2 d = pos - tg.point;
                const f32 dist = math::length(d);
                const f32 danger = tg.radius * (1.5f + tg.progress);
                if (dist >= danger || dist < math::kEpsilon) continue;
                accel += (d / dist) * (danger - dist) * steer.dodge_weight;
            }
        }

        // 4. Vessel-hugging: nudge away from walls when clearance is tight.
        if (steer.wall_weight > 0.0f) {
            const f32 clearance = world.sdf().sample(pos);
            if (clearance < steer.wall_clearance) {
                accel += world.sdf().gradient(pos) * steer.wall_weight *
                        (steer.wall_clearance - clearance);
            }
        }

        // 5. Archetype-specific local steering (Wave 2C hooks in here).
        if (behavior.local_steer) {
            Rng local_rng(seed ? seed->seed : 0u, static_cast<u64>(ctx.tick) + 2u);
            AiSteerContext sc{world, registry, e, pos, velocity.value, brain.state, ctx.dt, local_rng};
            behavior.local_steer(sc, accel);
        }

        // 6. Deterministic per-entity wander.
        accel += wander_impulse(seed ? seed->seed : 0u, ctx.tick, steer.jitter);

        const f32 max_speed = velocity.max_speed * speed_scale;
        velocity.value += accel * steer.accel * ctx.dt;
        velocity.value = math::clamp_length(velocity.value, math::max(max_speed, 0.0f));
        transform.position += velocity.value * ctx.dt;
    }
}

// ---------------------------------------------------------------------------
// Combat: resolves attacks whose active window opened this tick.
// ---------------------------------------------------------------------------
void system_named_combat(SystemContext& ctx) {
    entt::registry& registry = ctx.registry;
    NamedFrame& fr = frame(registry);
    if (fr.attacks.empty()) return;

    auto objectives = registry.view<comp::Objective, comp::Transform>();
    for (const AttackEvent& ev : fr.attacks) {
        for (auto obj_entity : objectives) {
            comp::Objective& obj = objectives.get<comp::Objective>(obj_entity);
            const comp::Transform& obj_t = objectives.get<comp::Transform>(obj_entity);
            const f32 dist = math::length(obj_t.position - ev.point);
            if (dist > ev.radius + obj.radius) continue;
            obj.integrity = math::max(0.0f, obj.integrity - ev.damage);
        }
    }
}

// ---------------------------------------------------------------------------
// PostUpdate: death resolution, ephemeral expiry, entity destruction.
// ---------------------------------------------------------------------------
void system_named_cleanup(SystemContext& ctx) {
    entt::registry& registry = ctx.registry;
    NamedFrame& fr = frame(registry);
    const ArchetypeRegistry* table = archetypes_if_any(registry);

    for (entt::entity e : fr.ordered) {
        const comp::AiBrain& brain = registry.get<comp::AiBrain>(e);
        if (brain.state != AiState::Dying) continue;
        const comp::NamedAgent& named = registry.get<comp::NamedAgent>(e);
        const ArchetypeBehavior& behavior = behavior_for(table, named.archetype);
        if (brain.state_timer >= behavior.death_fade) {
            registry.destroy(e);
        }
    }

    auto ephemeral = registry.view<comp::Ephemeral>();
    std::vector<entt::entity> expired;
    for (auto e : ephemeral) {
        comp::Ephemeral& em = ephemeral.get<comp::Ephemeral>(e);
        em.lifetime -= ctx.dt;
        if (em.lifetime <= 0.0f) expired.push_back(e);
    }
    for (entt::entity e : expired) registry.destroy(e);
}

} // namespace

void install(SimWorld& world) {
    EcsWorld& ecs = world.ecs();
    ecs.add_system(SystemPhase::PreUpdate, "named_timers", 0, &system_named_timers);
    ecs.add_system(SystemPhase::AI, "named_ai", 0, &system_named_ai);
    ecs.add_system(SystemPhase::Movement, "named_movement", 0, &system_named_movement);
    ecs.add_system(SystemPhase::Combat, "named_combat", 0, &system_named_combat);
    ecs.add_system(SystemPhase::PostUpdate, "named_cleanup", 0, &system_named_cleanup);
}

EntityId spawn(SimWorld& world, const SpawnParams& params) {
    EcsWorld& ecs = world.ecs();
    entt::registry& registry = ecs.registry();

    if (ecs.named_agent_count() >= kMaxNamedAgents) return EntityId{};

    const ArchetypeRegistry& table = archetypes(registry);
    const ArchetypeBehavior& b = table.get(params.archetype);

    NamedFrame& fr = frame(registry);
    const u32 seq = fr.next_seq++;

    const entt::entity e = registry.create();
    registry.emplace<comp::Transform>(e, comp::Transform{params.position, params.rotation, 1.0f});
    registry.emplace<comp::Velocity>(e, comp::Velocity{Vec2{0.0f, 0.0f}, b.max_speed});

    const f32 hp = math::max(1.0f, b.max_health * params.health_scale);
    registry.emplace<comp::Health>(e, comp::Health{hp, hp, b.armor});

    registry.emplace<comp::NamedAgent>(e, comp::NamedAgent{b.family, params.archetype, b.tier});

    comp::AiBrain brain{};
    brain.state = AiState::Spawning;
    brain.state_timer = 0.0f;
    brain.next_ability_cd = b.ability_cooldown;
    brain.rng_stream = seq;
    registry.emplace<comp::AiBrain>(e, brain);

    registry.emplace<comp::Attack>(e, b.attack);
    registry.emplace<comp::Steering>(e, b.steering);
    registry.emplace<comp::Sprite>(e, comp::Sprite{b.tint, b.sprite_size, b.atlas_index, 0});
    registry.emplace<comp::SpawnOrder>(e, comp::SpawnOrder{seq});

    // Deterministic per-entity RNG seed, drawn once from the sim RNG in
    // spawn-call order (which is itself deterministic given a fixed seed).
    const u64 seed = world.rng().next_u64();
    registry.emplace<comp::NamedSeed>(e, comp::NamedSeed{seed});

    return ecs.to_id(e);
}

// ---------------------------------------------------------------------------
// Placeholder elite
// ---------------------------------------------------------------------------

namespace {
struct PlaceholderEliteState {
    bool registered = false;
    u16 id = 0;
};
} // namespace

u16 placeholder_elite(entt::registry& registry) {
    PlaceholderEliteState& state = registry.ctx().emplace<PlaceholderEliteState>();
    if (state.registered) return state.id;

    ArchetypeBehavior b{};
    b.name = "placeholder_elite";
    b.family = PathogenFamily::Bacteria;
    b.tier = 1;
    b.max_health = 300.0f;
    b.armor = 2.0f;
    b.max_speed = 3.0f;
    b.ability_cooldown = 3.0f;
    b.death_fade = 0.5f;
    // speed_scale defaults (Spawning=0, Advancing=1, Telegraphing=0.15,
    // Attacking=0, Burrowed=0, Fleeing=1.4, Dying=0) already fit this
    // archetype: it creeps into its wind-up, plants for the hit, and stands
    // still while burrowed or dying.

    b.attack = comp::Attack{/*windup*/ 0.8f, /*active*/ 0.2f, /*recovery*/ 0.4f,
                            /*range*/ 10.0f, /*radius*/ 3.0f, /*damage*/ 12.0f, /*kind*/ 0};
    b.steering = comp::Steering{};

    b.sprite_size = 1.8f;
    b.tint = Vec4{0.25f, 0.75f, 0.68f, 1.0f}; // teal: a placeholder, not a family colour
    b.atlas_index = 0;

    // Table order matters only within a shared `from` state: IsDead is
    // authored first for every active state so death always pre-empts
    // whatever else that state was about to do.
    b.add(AiState::Spawning, AiState::Advancing, AiTrigger::StateTimerAtLeast, 0.25f);

    b.add(AiState::Advancing, AiState::Dying, AiTrigger::IsDead);
    b.add(AiState::Advancing, AiState::Burrowed, AiTrigger::HealthBelowFrac, 0.4f);
    b.add(AiState::Advancing, AiState::Telegraphing, AiTrigger::AbilityReady);

    b.add(AiState::Telegraphing, AiState::Dying, AiTrigger::IsDead);
    b.add(AiState::Telegraphing, AiState::Attacking, AiTrigger::TelegraphComplete);

    b.add(AiState::Attacking, AiState::Dying, AiTrigger::IsDead);
    b.add(AiState::Attacking, AiState::Advancing, AiTrigger::StateTimerAtLeast, b.attack.active);

    b.add(AiState::Burrowed, AiState::Dying, AiTrigger::IsDead);
    b.add(AiState::Burrowed, AiState::Advancing, AiTrigger::StateTimerAtLeast, 2.0f);

    ArchetypeRegistry& table = archetypes(registry);
    state.id = table.add(b);
    state.registered = true;
    return state.id;
}

u32 spawn_bench_population(SimWorld& world, u32 count) {
    entt::registry& registry = world.ecs().registry();
    const u16 archetype_id = placeholder_elite(registry);
    const Rect bounds = world.desc().world_bounds;
    Rng& rng = world.rng();

    const u32 cap = static_cast<u32>(kMaxNamedAgents);
    u32 spawned = 0;
    for (u32 i = 0; i < count && spawned < cap; ++i) {
        SpawnParams p;
        p.archetype = archetype_id;
        p.position = Vec2{rng.range_f(bounds.min.x, bounds.max.x),
                          rng.range_f(bounds.min.y, bounds.max.y)};
        p.rotation = rng.range_f(0.0f, math::kTwoPi);
        const EntityId id = spawn(world, p);
        if (id.valid()) ++spawned;
    }
    return spawned;
}

u32 setup_bench_scenario(SimWorld& world, u32 count) {
    install(world);
    return spawn_bench_population(world, count);
}

u64 state_hash(const entt::registry& registry) {
    u64 h = 1469598103934665603ULL;
    auto mix = [&h](const void* data, usize bytes) {
        const u8* p = static_cast<const u8*>(data);
        for (usize i = 0; i < bytes; ++i) {
            h ^= p[i];
            h *= 1099511628211ULL;
        }
    };

    // Built directly from the registry (not the NamedFrame scratch buffer,
    // which is only populated once a tick has run) and sorted by spawn order
    // so the hash never depends on EnTT's storage order.
    std::vector<entt::entity> ordered;
    auto view = registry.view<const comp::SpawnOrder, const comp::Transform,
                              const comp::Health, const comp::AiBrain>();
    ordered.reserve(view.size_hint());
    for (auto e : view) ordered.push_back(e);
    std::sort(ordered.begin(), ordered.end(), [&](entt::entity a, entt::entity b) {
        return registry.get<comp::SpawnOrder>(a).seq < registry.get<comp::SpawnOrder>(b).seq;
    });

    for (entt::entity e : ordered) {
        const auto& t = registry.get<comp::Transform>(e);
        const auto& h_comp = registry.get<comp::Health>(e);
        const auto& brain = registry.get<comp::AiBrain>(e);
        mix(&t.position, sizeof(t.position));
        mix(&h_comp.current, sizeof(h_comp.current));
        mix(&brain.state, sizeof(brain.state));
    }
    return h;
}

} // namespace immune::sim::named
