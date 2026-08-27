// game/towers/TowerSystem.cpp — placement, targeting, upgrades. Wave 2B.
//
// TowerSystem.h is a FROZEN CONTRACT: no signature, member, or friend may be
// added to it. Two consequences shape everything below:
//
//  1. Any state that must outlive a single call but isn't already one of the
//     header's declared members (`stats_`, `towers_`) cannot live on
//     TowerSystem itself. Per-tower bookkeeping needed for sell() refunds and
//     tissue restoration, and Neutrophil's NET slow-zone state, are instead
//     stored as ordinary EnTT components private to this translation unit
//     (`priv::TowerRecord`, `priv::ActiveNet`) — attached to entities this
//     file creates, cleaned up automatically by registry.destroy().
//  2. Helper logic that needs `stats_`/`towers_` can only live inside the
//     bodies of the methods the header actually declares (validate, place,
//     upgrade, sell, trigger_ability, find_target, stats, set_stats,
//     register_systems) — those have normal private access because they are
//     genuine member-function definitions. Free functions in the anonymous
//     namespace below (the default-stats loader, the six per-tower Combat
//     systems, the WouldBlockAllPaths heuristic) only ever go through the
//     class's *public* API (`stats()`, `find_target()`, `trigger_ability()`).
//
// WAVE 6C rewrote every Combat-phase system for the six-role roster (GUNNER /
// MORTAR / CRYO / TESLA / LASER / BLADE) and retuned the whole stats table.
// The LASER slot has since become HYDRO -- see system_hydro below, and the note
// on TowerType::GobletCell in core/Types.h for why.
// See the "Wave 6C: shared combat helpers" block below for the rules those
// systems are written against — in particular why exactly one of them uses
// projectiles and none of them damage chaff agent-by-agent.
#include "game/towers/TowerSystem.h"

#include "game/towers/TowerMechanics.h"

#include "core/Math.h"
#include "core/Rng.h"
#include "sim/CombatEvents.h"
#include "sim/SimWorld.h"
#include "sim/chaff/ChaffBuffers.h"
#include "sim/damage/DamageField.h"
#include "sim/ecs/Components.h"
#include "sim/ecs/EcsWorld.h"
#include "sim/flowfield/FlowField.h"
#include "sim/flowfield/TissueRaster.h"
#include "sim/fluid/Fluid.h"
#include "sim/projectile/Projectiles.h"
#include "sim/swarm/Swarmers.h"
#include "sim/spatial/SpatialHash.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace immune::game {

// TowerSystem.h (this class's own header) refers to sim::SimWorld etc. with an
// explicit `sim::` qualifier throughout, which this file keeps for anything
// declared in the header's own signatures. Everything below additionally
// needs `sim::comp::*`, `sim::EcsWorld`, `sim::SystemContext`, `sim::block_rect`,
// `sim::chaff_flags`, and friends constantly enough that an unqualified
// `comp::Tower` etc. is far more readable — hence this using-directive,
// exactly like the sim-side test files (e.g. tests/test_named_agents.cpp) do.
using namespace immune::sim;

namespace {

// Order must match TowerType's declaration order exactly.
constexpr const char* kTowerNames[kTowerTypeCount] = {
    "neutrophil",   // GUNNER
    "macrophage",   // MORTAR
    "interferon",   // CRYO
    "cytotoxic_t",  // TESLA
    "goblet_cell",  // HYDRO
    "nk_cell"};     // BLADE

// ---------------------------------------------------------------------------
// Private auxiliary ECS components (see file header comment).
// ---------------------------------------------------------------------------
namespace priv {

/// Attached to every tower entity at place() time. Carries what sell() needs
/// to refund ATP and restore the exact tissue cells that were blocked — not
/// just "set walkable = true", since a footprint's square corners can extend
/// slightly past the disc the clearance check guaranteed, and may have been
/// unwalkable (or a non-default cost) *before* this tower ever stood there.
struct TowerRecord {
    Rect footprint{};
    IVec2 cell_min{0, 0};
    IVec2 cell_dims{0, 0};
    std::vector<u8> saved_walkable;
    std::vector<f32> saved_cost;
    u32 invested_atp = 0;
};

/// A Goblet Cell's nozzle state. comp::Tower carries exactly one timer
/// (`cooldown`), which is enough for every tower that fires an instant, and not
/// enough for one that SPRAYS: the Goblet Cell needs a second clock for how much
/// of the current burst is left, plus a burst counter and a jitter seed the
/// emitter can advance per tick. TowerSystem.h is frozen and comp::Tower is not
/// this file's to widen, so it lives here as a private component, exactly like
/// ActiveNet below.
struct Nozzle {
    /// Seconds of spray left in the current burst. > 0 means "firing now".
    f32 burst_remaining = 0.0f;
    /// Aim locked at the moment the trigger was pulled. A burst does NOT track
    /// its target mid-spray, and that is the design: a jet that swivels to
    /// follow a moving target sprays a fan and never lands a slug anywhere.
    /// Committing to one line per burst is what makes the fluid arrive as a
    /// coherent column that piles up and splashes at one place.
    Vec2 aim{1.0f, 0.0f};
    u16 burst_id = 0;
    u32 seed = 0;
};

/// A live Neutrophil NET: a timed slow zone. Its own entity rather than tower
/// state, so it keeps ticking down (and applying kSlowed) independently of
/// the tower that dropped it, including across an upgrade or sell.
struct ActiveNet {
    Vec2 origin{0.0f, 0.0f};
    f32 radius = 0.0f;
    f32 remaining = 0.0f;
    EntityId owner{};
};

} // namespace priv

Rect footprint_rect(Vec2 pos, f32 radius) { return Rect{pos - Vec2{radius, radius}, pos + Vec2{radius, radius}}; }

// ---------------------------------------------------------------------------
// Default stats table (deliverable 1).
// ---------------------------------------------------------------------------

TowerStats make_stats(f32 range, f32 fire_interval, f32 damage, f32 kill_rate, f32 footprint_radius,
                      u32 build_cost, u32 upgrade_cost, f32 ability_cooldown, bool blocks_flow = true) {
    TowerStats s;
    s.range = range;
    s.fire_interval = fire_interval;
    s.damage = damage;
    s.kill_rate = kill_rate;
    s.footprint_radius = footprint_radius;
    s.build_cost = build_cost;
    s.upgrade_cost = upgrade_cost;
    s.ability_cooldown = ability_cooldown;
    s.family_mask = 0xFF; // every tower can affect every family; see report re: tuning scope.
    s.blocks_flow = blocks_flow;
    return s;
}

// ---------------------------------------------------------------------------
// MECHANISM CONSTANTS (Wave 6C), now data.
//
// These are the per-role knobs that have nowhere to live in the frozen
// TowerStats struct: the mortar's burst duty cycle, the T-cell's granule
// release rate and lifetime, the gunner's muzzle velocity and spread, the
// cryo cone's arc, the beam's half-width, the rotor's spin.
//
// They used to be file-static constexpr tables, mirrored by name in
// tests/test_towers.cpp. They now live in a mutable store that
// assets/config/towers.json writes through apply_tower_config()
// (game/towers/TowerMechanics.h). The initial contents below ARE those former
// constants, so a TowerSystem that never sees a config behaves exactly as it
// always did -- and game::default_game_config() reads the shipped JSON's
// mechanics values back out of here, so the two cannot drift.
// ---------------------------------------------------------------------------

/// Index into a `[3]` per-tier table from a 1..3 tier.
u32 tier_slot(u8 tier) { return tier >= 3 ? 2u : (tier == 2 ? 1u : 0u); }

TowerMechanics g_mechanics[kTowerTypeCount][3]{};
TowerGlobals g_globals{};
bool g_mechanics_ready = false;

void init_mechanics_once() {
    if (g_mechanics_ready) return;
    g_mechanics_ready = true;

    // GUNNER: muzzle velocity per tier. Bounded on purpose -- Projectiles.cpp
    // documents that a round whose per-tick step greatly exceeds the spatial
    // cell size (4.0) can tunnel past agents. Even tier 3 steps 1.0 per tick.
    constexpr f32 kGunnerRoundSpeed[3] = {45.0f, 52.0f, 60.0f};
    // MORTAR: burst radius per tier, deliberately far larger than any other
    // tower's footprint -- the "consequential" answer to a clump.
    constexpr f32 kMortarBurstRadius[3] = {5.00f, 5.75f, 6.50f};
    // CRYO: cone half-angle per tier.
    constexpr f32 kCryoArcRadians[3] = {0.60f, 0.66f, 0.72f};
    // TESLA: release rate against lifetime sets the standing granule cloud --
    // 36 per 0.9s over 6.8s is ~270 live granules at tier 1, ~1090 at tier 3,
    // which is why SimDesc::max_swarmers is five figures.
    constexpr u32 kCtlReleasePerShot[3] = {36u, 57u, 78u};
    constexpr f32 kCtlSwarmerLifetime[3] = {6.8f, 7.6f, 8.4f};
    constexpr f32 kCtlSwarmerSpeed[3] = {26.0f, 30.0f, 34.0f};
    constexpr f32 kCtlSwarmerDps[3] = {4.2f, 6.4f, 8.4f};
    // Generous relative to the tower's own range on purpose: a granule already
    // in the field should chase the horde, not expire at the edge of its
    // parent's reach.
    constexpr f32 kCtlSearchRadius[3] = {9.0f, 11.0f, 13.0f};
    // HYDRO: the nozzle, per tier. Thickness and speed both climb, and because
    // the emission rate is derived from swept area (mouth x speed x dt), the
    // two together mean tier 3 puts out roughly 2.2x the fluid tier 1 does --
    // the upgrade is visibly a fatter, faster, further-reaching jet rather than
    // the same jet with a bigger number attached.
    constexpr f32 kHydroNozzleRadius[3] = {0.85f, 1.05f, 1.30f};
    constexpr f32 kHydroJetSpeed[3] = {30.0f, 34.0f, 39.0f};
    // Burst length against TowerStats::fire_interval is the duty cycle. Well
    // under half, so there is always a visible gap where the slug of mucus is
    // in flight and the cell is visibly refilling.
    constexpr f32 kHydroBurstSeconds[3] = {0.28f, 0.34f, 0.40f};
    // Droplet lifetime. This is the "unspawns after a while" rule, and it is
    // deliberately short: long enough for a splash to pool and be read, short
    // enough that a board of Goblet Cells never silts up into a permanent lake.
    constexpr f32 kHydroDropletLifetime[3] = {1.6f, 1.9f, 2.2f};
    // How long a STRUCK NAMED AGENT stays weakened. Set comfortably above
    // TowerStats::fire_interval (0.95 / 0.88 / 0.80) so a target the tower keeps
    // hitting never sees the debuff lapse between bursts; a target it loses
    // sight of stops being a free buff to the rest of the roster within a few
    // seconds instead of forever.
    constexpr f32 kHydroMarkSeconds[3] = {2.2f, 2.6f, 3.0f};

    for (u32 tier = 0; tier < 3; ++tier) {
        g_mechanics[static_cast<u32>(TowerType::Neutrophil)][tier].gunner =
            GunnerParams{kGunnerRoundSpeed[tier], 0.45f, 0.045f};
        g_mechanics[static_cast<u32>(TowerType::Macrophage)][tier].mortar =
            MortarParams{0.30f, kMortarBurstRadius[tier], 0.4f, 1.5f};
        g_mechanics[static_cast<u32>(TowerType::Interferon)][tier].cryo =
            CryoParams{kCryoArcRadians[tier], 0.55f, 0.5f, 6u};
        g_mechanics[static_cast<u32>(TowerType::CytotoxicT)][tier].tesla =
            TeslaParams{kCtlReleasePerShot[tier], kCtlSwarmerLifetime[tier],
                        kCtlSwarmerSpeed[tier],   kCtlSwarmerDps[tier],
                        0.55f,                    kCtlSearchRadius[tier],
                        0.85f};
        g_mechanics[static_cast<u32>(TowerType::GobletCell)][tier].hydro =
            HydroParams{kHydroBurstSeconds[tier], kHydroJetSpeed[tier],
                        kHydroNozzleRadius[tier], 0.05f,
                        kHydroDropletLifetime[tier], 1.0f,
                        kHydroMarkSeconds[tier]};
        g_mechanics[static_cast<u32>(TowerType::NKCell)][tier].blade =
            BladeParams{9.0f, 0.0f, 5u};
    }
    g_globals = TowerGlobals{0.7f, 16u, 14.0f};
}

/// Shorthand for the mechanism block a tower is currently running on.
const TowerMechanics& mech(TowerType type, u8 tier) { return tower_mechanics(type, tier); }

// ---------------------------------------------------------------------------
// Cost-curve design goal (DESIGN.md §5.3/§7.1): upgrading one tier must be a
// reliably better ATP-per-output deal than placing a fresh tower, so
// reinforcing a concentrated position beats spreading thin. For every role
// below: tier 2's upgrade_cost is well under a fresh build, tier 2's output
// pushes past 2x tier 1's, tier 3's upgrade_cost is smaller still and its
// output pulls further ahead — accelerating value, decelerating cost.
//
// "Output" is per-role, because the six roles spend different stats:
//   GUNNER  damage / fire_interval          (projectile throughput)
//   MORTAR  kill_rate * burst / interval    (burst duty cycle)
//   CRYO    kill_rate                       (continuous cone)
//   TESLA   kill_rate * arc / interval      (discharge duty cycle)
//   HYDRO   kill_rate * burst / interval    (spray duty cycle)
//   BLADE   kill_rate                       (continuous rotor)
// tests/test_towers.cpp's tower_output() is exactly this table, and proves the
// claim numerically for all six.
// ---------------------------------------------------------------------------

void load_default_stats(TowerSystem& self) {
    // GUNNER — cheapest, longest-uptime, single-target. Its whole identity is
    // rate: 11 rounds/s at tier 1 up to 33/s at tier 3, so the stream reads as
    // continuous rather than as individual shots. kill_rate is 0 by design —
    // the Gunner is the one tower that does NOT publish a damage field.
    self.set_stats(TowerType::Neutrophil, 1, make_stats(18.0f, 0.045f, 1.4f, 0.0f, 1.4f, 70, 45, 6.0f));
    self.set_stats(TowerType::Neutrophil, 2, make_stats(20.0f, 0.0275f, 2.2f, 0.0f, 1.4f, 70, 38, 5.0f));
    self.set_stats(TowerType::Neutrophil, 3, make_stats(22.0f, 0.015f, 2.4f, 0.0f, 1.4f, 70, 0, 4.0f));

    // MORTAR — the longest range and by far the slowest cadence. One shell
    // every 2.6s that erases whatever was standing in a 5-unit circle.
    self.set_stats(TowerType::Macrophage, 1, make_stats(14.0f, 2.60f, 45.0f, 62.0f, 2.0f, 150, 95, 0.0f));
    self.set_stats(TowerType::Macrophage, 2, make_stats(15.5f, 2.30f, 95.0f, 145.0f, 2.0f, 150, 80, 0.0f));
    self.set_stats(TowerType::Macrophage, 3, make_stats(17.0f, 2.00f, 175.0f, 240.0f, 2.0f, 150, 0, 0.0f));

    // CRYO — deliberately the weakest kill_rate in the roster. Its output is
    // crowd control: everything in the cone is slowed, and anything caught deep
    // in it is locked down outright. No active ability (ability_cooldown 0):
    // it used to have a Flash Freeze panic-button nova, but that field always
    // rendered in the generic burst-Circle amber rather than the tower's own
    // cyan, so it read as a stray, unrelated buff flashing on the tower rather
    // than as an extension of the cone it was supposed to belong to.
    self.set_stats(TowerType::Interferon, 1, make_stats(24.0f, 0.55f, 3.0f, 2.0f, 2.4f, 110, 70, 0.0f));
    self.set_stats(TowerType::Interferon, 2, make_stats(27.0f, 0.50f, 7.0f, 5.4f, 2.4f, 110, 60, 0.0f));
    self.set_stats(TowerType::Interferon, 3, make_stats(30.0f, 0.45f, 14.0f, 10.5f, 2.4f, 110, 0, 0.0f));

    // TESLA — short base range but its chain reaches far past it by hopping.
    // Bursty: nothing at all between discharges.
    self.set_stats(TowerType::CytotoxicT, 1, make_stats(7.00f, 0.90f, 30.0f, 60.0f, 1.6f, 130, 84, 0.0f));
    self.set_stats(TowerType::CytotoxicT, 2, make_stats(7.75f, 0.75f, 62.0f, 135.0f, 1.6f, 130, 70, 0.0f));
    self.set_stats(TowerType::CytotoxicT, 3, make_stats(8.50f, 0.60f, 120.0f, 210.0f, 1.6f, 130, 0, 0.0f));

    // HYDRO — area denial that arrives late and lingers. `fire_interval` is the
    // reload between bursts, not a rate of fire, and `kill_rate` is the damage a
    // FULLY soaked patch takes per second, so its real output depends on how
    // much of the lane the fluid ends up covering. It kills slower than the
    // roster average on paper and harder in practice, because everything it
    // touches is also permanently weakened (chaff_flags::kMarked) and takes
    // 50% more from every OTHER tower for the rest of its life. Deliberately
    // does NOT slow: the mucus is a force multiplier for the rest of the
    // roster, not a second root alongside Interferon's cone and Neutrophil's
    // NET. Expensive, and the second-longest reach.
    self.set_stats(TowerType::GobletCell, 1, make_stats(16.0f, 0.95f, 6.0f, 14.0f, 1.4f, 160, 104, 0.0f));
    self.set_stats(TowerType::GobletCell, 2, make_stats(18.0f, 0.88f, 13.0f, 34.0f, 1.4f, 160, 88, 0.0f));
    self.set_stats(TowerType::GobletCell, 3, make_stats(20.0f, 0.80f, 26.0f, 62.0f, 1.4f, 160, 0, 0.0f));

    // BLADE — by far the shortest range in the roster (it is a contact weapon)
    // and by far the highest sustained kill_rate per unit of range. A wall
    // tower: it only works where the horde is forced to walk into it.
    self.set_stats(TowerType::NKCell, 1, make_stats(16.0f, 0.18f, 5.0f, 8.0f, 1.6f, 90, 58, 0.0f));
    self.set_stats(TowerType::NKCell, 2, make_stats(18.0f, 0.14f, 10.0f, 21.0f, 1.6f, 90, 49, 0.0f));
    self.set_stats(TowerType::NKCell, 3, make_stats(20.0f, 0.10f, 20.0f, 41.0f, 1.6f, 90, 0, 0.0f));
}

bool same_stats(const TowerStats& a, const TowerStats& b) {
    return a.range == b.range && a.fire_interval == b.fire_interval && a.damage == b.damage &&
           a.kill_rate == b.kill_rate && a.footprint_radius == b.footprint_radius &&
           a.build_cost == b.build_cost && a.upgrade_cost == b.upgrade_cost &&
           a.ability_cooldown == b.ability_cooldown && a.family_mask == b.family_mask &&
           a.blocks_flow == b.blocks_flow;
}

/// TowerSystem.h forbids adding a constructor or a loaded-flag member, so
/// there is no natural hook to populate `stats_` once per instance. The table
/// is filled lazily instead, the first time it is observed still untouched,
/// which is what lets a bare `TowerSystem ts;` -- in a test, in a bench
/// scenario, in the gym -- be usable without every caller remembering an init
/// call.
///
/// stats() does the actual populating, because it is a member and has direct
/// private access; this free helper only has to touch one row to trigger it.
/// It used to duplicate the check and read stats(Macrophage, 1), which was
/// mutual recursion waiting to happen.
void ensure_default_stats(const TowerSystem& self) {
    (void)self.stats(TowerType::Neutrophil, 1);
}

// ---------------------------------------------------------------------------
// WouldBlockAllPaths heuristic.
// ---------------------------------------------------------------------------

/// Local, bounded-cost stand-in for a full re-route check. TowerSystem.h
/// explicitly leaves the exact approach up to the implementer (it points at
/// FlowField::reachable() and mentions either probing from a spawn point or
/// test-blocking a scratch copy). A *global* scratch copy would need the
/// level's spawn point list, which SimWorld does not store (Level.h's
/// SpawnPoint/placement_zones never get threaded onto SimWorld — see the
/// report for this gap) — so this tests connectivity in a *local* window
/// around the footprint instead, on the theory that a placement can only
/// wall off "every lane" by fully closing the local vessel cross-section it
/// sits in; if a walkable detour exists within a modest margin, the (real,
/// unedited) flow field already had a path through there and will keep
/// routing through it after the rebake. Anchors are seeded only from points
/// currently `reachable()` per the live FlowField, so a placement inside an
/// already-dead-end pocket is never wrongly rejected.
bool would_block_all_paths(const sim::SimWorld& world, const Rect& footprint) {
    const sim::TissueMask& mask = world.tissue();
    const sim::FlowField& flow = world.flow();
    if (mask.width() <= 0 || mask.height() <= 0) return false;

    const f32 cs = math::max(mask.cell_size(), 1e-3f);
    const f32 pad = cs * 14.0f;
    const IVec2 c0raw = mask.world_to_cell(footprint.min - Vec2{pad, pad});
    const IVec2 c1raw = mask.world_to_cell(footprint.max + Vec2{pad, pad});
    const i32 x0 = math::clamp(c0raw.x, 0, mask.width() - 1);
    const i32 y0 = math::clamp(c0raw.y, 0, mask.height() - 1);
    const i32 x1 = math::clamp(c1raw.x, 0, mask.width() - 1);
    const i32 y1 = math::clamp(c1raw.y, 0, mask.height() - 1);
    const i32 w = x1 - x0 + 1;
    const i32 h = y1 - y0 + 1;
    if (w <= 0 || h <= 0) return false;

    const IVec2 f0 = mask.world_to_cell(footprint.min);
    const IVec2 f1 = mask.world_to_cell(footprint.max);
    auto blocked = [&](i32 x, i32 y) { return x >= f0.x && x <= f1.x && y >= f0.y && y <= f1.y; };

    // Transient BFS scratch only — never holds anything meaningful across
    // calls, so reusing capacity via function-local statics is safe even
    // though TowerSystem.h forbids adding persistent per-instance members.
    static thread_local std::vector<u32> visited_gen;
    static thread_local u32 gen = 0;
    static thread_local std::vector<IVec2> stack;
    static thread_local std::vector<IVec2> anchors;
    const usize cell_count = static_cast<usize>(w) * static_cast<usize>(h);
    if (visited_gen.size() < cell_count) visited_gen.assign(cell_count, 0);
    ++gen;
    stack.clear();
    anchors.clear();

    auto local_index = [&](i32 x, i32 y) {
        return static_cast<usize>(y - y0) * static_cast<usize>(w) + static_cast<usize>(x - x0);
    };

    for (i32 y = y0; y <= y1; ++y) {
        for (i32 x = x0; x <= x1; ++x) {
            const bool on_border = (x == x0 || x == x1 || y == y0 || y == y1);
            if (!on_border || !mask.walkable(x, y)) continue;
            if (!flow.reachable(mask.cell_to_world(x, y))) continue;
            anchors.push_back(IVec2{x, y});
        }
    }
    if (anchors.size() < 2) return false; // nothing that could be disconnected

    // DIFFERENTIAL, not absolute: flood the window twice from the same seed,
    // once over the mask as it stands and once with the footprint blocked, and
    // report only anchors the footprint newly cut off.
    //
    // Asking the second flood alone "did every anchor stay connected" answers
    // the wrong question, because a window can contain walls the tower had
    // nothing to do with. A level with an authored obstacle inside a lane
    // (game/level Level.h) puts border anchors on either side of an island
    // whose way round lies outside the window, so an absolute test calls every
    // placement near one WouldBlockAllPaths and the lane becomes unbuildable
    // for no reason. Comparing against the baseline attributes the
    // disconnection to the footprint or to nothing.
    const IVec2 offsets[4] = {IVec2{1, 0}, IVec2{-1, 0}, IVec2{0, 1}, IVec2{0, -1}};
    auto flood_from_first_anchor = [&](bool block_footprint, std::vector<u8>& reached) {
        auto open = [&](i32 x, i32 y) {
            if (!mask.in_range(x, y) || !mask.walkable(x, y)) return false;
            return !(block_footprint && blocked(x, y));
        };
        reached.assign(anchors.size(), 0u);
        ++gen;
        stack.clear();
        if (!open(anchors[0].x, anchors[0].y)) return;
        stack.push_back(anchors[0]);
        visited_gen[local_index(anchors[0].x, anchors[0].y)] = gen;
        while (!stack.empty()) {
            const IVec2 cur = stack.back();
            stack.pop_back();
            for (const IVec2& o : offsets) {
                const i32 nx = cur.x + o.x;
                const i32 ny = cur.y + o.y;
                if (nx < x0 || nx > x1 || ny < y0 || ny > y1 || !open(nx, ny)) continue;
                const usize idx = local_index(nx, ny);
                if (visited_gen[idx] == gen) continue;
                visited_gen[idx] = gen;
                stack.push_back(IVec2{nx, ny});
            }
        }
        for (usize i = 0; i < anchors.size(); ++i) {
            reached[i] = visited_gen[local_index(anchors[i].x, anchors[i].y)] == gen ? 1u : 0u;
        }
    };

    static thread_local std::vector<u8> reached_before;
    static thread_local std::vector<u8> reached_after;
    flood_from_first_anchor(/*block_footprint=*/false, reached_before);
    flood_from_first_anchor(/*block_footprint=*/true, reached_after);

    for (usize i = 1; i < anchors.size(); ++i) {
        if (reached_before[i] != 0u && reached_after[i] == 0u) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Tissue footprint snapshot/restore (place()/sell()).
// ---------------------------------------------------------------------------

void snapshot_and_block(sim::TissueMask& mask, const Rect& footprint, priv::TowerRecord& rec) {
    const IVec2 c0 = mask.world_to_cell(footprint.min);
    const IVec2 c1 = mask.world_to_cell(footprint.max);
    rec.footprint = footprint;
    rec.cell_min = c0;
    rec.cell_dims = IVec2{c1.x - c0.x + 1, c1.y - c0.y + 1};
    const usize n = static_cast<usize>(rec.cell_dims.x) * static_cast<usize>(rec.cell_dims.y);
    rec.saved_walkable.assign(n, 0);
    rec.saved_cost.assign(n, 1.0f);
    for (i32 y = c0.y; y <= c1.y; ++y) {
        for (i32 x = c0.x; x <= c1.x; ++x) {
            const usize idx =
                static_cast<usize>(y - c0.y) * static_cast<usize>(rec.cell_dims.x) + static_cast<usize>(x - c0.x);
            rec.saved_walkable[idx] = mask.walkable(x, y) ? 1u : 0u;
            rec.saved_cost[idx] = mask.cost(x, y);
        }
    }
    sim::block_rect(mask, footprint);
}

void restore_footprint(sim::TissueMask& mask, const priv::TowerRecord& rec) {
    for (i32 y = 0; y < rec.cell_dims.y; ++y) {
        for (i32 x = 0; x < rec.cell_dims.x; ++x) {
            const i32 wx = rec.cell_min.x + x;
            const i32 wy = rec.cell_min.y + y;
            const usize idx = static_cast<usize>(y) * static_cast<usize>(rec.cell_dims.x) + static_cast<usize>(x);
            mask.set_walkable(wx, wy, rec.saved_walkable[idx] != 0);
            mask.set_cost(wx, wy, rec.saved_cost[idx]);
        }
    }
}

// ---------------------------------------------------------------------------
// Wave 6C: shared combat helpers.
//
// THE LOAD-BEARING RULE (DamageField.h's own rationale, restated because it is
// the thing that is easiest to accidentally undo): the five AREA roles never
// touch chaff agent-by-agent to *damage* them. They publish one DamageField and
// let the aggregate damage system thin whatever it overlaps. Cost then scales
// with fields and cells, not with towers x 10,000 agents. Only the Gunner is
// the documented exception, and it uses the projectile store, not a per-agent
// loop either.
//
// What DOES touch agents individually here, and why each is bounded:
//   - acquire_focus()  reads per-cell OCCUPANCY over the cells a range circle
//     overlaps (tens of integers), then averages the positions of exactly ONE
//     cell's agents. Never a scan of the store.
//   - nearest_chaff()  is a generic "closest agent" helper. Bounded by the
//     caller's radius (a handful of cells) and used only on ticks where a
//     tower actually needs one. The Cytotoxic T no longer calls it at all --
//     its granules do their own searching in sim/swarm, on their own budget.
//   - the Cryo and Blade passes set chaff_flags / emit contact events over the
//     agents in their (small) shape. Flags and events have no aggregate path at
//     all — DamageField publishes damage, not state — so this is the only way
//     to express "slowed" or "the rotor touched this one". Both are capped, and
//     both run on a fire_interval pulse, not every tick.
// ---------------------------------------------------------------------------

constexpr u32 kNoIndex = static_cast<u32>(-1);

/// The data model has 3 upgrade tiers; vfx/Particles.cpp's art authoring keys
/// its escalation on visual_id 1..5. Spread the three tiers across that range
/// so tier 3 gets the top-end look (most barrels, most branches, most blades)
/// rather than the middle of it.
u16 tier_visual(u8 tier) { return tier >= 3 ? u16{5} : (tier == 2 ? u16{3} : u16{1}); }

sim::CombatEvent tower_event(sim::CombatEventType type, TowerType source, u8 tier, Vec2 origin) {
    sim::CombatEvent e;
    e.type = type;
    e.source = source;
    e.visual_id = tier_visual(tier);
    e.origin = origin;
    e.secondary = origin;
    return e;
}

bool family_allowed(u8 mask, u8 family) { return (mask & static_cast<u8>(1u << family)) != 0; }

bool chaff_targetable(const sim::ChaffBuffers& chaff, u32 idx, u8 mask) {
    if (idx >= chaff.count()) return false;
    const u8 f = chaff.flags[idx];
    if ((f & sim::chaff_flags::kAlive) == 0) return false;
    if ((f & sim::chaff_flags::kPendingKill) != 0) return false;
    return family_allowed(mask, chaff.family[idx]);
}

/// "Where is the horde, roughly" — the aim point every area tower and the
/// Gunner share. Cost is O(cells overlapping the range circle) for the search
/// plus O(one cell's occupancy) for the refine; it never walks the chaff store.
///
/// Picks the fullest spatial-hash cell that can actually contain an in-range
/// agent (a cell whose NEAREST point is out of range provably cannot), then
/// returns the centroid of that cell's in-range agents. Deterministic: cell
/// scan order is row-major and ties keep the first (lowest-index) cell.
///
/// `out_vel`, when given, receives the mean velocity of the SAME agents the
/// centroid averaged, so a caller with travel time to cover can lead the shot
/// (see lead_aim_point). Everything else ignores it — an area field lands the
/// tick it is published, so it has nothing to lead.
bool acquire_focus(const sim::SimWorld& world, Vec2 origin, f32 range, u8 mask, Vec2& out,
                   Vec2* out_vel = nullptr) {
    if (out_vel) *out_vel = Vec2{0.0f, 0.0f};
    const sim::SpatialHash& hash = world.spatial();
    const sim::ChaffBuffers& chaff = world.chaff();
    if (chaff.count() == 0) return false;
    const IVec2 dims = hash.grid_dims();
    if (dims.x <= 0 || dims.y <= 0) return false;

    const f32 cs = hash.cell_size();
    const Vec2 gmin = hash.bounds().min;
    const IVec2 lo = hash.cell_coord(origin - Vec2{range, range});
    const IVec2 hi = hash.cell_coord(origin + Vec2{range, range});
    const u32* occ = hash.occupancy();
    const f32 r2 = range * range;

    u32 best_cell = kNoIndex;
    u32 best_occ = 0;
    for (i32 y = lo.y; y <= hi.y; ++y) {
        for (i32 x = lo.x; x <= hi.x; ++x) {
            const u32 ci = hash.cell_index(IVec2{x, y});
            const u32 n = occ[ci];
            if (n <= best_occ) continue;
            const f32 cx0 = gmin.x + static_cast<f32>(x) * cs;
            const f32 cy0 = gmin.y + static_cast<f32>(y) * cs;
            const f32 dx = math::clamp(origin.x, cx0, cx0 + cs) - origin.x;
            const f32 dy = math::clamp(origin.y, cy0, cy0 + cs) - origin.y;
            if (dx * dx + dy * dy > r2) continue;
            best_occ = n;
            best_cell = ci;
        }
    }
    if (best_cell == kNoIndex) return false;

    u32 begin = 0, end = 0;
    hash.cell_range(best_cell, begin, end);
    const u32* indices = hash.indices();
    const u32 indexed = static_cast<u32>(hash.indexed_count());
    if (end > indexed) end = indexed;

    Vec2 in_range{0.0f, 0.0f};
    Vec2 anywhere{0.0f, 0.0f};
    Vec2 vel_in{0.0f, 0.0f};
    Vec2 vel_any{0.0f, 0.0f};
    u32 n_in = 0;
    u32 n_any = 0;
    for (u32 s = begin; s < end; ++s) {
        const u32 a = indices[s];
        if (!chaff_targetable(chaff, a, mask)) continue;
        const Vec2 p{chaff.pos_x[a], chaff.pos_y[a]};
        const Vec2 v{chaff.vel_x[a], chaff.vel_y[a]};
        anywhere += p;
        vel_any += v;
        ++n_any;
        if (math::length_sq(p - origin) > r2) continue;
        in_range += p;
        vel_in += v;
        ++n_in;
    }
    if (n_in > 0) {
        out = in_range / static_cast<f32>(n_in);
        if (out_vel) *out_vel = vel_in / static_cast<f32>(n_in);
        return true;
    }
    if (n_any == 0) return false;
    // Every agent in the fullest reachable cell sits just past the radius (its
    // near corner was in range, its agents are not). Aim at the clamped point
    // rather than dropping the target and stuttering.
    const Vec2 c = anywhere / static_cast<f32>(n_any);
    const Vec2 d = c - origin;
    const f32 l = math::length(d);
    out = (l > range && l > math::kEpsilon) ? origin + d * (range / l) : c;
    if (out_vel) *out_vel = vel_any / static_cast<f32>(n_any);
    return true;
}

/// Where to point so a round of `speed` and the target meet, given the target's
/// position and velocity RIGHT NOW. Without this the Gunner shoots at where the
/// horde was when the trigger was pulled, and every round lands one travel-time
/// behind a moving crowd — a constant, very visible lag, not a near-miss.
///
/// Solves |D + V*t| = speed*t for the earliest t >= 0, with D = target - origin:
///
///     (V.V - speed^2) t^2 + 2 (D.V) t + D.D = 0
///
/// `a` is negative whenever the round outruns the target (kGunnerRoundSpeed is
/// 45+ against agents that move single digits, so always, here) and `c` is a
/// squared length, so the roots have opposite signs and exactly one is valid.
/// The degenerate cases — target already on the muzzle, or somehow no positive
/// root — fall back to the unled point, which is the old behaviour.
Vec2 lead_aim_point(Vec2 origin, Vec2 target, Vec2 target_vel, f32 speed) {
    if (speed <= math::kEpsilon) return target;
    const Vec2 d = target - origin;
    const f32 a = math::length_sq(target_vel) - speed * speed;
    const f32 b = 2.0f * (d.x * target_vel.x + d.y * target_vel.y);
    const f32 c = math::length_sq(d);
    if (c <= math::kEpsilon) return target;

    f32 t = -1.0f;
    if (std::fabs(a) <= math::kEpsilon) {
        // Target receding at exactly muzzle speed: the quadratic collapses to a
        // line. Only meets if it is closing on the b term.
        if (std::fabs(b) > math::kEpsilon) t = -c / b;
    } else {
        const f32 disc = b * b - 4.0f * a * c;
        if (disc < 0.0f) return target;
        const f32 root = std::sqrt(disc);
        const f32 t0 = (-b - root) / (2.0f * a);
        const f32 t1 = (-b + root) / (2.0f * a);
        // Earliest non-negative root; they cannot both be negative while a < 0.
        if (t0 >= 0.0f && t1 >= 0.0f) t = math::min(t0, t1);
        else t = math::max(t0, t1);
    }
    if (!(t > 0.0f)) return target;
    return target + target_vel * t;
}

/// Nearest live matching chaff agent to `from` within `radius`, skipping the
/// `n_exclude` indices in `exclude`. Ties resolve to the lowest chaff index, so
/// the walk is a pure function of the store's contents.
u32 nearest_chaff(const sim::SimWorld& world, Vec2 from, f32 radius, u8 mask,
                  const u32* exclude, u32 n_exclude, Vec2& out_pos) {
    static thread_local std::vector<u32> scratch;
    if (scratch.capacity() < 1024) scratch.reserve(1024);
    scratch.clear();
    world.spatial().query_circle(from, radius, scratch);

    const sim::ChaffBuffers& chaff = world.chaff();
    const f32 r2 = radius * radius;
    u32 best = kNoIndex;
    f32 best_d2 = 0.0f;
    for (u32 idx : scratch) {
        if (!chaff_targetable(chaff, idx, mask)) continue;
        bool skip = false;
        for (u32 k = 0; k < n_exclude; ++k) {
            if (exclude[k] == idx) { skip = true; break; }
        }
        if (skip) continue;
        const f32 dx = chaff.pos_x[idx] - from.x;
        const f32 dy = chaff.pos_y[idx] - from.y;
        const f32 d2 = dx * dx + dy * dy;
        if (d2 > r2) continue;
        if (best == kNoIndex || d2 < best_d2) {
            best = idx;
            best_d2 = d2;
            out_pos = Vec2{chaff.pos_x[idx], chaff.pos_y[idx]};
        }
    }
    return best;
}

/// The aim point a tower should face this tick: the chaff focus if there is
/// one, otherwise the nearest named agent, otherwise nothing. `out_vel`, when
/// given, receives that target's velocity for lead_aim_point; it is zero when
/// the target has no Velocity to report.
bool acquire_aim_point(TowerSystem& self, sim::SystemContext& ctx, Vec2 origin,
                       const TowerStats& st, bool detect_hidden, Vec2& out,
                       Vec2* out_vel = nullptr) {
    if (acquire_focus(ctx.world, origin, st.range, st.family_mask, out, out_vel)) return true;
    if (out_vel) *out_vel = Vec2{0.0f, 0.0f};
    const EntityId named = self.find_target(ctx.world, origin, st.range, st.family_mask, detect_hidden);
    if (!named.valid()) return false;
    const entt::entity te = ctx.world.ecs().from_id(named);
    if (!ctx.registry.valid(te) || !ctx.registry.all_of<comp::Transform>(te)) return false;
    out = ctx.registry.get<comp::Transform>(te).position;
    if (out_vel) {
        if (const auto* v = ctx.registry.try_get<comp::Velocity>(te)) *out_vel = v->value;
    }
    return true;
}

/// Every role also hurts named agents when it fires. Armor is a flat reduction
/// per damage event (Components.h), and every one of these is a discrete event
/// (a round, a shell, a discharge, a beam refresh, a rotor pulse), so it is
/// applied straight rather than prorated by dt.
///
/// `mark_seconds` and `owner` are the Goblet Cell's alone: every other caller
/// passes the defaults (0, invalid), which skips the emplace entirely and
/// leaves this function's behaviour for the other five roles byte-for-byte
/// what it always was. Passing a positive `mark_seconds` refreshes
/// comp::Marked on whatever this hit lands on — the named-agent half of the
/// weaken debuff chaff gets from chaff_flags::kMarked (sim/fluid/Fluid.cpp).
/// Decay is system_marked_upkeep's job, not this function's: strike_named runs
/// once per tower per tick and has no business owning a timer's lifecycle.
void strike_named(TowerSystem& self, sim::SystemContext& ctx, comp::Tower& tw, Vec2 origin,
                  const TowerStats& st, bool detect_hidden, f32 mark_seconds = 0.0f,
                  EntityId owner = EntityId{}) {
    if (st.damage <= 0.0f) return;
    const EntityId target = self.find_target(ctx.world, origin, st.range, st.family_mask, detect_hidden);
    if (!target.valid()) return;
    const entt::entity te = ctx.world.ecs().from_id(target);
    if (!ctx.registry.valid(te) || !ctx.registry.all_of<comp::Health>(te)) return;
    comp::Health& hp = ctx.registry.get<comp::Health>(te);
    f32 amount = math::max(0.0f, st.damage - hp.armor);
    if (const auto* mk = ctx.registry.try_get<comp::Marked>(te)) amount *= mk->damage_multiplier;
    const bool was_alive = !hp.dead();
    hp.current -= amount;
    tw.current_target = target;

    // Elite/boss damage is the third damage path (chaff fields and projectiles
    // are the other two) and the only one the aggregate sim counters never see,
    // since named agents carry hit points rather than density. The sink lives on
    // the world's DamageSystem so this function needs no new plumbing of its
    // own; it is null in normal play. See sim/Attribution.h.
    if (sim::DamageAttribution* attribution = ctx.world.damage().attribution()) {
        if (owner.valid()) attribution->record_named(owner, amount, was_alive && hp.dead());
    }

    if (mark_seconds > 0.0f) {
        comp::Marked& mk = ctx.registry.get_or_emplace<comp::Marked>(te);
        mk.remaining = mark_seconds;
        mk.damage_multiplier = chaff_flags::kMarkedDamageMultiplier;
        mk.source = owner;
    }
}

Vec2 heading(f32 radians) { return Vec2{std::cos(radians), std::sin(radians)}; }
Vec2 perp(Vec2 v) { return Vec2{-v.y, v.x}; }

// ---------------------------------------------------------------------------
// GUNNER — Neutrophil. Real projectiles; the only tower that publishes no
// damage field at all.
// ---------------------------------------------------------------------------
void system_gunner(TowerSystem& self, sim::SystemContext& ctx) {
    auto view = ctx.registry.view<comp::Tower, comp::Transform>();
    for (auto e : view) {
        comp::Tower& tw = view.get<comp::Tower>(e);
        if (tw.type != TowerType::Neutrophil) continue;
        comp::Transform& tf = view.get<comp::Transform>(e);
        const TowerStats& st = self.stats(tw.type, tw.tier);

        Vec2 target{};
        Vec2 target_vel{};
        if (!acquire_aim_point(self, ctx, tf.position, st, false, target, &target_vel)) continue;

        const u32 slot = tier_slot(tw.tier);
        const f32 speed = mech(tw.type, tw.tier).gunner.round_speed;
        // Lead from the barrel, not the base. The muzzle sits a footprint out
        // along the aim, so the round has that much less ground to cover and
        // wants a correspondingly shorter lead — hence the second solve once
        // the first one has told us which way the barrel points. Both use the
        // same closed form; the refinement is two dozen flops.
        Vec2 led = lead_aim_point(tf.position, target, target_vel, speed);
        Vec2 aim = math::normalize_safe(led - tf.position);
        if (aim.x == 0.0f && aim.y == 0.0f) aim = heading(tf.rotation);
        const Vec2 muzzle = tf.position + aim * (st.footprint_radius + 0.15f);
        led = lead_aim_point(muzzle, target, target_vel, speed);
        Vec2 shot = math::normalize_safe(led - muzzle);
        if (shot.x == 0.0f && shot.y == 0.0f) shot = aim;

        // The barrel points where it will SHOOT, so the muzzle flash, the round
        // and the turret all agree on screen even while the horde slides past.
        tf.rotation = std::atan2(shot.y, shot.x);
        if (tw.cooldown > 0.0f) continue;

        // EXACTLY ONE Rng draw per round fired, taken before anything that could
        // fail. ProjectileSystem::update deliberately draws nothing, so the sim
        // stream advances once per shot and not once per round-in-flight.
        const f32 jitter = ctx.rng.range_f(-mech(tw.type, tw.tier).gunner.spread, mech(tw.type, tw.tier).gunner.spread);
        const f32 cj = std::cos(jitter);
        const f32 sj = std::sin(jitter);
        const Vec2 dir{shot.x * cj - shot.y * sj, shot.x * sj + shot.y * cj};

        sim::ProjectileSpawnParams round;
        round.position = muzzle;
        round.velocity = dir * speed;
        round.damage = st.damage;
        // Just enough to cross the full range. A round that outlives its
        // usefulness is a store slot another round wanted.
        round.lifetime = (st.range + 2.0f) / speed;
        round.hit_radius = mech(tw.type, tw.tier).gunner.hit_radius;
        round.family_mask = st.family_mask;
        round.owner = ctx.world.ecs().to_id(e);
        round.visual_id = tier_visual(tw.tier);
        ctx.world.projectiles().spawn(round);

        sim::CombatEvent flash = tower_event(sim::CombatEventType::MuzzleFlash, tw.type, tw.tier, muzzle);
        flash.direction = dir;
        flash.radius = mech(tw.type, tw.tier).gunner.hit_radius;
        flash.magnitude = st.damage;
        ctx.world.combat_events().push(flash);

        strike_named(self, ctx, tw, tf.position, st, false, 0.0f, ctx.world.ecs().to_id(e));
        tw.cooldown = st.fire_interval;
    }
}

// ---------------------------------------------------------------------------
// MORTAR — Macrophage. One big Circle burst on a long cooldown, lobbed at the
// densest thing in range.
// ---------------------------------------------------------------------------
void system_mortar(TowerSystem& self, sim::SystemContext& ctx) {
    auto view = ctx.registry.view<comp::Tower, comp::Transform>();
    for (auto e : view) {
        comp::Tower& tw = view.get<comp::Tower>(e);
        if (tw.type != TowerType::Macrophage) continue;
        comp::Transform& tf = view.get<comp::Transform>(e);
        const TowerStats& st = self.stats(tw.type, tw.tier);

        Vec2 target{};
        if (!acquire_aim_point(self, ctx, tf.position, st, false, target)) continue;
        Vec2 aim = math::normalize_safe(target - tf.position);
        if (aim.x == 0.0f && aim.y == 0.0f) aim = heading(tf.rotation);
        tf.rotation = std::atan2(aim.y, aim.x);
        if (tw.cooldown > 0.0f) continue;

        const MortarParams& mortar = mech(tw.type, tw.tier).mortar;
        const f32 radius = mortar.burst_radius;

        sim::DamageField burst;
        burst.shape = sim::FieldShape::Circle;
        burst.origin = target;
        burst.radius = radius;
        burst.kill_rate = st.kill_rate;
        // Mostly flat with a soft edge: a shell that only kills at the exact
        // centre does not read as a shell.
        burst.falloff = mortar.burst_falloff;
        burst.family_mask = st.family_mask;
        burst.marked_multiplier = mortar.marked_multiplier;
        burst.lifetime = mortar.burst_seconds;
        burst.owner = ctx.world.ecs().to_id(e);
        ctx.world.damage().submit(burst);

        sim::CombatEvent lob = tower_event(sim::CombatEventType::MuzzleFlash, tw.type, tw.tier,
                                           tf.position + aim * st.footprint_radius);
        lob.direction = aim;
        lob.magnitude = 1.0f;
        ctx.world.combat_events().push(lob);

        // radius drives the shockwave ring; the particle layer stages the
        // land -> charge -> burst timing itself off this single event.
        sim::CombatEvent boom = tower_event(sim::CombatEventType::Explosion, tw.type, tw.tier, target);
        boom.direction = aim;
        boom.radius = radius;
        boom.magnitude = st.kill_rate * mortar.burst_seconds;
        ctx.world.combat_events().push(boom);

        strike_named(self, ctx, tw, tf.position, st, false, 0.0f, ctx.world.ecs().to_id(e));
        tw.cooldown = st.fire_interval;
    }
}

// ---------------------------------------------------------------------------
// CRYO — Interferon. A persistent Cone field with the roster's lowest kill_rate;
// the point is the slow, and the lockdown deep inside the cone.
// ---------------------------------------------------------------------------
void system_cryo(TowerSystem& self, sim::SystemContext& ctx) {
    static thread_local std::vector<u32> scratch;
    if (scratch.capacity() < 2048) scratch.reserve(2048);

    auto view = ctx.registry.view<comp::Tower, comp::Transform>();
    for (auto e : view) {
        comp::Tower& tw = view.get<comp::Tower>(e);
        if (tw.type != TowerType::Interferon) continue;
        comp::Transform& tf = view.get<comp::Transform>(e);
        const TowerStats& st = self.stats(tw.type, tw.tier);
        const CryoParams& cryo = mech(tw.type, tw.tier).cryo;
        const f32 arc = cryo.arc_radians;

        Vec2 target{};
        Vec2 dir = heading(tf.rotation);
        const bool have_target = acquire_aim_point(self, ctx, tf.position, st, false, target);
        if (have_target) {
            const Vec2 d = math::normalize_safe(target - tf.position);
            if (d.x != 0.0f || d.y != 0.0f) dir = d;
            tf.rotation = std::atan2(dir.y, dir.x);
        }

        // Persistent (lifetime <= 0): resubmitted every tick by its owner, per
        // DamageField.h. The signal is continuous even between pulses.
        sim::DamageField cone;
        cone.shape = sim::FieldShape::Cone;
        cone.origin = tf.position;
        cone.direction = dir;
        cone.radius = st.range;
        cone.arc_radians = arc;
        cone.kill_rate = st.kill_rate;
        cone.falloff = 0.5f;
        cone.family_mask = st.family_mask;
        // Weakened chaff (the Goblet Cell's mucus) freezes faster too — same
        // flag every other damage path in the sim reads.
        cone.marked_multiplier = chaff_flags::kMarkedDamageMultiplier;
        cone.lifetime = 0.0f;
        cone.owner = ctx.world.ecs().to_id(e);
        ctx.world.damage().submit(cone);

        if (!have_target || tw.cooldown > 0.0f) continue;

        sim::CombatEvent pulse = tower_event(sim::CombatEventType::ConePulse, tw.type, tw.tier, tf.position);
        pulse.direction = dir;
        pulse.radius = st.range;
        pulse.arc_radians = arc;
        pulse.magnitude = 1.0f;
        ctx.world.combat_events().push(pulse);

        sim::CombatEvent emit = tower_event(sim::CombatEventType::MuzzleFlash, tw.type, tw.tier, tf.position);
        emit.direction = dir;
        ctx.world.combat_events().push(emit);

        // The slow itself. DamageField publishes damage, not state, so there is
        // no aggregate path for a flag — this pass is the only way to express
        // "slowed", and it runs on the pulse cadence, not every tick.
        scratch.clear();
        ctx.world.spatial().query_cone(tf.position, dir, st.range, arc, scratch);
        sim::ChaffBuffers& chaff = ctx.world.chaff();
        const f32 cos_half = std::cos(arc);
        const f32 r2 = st.range * st.range;
        const f32 inner = st.range * cryo.inner_fraction;
        u32 freezes = 0;
        for (u32 idx : scratch) {
            if (!chaff_targetable(chaff, idx, st.family_mask)) continue;
            const Vec2 d = Vec2{chaff.pos_x[idx], chaff.pos_y[idx]} - tf.position;
            const f32 d2 = math::length_sq(d);
            if (d2 > r2) continue;
            const f32 dist = std::sqrt(d2);
            if (dist > math::kEpsilon && (d.x * dir.x + d.y * dir.y) / dist < cos_half) continue;

            const bool was_slowed = (chaff.flags[idx] & sim::chaff_flags::kSlowed) != 0;
            chaff.flags[idx] |= sim::chaff_flags::kSlowed;

            // "Fully locked down" = caught deep in the cone rather than clipped
            // at its fringe. Nothing in the sim clears kSlowed, so this fires at
            // most once per agent, which is what keeps the PINGs a trickle.
            if (was_slowed || dist > inner || freezes >= cryo.max_freeze_events) continue;
            ++freezes;
            sim::CombatEvent frozen = tower_event(sim::CombatEventType::Freeze, tw.type, tw.tier,
                                                  Vec2{chaff.pos_x[idx], chaff.pos_y[idx]});
            frozen.target_family = static_cast<PathogenFamily>(chaff.family[idx]);
            frozen.direction = dir;
            frozen.radius = 0.7f + 0.1f * static_cast<f32>(tw.tier);
            frozen.magnitude = 1.0f;
            ctx.world.combat_events().push(frozen);
        }

        strike_named(self, ctx, tw, tf.position, st, false, 0.0f, ctx.world.ecs().to_id(e));
        tw.cooldown = st.fire_interval;
    }
}

// ---------------------------------------------------------------------------
// SWARM — Cytotoxic T. Releases a volley of lytic granules from its synapse
// electrode on every cooldown; the granules do the rest themselves.
//
// This tower publishes NO DamageField. It is the only anti-chaff tower in the
// roster that does not, and that is the whole point of the redesign: the
// aggregate path can only ever draw a region, and this tower needed to read as
// a population. All of its chaff damage now comes from sim/swarm, one attached
// granule at a time. See kCtl* above for the numbers and sim/swarm/Swarmers.h
// for what a granule does once released.
//
// The tower still needs an aim point, but only to orient the release cone and
// to keep the body sprite facing its work — it does not need a target to hit,
// because it is not hitting anything. When nothing is in range it holds fire
// rather than seeding granules into empty tissue.
// ---------------------------------------------------------------------------
void system_swarm(TowerSystem& self, sim::SystemContext& ctx) {
    auto view = ctx.registry.view<comp::Tower, comp::Transform>();
    for (auto e : view) {
        comp::Tower& tw = view.get<comp::Tower>(e);
        if (tw.type != TowerType::CytotoxicT) continue;
        comp::Transform& tf = view.get<comp::Transform>(e);
        const TowerStats& st = self.stats(tw.type, tw.tier);
        const u32 slot = tier_slot(tw.tier);

        Vec2 aim_point{};
        const bool have_aim = acquire_aim_point(self, ctx, tf.position, st, false, aim_point);
        if (have_aim) {
            const Vec2 d = math::normalize_safe(aim_point - tf.position);
            if (d.x != 0.0f || d.y != 0.0f) tf.rotation = std::atan2(d.y, d.x);
        }
        if (tw.cooldown > 0.0f) continue;
        if (!have_aim) continue;

        const Vec2 aim = math::normalize_safe(aim_point - tf.position);
        const Vec2 facing = (aim.x == 0.0f && aim.y == 0.0f) ? Vec2{1.0f, 0.0f} : aim;

        // Granules leave the tip of the electrode, not the middle of the cell.
        // entity.frag's sdf_cytotoxic puts that tip at local +x 0.50 on a quad
        // drawn at footprint_radius * 2, so the tip is footprint_radius out.
        const Vec2 muzzle = tf.position + facing * st.footprint_radius;

        const TeslaParams& tesla = mech(tw.type, tw.tier).tesla;
        const u32 release = tesla.release_per_shot;
        sim::SwarmerBuffers& swarm = ctx.world.swarmers();

        for (u32 k = 0; k < release; ++k) {
            // Granule identity. Everything stochastic about this release is
            // derived from this one word rather than drawn from the shared sim
            // Rng: a per-granule draw would make every downstream system's
            // numbers depend on the tier of every T-cell on the board.
            u32 gseed = (static_cast<u32>(ctx.tick * 2654435761ull) ^ (k * 0x9E3779B9u) ^
                         static_cast<u32>(ctx.world.ecs().to_id(e).value * 0x85EBCA6Bull)) | 1u;
            const auto draw = [&gseed]() {
                gseed = gseed * 1664525u + 1013904223u;
                return static_cast<f32>((gseed >> 8) & 0xFFFFu) / 65535.0f;   // [0,1]
            };

            // SCATTER the cone, do not fan it evenly. An even fan launched from
            // one point arrives as a crescent of dots — a tidy arc is the one
            // shape a swarm must never make, and it survives speed jitter
            // because every granule still sits on the same expanding circle.
            const f32 offset = (draw() * 2.0f - 1.0f) * tesla.launch_spread;
            const f32 ca = std::cos(offset);
            const f32 sa = std::sin(offset);
            const Vec2 dir{facing.x * ca - facing.y * sa, facing.x * sa + facing.y * ca};

            // Spread the origin across the electrode's mouth too, so a volley
            // does not visibly emanate from a single pixel.
            const Vec2 across{-facing.y, facing.x};
            const Vec2 origin = muzzle + across * ((draw() - 0.5f) * 0.7f)
                                       + facing * ((draw() - 0.5f) * 0.5f);

            // Speed spread on top, so granules launched on the same bearing
            // still separate along it.
            const f32 jitter = 0.60f + 0.80f * draw();

            sim::SwarmerSpawnParams p;
            p.position = origin;
            p.velocity = dir * (tesla.swarmer_speed * jitter);
            p.damage_per_second = tesla.swarmer_dps;
            p.lifetime = tesla.swarmer_lifetime;
            p.attach_radius = tesla.attach_radius;
            p.search_radius = tesla.search_radius;
            p.speed = tesla.swarmer_speed;
            p.family_mask = st.family_mask;
            p.owner = ctx.world.ecs().to_id(e);
            p.visual_id = tw.tier;
            p.seed = gseed;
            swarm.spawn(p);
        }

        // One release event for the VFX layer: the secretion at the electrode.
        // The granules themselves are simulated and drawn from sim state, so
        // there is deliberately no per-granule cosmetic event.
        sim::CombatEvent fired =
            tower_event(sim::CombatEventType::MuzzleFlash, tw.type, tw.tier, muzzle);
        fired.direction = facing;
        fired.magnitude = static_cast<f32>(release);
        ctx.world.combat_events().push(fired);

        strike_named(self, ctx, tw, tf.position, st, false, 0.0f, ctx.world.ecs().to_id(e));
        tw.cooldown = st.fire_interval;
    }
}

// ---------------------------------------------------------------------------
// HYDRO — Goblet Cell. Bursts of real, simulated fluid.
//
// WHAT REPLACED THE BEAM, AND WHY
// This slot used to be the B Cell's LASER: an instantaneous Rect damage field
// along the aim. That field is an AABB (core/Types.h has no rotated rect), so
// the aim had to be snapped to one of four axes or a diagonal beam's bounding
// box would have killed everything nowhere near the visible line. It worked,
// and it was the one tower in the roster whose picture and whose kill zone were
// arguing with each other.
//
// The Goblet Cell submits NO damage field at all. It opens its nozzle for
// `burst_seconds` and hands fluid to sim/fluid/Fluid.h, which owns everything
// after that: where the mucus travels, what it piles against, how it spreads
// when it lands, and what it is still covering three seconds later. Damage
// comes from that layer's coverage grid. There is nothing here for the picture
// to disagree with, because the picture IS the simulation.
//
// WHY THE AIM IS LOCKED FOR THE WHOLE BURST
// The obvious thing is to re-aim every tick while spraying, so the jet tracks.
// It looks terrible: the emitted column fans out across every direction the
// target passed through, arrives spread over an arc, and never accumulates
// enough fluid in one place to pile up and splash. Committing to the aim at
// trigger-pull sends one coherent slug down one line. The tower re-aims freely
// BETWEEN bursts, so it still tracks the horde — just at burst granularity,
// which is the same bargain the Mortar makes with its shell.
// ---------------------------------------------------------------------------
void system_hydro(TowerSystem& self, sim::SystemContext& ctx) {
    auto view = ctx.registry.view<comp::Tower, comp::Transform>();
    for (auto e : view) {
        comp::Tower& tw = view.get<comp::Tower>(e);
        if (tw.type != TowerType::GobletCell) continue;
        comp::Transform& tf = view.get<comp::Transform>(e);
        const TowerStats& st = self.stats(tw.type, tw.tier);
        const HydroParams& hyd = mech(tw.type, tw.tier).hydro;

        // The nozzle is created lazily, on the tower's first Combat tick, so
        // place() does not have to know this component exists.
        priv::Nozzle& noz = ctx.registry.get_or_emplace<priv::Nozzle>(
            e, priv::Nozzle{0.0f, heading(tf.rotation),
                            0u, static_cast<u32>(ctx.world.ecs().to_id(e).value)});

        Vec2 target{};
        const bool have_target = acquire_aim_point(self, ctx, tf.position, st, false, target);

        // ---- Between bursts: track, and pull the trigger when ready -------
        if (noz.burst_remaining <= 0.0f) {
            if (have_target) {
                const Vec2 aim = math::normalize_safe(target - tf.position);
                if (aim.x != 0.0f || aim.y != 0.0f) tf.rotation = std::atan2(aim.y, aim.x);
            }
            if (!have_target || tw.cooldown > 0.0f) continue;

            noz.aim = heading(tf.rotation);
            noz.burst_remaining = hyd.burst_seconds;
            ++noz.burst_id;
            // Reload starts now, not when the burst ends: fire_interval is the
            // whole cycle, so a longer burst eats into its own downtime rather
            // than extending the period and quietly nerfing the tower.
            tw.cooldown = st.fire_interval;

            // One flash per trigger pull. The spray itself is drawn from live
            // fluid state, so there is deliberately no per-tick emission event.
            sim::CombatEvent charge = tower_event(sim::CombatEventType::MuzzleFlash, tw.type,
                                                  tw.tier, tf.position + noz.aim * st.footprint_radius);
            charge.direction = noz.aim;
            charge.magnitude = hyd.jet_speed;
            ctx.world.combat_events().push(charge);

            strike_named(self, ctx, tw, tf.position, st, false, hyd.mark_seconds,
                        ctx.world.ecs().to_id(e));
        }

        // ---- Spraying -----------------------------------------------------
        // The muzzle sits a footprint out along the locked aim so fluid is born
        // clear of the cell body, not inside it — a particle spawned inside the
        // tower would be pushed out by the solver in a random direction and
        // read as the cell leaking.
        sim::FluidJetParams jet;
        jet.origin = tf.position + noz.aim * (st.footprint_radius + hyd.nozzle_radius * 0.5f);
        jet.direction = noz.aim;
        jet.speed = hyd.jet_speed;
        jet.spread = hyd.spread;
        jet.nozzle_radius = hyd.nozzle_radius;
        jet.flow_scale = hyd.flow_scale;
        jet.lifetime = hyd.droplet_lifetime;
        jet.damage_per_second = st.kill_rate;
        jet.family_mask = st.family_mask;
        jet.owner = ctx.world.ecs().to_id(e);
        jet.visual_id = tw.tier;
        jet.burst_id = noz.burst_id;
        // Advanced every tick so consecutive slabs do not leave the nozzle in
        // identical formation, and so the fractional emission rate really
        // dithers instead of quantizing.
        jet.seed = noz.seed;
        noz.seed = noz.seed * 1664525u + 1013904223u;

        ctx.world.fluid_system().emit(ctx.world.fluid(), jet, ctx.dt);
        noz.burst_remaining -= ctx.dt;
    }
}

// Shape ids 0..15 belong to agents and renderer overlays (blob, range ring,
// telegraph diamond, countdown ring, death burst — see entity.frag). Towers
// start here so the two spaces cannot collide.

/// World-space diameter of a tower's body sprite.
///
/// Most towers are drawn at their physical footprint: the sprite IS the lump of
/// cell sitting on the tissue, and its reach is communicated by the separate
/// DamageField / range-ring visuals.
///
/// The NK Cell is the exception. Its whole silhouette is a rotor whose blades
/// sweep the kill disc (system_blade submits a Circle field of st.range every
/// tick), so the blades have to physically reach that far or the visual lies
/// about where the tower kills. Its quad is therefore sized to the FIELD, not
/// the footprint, and entity.frag draws the small cell body as a hub at the
/// centre with the blades spanning out to the rim. This is why the NK sprite
/// must be re-sized on upgrade (see TowerSystem::upgrade) — for every other
/// tower the footprint never changes, but the NK Cell's reach does.
f32 tower_sprite_size(TowerType type, const TowerStats& st) {
    if (type == TowerType::NKCell) return st.range * 2.0f;
    return st.footprint_radius * 2.0f;
}

// ---------------------------------------------------------------------------
// BLADE — NK Cell. A short 360-degree Circle field pinned to the tower, running
// continuously, with a contact slash raised for whatever the rotor passes
// through. The one tower that still sees Burrowed named agents.
// ---------------------------------------------------------------------------
void system_blade(TowerSystem& self, sim::SystemContext& ctx) {
    static thread_local std::vector<u32> scratch;
    if (scratch.capacity() < 1024) scratch.reserve(1024);

    auto view = ctx.registry.view<comp::Tower, comp::Transform>();
    for (auto e : view) {
        comp::Tower& tw = view.get<comp::Tower>(e);
        if (tw.type != TowerType::NKCell) continue;
        comp::Transform& tf = view.get<comp::Transform>(e);
        const TowerStats& st = self.stats(tw.type, tw.tier);

        // The rotor never stops, so its facing is a pure function of elapsed
        // ticks rather than of any target.
        tf.rotation += mech(tw.type, tw.tier).blade.spin_rad_per_sec * ctx.dt;
        if (tf.rotation > math::kTwoPi) tf.rotation -= math::kTwoPi;
        const Vec2 arm = heading(tf.rotation);

        sim::DamageField rotor;
        rotor.shape = sim::FieldShape::Circle;
        rotor.origin = tf.position;
        rotor.radius = st.range;
        rotor.kill_rate = st.kill_rate;
        rotor.falloff = 0.0f;   // contact damage: uniform inside the disc
        rotor.family_mask = st.family_mask;
        rotor.marked_multiplier = chaff_flags::kMarkedDamageMultiplier;
        rotor.lifetime = 0.0f;  // persistent: continuous contact
        rotor.owner = ctx.world.ecs().to_id(e);
        ctx.world.damage().submit(rotor);

        if (tw.cooldown > 0.0f) continue;

        scratch.clear();
        ctx.world.spatial().query_circle(tf.position, st.range, scratch);
        const sim::ChaffBuffers& chaff = ctx.world.chaff();
        const f32 r2 = st.range * st.range;
        u32 slashes = 0;
        for (u32 idx : scratch) {
            if (slashes >= mech(tw.type, tw.tier).blade.max_slash_events) break;
            if (!chaff_targetable(chaff, idx, st.family_mask)) continue;
            const Vec2 p{chaff.pos_x[idx], chaff.pos_y[idx]};
            const Vec2 d = p - tf.position;
            if (math::length_sq(d) > r2) continue;
            ++slashes;
            sim::CombatEvent cut = tower_event(sim::CombatEventType::BladeSlash, tw.type, tw.tier, p);
            // Blade travel at the contact point is tangential, not radial.
            cut.direction = perp(math::normalize_safe(d));
            if (cut.direction.x == 0.0f && cut.direction.y == 0.0f) cut.direction = perp(arm);
            cut.target_family = static_cast<PathogenFamily>(chaff.family[idx]);
            cut.radius = st.range;
            cut.magnitude = 1.0f;
            ctx.world.combat_events().push(cut);
        }

        if (slashes > 0) {
            sim::CombatEvent sweep =
                tower_event(sim::CombatEventType::MuzzleFlash, tw.type, tw.tier, tf.position);
            sweep.direction = arm;
            sweep.radius = st.range;
            ctx.world.combat_events().push(sweep);
        }

        // The ONLY targeting path that may return a Burrowed named agent.
        strike_named(self, ctx, tw, tf.position, st, /*detect_hidden=*/true, 0.0f,
                     ctx.world.ecs().to_id(e));
        tw.cooldown = st.fire_interval;
    }
}

/// Decrements every live Neutrophil NET, applies chaff_flags::kSlowed to
/// whatever it overlaps this tick, and destroys expired NETs. ActiveNet is
/// exclusive to this file, so this fully owns its lifecycle (unlike
/// comp::Ephemeral — see system_ephemeral_drift below).
void system_net_upkeep(sim::SystemContext& ctx) {
    static thread_local std::vector<u32> scratch;
    static thread_local std::vector<entt::entity> expired;
    expired.clear();
    auto view = ctx.registry.view<priv::ActiveNet>();
    for (auto e : view) {
        priv::ActiveNet& net = view.get<priv::ActiveNet>(e);
        net.remaining -= ctx.dt;
        scratch.clear();
        ctx.world.spatial().query_circle(net.origin, net.radius, scratch);
        sim::ChaffBuffers& chaff = ctx.world.chaff();
        const f32 r2 = net.radius * net.radius;
        for (u32 idx : scratch) {
            if (idx >= chaff.count()) continue;
            const f32 dx = chaff.pos_x[idx] - net.origin.x;
            const f32 dy = chaff.pos_y[idx] - net.origin.y;
            if (dx * dx + dy * dy > r2) continue;
            chaff.flags[idx] |= sim::chaff_flags::kSlowed;
        }
        if (net.remaining <= 0.0f) expired.push_back(e);
    }
    for (entt::entity e : expired) ctx.registry.destroy(e);
}

/// Decays every live comp::Marked debuff and removes it once it expires.
/// Marked is a generic, frozen component (sim/ecs/Components.h) that any tower
/// could in principle apply, but the Goblet Cell (system_hydro, via
/// strike_named's mark_seconds parameter) is currently the only producer, so
/// its upkeep lives here next to the mucus system that emplaces it — the same
/// arrangement system_net_upkeep above has with the Neutrophil's NET, and for
/// the same reason: the component itself carries no lifecycle of its own.
///
/// Unlike ActiveNet, this never re-applies anything to chaff — comp::Marked is
/// exclusively the named-agent half of the weaken debuff (chaff_flags::kMarked
/// is the chaff half, and nothing ever clears that one at all; see
/// sim/fluid/Fluid.cpp for why a named agent's version gets a timer instead).
void system_marked_upkeep(sim::SystemContext& ctx) {
    static thread_local std::vector<entt::entity> expired;
    expired.clear();
    auto view = ctx.registry.view<comp::Marked>();
    for (auto e : view) {
        comp::Marked& mk = view.get<comp::Marked>(e);
        mk.remaining -= ctx.dt;
        if (mk.remaining <= 0.0f) expired.push_back(e);
    }
    for (entt::entity e : expired) ctx.registry.remove<comp::Marked>(e);
}

/// Integrates position for Ephemeral entities that also carry Velocity —
/// currently only Neutrophil's micro-units, spawned by trigger_ability.
/// Deliberately does NOT touch comp::Ephemeral::lifetime or destroy anything:
/// Wave 1D's `named::system_named_cleanup` (registered by `named::install`)
/// already owns comp::Ephemeral's decay/expiry for the whole sim. Duplicating
/// that here would double-decrement lifetime and risk destroy()ing an entity
/// twice in the same tick.
void system_ephemeral_drift(sim::SystemContext& ctx) {
    auto view = ctx.registry.view<comp::Ephemeral, comp::Transform, comp::Velocity>();
    for (auto e : view) {
        comp::Transform& tf = view.get<comp::Transform>(e);
        const comp::Velocity& vel = view.get<comp::Velocity>(e);
        tf.position += vel.value * ctx.dt;
    }
}

/// PreUpdate: decays comp::Tower's own cooldown timers. This one legitimately
/// needs no TowerSystem access at all.
void system_tower_cooldowns(sim::SystemContext& ctx) {
    auto view = ctx.registry.view<comp::Tower>();
    for (auto e : view) {
        comp::Tower& tw = view.get<comp::Tower>(e);
        tw.cooldown = math::max(0.0f, tw.cooldown - ctx.dt);
        tw.ability_cooldown = math::max(0.0f, tw.ability_cooldown - ctx.dt);
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Public API (deliverables 2-4).
// ---------------------------------------------------------------------------

const char* tower_type_name(TowerType type) {
    const u32 i = static_cast<u32>(type);
    return i < kTowerTypeCount ? kTowerNames[i] : "unknown";
}

bool parse_tower_type(std::string_view name, TowerType& out) {
    for (u32 i = 0; i < kTowerTypeCount; ++i) {
        if (name == kTowerNames[i]) {
            out = static_cast<TowerType>(i);
            return true;
        }
    }
    return false;
}

const TowerStats& TowerSystem::stats(TowerType type, u8 tier) const {
    // This is where the lazy population actually happens: stats() is a member
    // and has direct private access, so it needs no free helper and cannot
    // recurse into one. ensure_default_stats() just calls through to here.
    //
    // The sentinel is "row [0][0] is still EXACTLY TowerStats{}", not the old
    // "its build_cost is still 100". One field of one row was a real trap once
    // the table became loadable: a towers.json that happened to give the
    // Neutrophil a build cost of 100 would have had its entire table silently
    // replaced by the hardcoded defaults on the next call. Comparing the whole
    // row costs nothing on the hot path -- the very first field differs (range
    // 18 vs 8) for any populated table, so the compare short-circuits
    // immediately -- and leaves no realistic collision.
    if (same_stats(stats_[0][0], TowerStats{})) {
        load_default_stats(const_cast<TowerSystem&>(*this));
    }
    const u32 t = static_cast<u32>(type) < kTowerTypeCount ? static_cast<u32>(type) : 0u;
    const u32 k = (tier >= 1 && tier <= 3) ? static_cast<u32>(tier - 1) : 0u;
    return stats_[t][k];
}

void TowerSystem::set_stats(TowerType type, u8 tier, const TowerStats& s) {
    if (static_cast<u32>(type) >= kTowerTypeCount || tier < 1 || tier > 3) return;
    stats_[static_cast<u32>(type)][tier - 1] = s;
}

PlacementQuery TowerSystem::validate(const sim::SimWorld& world, TowerType type, Vec2 pos, u32 available_atp) const {
    ensure_default_stats(*this);
    PlacementQuery q;
    q.snapped_position = pos;

    const u32 t = static_cast<u32>(type);
    if (t >= kTowerTypeCount) {
        q.result = PlacementResult::NotOnTissue;
        return q;
    }
    const TowerStats& st = stats_[t][0]; // new placements are always tier 1

    const f32 clearance = world.sdf().sample(pos);
    q.clearance = clearance;
    if (clearance <= 0.0f) {
        q.result = PlacementResult::NotOnTissue;
        return q;
    }
    if (clearance < st.footprint_radius) {
        const Vec2 gn = math::normalize_safe(world.sdf().gradient(pos));
        if (gn.x != 0.0f || gn.y != 0.0f) q.snapped_position = pos + gn * (st.footprint_radius - clearance);
        q.result = PlacementResult::InsufficientClearance;
        return q;
    }

    for (const EntityId existing : towers_) {
        const entt::entity e = world.ecs().from_id(existing);
        if (!world.ecs().registry().valid(e) || !world.ecs().registry().all_of<comp::Tower, comp::Transform>(e))
            continue;
        const comp::Tower& etw = world.ecs().registry().get<comp::Tower>(e);
        const comp::Transform& etf = world.ecs().registry().get<comp::Transform>(e);
        const TowerStats& est = stats_[static_cast<u32>(etw.type)][etw.tier - 1];
        const f32 min_dist = st.footprint_radius + est.footprint_radius;
        if (math::length_sq(etf.position - pos) < min_dist * min_dist) {
            q.result = PlacementResult::Overlapping;
            return q;
        }
    }

    if (available_atp < st.build_cost) {
        q.result = PlacementResult::CannotAfford;
        return q;
    }

    // The level's schema-2 allowed_towers list. Checked HERE and not in the HUD
    // so the gym console and the balance bot are bound by it too.
    if (!tower_allowed(type)) {
        q.result = PlacementResult::TowerNotAllowed;
        return q;
    }

    // Buildable area. A level that authors no zones means "anywhere", which is
    // how every shipped level behaved before zones were enforceable at all --
    // so this branch is inert for content that does not opt in.
    //
    // The whole FOOTPRINT has to be inside one zone, not just the centre: a
    // tower half outside the buildable margin is exactly the placement the
    // author drew the rectangle to prevent.
    if (!world.placement_zones().empty()) {
        const Rect fp = footprint_rect(pos, st.footprint_radius);
        bool inside = false;
        for (const Rect& z : world.placement_zones()) {
            if (fp.min.x < z.min.x || fp.min.y < z.min.y) continue;
            if (fp.max.x > z.max.x || fp.max.y > z.max.y) continue;
            inside = true;
            break;
        }
        if (!inside) {
            q.result = PlacementResult::OutsidePlacementZone;
            return q;
        }
    }

    if (st.blocks_flow && would_block_all_paths(world, footprint_rect(pos, st.footprint_radius))) {
        q.result = PlacementResult::WouldBlockAllPaths;
        return q;
    }

    q.result = PlacementResult::Ok;
    return q;
}

EntityId TowerSystem::place(sim::SimWorld& world, TowerType type, Vec2 world_pos) {
    ensure_default_stats(*this);
    // No ATP is threaded through this signature (per the frozen header):
    // affordability is the caller's responsibility via validate(), same as
    // for upgrade()/sell(). Re-validate everything else here so place() is
    // never the caller's only correctness check.
    const PlacementQuery q = validate(world, type, world_pos, /*available_atp=*/0xFFFFFFFFu);
    if (!q.valid()) return EntityId{};

    const u32 t = static_cast<u32>(type);
    const TowerStats& st = stats_[t][0];
    const Rect footprint = footprint_rect(world_pos, st.footprint_radius);

    priv::TowerRecord rec;
    rec.invested_atp = st.build_cost;
    if (st.blocks_flow) {
        snapshot_and_block(world.tissue(), footprint, rec);
        world.flow().mark_dirty(footprint);
    } else {
        rec.footprint = footprint;
    }

    entt::registry& registry = world.ecs().registry();
    const entt::entity e = registry.create();
    registry.emplace<comp::Transform>(e, comp::Transform{world_pos, 0.0f, 1.0f});

    comp::Tower tw;
    tw.type = type;
    tw.tier = 1;
    tw.range = st.range;
    // Spin-up: a tower starts one full fire_interval from its first shot rather
    // than discharging on the tick it is dropped. For the Gunner that is 90ms
    // and invisible; for the Mortar it is the difference between "placed a
    // tower" and "instantly deleted the wave you were about to be hit by", and
    // tests/scripts/tower_thins_horde.json asserts exactly that (no kills on
    // the placement tick).
    tw.cooldown = st.fire_interval;
    tw.fire_interval = st.fire_interval;
    tw.ability_cooldown = 0.0f;
    registry.emplace<comp::Tower>(e, tw);
    // Shape ids start at kTowerShapeBase so tower and overlay id spaces cannot
    // collide.
    //
    // They previously did: atlas_index was the raw TowerType, so the Macrophage
    // (type 1) drew as the range-indicator RING, the Cytotoxic T (3) as the
    // telegraph countdown ring, and slot 4 as an elite death burst.
    // Every tower was wearing some other system's overlay.
    registry.emplace<comp::Sprite>(e, comp::Sprite{Vec4{1.0f, 1.0f, 1.0f, 1.0f}, tower_sprite_size(type, st),
                                                    static_cast<u16>(tower_globals().shape_base + static_cast<u16>(t)),
                                                    /*layer=*/1});
    registry.emplace<priv::TowerRecord>(e, std::move(rec));

    const EntityId id = world.ecs().to_id(e);
    towers_.push_back(id);
    return id;
}

u8 TowerSystem::upgrade(sim::SimWorld& world, EntityId tower) {
    ensure_default_stats(*this);
    entt::registry& registry = world.ecs().registry();
    const entt::entity e = world.ecs().from_id(tower);
    if (!registry.valid(e) || !registry.all_of<comp::Tower>(e)) return 0;

    comp::Tower& tw = registry.get<comp::Tower>(e);
    if (tw.tier >= 3) return 0;

    const TowerStats& cur = stats_[static_cast<u32>(tw.type)][tw.tier - 1];
    const u8 next_tier = static_cast<u8>(tw.tier + 1);
    const TowerStats& next = stats_[static_cast<u32>(tw.type)][next_tier - 1];

    tw.tier = next_tier;
    tw.range = next.range;
    tw.fire_interval = next.fire_interval;

    // Keep the body sprite in step with the new tier's stats. This only
    // actually changes anything for the NK Cell (whose quad tracks st.range so
    // its blades keep spanning the kill disc — see tower_sprite_size); every
    // other tower has a tier-invariant footprint and re-sizes to the same
    // value it already had. Done unconditionally anyway so that a future tower
    // with a growing footprint doesn't silently keep a stale sprite.
    if (auto* sprite = registry.try_get<comp::Sprite>(e)) {
        sprite->size = tower_sprite_size(tw.type, next);
    }

    if (auto* rec = registry.try_get<priv::TowerRecord>(e)) rec->invested_atp += cur.upgrade_cost;
    return next_tier;
}

u32 TowerSystem::upgrade_cost(const sim::SimWorld& world, EntityId tower) const {
    ensure_default_stats(const_cast<TowerSystem&>(*this));
    const entt::registry& registry = world.ecs().registry();
    const entt::entity e = world.ecs().from_id(tower);
    if (!registry.valid(e) || !registry.all_of<comp::Tower>(e)) return 0;
    const comp::Tower& tw = registry.get<comp::Tower>(e);
    if (tw.tier >= 3) return 0;
    // The CURRENT tier's upgrade_cost is the price of leaving it, which is the
    // same field upgrade() adds to invested_atp -- so what the player pays and
    // what a sell refunds are computed from one number, not two.
    return stats_[static_cast<u32>(tw.type)][tw.tier - 1].upgrade_cost;
}

u32 TowerSystem::sell(sim::SimWorld& world, EntityId tower) {
    ensure_default_stats(*this);
    entt::registry& registry = world.ecs().registry();
    const entt::entity e = world.ecs().from_id(tower);
    if (!registry.valid(e)) return 0;

    // Refund fraction: Economy.h (Wave 3A) owns EconomyConfig::refund_fraction
    // but TowerSystem has no reference to an Economy instance (the frozen
    // header gives sell() none), so this mirrors EconomyConfig's own default
    // (0.7) rather than inventing an unrelated number. The caller is expected
    // to actually credit the returned amount to its Economy.
    const f32 refund_fraction = tower_globals().refund_fraction;

    u32 refund = 0;
    if (auto* rec = registry.try_get<priv::TowerRecord>(e)) {
        refund = static_cast<u32>(static_cast<f32>(rec->invested_atp) * refund_fraction);
        if (registry.all_of<comp::Tower>(e)) {
            const comp::Tower& tw = registry.get<comp::Tower>(e);
            if (stats_[static_cast<u32>(tw.type)][0].blocks_flow && !rec->saved_walkable.empty()) {
                restore_footprint(world.tissue(), *rec);
                world.flow().mark_dirty(rec->footprint);
            }
        }
    }

    registry.destroy(e);
    towers_.erase(std::remove(towers_.begin(), towers_.end(), tower), towers_.end());
    return refund;
}

bool TowerSystem::trigger_ability(sim::SimWorld& world, EntityId tower) {
    ensure_default_stats(*this);
    entt::registry& registry = world.ecs().registry();
    const entt::entity e = world.ecs().from_id(tower);
    if (!registry.valid(e) || !registry.all_of<comp::Tower, comp::Transform>(e)) return false;

    comp::Tower& tw = registry.get<comp::Tower>(e);
    if (tw.ability_cooldown > 0.0f) return false;
    const TowerStats& st = stats_[static_cast<u32>(tw.type)][tw.tier - 1];
    if (st.ability_cooldown <= 0.0f) return false; // this type has no active ability

    const comp::Transform& tf = registry.get<comp::Transform>(e);

    switch (tw.type) {
    case TowerType::Neutrophil: {
        // Spawns a few short-lived micro-units that drift outward along the
        // local flow direction, and drops a timed NET slow zone. Both halves
        // of the "swarm response" mechanic on one ability, since the header
        // allows "alternatively/additionally".
        Vec2 dir = math::normalize_safe(world.flow().sample(tf.position));
        if (dir.x == 0.0f && dir.y == 0.0f) dir = Vec2{1.0f, 0.0f};
        for (int i = 0; i < 3; ++i) {
            const f32 jitter = world.rng().range_f(-0.5f, 0.5f);
            const f32 c = std::cos(jitter);
            const f32 s = std::sin(jitter);
            Vec2 vel{dir.x * c - dir.y * s, dir.x * s + dir.y * c};
            vel *= math::max(st.range, 1.0f);

            const entt::entity u = registry.create();
            registry.emplace<comp::Transform>(u, comp::Transform{tf.position, 0.0f, 0.4f});
            registry.emplace<comp::Velocity>(u, comp::Velocity{vel, math::length(vel)});
            registry.emplace<comp::Ephemeral>(u, comp::Ephemeral{1.5f, tower});
            registry.emplace<comp::Sprite>(u, comp::Sprite{Vec4{0.3f, 0.6f, 1.0f, 1.0f}, 0.3f, 100, 2});
        }

        const entt::entity net = registry.create();
        registry.emplace<comp::Transform>(net, comp::Transform{tf.position, 0.0f, 1.0f});
        registry.emplace<comp::Sprite>(net, comp::Sprite{Vec4{0.2f, 0.8f, 0.9f, 0.5f}, st.range, 101, 0});
        registry.emplace<priv::ActiveNet>(net, priv::ActiveNet{tf.position, st.range, 3.0f, tower});
        break;
    }
    default:
        return false; // no active ability defined for this type
    }

    tw.ability_cooldown = st.ability_cooldown;
    return true;
}

EntityId TowerSystem::find_target(const sim::SimWorld& world, Vec2 origin, f32 range, u8 family_mask,
                                  bool require_detect_hidden) const {
    const entt::registry& registry = world.ecs().registry();
    auto view = registry.view<const comp::NamedAgent, const comp::Transform, const comp::Health>();

    entt::entity best = entt::null;
    f32 best_d2 = range * range;
    for (auto e : view) {
        const comp::NamedAgent& agent = view.get<const comp::NamedAgent>(e);
        const u8 fam_bit = static_cast<u8>(1u << static_cast<u8>(agent.family));
        if ((family_mask & fam_bit) == 0) continue;

        const comp::Health& health = view.get<const comp::Health>(e);
        if (health.dead()) continue;

        if (const auto* brain = registry.try_get<comp::AiBrain>(e)) {
            if (brain->state == comp::AiState::Burrowed && !require_detect_hidden) continue;
        }

        const comp::Transform& tf = view.get<const comp::Transform>(e);
        const f32 d2 = math::length_sq(tf.position - origin);
        if (d2 <= best_d2) {
            best_d2 = d2;
            best = e;
        }
    }
    return best == entt::null ? EntityId{} : world.ecs().to_id(best);
}

void TowerSystem::register_systems(sim::SimWorld& world) {
    ensure_default_stats(*this);
    sim::EcsWorld& ecs = world.ecs();

    // Binding to a world means binding to THAT world's entities. App keeps one
    // TowerSystem across every level, so without this the placed-tower list
    // accumulates ids from levels that no longer exist -- ids the new world
    // hands straight back out to its own entities, which is enough to make
    // placement report Overlapping on empty tissue.
    towers_.clear();

    ecs.add_system(sim::SystemPhase::PreUpdate, "tower_cooldowns", 20, &system_tower_cooldowns);

    // Sort keys are the canonical roster order (core/Types.h). Fixed order is a
    // determinism requirement, not a preference: the Gunner draws from the sim
    // Rng when it fires, so any reshuffle here would move every downstream
    // system's numbers.
    ecs.add_system(sim::SystemPhase::Combat, "tower_gunner", 0,
                   [this](sim::SystemContext& ctx) { system_gunner(*this, ctx); });
    ecs.add_system(sim::SystemPhase::Combat, "tower_mortar", 1,
                   [this](sim::SystemContext& ctx) { system_mortar(*this, ctx); });
    ecs.add_system(sim::SystemPhase::Combat, "tower_cryo", 2,
                   [this](sim::SystemContext& ctx) { system_cryo(*this, ctx); });
    ecs.add_system(sim::SystemPhase::Combat, "tower_swarm", 3,
                   [this](sim::SystemContext& ctx) { system_swarm(*this, ctx); });
    ecs.add_system(sim::SystemPhase::Combat, "tower_hydro", 4,
                   [this](sim::SystemContext& ctx) { system_hydro(*this, ctx); });
    ecs.add_system(sim::SystemPhase::Combat, "tower_blade", 5,
                   [this](sim::SystemContext& ctx) { system_blade(*this, ctx); });
    ecs.add_system(sim::SystemPhase::Combat, "tower_net_upkeep", 8, &system_net_upkeep);
    ecs.add_system(sim::SystemPhase::Combat, "marked_upkeep", 9, &system_marked_upkeep);

    ecs.add_system(sim::SystemPhase::Movement, "tower_ephemeral_drift", 50, &system_ephemeral_drift);
}


// ---------------------------------------------------------------------------
// Config application (game/towers/TowerMechanics.h)
// ---------------------------------------------------------------------------

const TowerMechanics& tower_mechanics(TowerType type, u8 tier) {
    init_mechanics_once();
    const u32 t = static_cast<u32>(type) < kTowerTypeCount ? static_cast<u32>(type) : 0u;
    return g_mechanics[t][tier_slot(tier)];
}

const TowerGlobals& tower_globals() {
    init_mechanics_once();
    return g_globals;
}

void apply_tower_config(TowerSystem& towers, const TowerConfig& cfg) {
    init_mechanics_once();
    for (u32 t = 0; t < kTowerTypeCount; ++t) {
        for (u32 tier = 0; tier < 3; ++tier) {
            towers.set_stats(static_cast<TowerType>(t), static_cast<u8>(tier + 1),
                             cfg.stats[t][tier]);
            g_mechanics[t][tier] = cfg.mechanics[t][tier];
        }
    }
    g_globals = cfg.globals;
}

} // namespace immune::game
