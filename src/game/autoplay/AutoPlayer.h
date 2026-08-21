// game/autoplay/AutoPlayer.h — the bot that plays a level. NEW MODULE.
//
// RATIONALE
// Balance numbers are only worth reading if something was actually playing.
// This is that something: it places towers, upgrades them, and spends ATP
// against the same Economy and the same TowerSystem::validate() a human's
// mouse goes through. There is no privileged path -- if the bot can build it,
// a player standing in the same spot can build it.
//
// NO AUTHORED DATA
// Where to build is derived from the level itself (vessel widths, the flow
// field's cost-to-goal, lane ownership, and the author's own placement-zone
// hints), never from a per-level plan file. A plan file would be one more
// thing to keep in step with every level edit, and a stale plan produces a
// balance reading that describes the plan rather than the level. The cost of
// that choice is that the bot is a decent generalist rather than an expert on
// any one map, which is the right trade for a comparative measurement.
//
// WHAT IT DOES NOT DO
// No active abilities (out of scope), no selling, no retreating. It buys, and
// it buys on a fixed cadence rather than every tick, so it is not a
// superhuman reactor -- a bot that spends its ATP on the exact frame it clears
// a purchase threshold measures a game no human plays.
//
// DETERMINISM
// Every decision is a pure function of (plan, tick, economy, world state). No
// RNG, no wall clock. Same level + seed + profile replays exactly, which is
// what makes a report diffable across a config change.
#pragma once

#include "core/Types.h"
#include "game/level/Level.h"

#include <string>
#include <vector>

namespace immune::sim { class SimWorld; }

namespace immune::game {

class Economy;
class TowerSystem;
class WaveDirector;

/// How the bot spends. The spread BETWEEN profiles on one level is the most
/// informative single output of the harness: if every profile wins comfortably
/// the level is too easy, and if one profile wins where five lose, that
/// profile has found something the balance did not intend.
enum class AutoPlayProfile : u8 {
    /// Always buy the cheapest thing currently affordable. The baseline
    /// "average player" and the default.
    GreedyCheapest = 0,
    /// Fill every planned site before upgrading anything. Tests breadth.
    SpreadCoverage,
    /// Take existing towers to tier 3 before adding new ones. Tests depth,
    /// and is the profile that exposes an underpriced or overpowered tier 3.
    SaveForTier3,
    /// Build one tower type and nothing else. Six of these on one level rank
    /// the six towers against each other with everything else held constant.
    SingleType,
};

/// "greedy-cheapest" | "spread-coverage" | "save-for-tier3" | "single-type:<tower>".
/// Round-trips through parse_autoplay_profile().
std::string autoplay_profile_name(AutoPlayProfile profile, TowerType single_type);
bool parse_autoplay_profile(std::string_view text, AutoPlayProfile& out_profile,
                            TowerType& out_single_type);

struct AutoPlayConfig {
    AutoPlayProfile profile = AutoPlayProfile::GreedyCheapest;
    /// Only meaningful for SingleType.
    TowerType single_type = TowerType::Neutrophil;
    /// Ticks between decisions. 30 (half a second) keeps the bot inside human
    /// reaction range; 1 would let it convert income into towers the instant
    /// it could afford them, which flatters the economy.
    u32 decision_interval_ticks = 30;
    /// Hard cap on planned sites, so a long level does not produce a plan the
    /// economy could never fund and a report full of never-built entries.
    u32 max_sites = 32;
};

/// One place the bot intends to build, with the type it intends to build there.
struct PlannedSite {
    Vec2 position{0.0f, 0.0f};
    TowerType type = TowerType::Neutrophil;
    f32 score = 0.0f;
    f32 lumen_width = 0.0f;     ///< Vessel width at the site; the choke term.
    f32 cost_to_goal = 0.0f;    ///< Flow-field cost; lower = later in the path.
    EntityId built{};           ///< Valid once something stands here.
    /// Tier of the tower standing here, tracked by the bot rather than read
    /// back out of the ECS: the bot is the only thing that ever upgrades one,
    /// so its own record is authoritative and needs no registry lookup.
    u8 tier = 0;
    bool rejected = false;      ///< validate() refused it at purchase time.
};

/// What the bot did on one decision tick. Reported so the caller can book the
/// purchase into telemetry -- the bot deliberately owns no collector.
struct AutoPlayAction {
    enum class Kind : u8 { None = 0, Placed, Upgraded };
    Kind kind = Kind::None;
    EntityId tower{};
    TowerType type = TowerType::Neutrophil;
    Vec2 position{0.0f, 0.0f};
    u32 cost = 0;
    u8 tier = 0;   ///< Tier after the action.
};

class AutoPlayer {
public:
    void configure(const AutoPlayConfig& cfg) { cfg_ = cfg; }
    const AutoPlayConfig& config() const { return cfg_; }

    /// Builds the site list. Call once, after the level is instantiated (the
    /// flow field and SDF must be baked) and before the first tick.
    void plan(const LevelDef& level, const LaneOwnershipMap& lanes, const sim::SimWorld& world,
              const TowerSystem& towers, const WaveDirector& waves);

    /// One decision opportunity. Cheap and a no-op on non-decision ticks, so
    /// the caller can simply call it every tick.
    AutoPlayAction tick(sim::SimWorld& world, TowerSystem& towers, Economy& economy, u64 tick);

    const std::vector<PlannedSite>& sites() const { return sites_; }
    /// Sites the plan wanted that validate() refused when it came to it.
    u32 rejected_sites() const;

private:
    /// The action the current profile wants next, or kind None if nothing is
    /// affordable. Does not execute.
    AutoPlayAction choose(const sim::SimWorld& world, const TowerSystem& towers,
                          const Economy& economy);

    AutoPlayConfig cfg_{};
    std::vector<PlannedSite> sites_;
    /// Index into sites_ of the next site to try. Monotonic: a site the plan
    /// has passed over is not revisited, because the thing that made it
    /// invalid (a tower now standing next to it) does not un-happen.
    usize next_site_ = 0;
};

} // namespace immune::game
