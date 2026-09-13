// sim/ecs/Components.h — component set for named agents and towers. FROZEN CONTRACT.
// Owner: Wave 1D (components), Wave 2B/2C (behaviour).
//
// RATIONALE (DESIGN.md §8.2)
// The ECS is deliberately the *small* half of the sim: <= 200 named agents plus
// towers. It is where per-entity state machines, telegraphs, and individual
// collision are affordable. Chaff never enters the registry — if you find
// yourself adding a chaff entity, you have violated §8.2.
//
// Components are plain trivially-copyable structs with no methods and no
// virtuals, so EnTT stores them in dense pools.
#pragma once

#include "core/Types.h"

namespace immune::sim::comp {

struct Transform {
    Vec2 position{0.0f, 0.0f};
    f32 rotation = 0.0f;   ///< radians
    f32 scale = 1.0f;
};

struct Velocity {
    Vec2 value{0.0f, 0.0f};
    f32 max_speed = 4.0f;
};

struct Health {
    f32 current = 100.0f;
    f32 max = 100.0f;
    f32 armor = 0.0f;      ///< Flat reduction per damage event.
    bool dead() const { return current <= 0.0f; }
};

/// Named-agent (elite/boss) identity.
struct NamedAgent {
    PathogenFamily family = PathogenFamily::Virus;
    u16 archetype = 0;     ///< Index into the enemy roster table (Wave 2C).
    u8 tier = 1;           ///< 1 = elite, 2 = boss.
};

/// Behaviour state machine. Transitions are data-driven per archetype; the
/// enum is fixed so UI/telegraph/audio can key off it.
enum class AiState : u8 {
    Spawning = 0,
    Advancing,     ///< Following the flow field.
    Telegraphing,  ///< Wind-up before an attack; UI shows the tell.
    Attacking,
    Burrowed,      ///< Hidden; only detection towers (NK Cells) may target.
    Fleeing,
    Dying,
};

struct AiBrain {
    AiState state = AiState::Spawning;
    f32 state_timer = 0.0f;      ///< Seconds in the current state.
    f32 next_ability_cd = 0.0f;
    EntityId target{};
    /// Per-entity deterministic RNG stream id, forked from the sim Rng.
    u32 rng_stream = 0;
};

/// A wind-up the player can read and react to (DESIGN.md §2 readability pillar).
struct Telegraph {
    f32 duration = 0.0f;
    f32 elapsed = 0.0f;
    Vec2 target_point{0.0f, 0.0f};
    f32 radius = 0.0f;
    u8 kind = 0;   ///< Renderer picks the telegraph VFX by this.
    f32 progress() const { return duration > 0.0f ? elapsed / duration : 1.0f; }
};

/// Marks an entity as a placed immune-cell tower.
struct Tower {
    TowerType type = TowerType::Macrophage;
    u8 tier = 1;              ///< 1..3 upgrade tier.
    f32 range = 8.0f;
    f32 cooldown = 0.0f;      ///< Seconds until the next shot/pulse.
    f32 fire_interval = 1.0f;
    f32 ability_cooldown = 0.0f;
    EntityId current_target{};
};

/// The named-agent half of the weaken debuff (chaff_flags::kMarked is the chaff
/// half). Currently applied only by the Goblet Cell (see system_hydro in
/// game/towers/TowerSystem.cpp), which also owns decaying and removing it —
/// this component carries no lifecycle of its own.
struct Marked {
    f32 remaining = 0.0f;
    f32 damage_multiplier = 1.5f;
    EntityId source{};
};

/// The objective structure whose integrity is the lose condition. Its footprint
/// is an oriented rectangle; see game::ObjectivePoint, which is what fills this
/// in at load and where the shape's rationale lives.
struct Objective {
    f32 integrity = 100.0f;
    f32 max_integrity = 100.0f;
    Vec2 half_extents{3.0f, 3.0f};
    f32 rotation = 0.0f;   ///< RADIANS, CCW.
};

/// Short-lived spawned unit (Neutrophil micro-units, antibody projectiles).
struct Ephemeral {
    f32 lifetime = 0.0f;
    EntityId spawner{};
};

/// A temporary obstacle carved out of the tissue mask at runtime -- the Fibrin
/// Clot active ability (game/abilities). An oriented bar: `half_extents.x`
/// runs along the entity's Transform::rotation, `half_extents.y` across it.
///
/// Geometry and clock only, so the renderer can draw it at the right aspect
/// and fade it as it dissolves, and TowerSystem::validate can refuse to build
/// on top of one (a tower footprint that snapshotted the clot's blocked cells
/// would keep them blocked forever once the clot restored them). The tissue
/// cells it displaced live in a private component owned by the ability
/// system, the same split a tower's footprint snapshot makes.
struct Barrier {
    Vec2 half_extents{7.0f, 1.5f};
    f32 remaining = 0.0f;   ///< Seconds until it dissolves.
    f32 duration = 0.0f;    ///< What `remaining` started at.
};

/// Renderable tag: the ECS render pass draws entities carrying this.
struct Sprite {
    Vec4 tint{1.0f, 1.0f, 1.0f, 1.0f};
    f32 size = 1.0f;
    u16 atlas_index = 0;  ///< Procedural SDF shape id, not a texture index.
    u8 layer = 0;
};

} // namespace immune::sim::comp
