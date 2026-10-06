// game/towers/TowerSystem.h — placement and targeting.
// Owner: Wave 2B; reshaped by the swarmer-roster redesign.
//
// RATIONALE (DESIGN.md §4, §5, §8.4)
//  - Placement is grid-free and continuous, validated against the distance
//    field (needs clearance) and against the towers already standing.
//  - Towers are NOT obstacles. A placement never touches the TissueMask or the
//    flow field; the horde walks straight through a tower and the tower is
//    drawn over it. The footprint radius is spacing and sprite size only.
//  - A tower has no range and never DAMAGES anything itself: every tower is
//    a spawner that releases swarmers (sim/swarm/Swarmers.h) continuously
//    while a round is on, and the swarmers do the work -- they aggro on
//    whatever is inside their own search radius. What a tower owns is the
//    cadence, the volley size, and which way it faces; the facing is a
//    spatial-hash look-around, never an agent scan.
#pragma once

#include "core/Types.h"

#include <string>
#include <vector>

namespace immune { class Rng; }
namespace immune::sim { class SimWorld; struct SystemContext; }

namespace immune::game {

class Economy;

/// Static per-type baseline data. Loaded from assets/config/towers.json.
///
/// Deliberately carries NO damage number: a tower's output is entirely its
/// swarmers' (game/config/GameConfig.h's TowerMechanics), so a `damage` here
/// would be a second, disagreeing source of truth. Derive DPS from the
/// mechanics if a display or a bot needs one.
struct TowerStats {
    f32 fire_interval = 1.0f;    ///< Seconds between volleys.
    f32 footprint_radius = 1.0f; ///< Body radius: tower spacing and sprite size. Not an obstacle.
    u32 build_cost = 100;
    u8 family_mask = 0xFF;       ///< Which pathogen families it can affect.
    /// Integrity the tower is placed with (comp::Health::max). The horde
    /// spends it -- viruses latch on and feed, bacteria burn it from inside
    /// their aura (sim/hostile/HostileAttacks.h) -- and at zero the tower is
    /// torn down (TowerSystem's death system) with no refund.
    f32 max_health = 300.0f;
};

enum class PlacementResult : u8 {
    Ok = 0,
    NotOnTissue,        ///< Outside the walkable/placeable mask.
    InsufficientClearance,
    Overlapping,        ///< Too close to a solid obstacle; friendly cells do not count.
    CannotAfford,
    OutsidePlacementZone,
    /// The level's schema-2 `allowed_towers` list does not include this type.
    /// A level that is ABOUT one tower is the design lever this exists for.
    TowerNotAllowed,
    /// The player has not unlocked this type in the Strengthen Immunity tree
    /// (game/meta/ImmunityTree.h). Distinct from TowerNotAllowed: that one is
    /// the level's rule, this one is the player's progress.
    TowerLocked,
    NoBuildSite,          ///< A builder cannot lay or reinforce a scar nearby.
    AtCapacity,           ///< No free swarmer slot remains.
};

/// Result of a validation query. The build cursor UI renders from this every
/// frame, so it must be cheap and allocation-free.
struct PlacementQuery {
    PlacementResult result = PlacementResult::Ok;
    Vec2 snapped_position{0.0f, 0.0f};  ///< Light snap to the tissue surface.
    f32 clearance = 0.0f;
    Vec2 build_goal{0.0f, 0.0f};
    bool has_build_goal = false;
    bool valid() const { return result == PlacementResult::Ok; }
};

class TowerSystem {
public:
    /// Registers the tower ECS systems with the world's scheduler: the
    /// spawner, the debuff upkeep, and the death sweep that removes any
    /// tower whose Health the horde has emptied (sim/hostile). Destroyed
    /// towers leave placed_towers() and raise CombatEventType::TowerDestroyed.
    void register_systems(sim::SimWorld& world);

    /// Towers the horde has destroyed since register_systems(). Sold towers
    /// do not count.
    u32 towers_destroyed() const { return destroyed_; }

    const TowerStats& stats(TowerType type) const;
    void set_stats(TowerType type, const TowerStats& stats);

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

    /// The player's unlocked roster, as a literal bitmask over TowerType --
    /// the Strengthen Immunity tree's tower roots (PROGRESSION.md §4.1).
    /// Defaults to the whole roster, so every world with no meta-progression
    /// attached (tests, the gym, the headless modes) is unrestricted. Kept
    /// separate from the level's list so the HUD can tell "not on this level"
    /// from "not unlocked yet".
    static constexpr u32 kAllTowersMask = (1u << kTowerTypeCount) - 1u;
    void set_unlocked_towers(u32 mask) { unlocked_mask_ = mask; }
    u32 unlocked_towers() const { return unlocked_mask_; }
    bool tower_unlocked(TowerType t) const {
        return (unlocked_mask_ & (1u << static_cast<u32>(t))) != 0;
    }

    /// Non-mutating placement check. Safe to call every frame from the UI.
    PlacementQuery validate(const sim::SimWorld& world, TowerType type,
                            Vec2 world_pos, u32 available_atp) const;

    /// Checks a direct cell deployment using the cell's own body size.
    PlacementQuery validate_deploy(const sim::SimWorld& world, TowerType type,
                                   Vec2 world_pos, u32 available_atp) const;

    /// Deploys exactly one persistent cell; no tower entity is created.
    bool deploy(sim::SimWorld& world, TowerType type, Vec2 world_pos);
    /// Stable attribution handle for the most recently deployed cell.
    EntityId last_deployment_id() const { return last_deployment_id_; }

    /// Places a tower. On success: creates the ECS entity, stamps the tissue
    /// mask, and marks the flow field dirty over the footprint.
    /// Returns an invalid EntityId if validation fails.
    EntityId place(sim::SimWorld& world, TowerType type, Vec2 world_pos);

    /// Sells a tower: removes the entity, restores the tissue mask, and marks
    /// the flow field dirty again. Returns the ATP refunded.
    u32 sell(sim::SimWorld& world, EntityId tower);

    /// Nearest named agent inside `radius`. Burrowed agents are never
    /// returned: nothing in the roster can see one. Returns an invalid id if
    /// nothing is that close.
    EntityId find_target(const sim::SimWorld& world, Vec2 origin, f32 radius,
                         u8 family_mask) const;

    const std::vector<EntityId>& placed_towers() const { return towers_; }

    /// Whether towers release volleys at all. Towers spawn CONTINUOUSLY while
    /// this is on -- with or without anything to aim at -- and hold between
    /// rounds. game/session/LevelSession.cpp sets it from the wave phase every
    /// tick (off in Prep); it defaults to on so a world with no wave director
    /// (tests, the gym, headless captures) behaves as one long round.
    void set_releasing(bool on) { releasing_ = on; }
    bool releasing() const { return releasing_; }

private:
    TowerStats stats_[kTowerTypeCount]{};
    std::vector<EntityId> towers_;
    u32 destroyed_ = 0;
    u32 next_deployment_id_ = 1;
    EntityId last_deployment_id_{};
    bool releasing_ = true;
    /// Bitmask over TowerType; 0 = unrestricted. See set_allowed_towers().
    u32 allowed_mask_ = 0;
    /// Literal bitmask over TowerType. See set_unlocked_towers().
    u32 unlocked_mask_ = kAllTowersMask;
};

/// Deploys up to `quantity` cells at one point, charging ATP for each success.
/// Stops when the next cell cannot be afforded or placed.
u32 deploy_cells(TowerSystem& towers, sim::SimWorld& world, Economy& economy,
                 TowerType type, Vec2 world_pos, u32 quantity);

/// Human-readable name, for UI and for --sim-test script parsing.
const char* tower_type_name(TowerType type);
/// Parses a name back to a TowerType. Returns false if unknown.
bool parse_tower_type(std::string_view name, TowerType& out);

} // namespace immune::game
