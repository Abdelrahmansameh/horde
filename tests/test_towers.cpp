// Tests for game/towers/TowerSystem.cpp: placement validation, the tissue/
// flow-field footprint round-trip, upgrade/sell economics, find_target(), and
// — since Wave 6C — the combat behaviour of the six-role roster.
//
// Scene layout used by the placement/geometry tests (see make_world()):
//
//   y
//   15 #########################........##########################
//      #     left room (0..24)   .  corridor (25..34, rows 9-10)  .     right room (35..59)     #
//   4  #########################........##########################
//      0                        25       35                                                    60 (x, world units == cells, cell_size=1)
//
// The corridor is exactly 2 cells (2 world units) tall — narrow enough that a
// tower whose footprint clears it (per InsufficientClearance) also fully
// plugs it (WouldBlockAllPaths). Both rooms are deliberately much larger than
// would_block_all_paths()'s local search window (a ~14-cell margin around the
// footprint — see TowerSystem.cpp), so the window's border lands on genuine
// open interior on both sides of the corridor rather than swallowing an
// entire room and losing one side's anchors.
//
// WAVE 6C COMBAT TESTS
// Each role gets a test that proves its CHARACTERISTIC GEOMETRY, not merely
// that it did damage: the cone hits in front and not behind, the laser hits
// along its line and not off-axis, the blade hits adjacent and not distant,
// the mortar's burst is genuinely large, the tesla chains across separated
// targets, and the gunner puts real rounds into world.projectiles() that then
// damage chaff. Each also asserts the CombatEvent the VFX layer keys on — the
// whole particle layer is downstream of those events and renders nothing
// without them.
#include "game/towers/TowerSystem.h"

#include "core/JobSystem.h"
#include "core/Math.h"
#include "core/Profiler.h"
#include "core/Rng.h"
#include "platform/Window.h"
#include "render/Camera.h"
#include "render/Renderer.h"
#include "render/Screenshot.h"
#include "sim/CombatEvents.h"
#include "sim/SimWorld.h"
#include "sim/ecs/Components.h"
#include "sim/ecs/NamedAgents.h"
#include "vfx/Particles.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace immune;
using namespace immune::sim;
using namespace immune::game;

namespace {

constexpr i32 kW = 60;
constexpr i32 kH = 20;

Vec2 kRoomCenterLeft{12.0f, 10.0f};
Vec2 kRoomCenterRight{47.0f, 10.0f};
Vec2 kCorridorCenter{30.0f, 10.5f};   // centre of the 5-cell corridor (y 8..13)
Vec2 kGoal{55.0f, 10.0f};

SimWorld make_world() {
    SimWorld world;
    SimDesc desc;
    desc.seed = 12345;
    desc.max_chaff = 4096;
    desc.world_bounds = Rect{Vec2{0.0f, 0.0f}, Vec2{static_cast<f32>(kW), static_cast<f32>(kH)}};
    desc.spatial_cell_size = 2.0f;
    world.init(desc, nullptr);

    TissueMask& mask = world.tissue();
    mask.resize(kW, kH, 1.0f, Vec2{0.0f, 0.0f});
    for (i32 y = 4; y < 16; ++y)
        for (i32 x = 0; x < 25; ++x) mask.set_walkable(x, y, true); // left room
    for (i32 y = 4; y < 16; ++y)
        for (i32 x = 35; x < 60; ++x) mask.set_walkable(x, y, true); // right room
    // 5-cell-tall corridor. Sized to the tower footprints, not chosen freely:
    // this test only means anything if the corridor is wide enough that the
    // tower FITS (clearance >= footprint_radius, or validate() rejects it for
    // InsufficientClearance and never reaches the reachability check it is
    // actually testing) while still being narrow enough that the footprint
    // spans every corridor cell and genuinely plugs it.
    //
    // The SDF measures centre-to-wall less half a cell, so a corridor H cells
    // tall has centre clearance H/2 - 0.5. Footprint 1.6 therefore needs
    // H >= 4.2, i.e. 5, giving clearance 2.0. The 3.2-wide footprint then spans
    // y 8.9..12.1, which still intersects all five corridor cells (8..12) and
    // blocks them.
    //
    // It was 3 cells while footprints were <= 0.8 (clearance 1.0 >= 0.8). When
    // every tower's footprint_radius was doubled, a 1.6-radius tower stopped
    // fitting a 3-cell corridor at all and this test began failing on the
    // clearance assertion instead of exercising reachability.
    for (i32 y = 8; y < 13; ++y)
        for (i32 x = 25; x < 35; ++x) mask.set_walkable(x, y, true);

    world.sdf().bake(mask);
    FlowFieldBakeDesc fdesc;
    fdesc.goal_cells = {mask.world_to_cell(kGoal)};
    world.flow().bake(mask, fdesc);
    return world;
}

SystemContext make_ctx(SimWorld& world) {
    return SystemContext{world, world.ecs().registry(), world.rng(), kFixedDt, world.tick_index()};
}

void rebuild_spatial(SimWorld& world) {
    world.spatial().rebuild(world.chaff().pos_x.data(), world.chaff().pos_y.data(), world.chaff().count(), nullptr);
}

/// One combat step with the chaff held still.
///
/// SimWorld::tick() would also flow-move and compact the horde, which makes a
/// geometry assertion ("this cluster took damage and that one did not") race
/// the movement kernel. This runs exactly the damage-relevant half of the
/// canonical tick order (SimWorld.h): hash rebuild -> ECS systems -> aggregate
/// damage -> projectiles -> transient-field expiry. No movement, no compaction,
/// so chaff indices and positions are stable across the whole test.
void step_combat(SimWorld& world) {
    rebuild_spatial(world);
    SystemContext ctx = make_ctx(world);
    world.ecs().tick(ctx);
    world.damage().apply(world.chaff(), world.spatial(), world.rng(), kFixedDt);
    world.projectile_system().update(world.projectiles(), world.chaff(), world.spatial(),
                                     world.desc().world_bounds, world.rng(), kFixedDt,
                                     &world.combat_events());
    world.damage().clear_transient(kFixedDt);
}

/// Just the submission half of a step. Needed because step_combat() ends with
/// clear_transient(), which by contract drops every `lifetime <= 0` field —
/// i.e. exactly the PERSISTENT fields the Cryo/Laser/Blade resubmit each tick.
/// A test that wants to look at those has to look between submission and
/// expiry, which is the same window sim/damage itself evaluates them in.
void submit_only(SimWorld& world) {
    rebuild_spatial(world);
    SystemContext ctx = make_ctx(world);
    world.ecs().tick(ctx);
}

void spawn_chaff_cluster(SimWorld& world, Vec2 center, u32 count, f32 density, f32 spread = 0.3f) {
    Rng r(999);
    for (u32 i = 0; i < count; ++i) {
        ChaffSpawnParams p;
        p.position = center + Vec2{r.range_f(-spread, spread), r.range_f(-spread, spread)};
        p.density = density;
        p.family = PathogenFamily::Virus;
        world.chaff().spawn(p);
    }
}

void spawn_one(SimWorld& world, Vec2 pos, f32 density = 4.0f) {
    ChaffSpawnParams p;
    p.position = pos;
    p.density = density;
    p.family = PathogenFamily::Virus;
    world.chaff().spawn(p);
}

/// Total remaining chaff density inside a circle. Test-side only; the sim never
/// walks the store like this.
f32 density_in(const SimWorld& world, Vec2 center, f32 radius) {
    const ChaffBuffers& c = world.chaff();
    f32 sum = 0.0f;
    for (usize i = 0; i < c.count(); ++i) {
        const Vec2 p{c.pos_x[i], c.pos_y[i]};
        if (math::length_sq(p - center) <= radius * radius) sum += c.density[i];
    }
    return sum;
}

usize count_events(const SimWorld& world, CombatEventType type, TowerType source) {
    usize n = 0;
    for (const CombatEvent& e : world.combat_events().events()) {
        if (e.type == type && e.source == source) ++n;
    }
    return n;
}

const CombatEvent* first_event(const SimWorld& world, CombatEventType type, TowerType source) {
    for (const CombatEvent& e : world.combat_events().events()) {
        if (e.type == type && e.source == source) return &e;
    }
    return nullptr;
}

/// Clears a tower's cooldown so a test does not have to spend its spin-up
/// (2.6 s for the Mortar) in ticks before the interesting thing happens.
void ready_now(SimWorld& world, EntityId tower) {
    world.ecs().registry().get<comp::Tower>(world.ecs().from_id(tower)).cooldown = 0.0f;
}

EntityId spawn_named(SimWorld& world, Vec2 pos, f32 health_scale = 1.0f) {
    const u16 archetype = named::placeholder_elite(world.ecs().registry());
    named::SpawnParams p;
    p.archetype = archetype;
    p.position = pos;
    p.health_scale = health_scale;
    return named::spawn(world, p);
}

} // namespace

// ---------------------------------------------------------------------------
// Stats table
// ---------------------------------------------------------------------------

TEST_CASE("default stats table covers every type/tier with real, distinguishing numbers",
          "[towers][stats]") {
    TowerSystem ts;
    for (u32 t = 0; t < kTowerTypeCount; ++t) {
        const auto type = static_cast<TowerType>(t);
        const TowerStats& s1 = ts.stats(type, 1);
        const TowerStats& s2 = ts.stats(type, 2);
        const TowerStats& s3 = ts.stats(type, 3);
        INFO("tower type " << tower_type_name(type));
        REQUIRE(s1.build_cost > 0);
        REQUIRE(s1.footprint_radius > 0.0f);
        REQUIRE(s1.fire_interval > 0.0f);
        // Tiers scale range monotonically; every roster entry does in this table.
        REQUIRE(s2.range >= s1.range);
        REQUIRE(s3.range >= s2.range);
        // At least one of damage/kill_rate strictly increases tier-over-tier.
        // Every tower in the six-type roster is combat-capable, so unlike the
        // old eight-type roster there is no support-tower exception here.
        {
            const bool damage_grows = s3.damage > s1.damage;
            const bool kill_grows = s3.kill_rate > s1.kill_rate;
            REQUIRE((damage_grows || kill_grows));
        }
        // Rate goes UP with tier for every role (fire_interval goes down).
        REQUIRE(s3.fire_interval <= s1.fire_interval);
    }
}

TEST_CASE("the six roles occupy genuinely different niches in the stats table", "[towers][stats]") {
    // The point of the retune: a player should be able to describe each tower in
    // one phrase. These are the numeric shapes behind those phrases, so a future
    // tuning pass that accidentally flattens the roster fails here rather than
    // silently making six towers feel like one.
    TowerSystem ts;
    const TowerStats gunner = ts.stats(TowerType::Neutrophil, 1);
    const TowerStats mortar = ts.stats(TowerType::Macrophage, 1);
    const TowerStats cryo   = ts.stats(TowerType::Interferon, 1);
    const TowerStats tesla  = ts.stats(TowerType::CytotoxicT, 1);
    const TowerStats laser  = ts.stats(TowerType::BCell, 1);
    const TowerStats blade  = ts.stats(TowerType::NKCell, 1);

    // GUNNER is the only tower with no damage field at all: its damage is
    // delivered by real projectiles.
    REQUIRE(gunner.kill_rate == 0.0f);
    // ...and it is the fastest-firing thing that isn't the contact rotor.
    REQUIRE(gunner.fire_interval < cryo.fire_interval);
    REQUIRE(gunner.fire_interval < tesla.fire_interval);

    // MORTAR: slowest cadence in the roster by a wide margin, longest single
    // burst, and the highest instantaneous kill_rate.
    REQUIRE(mortar.fire_interval > 5.0f * laser.fire_interval);
    REQUIRE(mortar.kill_rate > tesla.kill_rate);

    // CRYO: the weakest killer in the roster. Its value is the slow.
    REQUIRE(cryo.kill_rate < mortar.kill_rate);
    REQUIRE(cryo.kill_rate < tesla.kill_rate);
    REQUIRE(cryo.kill_rate < laser.kill_rate);
    REQUIRE(cryo.kill_rate < blade.kill_rate);

    // LASER reaches furthest of the continuous towers; BLADE reaches least far
    // of anything, by a lot.
    REQUIRE(laser.range > gunner.range);
    REQUIRE(laser.range > cryo.range);
    REQUIRE(blade.range < 0.5f * gunner.range);

    // Cost ordering: the cheap workhorse, then the wall, then the specialists.
    REQUIRE(gunner.build_cost < blade.build_cost);
    REQUIRE(blade.build_cost < cryo.build_cost);
    REQUIRE(laser.build_cost > mortar.build_cost);
}

TEST_CASE("tower_type_name/parse_tower_type round-trip for every roster type", "[towers][naming]") {
    for (u32 t = 0; t < kTowerTypeCount; ++t) {
        const auto type = static_cast<TowerType>(t);
        TowerType parsed{};
        REQUIRE(parse_tower_type(tower_type_name(type), parsed));
        REQUIRE(parsed == type);
    }
    TowerType unused{};
    REQUIRE_FALSE(parse_tower_type("not_a_tower", unused));
}

// ---------------------------------------------------------------------------
// Upgrade-over-expand cost curve (DESIGN.md §5.3/§7.1).
//
// Each role spends a different stat, so there is no single unified "output"
// number that means anything across all six. tower_output() below mirrors
// exactly what each role's Combat-phase system actually reads:
//
//   GUNNER  damage / fire_interval            -- projectile throughput; its
//                                                kill_rate is 0 by design.
//   MORTAR  kill_rate * burst    / interval   -- BURST role: the field only
//   TESLA   kill_rate * arc_time / interval      exists for a fraction of the
//                                                cycle, so the duty cycle is
//                                                part of the output.
//   CRYO    kill_rate                         -- continuous, persistent field
//   LASER   kill_rate                            resubmitted every tick.
//   BLADE   kill_rate
//
// kMortarBurstSeconds / kTeslaArcSeconds are the same constants declared at the
// top of src/game/towers/TowerSystem.cpp; they have nowhere to live in the
// frozen TowerStats struct.
// ---------------------------------------------------------------------------

namespace {

constexpr f32 kMortarBurstSeconds = 0.30f;
constexpr f32 kTeslaArcSeconds = 0.12f;

f32 tower_output(TowerType type, const TowerStats& s) {
    const f32 rate = s.fire_interval > 0.0f ? 1.0f / s.fire_interval : 0.0f;
    switch (type) {
    case TowerType::Neutrophil: return s.damage * rate;
    case TowerType::Macrophage: return s.kill_rate * kMortarBurstSeconds * rate;
    case TowerType::CytotoxicT: return s.kill_rate * kTeslaArcSeconds * rate;
    case TowerType::Interferon:
    case TowerType::BCell:
    case TowerType::NKCell:
    default:                    return s.kill_rate;
    }
}

} // namespace

TEST_CASE("upgrading is a better ATP-per-output deal than a fresh tower, for every tower type",
          "[towers][economy][cost_curve]") {
    TowerSystem ts;
    for (u32 t = 0; t < kTowerTypeCount; ++t) {
        const auto type = static_cast<TowerType>(t);
        INFO("tower type " << tower_type_name(type));
        const TowerStats& s1 = ts.stats(type, 1);
        const TowerStats& s2 = ts.stats(type, 2);
        const TowerStats& s3 = ts.stats(type, 3);

        // Structural cost-curve shape (DESIGN.md §5.3): each upgrade step
        // costs noticeably less than a fresh tier-1 build, and the curve
        // decelerates further up the tree.
        REQUIRE(s1.upgrade_cost < s1.build_cost);
        REQUIRE(s2.upgrade_cost < s1.upgrade_cost);
        REQUIRE(s2.upgrade_cost > 0);

        const f32 o1 = tower_output(type, s1);
        const f32 o2 = tower_output(type, s2);
        const f32 o3 = tower_output(type, s3);
        REQUIRE(o1 > 0.0f);

        // Accelerating value: tier 2 pushes output well above 2x tier 1's,
        // and tier 3 pulls further ahead still (the absolute gap grows).
        REQUIRE(o2 > 2.0f * o1);
        REQUIRE((o3 - o2) > (o2 - o1));

        // The actual economic claim: ATP spent all the way up the upgrade
        // tree buys strictly more output-per-ATP than stopping at tier 1 (and
        // therefore than spending the same total ATP on N fresh tier-1
        // towers, which nets exactly tier 1's own output-per-ATP — spreading
        // thin is a visible tax, not the efficient move).
        const f32 cost_to_t1 = static_cast<f32>(s1.build_cost);
        const f32 cost_to_t2 = cost_to_t1 + static_cast<f32>(s1.upgrade_cost);
        const f32 cost_to_t3 = cost_to_t2 + static_cast<f32>(s2.upgrade_cost);
        const f32 eff1 = o1 / cost_to_t1;
        const f32 eff2 = o2 / cost_to_t2;
        const f32 eff3 = o3 / cost_to_t3;
        REQUIRE(eff2 > eff1);
        REQUIRE(eff3 > eff2);
    }
}

// ---------------------------------------------------------------------------
// validate()/place()/upgrade()/sell()
// ---------------------------------------------------------------------------

TEST_CASE("validate() rejects off-tissue and unaffordable placements, accepts an open interior spot",
          "[towers][placement]") {
    SimWorld world = make_world();
    TowerSystem ts;

    const auto off_tissue = ts.validate(world, TowerType::Macrophage, Vec2{13.0f, 2.0f}, 100000);
    REQUIRE(off_tissue.result == PlacementResult::NotOnTissue);

    const auto ok = ts.validate(world, TowerType::Macrophage, kRoomCenterLeft, 100000);
    REQUIRE(ok.result == PlacementResult::Ok);

    const auto broke = ts.validate(world, TowerType::Macrophage, kRoomCenterLeft, /*available_atp=*/0);
    REQUIRE(broke.result == PlacementResult::CannotAfford);
}

TEST_CASE("validate() rejects insufficient clearance near a wall", "[towers][placement]") {
    SimWorld world = make_world();
    TowerSystem ts;
    // Interferon (CRYO) has the roster's largest tier-1 footprint_radius (1.2);
    // 0.5 units from the left room's wall (x=0 boundary) leaves far less than that.
    REQUIRE(ts.stats(TowerType::Interferon, 1).footprint_radius > 0.5f);
    const auto q = ts.validate(world, TowerType::Interferon, Vec2{0.5f, 10.0f}, 100000);
    REQUIRE(q.result == PlacementResult::InsufficientClearance);
    REQUIRE(q.clearance < ts.stats(TowerType::Interferon, 1).footprint_radius);
    // The light snap should nudge back toward more clearance (away from x=0).
    REQUIRE(q.snapped_position.x > 0.5f);
}

TEST_CASE("validate() rejects a placement overlapping an existing tower", "[towers][placement]") {
    SimWorld world = make_world();
    TowerSystem ts;
    const EntityId first = ts.place(world, TowerType::Macrophage, kRoomCenterLeft);
    REQUIRE(first.valid());

    const auto q = ts.validate(world, TowerType::Macrophage, kRoomCenterLeft + Vec2{0.3f, 0.0f}, 100000);
    REQUIRE(q.result == PlacementResult::Overlapping);

    const auto far_enough = ts.validate(world, TowerType::Macrophage, kRoomCenterLeft + Vec2{4.0f, 0.0f}, 100000);
    REQUIRE(far_enough.result == PlacementResult::Ok);
}

TEST_CASE("validate() rejects a placement that would fully plug the only corridor, "
          "but allows the same tower with room to spare",
          "[towers][placement][reachability]") {
    SimWorld world = make_world();
    TowerSystem ts;

    // Sanity: the goal is reachable from the left room before any placement.
    REQUIRE(world.flow().reachable(kRoomCenterLeft));

    const auto blocked = ts.validate(world, TowerType::NKCell, kCorridorCenter, 100000);
    REQUIRE(blocked.result == PlacementResult::WouldBlockAllPaths);

    const auto open = ts.validate(world, TowerType::NKCell, kRoomCenterRight, 100000);
    REQUIRE(open.result == PlacementResult::Ok);
}

TEST_CASE("place() blocks tissue over exactly its footprint and marks the flow field dirty",
          "[towers][placement][flowfield]") {
    SimWorld world = make_world();
    TowerSystem ts;
    REQUIRE_FALSE(world.flow().has_pending_rebake());

    const EntityId id = ts.place(world, TowerType::Macrophage, kRoomCenterLeft);
    REQUIRE(id.valid());

    // footprint_radius (tier 1) is 1.0, so the tower's own cell must now be blocked.
    const IVec2 c = world.tissue().world_to_cell(kRoomCenterLeft);
    REQUIRE_FALSE(world.tissue().walkable(c.x, c.y));
    REQUIRE(world.flow().has_pending_rebake());

    world.flow().rebake_pending(world.tissue());
    REQUIRE_FALSE(world.flow().has_pending_rebake());
    // The room is large; a detour around the tower still reaches the goal.
    REQUIRE(world.flow().reachable(kRoomCenterLeft + Vec2{2.0f, 3.0f}));
}

TEST_CASE("a freshly placed tower spins up rather than discharging on the placement tick",
          "[towers][placement][combat]") {
    // tests/scripts/tower_thins_horde.json asserts chaff_killed_total == 0 on
    // the tick the tower lands. With a 2.6s Mortar cycle this is the difference
    // between placing a tower and instantly deleting the wave.
    SimWorld world = make_world();
    TowerSystem ts;
    ts.register_systems(world);
    const EntityId tower = ts.place(world, TowerType::Macrophage, kRoomCenterLeft);
    REQUIRE(tower.valid());
    const comp::Tower& tw = world.ecs().registry().get<comp::Tower>(world.ecs().from_id(tower));
    REQUIRE(tw.cooldown == ts.stats(TowerType::Macrophage, 1).fire_interval);

    spawn_chaff_cluster(world, kRoomCenterLeft + Vec2{6.0f, 0.0f}, 20, 1.0f);
    const f32 before = world.chaff().total_density();
    step_combat(world);
    REQUIRE(world.chaff().total_density() == before);
    REQUIRE(world.combat_events().size() == 0);
}

TEST_CASE("upgrade() advances tier and stats, caps at 3; sell() refunds ATP and restores tissue",
          "[towers][economy]") {
    SimWorld world = make_world();
    TowerSystem ts;
    const EntityId id = ts.place(world, TowerType::Macrophage, kRoomCenterLeft);
    REQUIRE(id.valid());

    const entt::entity e = world.ecs().from_id(id);
    REQUIRE(world.ecs().registry().get<comp::Tower>(e).tier == 1);
    REQUIRE(world.ecs().registry().get<comp::Tower>(e).range == ts.stats(TowerType::Macrophage, 1).range);

    REQUIRE(ts.upgrade(world, id) == 2);
    REQUIRE(world.ecs().registry().get<comp::Tower>(e).tier == 2);
    REQUIRE(world.ecs().registry().get<comp::Tower>(e).range == ts.stats(TowerType::Macrophage, 2).range);

    REQUIRE(ts.upgrade(world, id) == 3);
    REQUIRE(world.ecs().registry().get<comp::Tower>(e).tier == 3);
    REQUIRE(ts.upgrade(world, id) == 0); // already maxed

    // invested = tier1 build_cost (150) + upgrade to tier2 (95) + upgrade to
    // tier3 (80) = 325; refund at the documented 0.7 fraction = 227 (truncated).
    const u32 expected = static_cast<u32>(
        (static_cast<f32>(ts.stats(TowerType::Macrophage, 1).build_cost) +
         static_cast<f32>(ts.stats(TowerType::Macrophage, 1).upgrade_cost) +
         static_cast<f32>(ts.stats(TowerType::Macrophage, 2).upgrade_cost)) * 0.7f);
    const u32 refund = ts.sell(world, id);
    REQUIRE(refund == expected);
    REQUIRE(refund == 227);

    REQUIRE_FALSE(world.ecs().registry().valid(e));
    const IVec2 c = world.tissue().world_to_cell(kRoomCenterLeft);
    REQUIRE(world.tissue().walkable(c.x, c.y)); // tissue restored
    REQUIRE(world.flow().has_pending_rebake());  // dirtied again by the sell

    bool still_listed = false;
    for (EntityId t : ts.placed_towers())
        if (t == id) still_listed = true;
    REQUIRE_FALSE(still_listed);
}

// ---------------------------------------------------------------------------
// find_target()
// ---------------------------------------------------------------------------

TEST_CASE("find_target respects range and family_mask, and hides Burrowed agents unless asked",
          "[towers][targeting]") {
    SimWorld world = make_world();
    TowerSystem ts;
    const EntityId agent = spawn_named(world, kRoomCenterLeft);
    REQUIRE(agent.valid());
    const PathogenFamily family = world.ecs().registry().get<comp::NamedAgent>(world.ecs().from_id(agent)).family;
    const u8 fam_bit = static_cast<u8>(1u << static_cast<u8>(family));

    REQUIRE(ts.find_target(world, kRoomCenterLeft, 5.0f, 0xFF, false) == agent);
    REQUIRE_FALSE(ts.find_target(world, kRoomCenterLeft + Vec2{3.0f, 0.0f}, 1.0f, 0xFF, false).valid()); // out of range
    REQUIRE_FALSE(ts.find_target(world, kRoomCenterLeft + Vec2{20.0f, 0.0f}, 2.0f, 0xFF, false).valid());
    REQUIRE_FALSE(ts.find_target(world, kRoomCenterLeft, 5.0f, static_cast<u8>(~fam_bit), false).valid());

    world.ecs().registry().get<comp::AiBrain>(world.ecs().from_id(agent)).state = comp::AiState::Burrowed;
    REQUIRE_FALSE(ts.find_target(world, kRoomCenterLeft, 5.0f, 0xFF, false).valid());
    REQUIRE(ts.find_target(world, kRoomCenterLeft, 5.0f, 0xFF, /*require_detect_hidden=*/true) == agent);
}

// ---------------------------------------------------------------------------
// GUNNER — Neutrophil. Real projectiles.
// ---------------------------------------------------------------------------

TEST_CASE("GUNNER spawns real rounds into world.projectiles(), and those rounds damage chaff",
          "[towers][combat][gunner][projectiles]") {
    SimWorld world = make_world();
    TowerSystem ts;
    ts.register_systems(world);
    const EntityId tower = ts.place(world, TowerType::Neutrophil, kRoomCenterLeft);
    REQUIRE(tower.valid());
    ready_now(world, tower);

    const Vec2 horde = kRoomCenterLeft + Vec2{6.0f, 0.0f};
    spawn_chaff_cluster(world, horde, 24, 1.0f, /*spread=*/0.35f);
    const f32 before = density_in(world, horde, 1.5f);

    // One step: a round must exist in the store, and it must be moving toward
    // the horde rather than sitting on the muzzle.
    step_combat(world);
    REQUIRE(world.projectiles().count() >= 1);
    REQUIRE(world.projectiles().vel_x[0] > 10.0f);
    REQUIRE(count_events(world, CombatEventType::MuzzleFlash, TowerType::Neutrophil) == 1);

    // The Gunner is the one tower that publishes NO damage field. If this ever
    // fails, someone has quietly turned it back into an area tower.
    for (const DamageField& f : world.damage().fields()) {
        INFO("gunner must not submit a damage field");
        REQUIRE(f.owner != tower);
    }

    for (int i = 0; i < 90; ++i) step_combat(world);

    const f32 after = density_in(world, horde, 1.5f);
    INFO("density before=" << before << " after=" << after
         << " impacts=" << count_events(world, CombatEventType::ProjectileImpact, TowerType::Count));
    REQUIRE(after < before);
    // Rounds are what did it: the projectile system raises these, not the tower.
    REQUIRE(count_events(world, CombatEventType::ProjectileImpact, TowerType::Count) > 0);
    // ...and the stream is a stream: many shots over 1.5 simulated seconds.
    REQUIRE(count_events(world, CombatEventType::MuzzleFlash, TowerType::Neutrophil) >= 10);
}

TEST_CASE("GUNNER fire rate escalates hard with tier and rounds never outrun the spatial hash",
          "[towers][combat][gunner]") {
    TowerSystem ts;
    // Rate escalation: tier 3 must fire at least 2.5x as often as tier 1, or the
    // "continuous stream" reading never arrives.
    const f32 t1 = ts.stats(TowerType::Neutrophil, 1).fire_interval;
    const f32 t3 = ts.stats(TowerType::Neutrophil, 3).fire_interval;
    REQUIRE(t1 / t3 >= 2.5f);

    // Projectiles.cpp: a round whose per-tick step greatly exceeds the spatial
    // hash cell size tunnels past agents. The default cell is 4.0 world units.
    SimDesc defaults;
    for (u8 tier = 1; tier <= 3; ++tier) {
        SimWorld world = make_world();
        TowerSystem live;
        live.register_systems(world);
        const EntityId tower = live.place(world, TowerType::Neutrophil, kRoomCenterLeft);
        REQUIRE(tower.valid());
        for (u8 k = 1; k < tier; ++k) REQUIRE(live.upgrade(world, tower) == k + 1);
        ready_now(world, tower);
        spawn_chaff_cluster(world, kRoomCenterLeft + Vec2{6.0f, 0.0f}, 12, 1.0f);

        step_combat(world);
        REQUIRE(world.projectiles().count() >= 1);
        const f32 speed = math::length(Vec2{world.projectiles().vel_x[0], world.projectiles().vel_y[0]});
        const f32 step = speed * kFixedDt;
        INFO("tier " << static_cast<int>(tier) << " round speed=" << speed << " step/tick=" << step);
        REQUIRE(step < defaults.spatial_cell_size);
    }
}

// ---------------------------------------------------------------------------
// MORTAR — Macrophage. Big, slow, consequential.
// ---------------------------------------------------------------------------

TEST_CASE("MORTAR lands one big Circle burst and raises an Explosion whose radius drives the ring",
          "[towers][combat][mortar]") {
    SimWorld world = make_world();
    TowerSystem ts;
    ts.register_systems(world);
    const EntityId tower = ts.place(world, TowerType::Macrophage, kRoomCenterLeft);
    REQUIRE(tower.valid());
    ready_now(world, tower);

    // A wide field of targets so the burst's real footprint is measurable.
    const Vec2 horde{22.0f, 10.0f};
    spawn_chaff_cluster(world, horde, 200, 30.0f, /*spread=*/4.0f);
    std::vector<f32> before(world.chaff().count());
    for (usize i = 0; i < world.chaff().count(); ++i) before[i] = world.chaff().density[i];

    step_combat(world);

    const CombatEvent* boom = first_event(world, CombatEventType::Explosion, TowerType::Macrophage);
    REQUIRE(boom != nullptr);
    REQUIRE(boom->radius >= 5.0f);
    REQUIRE(boom->visual_id == 1);   // tier 1 -> the VFX layer's escalation slot 1

    // A Circle field owned by this tower, on a real (non-persistent) lifetime.
    bool found = false;
    for (const DamageField& f : world.damage().fields()) {
        if (f.owner != tower) continue;
        REQUIRE(f.shape == FieldShape::Circle);
        REQUIRE(f.radius >= 5.0f);
        REQUIRE(f.lifetime > 0.0f);
        found = true;
    }
    REQUIRE(found);

    // The burst is BIG: something at least 3.5 units from the impact point took
    // damage, which no other tower in the roster can claim at this range.
    f32 furthest_hit = 0.0f;
    for (usize i = 0; i < world.chaff().count(); ++i) {
        if (world.chaff().density[i] >= before[i]) continue;
        const Vec2 p{world.chaff().pos_x[i], world.chaff().pos_y[i]};
        furthest_hit = math::max(furthest_hit, math::length(p - boom->origin));
    }
    INFO("furthest damaged agent was " << furthest_hit << " units from the impact");
    REQUIRE(furthest_hit > 3.5f);
}

TEST_CASE("MORTAR measurably thins a spawned chaff cluster over many ticks",
          "[towers][combat][mortar][integration]") {
    SimWorld world = make_world();
    TowerSystem ts;
    ts.register_systems(world);
    REQUIRE(ts.place(world, TowerType::Macrophage, kRoomCenterLeft).valid());
    spawn_chaff_cluster(world, kRoomCenterLeft + Vec2{4.0f, 0.0f}, 40, 1.0f, /*spread=*/0.5f);

    const f32 density_before = world.chaff().total_density();
    for (int i = 0; i < 300; ++i) world.tick();
    const f32 density_after = world.chaff().total_density();

    INFO("density before=" << density_before << " after=" << density_after);
    REQUIRE(density_after < density_before);
}

// ---------------------------------------------------------------------------
// CRYO — Interferon. Cone geometry + the slow.
// ---------------------------------------------------------------------------

TEST_CASE("CRYO hits and slows what is in front of its cone and nothing behind it",
          "[towers][combat][cryo]") {
    SimWorld world = make_world();
    TowerSystem ts;
    ts.register_systems(world);
    const EntityId tower = ts.place(world, TowerType::Interferon, kRoomCenterLeft);
    REQUIRE(tower.valid());
    ready_now(world, tower);

    // In front, and deliberately inside the inner 55% of the cone's reach —
    // that band is where a caught agent reads as fully encased rather than
    // merely slowed, which is the Freeze event asserted below.
    const Vec2 in_front = kRoomCenterLeft + Vec2{3.5f, 0.0f};
    const Vec2 behind = kRoomCenterLeft - Vec2{5.0f, 0.0f};
    spawn_chaff_cluster(world, in_front, 40, 4.0f, /*spread=*/0.4f);   // the denser cell wins the aim
    spawn_chaff_cluster(world, behind, 6, 4.0f, /*spread=*/0.4f);

    const f32 front_before = density_in(world, in_front, 1.5f);
    const f32 back_before = density_in(world, behind, 1.5f);
    for (int i = 0; i < 40; ++i) step_combat(world);

    const CombatEvent* pulse = first_event(world, CombatEventType::ConePulse, TowerType::Interferon);
    REQUIRE(pulse != nullptr);
    REQUIRE(pulse->arc_radians > 0.0f);
    REQUIRE(pulse->radius >= ts.stats(TowerType::Interferon, 1).range);
    REQUIRE(pulse->direction.x > 0.5f);   // pointed at the dense side

    INFO("front " << front_before << " -> " << density_in(world, in_front, 1.5f)
         << ", behind " << back_before << " -> " << density_in(world, behind, 1.5f));
    REQUIRE(density_in(world, in_front, 1.5f) < front_before);
    REQUIRE(density_in(world, behind, 1.5f) == back_before);

    // The actual mechanism: crowd control, not damage. Everything in the cone
    // is slowed; nothing behind it is.
    const ChaffBuffers& c = world.chaff();
    usize slowed_front = 0, slowed_back = 0;
    for (usize i = 0; i < c.count(); ++i) {
        const Vec2 p{c.pos_x[i], c.pos_y[i]};
        const bool slowed = (c.flags[i] & chaff_flags::kSlowed) != 0;
        if (math::length_sq(p - in_front) <= 2.25f && slowed) ++slowed_front;
        if (math::length_sq(p - behind) <= 2.25f && slowed) ++slowed_back;
    }
    REQUIRE(slowed_front > 0);
    REQUIRE(slowed_back == 0);

    // ...and agents caught deep in the cone read as fully locked down.
    REQUIRE(count_events(world, CombatEventType::Freeze, TowerType::Interferon) > 0);

    // The CRYO is the roster's weakest killer by design.
    REQUIRE(ts.stats(TowerType::Interferon, 1).kill_rate <
            ts.stats(TowerType::NKCell, 1).kill_rate);
}

// ---------------------------------------------------------------------------
// TESLA — Cytotoxic T. Chain geometry.
// ---------------------------------------------------------------------------

TEST_CASE("TESLA chains from target to target, one ChainArc event per hop, each hop weaker",
          "[towers][combat][tesla]") {
    SimWorld world = make_world();
    TowerSystem ts;
    ts.register_systems(world);
    const EntityId tower = ts.place(world, TowerType::CytotoxicT, kRoomCenterLeft);
    REQUIRE(tower.valid());
    ready_now(world, tower);

    // A ladder of isolated agents 2.5 units apart: further than any single
    // tower field would cover as one blob, but inside the 3.0-unit hop radius.
    const Vec2 rungs[4] = {{14.0f, 10.0f}, {16.5f, 10.0f}, {19.0f, 10.0f}, {21.5f, 10.0f}};
    for (const Vec2& p : rungs) spawn_one(world, p, /*density=*/40.0f);
    std::vector<f32> before(world.chaff().count());
    for (usize i = 0; i < world.chaff().count(); ++i) before[i] = world.chaff().density[i];

    step_combat(world);

    const usize arcs = count_events(world, CombatEventType::ChainArc, TowerType::CytotoxicT);
    INFO("chain arcs raised: " << arcs);
    REQUIRE(arcs >= 3);

    // Per-hop falloff is carried in magnitude, which is exactly what the VFX
    // layer reads to make each successive link smaller.
    f32 prev_mag = 1.0e9f;
    Vec2 prev_secondary{-999.0f, -999.0f};
    for (const CombatEvent& e : world.combat_events().events()) {
        if (e.type != CombatEventType::ChainArc || e.source != TowerType::CytotoxicT) continue;
        REQUIRE(e.magnitude < prev_mag);
        REQUIRE(e.secondary != prev_secondary);   // every link ends somewhere new
        REQUIRE(math::length(e.secondary - e.origin) > 0.0f);
        prev_mag = e.magnitude;
        prev_secondary = e.secondary;
    }

    // The damage itself goes through the aggregate Chain field, and reaches
    // more than one rung.
    bool found_chain = false;
    for (const DamageField& f : world.damage().fields()) {
        if (f.owner == tower && f.shape == FieldShape::Chain) found_chain = true;
    }
    REQUIRE(found_chain);

    for (int i = 0; i < 10; ++i) step_combat(world);
    usize damaged = 0;
    for (usize i = 0; i < world.chaff().count(); ++i)
        if (world.chaff().density[i] < before[i]) ++damaged;
    INFO("rungs damaged: " << damaged);
    REQUIRE(damaged >= 3);
}

// ---------------------------------------------------------------------------
// LASER — B-Cell. Beam geometry.
// ---------------------------------------------------------------------------

TEST_CASE("LASER pierces everything along its line at once and misses everything off-axis",
          "[towers][combat][laser]") {
    SimWorld world = make_world();
    TowerSystem ts;
    ts.register_systems(world);
    const EntityId tower = ts.place(world, TowerType::BCell, kRoomCenterLeft);
    REQUIRE(tower.valid());
    ready_now(world, tower);

    const Vec2 near_on_axis{16.0f, 10.0f};
    const Vec2 far_on_axis{24.0f, 10.0f};
    const Vec2 off_axis{20.0f, 13.5f};
    spawn_chaff_cluster(world, near_on_axis, 30, 4.0f, /*spread=*/0.25f);  // wins the aim
    spawn_chaff_cluster(world, far_on_axis, 10, 4.0f, /*spread=*/0.25f);
    spawn_chaff_cluster(world, off_axis, 10, 4.0f, /*spread=*/0.25f);

    const f32 near_before = density_in(world, near_on_axis, 1.0f);
    const f32 far_before = density_in(world, far_on_axis, 1.0f);
    const f32 off_before = density_in(world, off_axis, 1.0f);

    for (int i = 0; i < 30; ++i) step_combat(world);

    const CombatEvent* beam = first_event(world, CombatEventType::BeamFired, TowerType::BCell);
    REQUIRE(beam != nullptr);
    REQUIRE(math::length(beam->secondary - beam->origin) >= 14.0f);   // spans the full reach
    REQUIRE(beam->direction.x > 0.5f);

    INFO("near " << near_before << "->" << density_in(world, near_on_axis, 1.0f)
         << "  far " << far_before << "->" << density_in(world, far_on_axis, 1.0f)
         << "  off " << off_before << "->" << density_in(world, off_axis, 1.0f));

    // Pierces: BOTH on-axis clusters lose density in the same window, including
    // the one hiding behind the first.
    REQUIRE(density_in(world, near_on_axis, 1.0f) < near_before);
    REQUIRE(density_in(world, far_on_axis, 1.0f) < far_before);
    // ...and the off-axis cluster, only 3.5 units to the side, is untouched.
    REQUIRE(density_in(world, off_axis, 1.0f) == off_before);

    submit_only(world);
    bool found_rect = false;
    for (const DamageField& f : world.damage().fields()) {
        if (f.owner != tower) continue;
        REQUIRE(f.shape == FieldShape::Rect);
        const Vec2 size = f.rect.size();
        REQUIRE(math::max(size.x, size.y) > 8.0f * math::min(size.x, size.y));  // long and thin
        found_rect = true;
    }
    REQUIRE(found_rect);
}

// ---------------------------------------------------------------------------
// BLADE — NK Cell. Contact geometry.
// ---------------------------------------------------------------------------

TEST_CASE("BLADE cuts what is adjacent to it and cannot touch what is a few units away",
          "[towers][combat][blade]") {
    SimWorld world = make_world();
    TowerSystem ts;
    ts.register_systems(world);
    const EntityId tower = ts.place(world, TowerType::NKCell, kRoomCenterLeft);
    REQUIRE(tower.valid());
    ready_now(world, tower);

    const f32 reach = ts.stats(TowerType::NKCell, 1).range;
    REQUIRE(reach < 4.0f);
    // Ringed around the tower, inside the rotor: 360 degrees, not a facing.
    const Vec2 adjacent[4] = {kRoomCenterLeft + Vec2{2.0f, 0.0f}, kRoomCenterLeft - Vec2{2.0f, 0.0f},
                              kRoomCenterLeft + Vec2{0.0f, 2.0f}, kRoomCenterLeft - Vec2{0.0f, 2.0f}};
    for (const Vec2& p : adjacent) spawn_chaff_cluster(world, p, 4, 6.0f, 0.2f);
    const Vec2 distant = kRoomCenterLeft + Vec2{7.0f, 0.0f};
    spawn_chaff_cluster(world, distant, 8, 6.0f, 0.2f);

    f32 adjacent_before = 0.0f;
    for (const Vec2& p : adjacent) adjacent_before += density_in(world, p, 0.6f);
    const f32 distant_before = density_in(world, distant, 0.6f);

    for (int i = 0; i < 30; ++i) step_combat(world);

    f32 adjacent_after = 0.0f;
    for (const Vec2& p : adjacent) adjacent_after += density_in(world, p, 0.6f);
    INFO("adjacent " << adjacent_before << "->" << adjacent_after << "  distant "
         << distant_before << "->" << density_in(world, distant, 0.6f));
    REQUIRE(adjacent_after < adjacent_before);
    REQUIRE(density_in(world, distant, 0.6f) == distant_before);

    // Every one of the four directions took damage: the rotor is 360 degrees.
    for (const Vec2& p : adjacent) {
        INFO("arm at " << p.x << "," << p.y);
        REQUIRE(density_in(world, p, 0.6f) < 24.0f);
    }

    REQUIRE(count_events(world, CombatEventType::BladeSlash, TowerType::NKCell) > 0);
    const CombatEvent* slash = first_event(world, CombatEventType::BladeSlash, TowerType::NKCell);
    REQUIRE(slash != nullptr);
    REQUIRE(math::length(slash->origin - kRoomCenterLeft) <= reach);   // contact point, on the agent
    REQUIRE(math::length(slash->direction) > 0.5f);                    // blade travel is a real vector

    submit_only(world);
    bool found_circle = false;
    for (const DamageField& f : world.damage().fields()) {
        if (f.owner != tower) continue;
        REQUIRE(f.shape == FieldShape::Circle);
        REQUIRE(f.origin == kRoomCenterLeft);   // pinned to the tower, not lobbed
        REQUIRE(f.radius == reach);
        found_circle = true;
    }
    REQUIRE(found_circle);
}

TEST_CASE("BLADE is still the only tower that can reach a Burrowed named agent",
          "[towers][combat][blade][targeting]") {
    SimWorld nk_world = make_world();
    TowerSystem nk_ts;
    nk_ts.register_systems(nk_world);
    const EntityId nk_tower = nk_ts.place(nk_world, TowerType::NKCell, kRoomCenterLeft);
    REQUIRE(nk_tower.valid());
    ready_now(nk_world, nk_tower);
    const EntityId nk_target = spawn_named(nk_world, kRoomCenterLeft + Vec2{0.5f, 0.0f});
    nk_world.ecs().registry().get<comp::AiBrain>(nk_world.ecs().from_id(nk_target)).state = comp::AiState::Burrowed;
    const f32 nk_hp_before = nk_world.ecs().registry().get<comp::Health>(nk_world.ecs().from_id(nk_target)).current;
    step_combat(nk_world);
    REQUIRE(nk_world.ecs().registry().get<comp::Health>(nk_world.ecs().from_id(nk_target)).current < nk_hp_before);

    SimWorld ct_world = make_world();
    TowerSystem ct_ts;
    ct_ts.register_systems(ct_world);
    const EntityId ct_tower = ct_ts.place(ct_world, TowerType::CytotoxicT, kRoomCenterLeft);
    REQUIRE(ct_tower.valid());
    ready_now(ct_world, ct_tower);
    const EntityId ct_target = spawn_named(ct_world, kRoomCenterLeft + Vec2{0.5f, 0.0f});
    ct_world.ecs().registry().get<comp::AiBrain>(ct_world.ecs().from_id(ct_target)).state = comp::AiState::Burrowed;
    const f32 ct_hp_before = ct_world.ecs().registry().get<comp::Health>(ct_world.ecs().from_id(ct_target)).current;
    step_combat(ct_world);
    REQUIRE(ct_world.ecs().registry().get<comp::Health>(ct_world.ecs().from_id(ct_target)).current == ct_hp_before);
}

// ---------------------------------------------------------------------------
// Cross-cutting contracts
// ---------------------------------------------------------------------------

TEST_CASE("every tower stamps its own TowerType and tier onto the events it raises",
          "[towers][combat][events]") {
    // vfx/Particles.cpp keys the ENTIRE per-tower look on (type, source,
    // visual_id). A tower that raises events with source == Count renders as
    // the generic grey fallback, which is indistinguishable from "broken".
    struct Case { TowerType type; CombatEventType expect; };
    const Case cases[] = {
        {TowerType::Neutrophil, CombatEventType::MuzzleFlash},
        {TowerType::Macrophage, CombatEventType::Explosion},
        {TowerType::Interferon, CombatEventType::ConePulse},
        {TowerType::CytotoxicT, CombatEventType::ChainArc},
        {TowerType::BCell, CombatEventType::BeamFired},
        {TowerType::NKCell, CombatEventType::BladeSlash},
    };

    for (const Case& c : cases) {
        for (u8 tier = 1; tier <= 3; ++tier) {
            INFO("tower " << tower_type_name(c.type) << " tier " << static_cast<int>(tier));
            SimWorld world = make_world();
            TowerSystem ts;
            ts.register_systems(world);
            const EntityId tower = ts.place(world, c.type, kRoomCenterLeft);
            REQUIRE(tower.valid());
            for (u8 k = 1; k < tier; ++k) REQUIRE(ts.upgrade(world, tower) == k + 1);
            ready_now(world, tower);
            // Something to shoot at, close enough for even the 3.2-unit rotor.
            spawn_chaff_cluster(world, kRoomCenterLeft + Vec2{2.0f, 0.0f}, 30, 20.0f, 0.3f);
            spawn_chaff_cluster(world, kRoomCenterLeft + Vec2{4.5f, 0.0f}, 20, 20.0f, 0.3f);

            for (int i = 0; i < 5; ++i) step_combat(world);

            const CombatEvent* e = first_event(world, c.expect, c.type);
            REQUIRE(e != nullptr);
            REQUIRE(e->source == c.type);
            // 3 data tiers spread across the VFX layer's 1..5 escalation axis.
            const u16 expected_visual = tier >= 3 ? 5 : (tier == 2 ? 3 : 1);
            REQUIRE(e->visual_id == expected_visual);
        }
    }
}

TEST_CASE("attaching a combat-event sink cannot change one bit of state_hash",
          "[towers][combat][determinism]") {
    // CombatEvents.h's central promise, tested from the producer side: events
    // are an OUTPUT of the tick. Same seed, same towers, same result — with the
    // sink drained every tick or never drained at all.
    auto run = [](bool drain_every_tick) {
        SimWorld world = make_world();
        TowerSystem ts;
        ts.register_systems(world);
        REQUIRE(ts.place(world, TowerType::Neutrophil, kRoomCenterLeft).valid());
        REQUIRE(ts.place(world, TowerType::NKCell, kRoomCenterLeft + Vec2{4.0f, 0.0f}).valid());
        spawn_chaff_cluster(world, kRoomCenterLeft + Vec2{5.0f, 1.0f}, 120, 3.0f, 1.5f);
        for (int i = 0; i < 240; ++i) {
            world.tick();
            if (drain_every_tick) world.combat_events().clear();
        }
        return world.state_hash();
    };
    REQUIRE(run(true) == run(false));
}

TEST_CASE("the same seed and the same towers produce the same tick-by-tick hashes",
          "[towers][combat][determinism]") {
    // The Gunner is the only tower that draws from the sim Rng, so this is
    // really a test that its one-draw-per-round rule holds.
    auto run = [] {
        SimWorld world = make_world();
        TowerSystem ts;
        ts.register_systems(world);
        REQUIRE(ts.place(world, TowerType::Neutrophil, kRoomCenterLeft).valid());
        spawn_chaff_cluster(world, kRoomCenterLeft + Vec2{5.0f, 0.0f}, 150, 2.0f, 1.2f);
        std::vector<u64> hashes;
        for (int i = 0; i < 180; ++i) {
            world.tick();
            hashes.push_back(world.state_hash());
        }
        return hashes;
    };
    const std::vector<u64> a = run();
    const std::vector<u64> b = run();
    REQUIRE(a == b);
    // ...and it isn't trivially constant.
    REQUIRE(a.front() != a.back());
}

TEST_CASE("a full roster of towers against a 10k horde stays inside the ecs_tick budget",
          "[towers][combat][perf]") {
    // AGENT_BRIEF §5 gives the ECS phase (<=200 named agents) a 2 ms budget, and
    // the Combat phase is where every tower system runs. No registered --bench
    // scenario places towers at all (app/Modes.cpp's run_bench never constructs
    // a TowerSystem), so `--bench chaff10k` measures an ecs_tick with no tower
    // work in it whatsoever. This is the actual measurement for Wave 6C's cost:
    // 24 towers, four of every role, against 10,000 agents.
    SimWorld world;
    SimDesc desc;
    desc.seed = 6060;
    desc.max_chaff = 16384;
    desc.world_bounds = Rect{Vec2{0.0f, 0.0f}, Vec2{256.0f, 144.0f}};
    world.init(desc, nullptr);

    TissueMask& mask = world.tissue();
    mask.resize(256, 144, 1.0f, Vec2{0.0f, 0.0f});
    for (i32 y = 4; y < 140; ++y)
        for (i32 x = 4; x < 252; ++x) mask.set_walkable(x, y, true);
    world.sdf().bake(mask);
    FlowFieldBakeDesc fdesc;
    fdesc.goal_cells = {mask.world_to_cell(Vec2{250.0f, 72.0f})};
    world.flow().bake(mask, fdesc);

    // 200 named agents too: strike_named() calls find_target() on every firing
    // tick, and find_target() is a linear scan of the named view. With no
    // elites present that scan is free, which would make this measurement a
    // lie about the real worst case.
    REQUIRE(named::setup_bench_scenario(world, 200) == 200);

    TowerSystem ts;
    ts.register_systems(world);
    u32 placed = 0;
    for (u32 i = 0; i < 24; ++i) {
        const auto type = static_cast<TowerType>(i % kTowerTypeCount);
        const Vec2 at{20.0f + 28.0f * static_cast<f32>(i % 8), 40.0f + 30.0f * static_cast<f32>(i / 8)};
        if (ts.place(world, type, at).valid()) ++placed;
    }
    REQUIRE(placed == 24);

    Rng r(31337);
    for (u32 i = 0; i < 10000; ++i) {
        ChaffSpawnParams p;
        p.position = Vec2{r.range_f(10.0f, 246.0f), r.range_f(10.0f, 134.0f)};
        p.density = 1000.0f;   // survives the whole run so the load never thins
        p.family = static_cast<PathogenFamily>(i % kFamilyCount);
        world.chaff().spawn(p);
    }
    REQUIRE(world.chaff().count() == 10000);

    Profiler profiler;
    profiler.reserve(400);
    for (int i = 0; i < 30; ++i) world.tick(nullptr);   // warm up caches / scratch
    for (int i = 0; i < 240; ++i) world.tick(&profiler);

    const auto stats = profiler.summarize();
    const TimingStats ecs = stats.at(prof_key::kEcsTick);
    const TimingStats chaff = stats.at(prof_key::kChaffUpdate);
    // chaff_update is printed for context only and is NOT comparable to the
    // §8.6 gate: this world is built with jobs == nullptr, so the movement
    // kernel runs fully serial here while `--bench` runs it on the JobSystem.
    std::fprintf(stderr,
                 "[wave6c perf] 24 towers @ 10k chaff -- ecs_tick avg=%.3fms p99=%.3fms max=%.3fms | "
                 "chaff_update (SERIAL, not the gate) avg=%.3fms p99=%.3fms | live rounds=%zu\n",
                 ecs.avg_ms, ecs.p99_ms, ecs.max_ms, chaff.avg_ms, chaff.p99_ms,
                 world.projectiles().count());

    CHECK(ecs.p99_ms < 2.0);
    CHECK(ecs.avg_ms < 1.0);
}

// ---------------------------------------------------------------------------
// Neutrophil's NET ability (kept from Wave 2B) and Interferon's flash freeze.
// ---------------------------------------------------------------------------

TEST_CASE("Neutrophil's NET ability slows chaff and spawns drifting micro-units",
          "[towers][ability][neutrophil]") {
    SimWorld world = make_world();
    TowerSystem ts;
    ts.register_systems(world);
    const EntityId tower = ts.place(world, TowerType::Neutrophil, kRoomCenterLeft);
    REQUIRE(tower.valid());
    spawn_chaff_cluster(world, kRoomCenterLeft, 5, 1.0f, /*spread=*/0.1f);
    rebuild_spatial(world);

    const usize entities_before = world.ecs().entity_count();
    REQUIRE(ts.trigger_ability(world, tower));
    REQUIRE(world.ecs().entity_count() == entities_before + 4);   // 3 micro-units + 1 NET

    const entt::entity micro = *world.ecs().registry().view<comp::Ephemeral, comp::Velocity>().begin();
    const Vec2 pos_before = world.ecs().registry().get<comp::Transform>(micro).position;
    step_combat(world);
    REQUIRE(world.ecs().registry().get<comp::Transform>(micro).position != pos_before);

    for (usize i = 0; i < world.chaff().count(); ++i) {
        REQUIRE((world.chaff().flags[i] & chaff_flags::kSlowed) != 0);
    }
}

TEST_CASE("Interferon's flash-freeze ability locks down a full circle and raises Freeze events",
          "[towers][ability][interferon]") {
    SimWorld world = make_world();
    TowerSystem ts;
    ts.register_systems(world);
    const EntityId tower = ts.place(world, TowerType::Interferon, kRoomCenterLeft);
    REQUIRE(tower.valid());
    // Behind the tower as well as in front: the ability is a nova, not a cone.
    spawn_chaff_cluster(world, kRoomCenterLeft + Vec2{4.0f, 0.0f}, 6, 2.0f, 0.2f);
    spawn_chaff_cluster(world, kRoomCenterLeft - Vec2{4.0f, 0.0f}, 6, 2.0f, 0.2f);
    rebuild_spatial(world);

    REQUIRE(ts.trigger_ability(world, tower));
    REQUIRE(world.ecs().registry().get<comp::Tower>(world.ecs().from_id(tower)).ability_cooldown > 0.0f);
    for (usize i = 0; i < world.chaff().count(); ++i) {
        REQUIRE((world.chaff().flags[i] & chaff_flags::kSlowed) != 0);
    }
    REQUIRE(count_events(world, CombatEventType::Freeze, TowerType::Interferon) > 0);
    REQUIRE_FALSE(ts.trigger_ability(world, tower));   // on cooldown now
}

// ---------------------------------------------------------------------------
// VISUAL PROOF
//
// The --screenshot CLI mode cannot demonstrate this work: app/Modes.cpp's
// run_screenshot() places no towers and never calls submit_projectiles() or
// submit_particles(), so a screenshot taken through the CLI shows tissue and
// chaff and nothing else no matter how many combat events the sim raises. That
// file is orchestrator-owned and outside this wave's directories (see the
// report). These tests therefore drive the SAME headless-GL Renderer path
// through the public APIs directly — exactly the fallback
// tests/test_render_vfx.cpp already established for the same reason — and
// write PNGs to $TEMP for manual read-back.
// ---------------------------------------------------------------------------

namespace {

struct HeadlessGl {
    platform::Window window;
    bool ok = false;
    HeadlessGl(i32 w, i32 h) { ok = platform::create_headless_gl(window, w, h); }
};

std::string scratch_path(const std::string& filename) {
    const char* t = std::getenv("TEMP");
    if (t == nullptr) t = std::getenv("TMP");
    const std::string dir = t != nullptr ? std::string(t) : std::string(".");
    return dir + "/" + filename;
}

constexpr f32 kShowW = 120.0f;
constexpr f32 kShowH = 68.0f;

/// Open arena with a two-cell wall margin, so every tower has clearance and the
/// horde has somewhere to be.
SimWorld make_showcase_world(u64 seed) {
    SimWorld world;
    SimDesc desc;
    desc.seed = seed;
    desc.max_chaff = 16384;
    desc.world_bounds = Rect{Vec2{0.0f, 0.0f}, Vec2{kShowW, kShowH}};
    world.init(desc, nullptr);

    TissueMask& mask = world.tissue();
    mask.resize(static_cast<i32>(kShowW), static_cast<i32>(kShowH), 1.0f, Vec2{0.0f, 0.0f});
    for (i32 y = 3; y < static_cast<i32>(kShowH) - 3; ++y)
        for (i32 x = 3; x < static_cast<i32>(kShowW) - 3; ++x) mask.set_walkable(x, y, true);
    world.sdf().bake(mask);
    FlowFieldBakeDesc fdesc;
    fdesc.goal_cells = {mask.world_to_cell(Vec2{kShowW - 5.0f, kShowH * 0.5f})};
    world.flow().bake(mask, fdesc);
    return world;
}

/// One frame of the real App loop's VFX half: drain the tick's combat events
/// into particles, clear the sink, advance the particles on the render clock.
void pump_vfx(SimWorld& world, vfx::ParticleSystem& particles, f32 dt) {
    const auto& events = world.combat_events().events();
    particles.emit_for_events(events.data(), events.size());
    world.combat_events().clear();
    particles.update(dt, nullptr);
}

/// Same submission order App::render_frame() uses, then a PNG.
bool capture(SimWorld& world, vfx::ParticleSystem& particles, render::Renderer& renderer,
             const render::Camera& camera, const std::string& out, render::FrameStats& out_stats) {
    static std::vector<vfx::ParticleInstance> scratch;
    renderer.begin_frame(camera, 0.0f);
    renderer.submit_tissue(world.tissue(), world.sdf(), 0.0f);
    renderer.submit_chaff(world.chaff(), world.spatial());
    renderer.submit_entities(world.ecs());
    renderer.submit_fields(world.damage().fields().data(), world.damage().fields().size());
    renderer.submit_projectiles(world.projectiles());
    particles.build_instances(vfx::BlendMode::Additive, scratch);
    renderer.submit_particles(scratch.data(), scratch.size(), vfx::BlendMode::Additive);
    particles.build_instances(vfx::BlendMode::AlphaBlend, scratch);
    renderer.submit_particles(scratch.data(), scratch.size(), vfx::BlendMode::AlphaBlend);
    renderer.end_frame();
    out_stats = renderer.stats();

    std::vector<u8> pixels;
    i32 w = 0, h = 0;
    if (!renderer.read_pixels(pixels, w, h)) return false;
    return render::write_png_rgba(out, pixels.data(), w, h);
}

} // namespace

TEST_CASE("VISUAL: all six towers fire at once, with live projectiles and particles",
          "[towers][combat][visual][gl]") {
    HeadlessGl gl(1600, 900);
    if (!gl.ok) { WARN("headless GL unavailable; skipping"); return; }

    SimWorld world = make_showcase_world(4242);
    TowerSystem ts;
    ts.register_systems(world);

    struct Site { TowerType type; Vec2 tower; Vec2 horde; };
    const Site sites[] = {
        {TowerType::Neutrophil, {22.0f, 50.0f}, {30.0f, 50.0f}},   // GUNNER
        {TowerType::Macrophage, {22.0f, 18.0f}, {34.0f, 18.0f}},   // MORTAR
        {TowerType::Interferon, {62.0f, 50.0f}, {69.0f, 50.0f}},   // CRYO
        {TowerType::CytotoxicT, {62.0f, 18.0f}, {67.0f, 18.0f}},   // TESLA
        {TowerType::BCell,      {98.0f, 50.0f}, {108.0f, 50.0f}},  // LASER
        {TowerType::NKCell,     {98.0f, 18.0f}, {100.0f, 18.0f}},  // BLADE
    };

    std::vector<EntityId> towers;
    for (const Site& s : sites) {
        const EntityId id = ts.place(world, s.type, s.tower);
        INFO("placing " << tower_type_name(s.type));
        REQUIRE(id.valid());
        // Tier 3 everywhere: this is the escalated look the brief describes.
        REQUIRE(ts.upgrade(world, id) == 2);
        REQUIRE(ts.upgrade(world, id) == 3);
        towers.push_back(id);
    }

    vfx::ParticleSystem particles;
    particles.init(vfx::ParticleSystem::kDefaultCapacity, 0xBEEF'CAFEull);

    auto reseed_hordes = [&] {
        for (const Site& s : sites) spawn_chaff_cluster(world, s.horde, 60, 20.0f, /*spread=*/1.8f);
    };
    reseed_hordes();

    // Warm up: rounds get into flight, the Cryo cone builds up its slow, the
    // Blade grinds. Top the hordes back up so nothing is starved at capture.
    for (int t = 0; t < 90; ++t) {
        world.tick();
        pump_vfx(world, particles, kFixedDt);
        if (t % 30 == 29) reseed_hordes();
    }

    // Showtime: every tower fires on the same tick, so one still frame carries
    // all six vocabularies at once instead of whichever happened to be mid-
    // cooldown. Capture 4 ticks later: past the Mortar's 0.055s charge stage
    // (so the shockwave ring is out) and still inside the Laser's 0.09s beam
    // and the Tesla's ~0.07s arcs.
    for (EntityId id : towers) ready_now(world, id);
    for (int t = 0; t < 4; ++t) {
        world.tick();
        pump_vfx(world, particles, kFixedDt);
    }

    INFO("live particles: " << particles.live_count() << ", live rounds: " << world.projectiles().count());
    REQUIRE(particles.live_count() > 200);
    REQUIRE(world.projectiles().count() > 0);

    render::RendererDesc rd;
    rd.framebuffer_width = gl.window.width();
    rd.framebuffer_height = gl.window.height();
    rd.max_chaff_instances = static_cast<u32>(world.desc().max_chaff);
    render::Renderer renderer;
    REQUIRE(renderer.init(rd));

    render::Camera camera;
    camera.set_viewport(gl.window.width(), gl.window.height());
    camera.set_bounds(world.desc().world_bounds);
    camera.set_center(world.desc().world_bounds.center());
    camera.set_view_height(kShowH);
    camera.clamp_to_bounds();

    const std::string out = scratch_path("wave6c_six_towers.png");
    render::FrameStats stats{};
    REQUIRE(capture(world, particles, renderer, camera, out, stats));
    std::fprintf(stderr,
                 "[wave6c] %s  particles_drawn=%u projectiles_drawn=%u fields=%u chaff=%u draws=%u\n",
                 out.c_str(), stats.particle_instances_drawn, stats.projectile_instances_drawn,
                 stats.vfx_fields_drawn, stats.chaff_instances_drawn, stats.draw_calls);

    REQUIRE(stats.particle_instances_drawn > 200);
    REQUIRE(stats.projectile_instances_drawn > 0);

    // Per-role close-ups of the SAME instant. Nothing is advanced between these
    // captures, so they are literally one frame seen six times at 5x the linear
    // zoom — the Blade's contact slashes and the Tesla's arcs are a couple of
    // world units across and do not survive a whole-world framing at 1600px.
    for (const Site& s : sites) {
        camera.set_view_height(26.0f);
        camera.set_center(math::lerp(s.tower, s.horde, 0.55f));
        render::FrameStats close{};
        const std::string path = scratch_path(std::string("wave6c_") + tower_type_name(s.type) + ".png");
        REQUIRE(capture(world, particles, renderer, camera, path, close));
        std::fprintf(stderr, "[wave6c] %s particles=%u rounds=%u\n", path.c_str(),
                     close.particle_instances_drawn, close.projectile_instances_drawn);
    }

    renderer.shutdown();
}

TEST_CASE("VISUAL: a maxed GUNNER reads as a continuous stream of rounds",
          "[towers][combat][visual][gl][gunner]") {
    HeadlessGl gl(1280, 720);
    if (!gl.ok) { WARN("headless GL unavailable; skipping"); return; }

    SimWorld world = make_showcase_world(77);
    TowerSystem ts;
    ts.register_systems(world);

    const Vec2 tower_pos{40.0f, 34.0f};
    const EntityId tower = ts.place(world, TowerType::Neutrophil, tower_pos);
    REQUIRE(tower.valid());
    REQUIRE(ts.upgrade(world, tower) == 2);
    REQUIRE(ts.upgrade(world, tower) == 3);

    vfx::ParticleSystem particles;
    particles.init(vfx::ParticleSystem::kDefaultCapacity, 0x5EED'1234ull);

    // A standing wall of tanky chaff at the far end of the Gunner's reach, held
    // in place by step_combat() (no flow movement, no compaction). Held still on
    // purpose: rounds-in-flight is fire_rate x flight_time, and letting the
    // horde drift or get shoved back onto the muzzle by separation pressure
    // would measure the horde's behaviour rather than the Gunner's.
    const Vec2 horde = tower_pos + Vec2{10.0f, 0.0f};
    spawn_chaff_cluster(world, horde, 28, 400.0f, /*spread=*/1.2f);
    for (int t = 0; t < 120; ++t) {
        step_combat(world);
        pump_vfx(world, particles, kFixedDt);
    }

    const usize live_rounds = world.projectiles().count();
    INFO("live rounds in flight: " << live_rounds << ", particles: " << particles.live_count());
    // A stream, not a shot: several rounds simultaneously in the air.
    REQUIRE(live_rounds >= 3);
    REQUIRE(particles.live_count() > 100);

    render::RendererDesc rd;
    rd.framebuffer_width = gl.window.width();
    rd.framebuffer_height = gl.window.height();
    rd.max_chaff_instances = static_cast<u32>(world.desc().max_chaff);
    render::Renderer renderer;
    REQUIRE(renderer.init(rd));

    render::Camera camera;
    camera.set_viewport(gl.window.width(), gl.window.height());
    camera.set_bounds(world.desc().world_bounds);
    camera.set_center(tower_pos + Vec2{5.0f, 0.0f});
    camera.set_view_height(22.0f);

    const std::string out = scratch_path("wave6c_gunner_stream.png");
    render::FrameStats stats{};
    REQUIRE(capture(world, particles, renderer, camera, out, stats));
    std::fprintf(stderr, "[wave6c] %s  particles_drawn=%u projectiles_drawn=%u rounds_live=%zu\n",
                 out.c_str(), stats.particle_instances_drawn, stats.projectile_instances_drawn,
                 live_rounds);

    REQUIRE(stats.projectile_instances_drawn == static_cast<u32>(live_rounds));
    renderer.shutdown();
}
