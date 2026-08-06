// core/Types.h — fundamental scalar/vector aliases shared by every module.
// FROZEN CONTRACT (Wave 0). See docs/ARCHITECTURE.md.
#pragma once

#include <cstddef>
#include <cstdint>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

namespace immune {

using u8  = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i8  = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;
using f32 = float;
using f64 = double;
using usize = std::size_t;

using Vec2 = glm::vec2;
using Vec3 = glm::vec3;
using Vec4 = glm::vec4;
using IVec2 = glm::ivec2;

/// Monotonic simulation tick index. Sim runs at exactly kTicksPerSecond.
using Tick = u64;

inline constexpr u32 kTicksPerSecond = 60;
/// Sim step in seconds, as f32 — what sim math uses.
inline constexpr f32 kFixedDt = 1.0f / static_cast<f32>(kTicksPerSecond);
/// Sim step as f64. The clock accumulator MUST use this: accumulating in f32
/// makes one simulated second yield 59 ticks instead of 60.
inline constexpr f64 kFixedDtSeconds = 1.0 / static_cast<f64>(kTicksPerSecond);

/// Handle to a named (ECS) entity as seen from outside sim/ecs.
/// 0 is the reserved null handle.
struct EntityId {
    u32 value = 0;
    constexpr bool valid() const { return value != 0; }
    friend constexpr bool operator==(EntityId a, EntityId b) { return a.value == b.value; }
    friend constexpr bool operator!=(EntityId a, EntityId b) { return a.value != b.value; }
};

/// Pathogen families (DESIGN.md §6). Chaff SoA stores this as a u8.
/// The renderer issues one instanced draw call per family, so the enum order
/// is also the draw-batch order. Do not reorder without updating shaders.
enum class PathogenFamily : u8 {
    Virus = 0,
    Bacteria = 1,
    FungalSpore = 2,
    Parasite = 3,
    CancerCell = 4,
    Allergen = 5,
    Count = 6
};

inline constexpr u32 kFamilyCount = static_cast<u32>(PathogenFamily::Count);

/// Immune cell tower types (DESIGN.md §5).
///
/// Six archetypes, each a recognizable tower-defense role wearing immune-system
/// clothing. The role name is what players actually think in ("the gunner"),
/// the cell name is the fiction and stays the code identifier — DESIGN.md,
/// level JSON, sim-test scripts, and save files all speak the biological name.
/// Enum ORDER is the canonical roster order: build menu, stats table rows, and
/// tower_type_name()/parse_tower_type() all follow it.
///
///   Neutrophil  GUNNER  high-rate single-target stream of real projectiles
///   Macrophage  MORTAR  slow lobbed vesicle, huge delayed area burst
///   Interferon  CRYO    signal cone, slows then fully encases
///   CytotoxicT  TESLA   instantaneous jagged chain between targets
///   BCell       LASER   continuous straight piercing antibody beam
///   NKCell      BLADE   close-range 360 rotor, continuous contact damage
///
/// Replaces the earlier 8-type roster; Dendritic and MastCell are retired and
/// ComplementCascade became Interferon (the Complement Cascade survives as an
/// active ability, see game/abilities, not as a tower).
enum class TowerType : u8 {
    Neutrophil = 0,   ///< GUNNER
    Macrophage = 1,   ///< MORTAR
    Interferon = 2,   ///< CRYO
    CytotoxicT = 3,   ///< TESLA
    BCell = 4,        ///< LASER
    NKCell = 5,       ///< BLADE
    Count = 6
};

inline constexpr u32 kTowerTypeCount = static_cast<u32>(TowerType::Count);

/// Axis-aligned rectangle in world space. Used for region queries and rebake bounds.
struct Rect {
    Vec2 min{0.0f, 0.0f};
    Vec2 max{0.0f, 0.0f};

    Vec2 size() const { return max - min; }
    Vec2 center() const { return (min + max) * 0.5f; }
    bool contains(Vec2 p) const {
        return p.x >= min.x && p.x <= max.x && p.y >= min.y && p.y <= max.y;
    }
    bool overlaps(const Rect& o) const {
        return !(o.min.x > max.x || o.max.x < min.x || o.min.y > max.y || o.max.y < min.y);
    }
};

} // namespace immune
