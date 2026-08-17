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
// See the "Wave 6C: shared combat helpers" block below for the rules those
// systems are written against — in particular why exactly one of them uses
// projectiles and none of them damage chaff agent-by-agent.
#include "game/towers/TowerSystem.h"

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
    "b_cell",       // LASER
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

/// TowerStats{}'s default-member-initializer build_cost. `stats_[8][3]{}`
/// value-initializes every slot identically to this, so
/// `stats(Macrophage, 1).build_cost == kUnpopulatedBuildCost` is a reliable
/// "load_default_stats() has never populated this instance" sentinel — see
/// ensure_default_stats() below for why a sentinel is needed at all instead
/// of a constructor or a loaded-flag member.
constexpr u32 kUnpopulatedBuildCost = 100;

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
// MECHANISM CONSTANTS (Wave 6C).
//
// These are the per-role knobs that have nowhere to live in the frozen
// TowerStats struct. They are mirrored, with the same names, in
// tests/test_towers.cpp — the upgrade-economics test has to reconstruct each
// role's real per-second output, and for the two BURST roles that means
// knowing the burst's duty cycle, not just its instantaneous kill_rate.
// ---------------------------------------------------------------------------

/// MORTAR: how long the digestive burst's Circle field lives. Total density
/// removed per shell is kill_rate * this.
constexpr f32 kMortarBurstSeconds = 0.30f;
/// MORTAR: burst radius per tier. Deliberately far larger than any other
/// tower's footprint — the mortar is the "consequential" answer to a clump.
constexpr f32 kMortarBurstRadius[3] = {5.00f, 5.75f, 6.50f};

/// CYTOTOXIC T — the swarm. This tower used to discharge a Chain field: one
/// disc of damage, one flash, done. The disc was the problem. An area glow says
/// "this region is being hurt" and says nothing at all about a cell that kills
/// other cells one at a time, which is the entire identity of a CTL.
///
/// It now releases SWARMERS (sim/swarm/Swarmers.h) — individual lytic granules
/// that fly out of the synapse electrode, pick a pathogen, latch on, drain it,
/// and move to the next one when it dies. Each dissolves after its own
/// lifetime, and the tower keeps releasing more the whole time, so the cloud
/// settles at a standing population instead of growing without bound.
///
/// The equilibrium size is what the player actually reads, and it is just
/// (release rate) * (lifetime): at tier 1 that is 36 per 0.9s * 6.8s ~ 270 live
/// granules, at tier 3 it is 78 per 0.6s * 8.4s ~ 1090. A tier-3 T-cell is
/// therefore not "a small horde of its own" any more — it is a horde outright,
/// and a board of them is the reason SimDesc::max_swarmers is five figures.
///
/// Counts and per-granule damage move as a PAIR, and this tuning deliberately
/// moves BOTH up: 3x the release rate, 2x the lifetime, 2x the damage and 2x
/// the speed. That is ~6x the standing population; MEASURED kill throughput
/// against a finite horde went 241 -> 750 per 300 ticks, i.e. about 3x, not 6x,
/// because a tower that has cleared its own range holds fire and the surplus
/// granules find nothing to drain. The ceiling is far higher than the observed
/// figure and only shows up under sustained pressure.
///
/// This makes the Cytotoxic T the strongest anti-chaff tower in the roster by a
/// clear margin (Interferon 574, Macrophage 137 on the same scenario). That is
/// an explicit balance decision, not an accident of chasing the look — and if
/// it ever needs pulling back the honest lever is dps, because cutting the
/// count also cuts the spectacle that motivated the whole design.
constexpr u32 kCtlReleasePerShot[3] = {36u, 57u, 78u};
constexpr f32 kCtlSwarmerLifetime[3] = {6.8f, 7.6f, 8.4f};
/// Travel speed. Fast enough that granules cross the tower's whole range in
/// well under a second, so the cloud reads as darting rather than drifting.
constexpr f32 kCtlSwarmerSpeed[3] = {26.0f, 30.0f, 34.0f};
/// Density drained per second by ONE attached granule.
constexpr f32 kCtlSwarmerDps[3] = {4.2f, 6.4f, 8.4f};
/// How close a granule gets before it latches, and how far it will look for a
/// new host once it is loose. The search radius is generous relative to the
/// tower's own range on purpose — a granule already in the field should chase
/// the horde rather than expire politely at the edge of its parent's reach.
constexpr f32 kCtlAttachRadius = 0.55f;
constexpr f32 kCtlSearchRadius[3] = {9.0f, 11.0f, 13.0f};
/// Launch cone half-angle. Wide, because a tight cone reads as a burst of
/// bullets and a wide one reads as a cloud being released.
constexpr f32 kCtlLaunchSpread = 0.85f;

/// GUNNER: muzzle velocity per tier, in world units/second. Bounded on purpose:
/// Projectiles.cpp documents that a round whose per-tick step greatly exceeds
/// the spatial-hash cell size (default 4.0) can tunnel past agents. At 60 Hz
/// even the tier-3 figure steps 1.0 units per tick — a quarter of a cell.
constexpr f32 kGunnerRoundSpeed[3] = {45.0f, 52.0f, 60.0f};
constexpr f32 kGunnerHitRadius = 0.45f;
/// GUNNER: muzzle spread half-angle, in radians. The ONLY sim-RNG draw any
/// tower makes per tick, and exactly one draw per round fired.
constexpr f32 kGunnerSpread = 0.045f;

/// CRYO: cone half-angle per tier, and the fraction of the cone's reach inside
/// which a newly-caught agent counts as fully encased rather than merely slowed.
constexpr f32 kCryoArcRadians[3] = {0.60f, 0.66f, 0.72f};
constexpr f32 kCryoInnerFraction = 0.55f;
/// CRYO: cap on Freeze events per pulse. A cosmetic bound only — every agent in
/// the cone is still slowed; this just stops one pulse into a 10k horde from
/// filling the whole event sink with PINGs.
constexpr u32 kCryoMaxFreezeEvents = 6;

/// LASER: beam half-thickness per tier. The beam's Rect is an AABB (that is
/// what DamageField::rect is), which is why the aim is axis-snapped — see
/// system_laser.
constexpr f32 kLaserHalfWidth[3] = {0.50f, 0.60f, 0.70f};

/// BLADE: rotor angular velocity, and the per-pulse cap on BladeSlash events
/// (same cosmetic-bound rationale as the Cryo cap).
constexpr f32 kBladeSpinRadPerSec = 9.0f;
constexpr u32 kBladeMaxSlashEvents = 5;

/// Index into a `[3]` per-tier constant table from a 1..3 tier.
u32 tier_slot(u8 tier) { return tier >= 3 ? 2u : (tier == 2 ? 1u : 0u); }

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
//   LASER   kill_rate                       (continuous beam)
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
    // in it is locked down outright.
    self.set_stats(TowerType::Interferon, 1, make_stats(24.0f, 0.55f, 3.0f, 2.0f, 2.4f, 110, 70, 12.0f));
    self.set_stats(TowerType::Interferon, 2, make_stats(27.0f, 0.50f, 7.0f, 5.4f, 2.4f, 110, 60, 10.0f));
    self.set_stats(TowerType::Interferon, 3, make_stats(30.0f, 0.45f, 14.0f, 10.5f, 2.4f, 110, 0, 8.0f));

    // TESLA — short base range but its chain reaches far past it by hopping.
    // Bursty: nothing at all between discharges.
    self.set_stats(TowerType::CytotoxicT, 1, make_stats(7.00f, 0.90f, 30.0f, 60.0f, 1.6f, 130, 84, 0.0f));
    self.set_stats(TowerType::CytotoxicT, 2, make_stats(7.75f, 0.75f, 62.0f, 135.0f, 1.6f, 130, 70, 0.0f));
    self.set_stats(TowerType::CytotoxicT, 3, make_stats(8.50f, 0.60f, 120.0f, 210.0f, 1.6f, 130, 0, 0.0f));

    // LASER — the longest *continuous* reach and the most expensive. Pierces
    // everything on the line at once, but only along one axis at a time.
    self.set_stats(TowerType::BCell, 1, make_stats(16.0f, 0.30f, 6.0f, 6.0f, 1.4f, 160, 104, 0.0f));
    self.set_stats(TowerType::BCell, 2, make_stats(18.0f, 0.26f, 13.0f, 16.0f, 1.4f, 160, 88, 0.0f));
    self.set_stats(TowerType::BCell, 3, make_stats(20.0f, 0.22f, 26.0f, 31.0f, 1.4f, 160, 0, 0.0f));

    // BLADE — by far the shortest range in the roster (it is a contact weapon)
    // and by far the highest sustained kill_rate per unit of range. A wall
    // tower: it only works where the horde is forced to walk into it.
    self.set_stats(TowerType::NKCell, 1, make_stats(16.0f, 0.18f, 5.0f, 8.0f, 1.6f, 90, 58, 0.0f));
    self.set_stats(TowerType::NKCell, 2, make_stats(18.0f, 0.14f, 10.0f, 21.0f, 1.6f, 90, 49, 0.0f));
    self.set_stats(TowerType::NKCell, 3, make_stats(20.0f, 0.10f, 20.0f, 41.0f, 1.6f, 90, 0, 0.0f));
}

/// TowerSystem.h forbids adding a constructor, so there is no natural hook to
/// populate `stats_` with real per-type/tier values once per instance. This
/// checks the sentinel described at kUnpopulatedBuildCost and lazily
/// populates the whole table the first time any public entry point observes
/// it still holds the header's uniform default. Idempotent; safe to call
/// from const methods via const_cast because the underlying TowerSystem
/// object is never actually const in practice (App owns a plain member; the
/// tests below construct plain locals).
void ensure_default_stats(const TowerSystem& self) {
    if (self.stats(TowerType::Macrophage, 1).build_cost != kUnpopulatedBuildCost) return;
    load_default_stats(const_cast<TowerSystem&>(self));
}

// ---------------------------------------------------------------------------
// WouldBlockAllPaths heuristic.
// ---------------------------------------------------------------------------

/// Local, bounded-cost stand-in for a full re-route check. TowerSystem.h
/// explicitly leaves the exact approach up to the implementer (it points at
/// FlowField::reachable() and mentions either probing from a portal or
/// test-blocking a scratch copy). A *global* scratch copy would need the
/// level's portal list, which SimWorld does not store (Level.h's
/// SpawnPortal/placement_zones never get threaded onto SimWorld — see the
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
    auto passable = [&](i32 x, i32 y) { return mask.in_range(x, y) && !blocked(x, y) && mask.walkable(x, y); };

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

    stack.push_back(anchors[0]);
    visited_gen[local_index(anchors[0].x, anchors[0].y)] = gen;
    const IVec2 offsets[4] = {IVec2{1, 0}, IVec2{-1, 0}, IVec2{0, 1}, IVec2{0, -1}};
    while (!stack.empty()) {
        const IVec2 cur = stack.back();
        stack.pop_back();
        for (const IVec2& o : offsets) {
            const i32 nx = cur.x + o.x;
            const i32 ny = cur.y + o.y;
            if (nx < x0 || nx > x1 || ny < y0 || ny > y1 || !passable(nx, ny)) continue;
            const usize idx = local_index(nx, ny);
            if (visited_gen[idx] == gen) continue;
            visited_gen[idx] = gen;
            stack.push_back(IVec2{nx, ny});
        }
    }

    for (usize i = 1; i < anchors.size(); ++i) {
        if (visited_gen[local_index(anchors[i].x, anchors[i].y)] != gen) return true;
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
void strike_named(TowerSystem& self, sim::SystemContext& ctx, comp::Tower& tw, Vec2 origin,
                  const TowerStats& st, bool detect_hidden) {
    if (st.damage <= 0.0f) return;
    const EntityId target = self.find_target(ctx.world, origin, st.range, st.family_mask, detect_hidden);
    if (!target.valid()) return;
    const entt::entity te = ctx.world.ecs().from_id(target);
    if (!ctx.registry.valid(te) || !ctx.registry.all_of<comp::Health>(te)) return;
    comp::Health& hp = ctx.registry.get<comp::Health>(te);
    f32 amount = math::max(0.0f, st.damage - hp.armor);
    if (const auto* mk = ctx.registry.try_get<comp::Marked>(te)) amount *= mk->damage_multiplier;
    hp.current -= amount;
    tw.current_target = target;
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
        const f32 speed = kGunnerRoundSpeed[slot];
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
        const f32 jitter = ctx.rng.range_f(-kGunnerSpread, kGunnerSpread);
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
        round.hit_radius = kGunnerHitRadius;
        round.family_mask = st.family_mask;
        round.owner = ctx.world.ecs().to_id(e);
        round.visual_id = tier_visual(tw.tier);
        ctx.world.projectiles().spawn(round);

        sim::CombatEvent flash = tower_event(sim::CombatEventType::MuzzleFlash, tw.type, tw.tier, muzzle);
        flash.direction = dir;
        flash.radius = kGunnerHitRadius;
        flash.magnitude = st.damage;
        ctx.world.combat_events().push(flash);

        strike_named(self, ctx, tw, tf.position, st, false);
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

        const f32 radius = kMortarBurstRadius[tier_slot(tw.tier)];

        sim::DamageField burst;
        burst.shape = sim::FieldShape::Circle;
        burst.origin = target;
        burst.radius = radius;
        burst.kill_rate = st.kill_rate;
        // Mostly flat with a soft edge: a shell that only kills at the exact
        // centre does not read as a shell.
        burst.falloff = 0.4f;
        burst.family_mask = st.family_mask;
        burst.marked_multiplier = 1.5f;
        burst.lifetime = kMortarBurstSeconds;
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
        boom.magnitude = st.kill_rate * kMortarBurstSeconds;
        ctx.world.combat_events().push(boom);

        strike_named(self, ctx, tw, tf.position, st, false);
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
        const f32 arc = kCryoArcRadians[tier_slot(tw.tier)];

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
        const f32 inner = st.range * kCryoInnerFraction;
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
            if (was_slowed || dist > inner || freezes >= kCryoMaxFreezeEvents) continue;
            ++freezes;
            sim::CombatEvent frozen = tower_event(sim::CombatEventType::Freeze, tw.type, tw.tier,
                                                  Vec2{chaff.pos_x[idx], chaff.pos_y[idx]});
            frozen.target_family = static_cast<PathogenFamily>(chaff.family[idx]);
            frozen.direction = dir;
            frozen.radius = 0.7f + 0.1f * static_cast<f32>(tw.tier);
            frozen.magnitude = 1.0f;
            ctx.world.combat_events().push(frozen);
        }

        strike_named(self, ctx, tw, tf.position, st, false);
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

        const u32 release = kCtlReleasePerShot[slot];
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
            const f32 offset = (draw() * 2.0f - 1.0f) * kCtlLaunchSpread;
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
            p.velocity = dir * (kCtlSwarmerSpeed[slot] * jitter);
            p.damage_per_second = kCtlSwarmerDps[slot];
            p.lifetime = kCtlSwarmerLifetime[slot];
            p.attach_radius = kCtlAttachRadius;
            p.search_radius = kCtlSearchRadius[slot];
            p.speed = kCtlSwarmerSpeed[slot];
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

        strike_named(self, ctx, tw, tf.position, st, false);
        tw.cooldown = st.fire_interval;
    }
}

// ---------------------------------------------------------------------------
// LASER — B-Cell. A long thin Rect along the aim, piercing everything on the
// line at once.
//
// AXIS SNAP, and why it is not a shortcut: DamageField::rect is a `Rect`, which
// is an axis-aligned box (core/Types.h) and is tested as one by
// DamageField.cpp's test_rect. There is no rotated-rect shape in the frozen
// contract, and the AABB of a diagonal beam is a huge square that would hit
// everything nowhere near the line. So the aim is snapped to the nearest of the
// four axes and the field is then EXACTLY the beam the player sees. The tower
// re-aims as the horde moves; it simply commits to one axis at a time.
// ---------------------------------------------------------------------------
Vec2 axis_snap(Vec2 v) {
    if (v.x == 0.0f && v.y == 0.0f) return Vec2{1.0f, 0.0f};
    if (std::abs(v.x) >= std::abs(v.y)) return Vec2{v.x >= 0.0f ? 1.0f : -1.0f, 0.0f};
    return Vec2{0.0f, v.y >= 0.0f ? 1.0f : -1.0f};
}

void system_laser(TowerSystem& self, sim::SystemContext& ctx) {
    auto view = ctx.registry.view<comp::Tower, comp::Transform>();
    for (auto e : view) {
        comp::Tower& tw = view.get<comp::Tower>(e);
        if (tw.type != TowerType::BCell) continue;
        comp::Transform& tf = view.get<comp::Transform>(e);
        const TowerStats& st = self.stats(tw.type, tw.tier);
        const f32 half_w = kLaserHalfWidth[tier_slot(tw.tier)];

        Vec2 target{};
        const bool have_target = acquire_aim_point(self, ctx, tf.position, st, false, target);
        const Vec2 dir = have_target ? axis_snap(target - tf.position) : axis_snap(heading(tf.rotation));
        tf.rotation = std::atan2(dir.y, dir.x);

        const Vec2 tip = tf.position + dir * st.range;
        // Thickness is perpendicular to the (axis-aligned) beam only.
        const Vec2 thickness{dir.x != 0.0f ? 0.0f : half_w, dir.y != 0.0f ? 0.0f : half_w};
        sim::DamageField beam;
        beam.shape = sim::FieldShape::Rect;
        beam.rect = Rect{Vec2{math::min(tf.position.x, tip.x), math::min(tf.position.y, tip.y)} - thickness,
                         Vec2{math::max(tf.position.x, tip.x), math::max(tf.position.y, tip.y)} + thickness};
        beam.kill_rate = st.kill_rate;
        beam.falloff = 0.0f;   // a beam is as lethal at its tip as at its muzzle
        beam.family_mask = st.family_mask;
        beam.marked_multiplier = 1.5f;
        beam.lifetime = 0.0f;  // persistent: continuous fire, resubmitted each tick
        beam.owner = ctx.world.ecs().to_id(e);
        ctx.world.damage().submit(beam);

        if (!have_target || tw.cooldown > 0.0f) continue;

        const Vec2 muzzle = tf.position + dir * (st.footprint_radius + 0.1f);
        sim::CombatEvent fired = tower_event(sim::CombatEventType::BeamFired, tw.type, tw.tier, muzzle);
        fired.secondary = tip;
        fired.direction = dir;
        fired.radius = half_w;
        fired.magnitude = st.kill_rate;
        ctx.world.combat_events().push(fired);

        sim::CombatEvent charge = tower_event(sim::CombatEventType::MuzzleFlash, tw.type, tw.tier, muzzle);
        charge.direction = dir;
        ctx.world.combat_events().push(charge);

        strike_named(self, ctx, tw, tf.position, st, false);
        tw.cooldown = st.fire_interval;
    }
}

// Shape ids 0..15 belong to agents and renderer overlays (blob, range ring,
// telegraph diamond, countdown ring, death burst — see entity.frag). Towers
// start here so the two spaces cannot collide.
constexpr u16 kTowerShapeBase = 16;

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
        tf.rotation += kBladeSpinRadPerSec * ctx.dt;
        if (tf.rotation > math::kTwoPi) tf.rotation -= math::kTwoPi;
        const Vec2 arm = heading(tf.rotation);

        sim::DamageField rotor;
        rotor.shape = sim::FieldShape::Circle;
        rotor.origin = tf.position;
        rotor.radius = st.range;
        rotor.kill_rate = st.kill_rate;
        rotor.falloff = 0.0f;   // contact damage: uniform inside the disc
        rotor.family_mask = st.family_mask;
        rotor.marked_multiplier = 1.5f;
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
            if (slashes >= kBladeMaxSlashEvents) break;
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
        strike_named(self, ctx, tw, tf.position, st, /*detect_hidden=*/true);
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
    // NOTE: deliberately does NOT call the free ensure_default_stats() helper
    // here — that helper itself calls stats(Macrophage, 1) to read the
    // sentinel, and stats() calling back into ensure_default_stats() would be
    // infinite mutual recursion (this bit a first draft: SIGSEGV stack
    // overflow on the very first stats() call). Since stats() is a genuine
    // member function it has direct private access to stats_ and can do the
    // same sentinel check inline instead.
    if (stats_[0][0].build_cost == kUnpopulatedBuildCost) {
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

    // NOTE: PlacementResult::OutsidePlacementZone is never returned.
    // LevelDef::placement_zones (game/level/Level.h) is never threaded onto
    // sim::SimWorld, so TowerSystem has no data source for it. See the report.

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
    // telegraph countdown ring, and the B-Cell (4) as an elite death burst.
    // Every tower was wearing some other system's overlay.
    registry.emplace<comp::Sprite>(e, comp::Sprite{Vec4{1.0f, 1.0f, 1.0f, 1.0f}, tower_sprite_size(type, st),
                                                    static_cast<u16>(kTowerShapeBase + static_cast<u16>(t)),
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
    constexpr f32 kRefundFraction = 0.7f;

    u32 refund = 0;
    if (auto* rec = registry.try_get<priv::TowerRecord>(e)) {
        refund = static_cast<u32>(static_cast<f32>(rec->invested_atp) * kRefundFraction);
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
    case TowerType::Interferon: {
        // FLASH FREEZE. The cone is the Cryo's per-tick job; the ability is the
        // panic button: a full 360-degree lockdown of everything in range, plus
        // a short high-rate field so it is not purely cosmetic.
        sim::DamageField nova;
        nova.shape = sim::FieldShape::Circle;
        nova.origin = tf.position;
        nova.radius = st.range;
        nova.kill_rate = st.kill_rate * 4.0f;
        nova.falloff = 0.0f;
        nova.family_mask = st.family_mask;
        nova.lifetime = 0.35f;
        nova.owner = tower;
        world.damage().submit(nova);

        static thread_local std::vector<u32> scratch;
        if (scratch.capacity() < 2048) scratch.reserve(2048);
        scratch.clear();
        world.spatial().query_circle(tf.position, st.range, scratch);
        sim::ChaffBuffers& chaff = world.chaff();
        const f32 r2 = st.range * st.range;
        u32 freezes = 0;
        for (u32 idx : scratch) {
            if (idx >= chaff.count()) continue;
            if ((chaff.flags[idx] & sim::chaff_flags::kAlive) == 0) continue;
            const Vec2 d = Vec2{chaff.pos_x[idx], chaff.pos_y[idx]} - tf.position;
            if (math::length_sq(d) > r2) continue;
            const bool was_slowed = (chaff.flags[idx] & sim::chaff_flags::kSlowed) != 0;
            chaff.flags[idx] |= sim::chaff_flags::kSlowed;
            if (was_slowed || freezes >= 12u) continue;
            ++freezes;
            sim::CombatEvent frozen;
            frozen.type = sim::CombatEventType::Freeze;
            frozen.source = TowerType::Interferon;
            frozen.visual_id = tier_visual(tw.tier);
            frozen.origin = Vec2{chaff.pos_x[idx], chaff.pos_y[idx]};
            frozen.secondary = frozen.origin;
            frozen.target_family = static_cast<PathogenFamily>(chaff.family[idx]);
            frozen.radius = 0.9f;
            world.combat_events().push(frozen);
        }
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
    ecs.add_system(sim::SystemPhase::Combat, "tower_laser", 4,
                   [this](sim::SystemContext& ctx) { system_laser(*this, ctx); });
    ecs.add_system(sim::SystemPhase::Combat, "tower_blade", 5,
                   [this](sim::SystemContext& ctx) { system_blade(*this, ctx); });
    ecs.add_system(sim::SystemPhase::Combat, "tower_net_upkeep", 8, &system_net_upkeep);

    ecs.add_system(sim::SystemPhase::Movement, "tower_ephemeral_drift", 50, &system_ephemeral_drift);
}

} // namespace immune::game
