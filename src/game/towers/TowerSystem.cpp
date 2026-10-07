// Normal play deploys one persistent immune cell per placement. This system
// validates its location, configures its swarmer profile, and records a stable
// attribution handle. Swarmers.h owns its movement and combat thereafter.
// Legacy tower creation/release methods remain for gym commands and old tests;
// the HUD and autoplay use validate_deploy()/deploy() instead.
#include "game/towers/TowerSystem.h"

#include "game/towers/TowerMechanics.h"
#include "game/economy/Economy.h"

#include "core/Math.h"
#include "core/Rng.h"
#include "sim/CombatEvents.h"
#include "sim/SimWorld.h"
#include "sim/chaff/ChaffBuffers.h"
#include "sim/ecs/Components.h"
#include "sim/ecs/EcsWorld.h"
#include "sim/flowfield/FlowField.h"
#include "sim/flowfield/LaneConnectivity.h"
#include "sim/flowfield/RuntimeBlock.h"
#include "sim/scar/Scars.h"
#include "sim/swarm/Swarmers.h"
#include "sim/spatial/SpatialHash.h"
#include "vfx/DeathVfx.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

namespace immune::game {

// TowerSystem.h refers to sim::SimWorld etc. with an explicit `sim::`
// qualifier throughout, which this file keeps for anything declared in the
// header's own signatures. Everything below additionally needs
// `sim::comp::*`, `sim::EcsWorld`, `sim::SystemContext`, `sim::chaff_flags`,
// and friends constantly enough that an unqualified `comp::Tower` etc. is far
// more readable -- hence this using-directive, exactly like the sim-side test
// files do.
using namespace immune::sim;

namespace {

// Order must match TowerType's declaration order exactly.
constexpr const char* kTowerNames[kTowerTypeCount] = {
    "neutrophil",   // SHOOTER
    "macrophage",   // ARBOR GRABBER
    "cytotoxic_t",  // LATCH
    "goblet_cell",  // MUCUS BOMBER
    "fibroblast"};  // BUILDER

// ---------------------------------------------------------------------------
// Private auxiliary ECS components (see file header comment).
// ---------------------------------------------------------------------------
namespace priv {

/// Attached to every tower entity at place() time. Carries what sell() needs
/// to refund ATP.
struct TowerRecord {
    u32 invested_atp = 0;
};

} // namespace priv

Rect footprint_rect(Vec2 pos, f32 radius) { return Rect{pos - Vec2{radius, radius}, pos + Vec2{radius, radius}}; }

// ---------------------------------------------------------------------------
// Default stats table.
// ---------------------------------------------------------------------------

TowerStats make_stats(f32 fire_interval, f32 footprint_radius, u32 build_cost,
                      f32 max_health) {
    TowerStats s;
    s.fire_interval = fire_interval;
    s.footprint_radius = footprint_radius;
    s.build_cost = build_cost;
    s.family_mask = 0xFF; // every tower can affect every family
    s.max_health = max_health;
    return s;
}

// ---------------------------------------------------------------------------
// SWARMER TABLE, compiled-in defaults.
//
// These are the per-tower knobs that are not stats: what a volley is, how a
// swarmer flies, and what it does on contact. They live in a mutable store
// that assets/config/towers.json writes through apply_tower_config()
// (game/towers/TowerMechanics.h). The initial contents below mirror the
// shipped JSON, so a TowerSystem that never sees a config behaves the same --
// and game::default_game_config() reads its bootstrap values back out of
// here, so the two cannot drift. (The one deliberate difference is
// footprint_radius in load_default_stats below: the compiled-in bodies are
// half the shipped size, because the test scenes in tests/test_towers.cpp
// are built around the smaller footprints.)
// ---------------------------------------------------------------------------

TowerMechanics g_mechanics[kTowerTypeCount]{};
TowerGlobals g_globals{};
bool g_mechanics_ready = false;

void init_mechanics_once() {
    if (g_mechanics_ready) return;
    g_mechanics_ready = true;

    // SHOOTER -- Neutrophil. Few, long-lived swarmers that keep a standoff
        // and pour rounds in. Permanent tree purchases improve their output.
    {
        TowerMechanics& m = g_mechanics[static_cast<u32>(TowerType::Neutrophil)];
        constexpr u32 kRelease = 2u;
        constexpr f32 kLife = 6.0f;
        constexpr f32 kSpeed = 22.0f;
        constexpr f32 kSearch = 28.0f;
        constexpr f32 kStandoff = 16.0f;
        // Big enough that the magazine's granules read at play zoom.
        constexpr f32 kSize = 2.00f;
        m.swarm = SwarmParams{kRelease, kLife, kSpeed, kSearch,
                              kStandoff, 0.70f, kSize};
        // A MAGAZINE per unit (sim/swarm/Swarmers.h): gather, a quick
        // fanned volley, then a reload that regrows the rounds. Damage
        // per round is sized so rounds per second across the whole
        // cycle (shooter_rounds_per_second) keeps the old stream's DPS.
        constexpr f32 kFire = 0.03f;
        constexpr f32 kDamage = 1.6f;
        constexpr f32 kRoundSpeed = 20.0f;
        constexpr f32 kSpread = 0.04f;
        constexpr u32 kMagazine = 12u;
        constexpr f32 kGather = 0.22f;
        constexpr f32 kReload = 1.30f;
        constexpr f32 kCone = 0.30f;
        m.shooter = ShooterParams{kFire, kDamage, kRoundSpeed, 0.45f, kSpread,
                                  kSize * 2.4f, 0.75f, 1.0f, 1.5f};
        m.shooter.magazine_size = kMagazine;
        m.shooter.gather_seconds = kGather;
        m.shooter.reload_seconds = kReload;
        m.shooter.volley_cone = kCone;
        // Small and packed in the cell; they swell once they leave it.
        m.shooter.granule_size = 0.06f;
        m.shooter.magazine_spread = 0.19f;
        m.shooter.round_size = 0.10f;
        m.shooter.round_grow_seconds = 0.15f;
    }
    // ARBOR GRABBER -- Macrophage. A smaller number of heavier cells
    // throw several independent branching pseudopods in every direction.
    // Each arm catches one target, but the arm slots cycle concurrently.
    {
        TowerMechanics& m = g_mechanics[static_cast<u32>(TowerType::Macrophage)];
        constexpr u32 kRelease = 1u;
        constexpr f32 kLife = 9.0f;
        constexpr f32 kSpeed = 12.0f;
        constexpr f32 kSearch = 26.0f;
        constexpr f32 kSize = 2.60f;
        constexpr f32 kReach = 7.5f;
        constexpr f32 kHealth = 120.0f;
        constexpr u32 kArms = 2u;
        constexpr f32 kExtend = 0.14f;
        constexpr f32 kLatch = 0.050f;
        constexpr f32 kPull = 0.28f;
        constexpr f32 kRecover = 0.080f;
        constexpr f32 kFakeMass = 0.60f;
        m.swarm = SwarmParams{kRelease, kLife, kSpeed, kSearch,
                              kReach, 0.75f, kSize, kHealth};
        constexpr u32 kMaxCaptives = 3u;
        constexpr f32 kClusterRadius = 2.4f;
        m.arbor_grabber = ArborGrabberParams{kArms, kMaxCaptives,
                                              kClusterRadius, kExtend, kLatch,
                                              kPull, kRecover, kFakeMass,
                                              0.0f, 1.0f, 1.1f, 4.5f, 0.9f};
    }
    // LATCH -- Cytotoxic T. The original: many small swarmers, each one a
    // lytic granule that rides its host and drains it. Still the biggest
    // cloud in the roster -- 4 per half-second over 4s is ~32 live at
    // baseline, and a full board of them is why
    // SimDesc::max_swarmers is five figures.
    {
        TowerMechanics& m = g_mechanics[static_cast<u32>(TowerType::CytotoxicT)];
        constexpr u32 kRelease = 4u;
        constexpr f32 kLife = 4.0f;
        constexpr f32 kSpeed = 20.0f;
        constexpr f32 kSearch = 12.0f;
        constexpr f32 kSize = 1.41f;
        m.swarm = SwarmParams{kRelease, kLife, kSpeed, kSearch,
                              0.55f, 0.85f, kSize};
        constexpr f32 kDps = 4.2f;
        m.latch = LatchParams{kDps};
    }
    // MUCUS BOMBER -- Goblet Cell. Swarmers that pop into a splash of real
    // fluid (sim/fluid) which slows what it soaks; the splash is the
    // attack, and the fluid solver owns it from the instant it lands.
    {
        TowerMechanics& m = g_mechanics[static_cast<u32>(TowerType::GobletCell)];
        constexpr u32 kRelease = 1u;
        constexpr f32 kSpeed = 16.0f;
        constexpr f32 kSearch = 14.0f;
        constexpr f32 kSize = 1.80f;
        m.swarm = SwarmParams{kRelease, 6.0f, kSpeed, kSearch, 0.70f, 0.60f, kSize};
        constexpr u32 kDroplets = 20u;
        constexpr f32 kRadius = 1.0f;
        constexpr f32 kSplashSpeed = 6.0f;
        constexpr f32 kDropLife = 1.6f;
        constexpr f32 kSlowDuration = 3.0f;
        constexpr f32 kSlowFactor = 0.12f;
        m.mucus_bomber = MucusBomberParams{1.0f, kDroplets, kRadius, kSplashSpeed,
                                           kDropLife, kSlowDuration, kSlowFactor};
    }
    // BUILDER -- Fibroblast. One builder at a time, walking out to lay a
    // collagen scar across the lane (sim/scar). search_radius doubles as
    // the build reach the HUD ring shows; attach_radius is how close to
    // its site a builder has to get. Tier buys longer, tougher walls and
    // more of them, not more builders.
    {
        TowerMechanics& m = g_mechanics[static_cast<u32>(TowerType::Fibroblast)];
        constexpr f32 kSpeed = 12.0f;
        constexpr f32 kReach = 14.0f;
        // Tough for a swarmer: a builder has to survive a walk through the
        // crowd to a site behind its front, with viruses climbing on the
        // whole way.
        constexpr f32 kUnitHealth = 60.0f;
        m.swarm = SwarmParams{1u, 8.0f, kSpeed, kReach, 0.8f, 0.60f, 1.60f, kUnitHealth};
        constexpr f32 kHalfLength = 4.0f;
        constexpr f32 kHalfWidth = 0.8f;
        constexpr f32 kHealth = 220.0f;
        constexpr f32 kReinforce = 50.0f;
        constexpr u32 kMaxScars = 3u;
        m.builder = BuilderParams{kReach, 4.0f, 1.0f, 12u, kHalfLength, kHalfWidth, 0.2f,
                                  kHealth, kReinforce, 6.0f, kMaxScars, 0.0f, 0.0f};
    }
    g_globals = TowerGlobals{0.7f, 16u};
}

/// Shorthand for the mechanism block a tower is currently running on.
const TowerMechanics& mech(TowerType type) { return tower_mechanics(type); }

// ---------------------------------------------------------------------------
// Baseline build costs. Permanent upgrades are applied to the run config.
// ---------------------------------------------------------------------------
void load_default_stats(TowerSystem& self) {
    self.set_stats(TowerType::Neutrophil, make_stats(1.00f, 1.4f, 8, 220.0f));
    self.set_stats(TowerType::CytotoxicT, make_stats(0.50f, 1.6f, 12, 280.0f));
    self.set_stats(TowerType::GobletCell, make_stats(1.60f, 1.4f, 24, 300.0f));
    self.set_stats(TowerType::Fibroblast, make_stats(3.00f, 1.5f, 30, 260.0f));
    self.set_stats(TowerType::Macrophage, make_stats(1.70f, 5.0f, 20, 720.0f));
}

bool same_stats(const TowerStats& a, const TowerStats& b) {
    return a.fire_interval == b.fire_interval &&
           a.footprint_radius == b.footprint_radius && a.build_cost == b.build_cost &&
           a.family_mask == b.family_mask &&
           a.max_health == b.max_health;
}

/// There is no constructor hook to populate `stats_` once per instance, so
/// the table is filled lazily instead, the first time it is observed still
/// untouched, which is what lets a bare `TowerSystem ts;` -- in a test, in a
/// bench scenario, in the gym -- be usable without every caller remembering
/// an init call. stats() does the actual populating; this helper only has to
/// touch one row to trigger it. The sentinel compare in stats() has to work
/// on a populated row, and every populated row has a non-zero build cost.
void ensure_default_stats(const TowerSystem& self) {
    (void)self.stats(TowerType::Neutrophil);
}

// ---------------------------------------------------------------------------
// Aiming helpers.
//
// A tower needs an aim point for exactly one reason: to orient the release
// cone and the body sprite. It does not need a target to HIT, because it is
// not hitting anything -- its swarmers choose their own targets once released
// -- and it releases whether or not it found one. The look-around radius is
// the swarmers' own search_radius: "face the crowd my units could aggro on".
//
// acquire_focus() reads per-cell OCCUPANCY over the cells that circle
// overlaps (tens of integers), then averages the positions of exactly ONE
// cell's agents. Never a scan of the store.
// ---------------------------------------------------------------------------

constexpr u32 kNoIndex = static_cast<u32>(-1);

Vec2 heading(f32 radians) { return Vec2{std::cos(radians), std::sin(radians)}; }

sim::CombatEvent tower_event(sim::CombatEventType type, TowerType source, Vec2 origin) {
    sim::CombatEvent e;
    e.type = type;
    e.source = source;
    e.visual_id = 1;
    e.origin = origin;
    e.secondary = origin;
    return e;
}

bool family_allowed(u8 mask, u8 family) { return (mask & static_cast<u8>(1u << family)) != 0; }

/// What a tower may aim at. Burrowed (kHidden) chaff is excluded: nothing in
/// the roster can see it, so a tower must not spend a volley on it either.
bool chaff_targetable(const sim::ChaffBuffers& chaff, u32 idx, u8 mask) {
    if (idx >= chaff.count()) return false;
    const u8 f = chaff.flags[idx];
    if ((f & sim::chaff_flags::kAlive) == 0) return false;
    if ((f & sim::chaff_flags::kPendingKill) != 0) return false;
    if ((f & sim::chaff_flags::kHidden) != 0) return false;
    return family_allowed(mask, chaff.family[idx]);
}

/// "Where is the horde, roughly" -- the aim point every tower shares. Cost is
/// O(cells overlapping the range circle) for the search plus O(one cell's
/// occupancy) for the refine; it never walks the chaff store.
///
/// Picks the fullest spatial-hash cell that can actually contain an in-range
/// agent (a cell whose NEAREST point is out of range provably cannot), then
/// returns the centroid of that cell's in-range agents. Deterministic: cell
/// scan order is row-major and ties keep the first (lowest-index) cell.
bool acquire_focus(const sim::SimWorld& world, Vec2 origin, f32 range, u8 mask, Vec2& out) {
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
    u32 n_in = 0;
    u32 n_any = 0;
    for (u32 s = begin; s < end; ++s) {
        const u32 a = indices[s];
        if (!chaff_targetable(chaff, a, mask)) continue;
        const Vec2 p{chaff.pos_x[a], chaff.pos_y[a]};
        anywhere += p;
        ++n_any;
        if (math::length_sq(p - origin) > r2) continue;
        in_range += p;
        ++n_in;
    }
    if (n_in > 0) {
        out = in_range / static_cast<f32>(n_in);
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
    return true;
}

/// The aim point a tower should face this tick: the chaff focus if there is
/// one, otherwise the nearest named agent, otherwise nothing.
bool acquire_aim_point(TowerSystem& self, sim::SystemContext& ctx, Vec2 origin, f32 look_radius,
                       const TowerStats& st, Vec2& out) {
    if (acquire_focus(ctx.world, origin, look_radius, st.family_mask, out)) return true;
    const EntityId named = self.find_target(ctx.world, origin, look_radius, st.family_mask);
    if (!named.valid()) return false;
    const entt::entity te = ctx.world.ecs().from_id(named);
    if (!ctx.registry.valid(te) || !ctx.registry.all_of<comp::Transform>(te)) return false;
    out = ctx.registry.get<comp::Transform>(te).position;
    return true;
}

// ---------------------------------------------------------------------------
// Where a Fibroblast sends a builder.
//
// A builder is released with a SITE (SwarmerSpawnParams::goal) and walks
// there; the wall goes down where the site is, so the site has to be a place
// a wall can stand. Chosen here, at release, rather than by the unit on
// arrival, because everything that decides it -- the tissue mask, the flow
// field, the scars already standing, the towers -- is reachable from the
// game layer and none of it from the swarmer kernel. The scar system checks
// again when the wall goes down (another builder may have got there first);
// a site that has meanwhile been taken becomes a reinforcement.
//
// Random sites in the annulus [build_min_radius, build_radius] around the
// tower, `build_candidates` tries; a site must be on walkable tissue, carry
// a flow direction (be in a lane the horde uses), have the profile's SDF
// clearance, keep scar_spacing from every live scar, stay clear of every
// tower's footprint, not overlap a standing wall or one a builder is already
// on its way to lay, and -- the expensive test, last -- not seal the lane.
// The wall is cut back to the lane's edges first (fit_scar_to_tissue), so
// every one of those tests sees the wall as it will actually stand.
// The draws come from the builder's own seed stream, never the shared sim
// Rng, for the same reason the volley scatter does.
//
// With the owner at max_scars, or no site found and something to reinforce,
// the builder is sent to a standing scar instead and lays collagen on it.
// Nothing at all to do means no builder: the volley is skipped.
// ---------------------------------------------------------------------------
template <typename Draw>
bool pick_scar_site(const sim::SimWorld& world, const std::vector<EntityId>& placed, EntityId owner,
                    Vec2 tower_pos, const sim::SwarmerProfile& pr, Draw&& draw, Vec2& out) {
    const sim::ScarSystem& scars = world.scars();
    const sim::TissueMask& mask = world.tissue();
    const sim::FlowField& flow = world.flow();
    if (mask.width() <= 0 || mask.height() <= 0) return false;

    const f32 reach = math::max(pr.build_radius, 0.0f);
    const auto send_to_reinforce = [&]() {
        if (pr.scar_reinforce <= 0.0f) return false;
        const EntityId target = scars.nearest(world, tower_pos, reach, EntityId{});
        if (!target.valid()) return false;
        const entt::entity e = world.ecs().from_id(target);
        if (!world.ecs().registry().valid(e)) return false;
        out = world.ecs().registry().get<comp::Transform>(e).position;
        return true;
    };

    if (pr.max_scars > 0 && owner.valid() &&
        scars.count_owned(world, owner) >= pr.max_scars) return send_to_reinforce();

    const f32 r_min = math::clamp(pr.build_min_radius, 0.0f, reach);
    const Vec2 he{math::max(pr.scar_half_length, 0.0f), math::max(pr.scar_half_width, 0.0f)};
    const Rect& play = world.desc().world_bounds;
    const entt::registry& registry = world.ecs().registry();

    // The walls builders are already walking out to lay. They do not stand
    // yet, so scars.overlaps() cannot see them: without this, one volley's
    // rank (each unit is spawned before the next picks) or successive volleys
    // send builders to the same spot, and the later ones arrive to find a
    // wall already there. A reinforcement goal is a standing scar's centre,
    // which the site must avoid anyway.
    static thread_local std::vector<sim::Bar> planned;
    planned.clear();
    const sim::SwarmerBuffers& sw = world.swarmers();
    for (usize i = 0; i < sw.count(); ++i) {
        if ((sw.flags[i] & sim::swarmer_flags::kHasGoal) == 0) continue;
        const sim::SwarmerProfile& other = sw.profile_of(i);
        if (other.kind != sim::SwarmerKind::Builder) continue;
        sim::Bar b;
        b.center = Vec2{sw.goal_x[i], sw.goal_y[i]};
        b.half_extents = Vec2{math::max(other.scar_half_length, 0.0f), math::max(other.scar_half_width, 0.0f)};
        b.rotation = sim::scar_rotation(flow, b.center, sw.owner[i], other.scar_tilt);
        sim::fit_scar_to_tissue(world.sdf(), b);
        planned.push_back(b);
    }

    for (u32 c = 0; c < pr.build_candidates; ++c) {
        // Uniform over the annulus' AREA, not its radius, so sites do not
        // bunch up near the tower.
        const f32 r = std::sqrt(math::lerp(r_min * r_min, reach * reach, draw()));
        const f32 a = draw() * math::kTwoPi;
        const Vec2 p = tower_pos + Vec2{std::cos(a), std::sin(a)} * r;

        if (!play.contains(p)) continue;
        const IVec2 cell = mask.world_to_cell(p);
        if (!mask.walkable(cell.x, cell.y)) continue;
        const Vec2 f = flow.sample(p);
        if (f.x == 0.0f && f.y == 0.0f) continue;
        if (world.sdf().sample(p) < pr.build_min_clearance) continue;
        if (pr.scar_spacing > 0.0f && scars.nearest(world, p, pr.scar_spacing).valid()) continue;

        bool on_tower = false;
        for (const EntityId id : placed) {
            const entt::entity te = world.ecs().from_id(id);
            if (!registry.valid(te) || !registry.all_of<comp::Transform, comp::Sprite>(te)) continue;
            const f32 body = registry.get<comp::Sprite>(te).size * 0.5f + he.y;
            if (math::length_sq(registry.get<comp::Transform>(te).position - p) < body * body) {
                on_tower = true;
                break;
            }
        }
        if (on_tower) continue;

        // The same bar the scar system will lay on arrival, tilt included
        // and cut back to the lane's edges, or the sever check answers for
        // a different wall.
        sim::Bar bar;
        bar.center = p;
        bar.half_extents = he;
        bar.rotation = sim::scar_rotation(flow, p, owner, pr.scar_tilt);
        if (!sim::fit_scar_to_tissue(world.sdf(), bar)) continue;

        // scar_spacing above only rejected sites near another scar's CENTER;
        // a long wall's far end reaches well past that, so check the actual
        // bar geometry too before this one gets laid on top of it.
        if (scars.overlaps(world, bar)) continue;
        bool claimed = false;
        for (const sim::Bar& other : planned) {
            if (sim::bars_overlap(bar, other)) {
                claimed = true;
                break;
            }
        }
        if (claimed) continue;

        const bool sever = sim::would_sever_lane(mask, flow, bar.bounds(), [&](i32 x, i32 y) {
            return bar.contains(mask.cell_to_world(x, y));
        });
        if (sever) continue;

        out = p;
        return true;
    }
    return send_to_reinforce();
}

// ---------------------------------------------------------------------------
// THE ONE COMBAT SYSTEM. Every tower: face the horde, and on cooldown release
// a volley of swarmers from the cell's face -- continuously, for as long as
// the round is on.
//
// This publishes NO DamageField and touches no agent. With nothing in sight
// it still releases: the units go out in a full ring instead of a cone and
// aggro on whatever wanders into their search radius, and a bomber that finds
// nothing pops on expiry where it stands, which is the design.
// ---------------------------------------------------------------------------
void system_spawner(TowerSystem& self, sim::SystemContext& ctx) {
    auto view = ctx.registry.view<comp::Tower, comp::Transform>();
    for (auto e : view) {
        comp::Tower& tw = view.get<comp::Tower>(e);
        comp::Transform& tf = view.get<comp::Transform>(e);
        const TowerStats& st = self.stats(tw.type);
        const SwarmParams& swarm_params = mech(tw.type).swarm;

        Vec2 aim_point{};
        const bool have_aim =
            acquire_aim_point(self, ctx, tf.position, swarm_params.search_radius, st, aim_point);
        if (have_aim) {
            const Vec2 d = math::normalize_safe(aim_point - tf.position);
            if (d.x != 0.0f || d.y != 0.0f) tf.rotation = std::atan2(d.y, d.x);
        } else {
            // Nothing in sight: face up the lane, where the horde comes from.
            // The flow points at the goal, so upstream is its opposite.
            const Vec2 up = -ctx.world.flow().sample(tf.position);
            if (math::length_sq(up) > math::kEpsilon) tf.rotation = std::atan2(up.y, up.x);
        }
        if (!self.releasing()) continue;
        if (tw.cooldown > 0.0f) continue;

        const Vec2 facing = heading(tf.rotation);
        // A blind volley goes all round rather than down the cone: there is
        // nothing to point it at, and a ring gives the units every approach.
        const f32 spread = have_aim ? swarm_params.launch_spread : math::kPi;

        // Swarmers leave the cell's FACE, not its middle. The quad is drawn at
        // footprint_radius * 2, so local +x 0.5 is one footprint_radius out;
        // releasing at 0.52 of one puts the volley just past the membrane,
        // where the cell is visibly secreting.
        const Vec2 muzzle = tf.position + facing * (st.footprint_radius * 0.52f);

        const u32 release = swarm_params.release_per_shot;
        sim::SwarmerBuffers& swarm = ctx.world.swarmers();

        // Re-register the profile on every volley. Fifteen small structs at
        // most, and it is what lets a towers.json hot-reload reach the units
        // already in flight without any world plumbing.
        const u16 slot = sim::swarmer_profile_slot(tw.type);
        swarm.set_profile(slot, swarmer_profile(tw.type));

        const EntityId owner = ctx.world.ecs().to_id(e);
        const sim::SwarmerProfile& profile = swarm.profile_at(slot);
        u32 released = 0;
        for (u32 k = 0; k < release; ++k) {
            // Swarmer identity. Everything stochastic about this release is
            // derived from this one word rather than drawn from the shared sim
            // Rng: a per-swarmer draw would make every downstream system's
            // numbers depend on the selected tower types on the board.
            u32 gseed = (static_cast<u32>(ctx.tick * 2654435761ull) ^ (k * 0x9E3779B9u) ^
                         static_cast<u32>(owner.value * 0x85EBCA6Bull)) | 1u;
            const auto draw = [&gseed]() {
                gseed = gseed * 1664525u + 1013904223u;
                return static_cast<f32>((gseed >> 8) & 0xFFFFu) / 65535.0f;   // [0,1]
            };

            // SCATTER the cone, do not fan it evenly. An even fan launched from
            // one point arrives as a crescent of dots -- a tidy arc is the one
            // shape a swarm must never make.
            const f32 offset = (draw() * 2.0f - 1.0f) * spread;
            const f32 ca = std::cos(offset);
            const f32 sa = std::sin(offset);
            const Vec2 dir{facing.x * ca - facing.y * sa, facing.x * sa + facing.y * ca};

            // Spread the origin across the width of the face too, so a volley
            // does not visibly emanate from a single pixel.
            const Vec2 across{-facing.y, facing.x};
            const Vec2 origin = muzzle + across * ((draw() - 0.5f) * st.footprint_radius * 0.5f)
                                       + facing * ((draw() - 0.5f) * 0.5f);

            // Speed spread on top, so swarmers launched on the same bearing
            // still separate along it.
            const f32 jitter = 0.60f + 0.80f * draw();

            sim::SwarmerSpawnParams p;
            p.position = origin;
            p.velocity = dir * (swarm_params.speed * jitter);
            p.profile = slot;
            p.family_mask = st.family_mask;
            p.owner = owner;
            p.visual_id = 1;
            p.seed = gseed;
            // One volley is one squad (shooters march as a rank); the release
            // tick names it and k orders the rank.
            p.group = static_cast<u32>(ctx.tick);
            p.slot = static_cast<u16>(k);
            // A builder needs somewhere to build. Nowhere means no builder:
            // a unit released to drift and dissolve is a slot wasted and a
            // tower that visibly does nothing for no reason.
            if (profile.kind == sim::SwarmerKind::Builder) {
                Vec2 site;
                if (!pick_scar_site(ctx.world, self.placed_towers(), owner, tf.position, profile, draw, site)) {
                    continue;
                }
                p.goal = site;
                p.has_goal = true;
                // Leave toward the site rather than down the aim cone.
                const Vec2 to_site = math::normalize_safe(site - origin);
                if (to_site.x != 0.0f || to_site.y != 0.0f) p.velocity = to_site * (swarm_params.speed * jitter);
            }
            if (swarm.spawn(p)) ++released;
        }

        // A tower with nothing to release (a Fibroblast with nowhere to build)
        // holds its cooldown and tries again next tick, and announces nothing.
        if (released == 0 && profile.kind == sim::SwarmerKind::Builder) continue;

        // One release event for the VFX layer: the secretion at the face. The
        // swarmers themselves are simulated and drawn from sim state, so there
        // is deliberately no per-unit cosmetic event here.
        sim::CombatEvent fired =
            tower_event(sim::CombatEventType::MuzzleFlash, tw.type, muzzle);
        fired.direction = facing;
        fired.magnitude = static_cast<f32>(released);
        ctx.world.combat_events().push(fired);

        tw.cooldown = st.fire_interval;
    }
}

/// World-space diameter of a tower's body sprite: the sprite IS the lump of
/// cell sitting on the tissue, and its reach is communicated by the range
/// ring, not the body.
f32 tower_sprite_size(const TowerStats& st) { return st.footprint_radius * 2.0f; }

/// Decays every live comp::Marked debuff and removes it once it expires.
/// Marked is a generic component (sim/ecs/Components.h) that any system could
/// apply; the Goblet Cell's splashes (SimWorld::apply_swarmer_effects) are the
/// only producer, and the component itself carries no lifecycle of its own.
///
/// This never re-applies anything to chaff -- comp::Marked is exclusively the
/// named-agent half of the weaken debuff (chaff_flags::kMarked is the chaff
/// half, and nothing ever clears that one at all; see sim/fluid/Fluid.cpp).
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

/// Same arrangement for comp::Slowed, the named-agent half of the mucus
/// slow. The chaff half expires in SimWorld::tick.
void system_slowed_upkeep(sim::SystemContext& ctx) {
    static thread_local std::vector<entt::entity> expired;
    expired.clear();
    auto view = ctx.registry.view<comp::Slowed>();
    for (auto e : view) {
        comp::Slowed& sl = view.get<comp::Slowed>(e);
        sl.remaining -= ctx.dt;
        if (sl.remaining <= 0.0f) expired.push_back(e);
    }
    for (entt::entity e : expired) ctx.registry.remove<comp::Slowed>(e);
}

/// PreUpdate, first of all: tears down every tower the horde has emptied.
///
/// Damage lands on comp::Health from SimWorld::apply_hostile_effects, at the
/// end of the previous tick; this runs before the spawner so a dead tower
/// never releases a farewell volley. The entity is destroyed here rather
/// than in sim/ because TowerSystem owns placed_towers(), and a stale id in
/// that list makes validate() report Overlapping on empty tissue (the same
/// reason register_systems() clears it). No refund: a tower the horde ate
/// is gone, and the ATP with it -- that is what makes mid-lane placement a
/// real bet rather than a free one.
void system_tower_death(TowerSystem& self, std::vector<EntityId>& towers, u32& destroyed,
                        sim::SystemContext& ctx) {
    static thread_local std::vector<entt::entity> dead;
    dead.clear();
    auto view = ctx.registry.view<const comp::Tower, const comp::Health>();
    for (auto e : view) {
        if (view.get<const comp::Health>(e).dead()) dead.push_back(e);
    }
    if (dead.empty()) return;
    // Sorted so the events -- and therefore the VFX RNG draws -- come out in
    // an order that does not depend on EnTT's pool layout.
    std::sort(dead.begin(), dead.end());
    for (entt::entity e : dead) {
        const comp::Tower& tw = ctx.registry.get<comp::Tower>(e);
        const TowerStats& st = self.stats(tw.type);
        Vec2 origin{0.0f, 0.0f};
        if (const auto* tf = ctx.registry.try_get<comp::Transform>(e)) origin = tf->position;

        sim::CombatEvent gone = tower_event(sim::CombatEventType::TowerDestroyed, tw.type, origin);
        gone.radius = st.footprint_radius;
        gone.magnitude = st.max_health;
        ctx.world.combat_events().push(gone);

        const EntityId id = ctx.world.ecs().to_id(e);
        towers.erase(std::remove(towers.begin(), towers.end(), id), towers.end());
        ctx.registry.destroy(e);
        ++destroyed;
    }
}

/// PreUpdate: decays comp::Tower's own cooldown timers.
void system_tower_cooldowns(sim::SystemContext& ctx) {
    auto view = ctx.registry.view<comp::Tower>();
    for (auto e : view) {
        comp::Tower& tw = view.get<comp::Tower>(e);
        tw.cooldown = math::max(0.0f, tw.cooldown - ctx.dt);
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Public API.
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

const TowerStats& TowerSystem::stats(TowerType type) const {
    if (same_stats(stats_[0], TowerStats{})) {
        load_default_stats(const_cast<TowerSystem&>(*this));
    }
    const u32 t = static_cast<u32>(type) < kTowerTypeCount ? static_cast<u32>(type) : 0u;
    return stats_[t];
}

void TowerSystem::set_stats(TowerType type, const TowerStats& value) {
    if (static_cast<u32>(type) >= kTowerTypeCount) return;
    stats_[static_cast<u32>(type)] = value;
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
    const TowerStats& st = stats_[t];

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
        const TowerStats& est = stats_[static_cast<u32>(etw.type)];
        const f32 min_dist = st.footprint_radius + est.footprint_radius;
        if (math::length_sq(etf.position - pos) < min_dist * min_dist) {
            q.result = PlacementResult::Overlapping;
            return q;
        }
    }

    // A live Fibrin Clot (game/abilities, comp::Barrier) counts as an
    // obstruction too: the bar is solid ground for the horde, and a tower
    // standing inside it would sit in the one place the clot's own rebake
    // routes nothing past. Disc-vs-oriented-box in the bar's frame.
    {
        auto barriers = world.ecs().registry().view<const comp::Barrier, const comp::Transform>();
        for (auto e : barriers) {
            const comp::Barrier& b = barriers.get<const comp::Barrier>(e);
            const comp::Transform& btf = barriers.get<const comp::Transform>(e);
            const f32 cs = std::cos(btf.rotation);
            const f32 sn = std::sin(btf.rotation);
            const Vec2 d = pos - btf.position;
            const f32 lx = d.x * cs + d.y * sn;
            const f32 ly = -d.x * sn + d.y * cs;
            const f32 qx = math::max(std::fabs(lx) - b.half_extents.x, 0.0f);
            const f32 qy = math::max(std::fabs(ly) - b.half_extents.y, 0.0f);
            if (qx * qx + qy * qy < st.footprint_radius * st.footprint_radius) {
                q.result = PlacementResult::Overlapping;
                return q;
            }
        }
    }
    // A collagen scar (sim/scar, comp::Scar) is the same obstruction, for
    // the same reason, and it can stand for the rest of the level.
    {
        auto scars = world.ecs().registry().view<const comp::Scar, const comp::Transform>();
        for (auto e : scars) {
            const comp::Scar& sc = scars.get<const comp::Scar>(e);
            const comp::Transform& stf = scars.get<const comp::Transform>(e);
            sim::Bar bar;
            bar.center = stf.position;
            bar.half_extents = sc.half_extents;
            bar.rotation = stf.rotation;
            if (bar.distance_sq(pos) < st.footprint_radius * st.footprint_radius) {
                q.result = PlacementResult::Overlapping;
                return q;
            }
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
    // The player's own roster (the Strengthen Immunity tree). Same reasoning:
    // a tower the player has not unlocked is not buildable by any path.
    if (!tower_unlocked(type)) {
        q.result = PlacementResult::TowerLocked;
        return q;
    }

    // The play area. "Anywhere" has always meant "anywhere on tissue", and
    // tissue used to stop at world_bounds because the mask did. It no longer
    // does -- a level may run a lane out past the edge to an off-map spawn
    // point (game::level_sim_bounds()) -- and the approach corridor is scenery,
    // not board: letting a tower stand out there would put the kill zone where
    // the camera cannot even be dragged. Restores the pre-off-map behaviour
    // exactly for every level whose geometry stays inside its own rect.
    if (!world.desc().world_bounds.contains(pos)) {
        q.result = PlacementResult::OutsidePlacementZone;
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

    q.result = PlacementResult::Ok;
    return q;
}

PlacementQuery TowerSystem::validate_deploy(const sim::SimWorld& world, TowerType type,
                                            Vec2 pos, u32 available_atp) const {
    PlacementQuery q;
    q.snapped_position = pos;
    if (static_cast<u32>(type) >= kTowerTypeCount) {
        q.result = PlacementResult::NotOnTissue;
        return q;
    }
    const f32 radius = tower_mechanics(type).swarm.size;
    q.clearance = world.sdf().sample(pos);
    if (q.clearance <= 0.0f) q.result = PlacementResult::NotOnTissue;
    else if (q.clearance < radius) q.result = PlacementResult::InsufficientClearance;
    else if (!world.desc().world_bounds.contains(pos)) q.result = PlacementResult::OutsidePlacementZone;
    else if (!tower_allowed(type)) q.result = PlacementResult::TowerNotAllowed;
    else if (!tower_unlocked(type)) q.result = PlacementResult::TowerLocked;
    else if (available_atp < stats(type).build_cost) q.result = PlacementResult::CannotAfford;
    if (!q.valid()) return q;

    if (!world.placement_zones().empty()) {
        const Rect fp = footprint_rect(pos, radius);
        bool inside = false;
        for (const Rect& z : world.placement_zones()) {
            if (fp.min.x >= z.min.x && fp.min.y >= z.min.y &&
                fp.max.x <= z.max.x && fp.max.y <= z.max.y) {
                inside = true;
                break;
            }
        }
        if (!inside) q.result = PlacementResult::OutsidePlacementZone;
    }
    if (!q.valid()) return q;

    const auto& registry = world.ecs().registry();
    const auto barriers = registry.view<const comp::Barrier, const comp::Transform>();
    for (auto e : barriers) {
        const auto& b = barriers.get<const comp::Barrier>(e);
        const auto& tf = barriers.get<const comp::Transform>(e);
        sim::Bar bar{tf.position, b.half_extents, tf.rotation};
        if (bar.distance_sq(pos) < radius * radius) q.result = PlacementResult::Overlapping;
    }
    const auto scars = registry.view<const comp::Scar, const comp::Transform>();
    for (auto e : scars) {
        const auto& sc = scars.get<const comp::Scar>(e);
        const auto& tf = scars.get<const comp::Transform>(e);
        sim::Bar bar{tf.position, sc.half_extents, tf.rotation};
        if (bar.distance_sq(pos) < radius * radius) q.result = PlacementResult::Overlapping;
    }
    // Friendly cells never reserve space. A full swarmer store is a separate
    // limit, not an overlap with the cells already at this location.
    if (world.swarmers().full()) q.result = PlacementResult::AtCapacity;
    if (q.valid() && tower_kind(type) == sim::SwarmerKind::Builder) {
        u32 seed = (static_cast<u32>(world.tick_index()) * 2654435761u) ^
                   (next_deployment_id_ * 2246822519u) ^
                   (static_cast<u32>(type) * 3266489917u);
        seed |= 1u;
        const auto draw = [&seed]() {
            seed = seed * 1664525u + 1013904223u;
            return static_cast<f32>((seed >> 8) & 0xFFFFu) / 65535.0f;
        };
        q.has_build_goal = pick_scar_site(world, {},
            EntityId{0x80000000u | next_deployment_id_}, pos,
            swarmer_profile(type), draw, q.build_goal);
        if (!q.has_build_goal) q.result = PlacementResult::NoBuildSite;
    }
    return q;
}

bool TowerSystem::deploy(sim::SimWorld& world, TowerType type, Vec2 world_pos) {
    const PlacementQuery q = validate_deploy(world, type, world_pos, 0xFFFFFFFFu);
    if (!q.valid()) return false;

    sim::SwarmerBuffers& swarm = world.swarmers();
    const u16 profile_slot = sim::swarmer_profile_slot(type);
    swarm.set_profile(profile_slot, swarmer_profile(type));
    const sim::SwarmerProfile& profile = swarm.profile_at(profile_slot);
    u32 seed = (static_cast<u32>(world.tick_index()) * 2654435761u) ^
               (next_deployment_id_ * 2246822519u) ^
               (static_cast<u32>(type) * 3266489917u);
    seed |= 1u;

    sim::SwarmerSpawnParams p;
    p.position = q.snapped_position;
    p.profile = profile_slot;
    p.family_mask = stats(type).family_mask;
    p.visual_id = 1;
    p.group = static_cast<u32>(world.tick_index()) ^ static_cast<u32>(swarm.count());
    p.persistent = true;
    // The high range is reserved for direct-cell attribution. No ECS tower
    // owns this unit; the handle only groups its damage and its scar.
    p.owner = EntityId{0x80000000u | next_deployment_id_};
    if (profile.kind == sim::SwarmerKind::Builder) {
        p.goal = q.build_goal;
        p.has_goal = true;
        p.velocity = math::normalize_safe(q.build_goal - p.position) * profile.speed;
    }
    p.seed = seed;
    if (!swarm.spawn(p)) return false;
    last_deployment_id_ = p.owner;
    ++next_deployment_id_;
    return true;
}

u32 deploy_cells(TowerSystem& towers, sim::SimWorld& world, Economy& economy,
                 TowerType type, Vec2 world_pos, u32 quantity) {
    if (static_cast<u32>(type) >= kTowerTypeCount) return 0;
    const u32 cost = towers.stats(type).build_cost;
    u32 deployed = 0;
    for (; deployed < quantity; ++deployed) {
        const PlacementQuery q = towers.validate_deploy(world, type, world_pos, economy.atp());
        if (!q.valid() || !towers.deploy(world, type, q.snapped_position)) break;
        economy.spend(cost);
    }
    return deployed;
}

EntityId TowerSystem::place(sim::SimWorld& world, TowerType type, Vec2 world_pos) {
    ensure_default_stats(*this);
    // No ATP is threaded through this signature (per the frozen header):
    // affordability is the caller's responsibility via validate(), same as
    // for sell(). Re-validate everything else here so place() is
    // never the caller's only correctness check.
    const PlacementQuery q = validate(world, type, world_pos, /*available_atp=*/0xFFFFFFFFu);
    if (!q.valid()) return EntityId{};

    const u32 t = static_cast<u32>(type);
    const TowerStats& st = stats_[t];

    // Deliberately no TissueMask edit and no FlowField::mark_dirty here: the
    // tower is not an obstacle (see the file header).
    priv::TowerRecord rec;
    rec.invested_atp = st.build_cost;

    entt::registry& registry = world.ecs().registry();
    const entt::entity e = registry.create();
    registry.emplace<comp::Transform>(e, comp::Transform{world_pos, 0.0f, 1.0f});

    comp::Tower tw;
    tw.type = type;
    // A tower has no range; what the component carries is its swarmers'
    // aggro radius, which is what the HUD ring and the balance bot want.
    tw.range = tower_mechanics(type).swarm.search_radius;
    // Spin-up: a tower starts one full fire_interval from its first volley
    // rather than releasing on the tick it is dropped -- the difference
    // between "placed a tower" and "instantly answered the wave you were about
    // to be hit by", and tests/scripts/tower_thins_horde.json asserts exactly
    // that (no kills on the placement tick).
    tw.cooldown = st.fire_interval;
    tw.fire_interval = st.fire_interval;
    registry.emplace<comp::Tower>(e, tw);
    // What the horde chews on (sim/hostile). Armor stays 0: a tower's
    // resilience is expressed as more integrity, not as a per-bite floor,
    // so a single virus is still worth scraping off.
    registry.emplace<comp::Health>(e, comp::Health{st.max_health, st.max_health, 0.0f});
    // Shape ids start at kTowerShapeBase so tower and overlay id spaces cannot
    // collide.
    //
    // They previously did: atlas_index was the raw TowerType, so the Macrophage
    // (type 1) drew as the range-indicator RING, the Cytotoxic T (3) as the
    // telegraph countdown ring, and slot 4 as an elite death burst.
    // Every tower was wearing some other system's overlay.
    registry.emplace<comp::Sprite>(e, comp::Sprite{Vec4{1.0f, 1.0f, 1.0f, 1.0f}, tower_sprite_size(st),
                                                    static_cast<u16>(tower_globals().shape_base + static_cast<u16>(t)),
                                                    /*layer=*/1});
    registry.emplace<priv::TowerRecord>(e, std::move(rec));

    const EntityId id = world.ecs().to_id(e);
    towers_.push_back(id);
    return id;
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
    }

    registry.destroy(e);
    towers_.erase(std::remove(towers_.begin(), towers_.end(), tower), towers_.end());
    return refund;
}

EntityId TowerSystem::find_target(const sim::SimWorld& world, Vec2 origin, f32 radius,
                                  u8 family_mask) const {
    const entt::registry& registry = world.ecs().registry();
    auto view = registry.view<const comp::NamedAgent, const comp::Transform, const comp::Health>();

    entt::entity best = entt::null;
    f32 best_d2 = radius * radius;
    for (auto e : view) {
        const comp::NamedAgent& agent = view.get<const comp::NamedAgent>(e);
        const u8 fam_bit = static_cast<u8>(1u << static_cast<u8>(agent.family));
        if ((family_mask & fam_bit) == 0) continue;

        const comp::Health& health = view.get<const comp::Health>(e);
        if (health.dead()) continue;

        // Burrowed is invisible to the whole roster.
        if (const auto* brain = registry.try_get<comp::AiBrain>(e)) {
            if (brain->state == comp::AiState::Burrowed) continue;
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
    destroyed_ = 0;
    next_deployment_id_ = 1;
    last_deployment_id_ = EntityId{};
    // Death first, then cooldowns: see system_tower_death.
    ecs.add_system(sim::SystemPhase::PreUpdate, "tower_death", 0,
                   [this](sim::SystemContext& ctx) {
                       system_tower_death(*this, towers_, destroyed_, ctx);
                   });
    ecs.add_system(sim::SystemPhase::PreUpdate, "tower_cooldowns", 20, &system_tower_cooldowns);

    // One spawner system for the whole roster. Fixed sort keys are a
    // determinism requirement, not a preference.
    ecs.add_system(sim::SystemPhase::Combat, "tower_spawner", 0,
                   [this](sim::SystemContext& ctx) { system_spawner(*this, ctx); });
    ecs.add_system(sim::SystemPhase::Combat, "marked_upkeep", 9, &system_marked_upkeep);
    ecs.add_system(sim::SystemPhase::Combat, "slowed_upkeep", 10, &system_slowed_upkeep);
}


// ---------------------------------------------------------------------------
// Config application (game/towers/TowerMechanics.h)
// ---------------------------------------------------------------------------

const TowerMechanics& tower_mechanics(TowerType type) {
    init_mechanics_once();
    const u32 t = static_cast<u32>(type) < kTowerTypeCount ? static_cast<u32>(type) : 0u;
    return g_mechanics[t];
}

const TowerGlobals& tower_globals() {
    init_mechanics_once();
    return g_globals;
}

sim::SwarmerProfile swarmer_profile(TowerType type) {
    const TowerMechanics& m = tower_mechanics(type);
    sim::SwarmerProfile p;
    p.kind = tower_kind(type);
    p.source = type;

    p.lifetime = m.swarm.lifetime;
    p.speed = m.swarm.speed;
    p.search_radius = m.swarm.search_radius;
    p.attach_radius = m.swarm.attach_radius;
    p.size = m.swarm.size;
    p.max_health = m.swarm.max_health;

    p.dps = m.latch.dps;
    p.attach_seconds = m.latch.attach_seconds;
    p.attach_steps = m.latch.attach_steps;

    p.fire_interval = m.shooter.fire_interval;
    p.round_damage = m.shooter.round_damage;
    p.round_speed = m.shooter.round_speed;
    p.round_hit_radius = m.shooter.round_hit_radius;
    p.round_spread = m.shooter.round_spread;
    p.formation_spacing = m.shooter.formation_spacing;
    p.kite_fraction = m.shooter.kite_fraction;
    p.kite_flow_weight = m.shooter.kite_flow_weight;
    p.kite_speed_mult = m.shooter.kite_speed_mult;
    p.magazine_size = m.shooter.magazine_size;
    p.gather_seconds = m.shooter.gather_seconds;
    p.reload_seconds = m.shooter.reload_seconds;
    p.volley_cone = m.shooter.volley_cone;
    p.granule_size = m.shooter.granule_size;
    p.magazine_spread = m.shooter.magazine_spread;
    p.round_size = m.shooter.round_size;
    p.round_grow_seconds = m.shooter.round_grow_seconds;
    p.spawn_seconds = m.shooter.spawn_seconds;
    p.spawn_start_scale = m.shooter.spawn_start_scale;
    p.spawn_overshoot = m.shooter.spawn_overshoot;
    p.spawn_peak = m.shooter.spawn_peak;
    p.spawn_ease_power = m.shooter.spawn_ease_power;

    switch (p.kind) {
    case sim::SwarmerKind::Bomber:      p.chase_seconds = m.bomber.chase_seconds; break;
    case sim::SwarmerKind::MucusBomber: p.chase_seconds = m.mucus_bomber.chase_seconds; break;
    case sim::SwarmerKind::ArborGrabber:p.chase_seconds = 0.0f; break;
    default:                            p.chase_seconds = 0.0f; break;
    }

    p.burst_radius = m.bomber.burst_radius;
    p.burst_damage = m.bomber.burst_damage;
    p.burst_seconds = m.bomber.burst_seconds;
    p.burst_falloff = m.bomber.burst_falloff;
    p.burst_named_damage = m.bomber.named_damage;

    p.splash_droplets = m.mucus_bomber.droplets;
    p.splash_radius = m.mucus_bomber.splash_radius;
    p.splash_speed = m.mucus_bomber.splash_speed;
    p.splash_lifetime = m.mucus_bomber.droplet_lifetime;
    p.mucus_slow_duration = m.mucus_bomber.slow_duration;
    p.mucus_slow_factor = m.mucus_bomber.slow_factor;

    p.arbor_arm_count = math::min<u32>(m.arbor_grabber.arm_count, sim::kArborMaxArms);
    p.arbor_max_captives = math::max<u32>(1u, math::min<u32>(m.arbor_grabber.max_captives,
                                                              sim::kArborMaxCaptives));
    p.arbor_cluster_radius = math::max(m.arbor_grabber.cluster_radius, 0.0f);
    p.arbor_extend_seconds = m.arbor_grabber.extend_seconds;
    p.arbor_latch_seconds = m.arbor_grabber.latch_seconds;
    p.arbor_pull_seconds = m.arbor_grabber.pull_seconds;
    p.arbor_recover_seconds = m.arbor_grabber.recover_seconds;
    p.arbor_fake_mass = math::saturate(m.arbor_grabber.fake_mass);
    // The kite_* fields are one set on the profile; an arbor grabber's body
    // reads its own tuning through them.
    if (p.kind == sim::SwarmerKind::ArborGrabber) {
        p.kite_fraction = m.arbor_grabber.kite_fraction;
        p.kite_flow_weight = m.arbor_grabber.kite_flow_weight;
        p.kite_speed_mult = m.arbor_grabber.kite_speed_mult;
        // The LANE WALL: the rank's spacing rides the shooter's formation
        // field, and the body pushes the horde back instead of yielding.
        p.formation_spacing = m.arbor_grabber.wall_spacing;
        p.body_block = math::saturate(m.arbor_grabber.body_block);
    }

    p.scar_half_length = m.builder.scar_half_length;
    p.scar_half_width = m.builder.scar_half_width;
    p.scar_tilt = m.builder.scar_tilt;
    p.scar_health = m.builder.scar_health;
    p.scar_reinforce = m.builder.scar_reinforce;
    p.scar_spacing = m.builder.scar_spacing;
    p.max_scars = m.builder.max_scars;
    p.scar_lifetime = m.builder.scar_lifetime;
    p.build_radius = m.builder.build_radius;
    p.build_min_radius = m.builder.build_min_radius;
    p.build_min_clearance = m.builder.build_min_clearance;
    p.build_candidates = m.builder.build_candidates;
    p.builder_crowd_push = m.builder.crowd_push;

    // Strengthen Immunity capstones. Off in every loaded config; only the
    // tree's run-start copy turns them on (GameConfig.h, CapstoneParams).
    p.kill_pulse_radius = m.capstone.kill_pulse_radius;
    p.kill_pulse_damage = m.capstone.kill_pulse_damage;
    p.named_damage_mult = m.capstone.named_damage_mult;
    p.heal_per_kill = m.capstone.heal_per_kill;
    return p;
}

void apply_tower_config(TowerSystem& towers, const TowerConfig& cfg) {
    init_mechanics_once();
    for (u32 t = 0; t < kTowerTypeCount; ++t) {
        towers.set_stats(static_cast<TowerType>(t), cfg.stats[t]);
        g_mechanics[t] = cfg.mechanics[t];
        // vfx/ cannot see game/, so the units' death burst is pushed down,
        // the same way apply_enemy_config does the pathogens'.
        vfx::set_swarmer_death_vfx(static_cast<TowerType>(t), cfg.death_vfx[t]);
    }
    g_globals = cfg.globals;
}

} // namespace immune::game
