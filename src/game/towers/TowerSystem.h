// game/towers/TowerSystem.h — placement, targeting, upgrades. FROZEN CONTRACT.
// Owner: Wave 2B.
//
// RATIONALE (DESIGN.md §4, §5, §8.4)
//  - Placement is grid-free and continuous, validated against the distance
//    field (needs clearance) and against the towers already standing.
//  - Towers are NOT obstacles. A placement never touches the TissueMask or the
//    flow field; the horde walks straight through a tower and the tower is
//    drawn over it. The footprint radius is spacing and sprite size only.
//  - Targeting goes through the spatial hash. A tower asks the grid for cells
//    in range; it never iterates agents. Anti-chaff towers do not target at all:
//    they publish a DamageField and let the aggregate damage system do the work.
#pragma once

#include "core/Types.h"

#include <string>
#include <vector>

namespace immune { class Rng; }
namespace immune::sim { class SimWorld; }

namespace immune::game {

/// Static per-type, per-tier data. Loaded from a data table (Wave 2B).
struct TowerStats {
    f32 range = 8.0f;
    f32 fire_interval = 1.0f;
    f32 damage = 10.0f;          ///< Named-agent damage per shot.
    f32 kill_rate = 0.0f;        ///< Chaff density removed per second in-field.
    f32 footprint_radius = 1.0f; ///< Body radius: tower spacing and sprite size. Not an obstacle.
    u32 build_cost = 100;
    u32 upgrade_cost = 150;
    f32 ability_cooldown = 0.0f;
    u8 family_mask = 0xFF;       ///< Which pathogen families it can affect.
};

enum class PlacementResult : u8 {
    Ok = 0,
    NotOnTissue,        ///< Outside the walkable/placeable mask.
    InsufficientClearance,
    Overlapping,        ///< Too close to an existing tower.
    CannotAfford,
    OutsidePlacementZone,
    /// The level's schema-2 `allowed_towers` list does not include this type.
    /// A level that is ABOUT one tower is the design lever this exists for.
    TowerNotAllowed,
};

/// Result of a validation query. The build cursor UI renders from this every
/// frame, so it must be cheap and allocation-free.
struct PlacementQuery {
    PlacementResult result = PlacementResult::Ok;
    Vec2 snapped_position{0.0f, 0.0f};  ///< Light snap to the tissue surface.
    f32 clearance = 0.0f;
    bool valid() const { return result == PlacementResult::Ok; }
};

class TowerSystem {
public:
    /// Registers the tower ECS systems with the world's scheduler.
    void register_systems(sim::SimWorld& world);

    const TowerStats& stats(TowerType type, u8 tier) const;
    void set_stats(TowerType type, u8 tier, const TowerStats& stats);

    /// Restricts which tower types may be built, as a bitmask over TowerType.
    /// 0 means "no restriction", which is what every level without a schema-2
    /// `allowed_towers` list means -- so this is inert unless a level opts in.
    ///
    /// Enforced in validate() rather than by hiding HUD buttons, so the gym
    /// console and the balance bot obey it too: a rule only the UI knows about
    /// is a rule the game does not actually have.
    void set_allowed_towers(u32 mask) { allowed_mask_ = mask; }
    u32 allowed_towers() const { return allowed_mask_; }
    bool tower_allowed(TowerType t) const {
        return allowed_mask_ == 0 || (allowed_mask_ & (1u << static_cast<u32>(t))) != 0;
    }

    /// Non-mutating placement check. Safe to call every frame from the UI.
    PlacementQuery validate(const sim::SimWorld& world, TowerType type,
                            Vec2 world_pos, u32 available_atp) const;

    /// Places a tower. On success: creates the ECS entity, stamps the tissue
    /// mask, and marks the flow field dirty over the footprint.
    /// Returns an invalid EntityId if validation fails.
    EntityId place(sim::SimWorld& world, TowerType type, Vec2 world_pos);

    /// Upgrades in place. Returns the new tier, or 0 if not upgradeable.
    ///
    /// Charges nothing: this header gives the system no Economy, so paying is
    /// the caller's job -- price the move with upgrade_cost() and spend before
    /// or after, but do spend. (It went unpaid in app/ from Wave 3A until the
    /// balance harness noticed every upgrade_cost in towers.json was inert.)
    u8 upgrade(sim::SimWorld& world, EntityId tower);

    /// ATP the next tier costs for `tower`: 0 if it is already tier 3, not a
    /// tower, or gone. Additive to this frozen header because three callers now
    /// need the same tier arithmetic -- the build HUD (to grey the button), the
    /// intent handler (to charge), and the balance bot (to plan a purchase) --
    /// and three copies of it is three chances to disagree.
    u32 upgrade_cost(const sim::SimWorld& world, EntityId tower) const;

    /// Sells a tower: removes the entity, restores the tissue mask, and marks
    /// the flow field dirty again. Returns the ATP refunded.
    u32 sell(sim::SimWorld& world, EntityId tower);

    /// Fires the tower's single active ability if off cooldown.
    bool trigger_ability(sim::SimWorld& world, EntityId tower);

    /// Nearest / strongest target inside range, resolved through the spatial
    /// hash. Returns an invalid id if nothing is in range.
    EntityId find_target(const sim::SimWorld& world, Vec2 origin, f32 range,
                         u8 family_mask, bool require_detect_hidden) const;

    const std::vector<EntityId>& placed_towers() const { return towers_; }

private:
    TowerStats stats_[kTowerTypeCount][3]{};
    std::vector<EntityId> towers_;
    /// Bitmask over TowerType; 0 = unrestricted. See set_allowed_towers().
    u32 allowed_mask_ = 0;
};

/// Human-readable name, for UI and for --sim-test script parsing.
const char* tower_type_name(TowerType type);
/// Parses a name back to a TowerType. Returns false if unknown.
bool parse_tower_type(std::string_view name, TowerType& out);

} // namespace immune::game
