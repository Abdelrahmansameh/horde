// game/towers/TowerSystem.h — placement, targeting, upgrades. FROZEN CONTRACT.
// Owner: Wave 2B.
//
// RATIONALE (DESIGN.md §4, §5, §8.4)
//  - Placement is grid-free and continuous, validated against the distance
//    field (needs clearance) and against reachability (a placement that fully
//    walls off every lane is rejected, not allowed-then-exploited).
//  - A successful placement edits the TissueMask and marks the flow field dirty
//    over the tower's footprint only — never a full rebake.
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
    f32 footprint_radius = 1.0f; ///< Tissue blocked; drives the flow rebake rect.
    u32 build_cost = 100;
    u32 upgrade_cost = 150;
    f32 ability_cooldown = 0.0f;
    u8 family_mask = 0xFF;       ///< Which pathogen families it can affect.
    bool blocks_flow = true;     ///< False for support cells that agents flow past.
};

enum class PlacementResult : u8 {
    Ok = 0,
    NotOnTissue,        ///< Outside the walkable/placeable mask.
    InsufficientClearance,
    Overlapping,        ///< Too close to an existing tower.
    WouldBlockAllPaths, ///< Every lane becomes unreachable.
    CannotAfford,
    OutsidePlacementZone,
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

    /// Non-mutating placement check. Safe to call every frame from the UI.
    PlacementQuery validate(const sim::SimWorld& world, TowerType type,
                            Vec2 world_pos, u32 available_atp) const;

    /// Places a tower. On success: creates the ECS entity, stamps the tissue
    /// mask, and marks the flow field dirty over the footprint.
    /// Returns an invalid EntityId if validation fails.
    EntityId place(sim::SimWorld& world, TowerType type, Vec2 world_pos);

    /// Upgrades in place. Returns the new tier, or 0 if not upgradeable.
    u8 upgrade(sim::SimWorld& world, EntityId tower);

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
};

/// Human-readable name, for UI and for --sim-test script parsing.
const char* tower_type_name(TowerType type);
/// Parses a name back to a TowerType. Returns false if unknown.
bool parse_tower_type(std::string_view name, TowerType& out);

} // namespace immune::game
